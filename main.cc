#include <iostream>
#include <stdlib.h>
#include <fstream>
#include <sstream>
#include <limits>

#include <ceph_hash.h>
#include <CrushWrapper.h>
#include <CrushCompiler.h>
#include <crush.h>
#include <CrushTester.h>
#include <rados.h>

using namespace std;
using namespace ceph;

#define BLOCK_OBJECT_SUFFIX_WIDTH 12

struct image_info_s {
  string image_name;
  size_t size_kb;
  unsigned num_objects;
  string block_name_prefix;
  image_info_s(const char *argv);
};

struct pool_info_s {
  int64_t pool_id;
  string pool_name;
  size_t rep_size;
  size_t min_size;
  unsigned crush_ruleset;
  int object_hash;
  int flags;
  int pg_num,
      pgp_num;
  int pg_num_mask, pgp_num_mask;
  pool_info_s(const char *argv);
};

const char *g_crush_mapfile = NULL;
image_info_s *g_image_info  = NULL;
pool_info_s *g_pool_info    = NULL;
const char *g_exe_name      = NULL;

int g_object_index                = -1;
static unsigned g_num_img_objects = 0;

static map<unsigned, list<string> > g_dev_map;

void print_usage(const char *msg=NULL) {
  if (msg)
    cerr << msg << std::endl;
  cerr << g_exe_name << std::endl << 
    "     --pool-info <pool information> " << std::endl << 
    "     --image-info <image information> " << std::endl <<
    "     --crushmap <crushmap file path> " << std::endl << 
    "     [--object <object index>]" << std::endl;
  exit(-1);
}

void get_quoted_string(string& in, string& out) {
  size_t start, end;

  start = in.find_first_of("'");
  if (start == string::npos) {
    out = in;
    return;
  }
  end = in.find_first_of("'", start + 1);
  if (end == string::npos) {
    cerr << "Error - there is no end quote in the string " << in << std::endl;
    assert(0);
  }

  out.assign(in, start+1, end - (start + 1));
}

static inline unsigned calc_mask(unsigned num) {
  int num_bits = 0, tmp_num = (num - 1);
  while(tmp_num > 0) {
    tmp_num >>= 1;
    num_bits++;
  }
  return (unsigned)((1 << num_bits) - 1);
}

pool_info_s::pool_info_s(const char *argv) : pool_id(-1), rep_size(0),
  crush_ruleset((unsigned)-1), flags(0) {
  /*
   * Pool information will be of this format
   *
   * "pool 0 'data' rep size 2 min_size 1 crush_ruleset 0 object_hash rjenkins \
   *  pg_num 128 pgp_num 128 last_change 1 owner 0 flags 0x1 crash_replay_interval 45"
   *
   */

  istringstream iss(argv);

  while(!iss.eof()) {
    string tmp;
    iss >> tmp;
    if (tmp.compare("pool") == 0) {
      iss >> this->pool_id;
      iss >> tmp;
      get_quoted_string(tmp, this->pool_name);
    } else if (tmp.compare("rep") == 0) {
      iss >> tmp;
      if (tmp.compare("size") != 0) {
        print_usage("Error : Pool information 'rep' should be followed by 'size'");
      }
      iss >> this->rep_size;
    } else if (tmp.compare("min_size") == 0) {
      iss >> this->min_size;
    } else if (tmp.compare("crush_ruleset") == 0) {
      iss >> this->crush_ruleset;
    } else if (tmp.compare("object_hash") == 0) {
      iss >> tmp;
      if (tmp.compare("rjenkins") == 0)
        this->object_hash = CEPH_STR_HASH_RJENKINS;
      else
        this->object_hash = CEPH_STR_HASH_LINUX;
    } else if (tmp.compare("pg_num") == 0) {
      iss >> this->pg_num;
      this->pg_num_mask = calc_mask(pg_num);
    } else if (tmp.compare("pgp_num") == 0) {
      iss >> this->pgp_num;
      this->pgp_num_mask = calc_mask(this->pgp_num);
    } else if (tmp.compare("flags") == 0) {
      iss >> this->flags;
    }
  }

  if (!this->rep_size ||
      (this->crush_ruleset == (unsigned)-1) ||
      (this->pool_id == -1)) {
    print_usage("Error : Pool information is in wrong format");
  }
}

image_info_s::image_info_s(const char *argv) {
  /*
   * Image information will be of this format
   *
   * "rbd image 'img_name': \
   *  size 10240 KB in 3 objects \
   *  order 22 (4096 KB objects) \
   *  block_name_prefix: rb.0.1008.74b0dc51 \
   *  format: 1"
   *
   */

  istringstream iss(argv);
  while(!iss.eof()) {
    string tmp;

    iss >> tmp;
    if (tmp.compare("rbd") == 0) {
      iss >> tmp;
      if (tmp.compare("image") != 0) 
        print_usage("Error : Image information 'rbd' "
                    "should be followed by 'image'");

      iss >> tmp;
      get_quoted_string(tmp, this->image_name);
    } else if (tmp.compare("size") == 0) {
      size_t sz;

      iss >> sz;
      iss >> tmp;
      if (tmp.compare("KB") == 0) {
        this->size_kb = sz;
      } else if (tmp.compare("MB") == 0) {
        this->size_kb = sz*1024;
      }

      iss >> tmp;
      iss >> this->num_objects;
      iss >> tmp;
      if (tmp.compare("objects") != 0) 
        print_usage("Error : Image information unexpected format in 2nd line");
    } else if (tmp.compare("order") == 0) {
      iss.ignore(numeric_limits<streamsize>::max(), ')');
    } else if (tmp.compare("block_name_prefix:") == 0) {
      iss >> this->block_name_prefix;
    }
  }
}

void check_and_print_usage(int argc, char *argv[]) {
  int arg_index = 1;

  g_exe_name = argv[0];
  while(arg_index < argc) {
    if (strcmp(argv[arg_index], "--pool-info") == 0) {
      g_pool_info = new pool_info_s(argv[arg_index + 1]);
      arg_index += 2;
    } else if (strcmp(argv[arg_index], "--image-info") == 0) {
      g_image_info = new image_info_s(argv[arg_index + 1]);
      arg_index += 2;
    } else if (strcmp(argv[arg_index], "--crushmap") == 0) {
      if ((arg_index + 1) == argc) {
        print_usage("Error : --crushmap should be followed by file path");
      }

      g_crush_mapfile = new char[strlen(argv[arg_index + 1]) + 1];
      strcpy((char *)g_crush_mapfile, argv[arg_index+1]);
      arg_index += 2;
    } else if (strcmp(argv[arg_index], "--object") == 0) {
      if ((arg_index + 1) == argc) {
        print_usage("Error : --object should be followed by a valid object"
                    " index (0 .. num_objects)");
      }

      g_object_index = atoi(argv[arg_index + 1]);
      arg_index += 2;
    } else if ((strcmp(argv[arg_index], "--help") == 0) ||
               (strcmp(argv[arg_index], "-h") == 0)) {
      print_usage(NULL);
    } else {
      print_usage("Error : Unknown argument");
    } 
  }

  if (!g_pool_info) {
    print_usage("Error : Pool information is mandatory");
  }

  if (!g_image_info) {
    print_usage("Error : Image information is mandatory");
  }

  if (!g_crush_mapfile) {
    print_usage("Error : Crushmap path is mandatory");
  }

  if ((int)g_image_info->num_objects <= g_object_index) {
    print_usage("Error : Object index is not in acceptable range");
  }
}

int get_crush_from_text(const char *fname, CrushWrapper& crush) {
  ostringstream oss;
  ifstream in;

  in.open(fname);
  if (!in.is_open()) {
    cerr << "Error compiling - file cannot be opened" << std::endl;
    return -1;
  }
  CrushCompiler crushc(crush, 
                       oss, 1);
  if(crushc.compile(in) != 0) {
    cerr << "Error compiling - err = " << oss.str().c_str() << std::endl;
    return -1;
  }
  return 0;
}

int get_crush_from_compiled(const char *fname, CrushWrapper& crush) {
  ifstream in;

  in.open(fname);
  if (!in.is_open()) {
    cerr << "Error compiling - file cannot be opened" << std::endl;
    return -1;
  }

  in.seekg (0, in.end);
  size_t flen = in.tellg();
  in.seekg (0, in.beg);

  char *data = new char[flen+1];
  in.read(data, flen);
  if (!in) {
    cerr << "Error compiling - cannot read " << flen << 
      " bytes from file" << std::endl;
    return -1;
  }

  bufferlist bl;
  bl.append(data, flen);

  bufferlist::iterator blit = bl.begin();
  crush.decode(blit);
  delete(data);
  return 0;
}

static int calc_x(string& name) {
#define FLAG_HASHPSPOOL 1 /* NOTE: Cannot include the header, 
                           * as it creates unncessary dependency
                           */
  unsigned seed = ceph_str_hash(g_pool_info->object_hash, 
                                name.c_str(), 
                                name.length());

  seed = ceph_stable_mod(seed, g_pool_info->pg_num, g_pool_info->pg_num_mask);
  if (g_pool_info->flags & FLAG_HASHPSPOOL) {
    return
      crush_hash32_2(CRUSH_HASH_RJENKINS1,
                     ceph_stable_mod(seed, g_pool_info->pgp_num,
                                     g_pool_info->pgp_num_mask),
                     g_pool_info->pool_id);
  }

  return
    ceph_stable_mod(seed, g_pool_info->pgp_num, g_pool_info->pgp_num_mask) +
    g_pool_info->pool_id;
}

static void calc_placement(string& image_name, CrushWrapper& crush,
                           const vector<unsigned>& weight) {
  vector<int> out;
  map<unsigned, list<string> >::iterator dev_map_it;

  int x = calc_x(image_name);
  crush.do_rule(g_pool_info->crush_ruleset, x, out, g_pool_info->rep_size, 
                weight);

  for (unsigned loop = 0; loop < out.size(); loop++) {
    if ((dev_map_it = g_dev_map.find(out[loop])) == g_dev_map.end()) {
      list<string> lst;
      lst.insert(lst.end(), image_name);
      g_dev_map[out[loop]] = lst;
    } else {
      list<string>& lst = dev_map_it->second;
      lst.insert(lst.end(), image_name);
    }
  }
  g_num_img_objects++;
  cout << "'" << image_name << "' will be placed in  => " << out << std::endl;
}

static void calc_obj_placement(int index, CrushWrapper& crush, 
                               const vector<unsigned>& weight) {
  ostringstream oss;

  oss << g_image_info->block_name_prefix << "." << setfill('0') << 
    setw(BLOCK_OBJECT_SUFFIX_WIDTH) << std::hex << index << std::dec;

  string image_name = oss.str().c_str();
  calc_placement(image_name, crush, weight);
}

int main(int argc, char *argv[]) {
  check_and_print_usage(argc, argv);

  ifstream in;
  in.open(g_crush_mapfile);
  if (!in.is_open()) {
    cerr << "Cannot open map file at " << g_crush_mapfile << std::endl;
    return -1;
  }

  unsigned magic;
  in.read((char *)&magic, sizeof(magic));
  in.close();

  CrushWrapper crush;

  if (magic != CRUSH_MAGIC) {
    if (get_crush_from_text(g_crush_mapfile, crush) < 0) {
      return -1;
    }
  } else {
    if (get_crush_from_compiled(g_crush_mapfile, crush) < 0) {
      cerr << "Error compiling map file" << std::endl;
      return -1;
    }
  }

  if ((int)g_pool_info->crush_ruleset >= crush.get_max_rules()) {
    print_usage("crush_ruleset for the pool is not proper");
  }

  vector<unsigned> weight;
  weight.resize(crush.get_max_devices());
  for (int loop = 0; loop < crush.get_max_devices(); loop++) {
    weight[loop] = CEPH_OSD_IN;
  }

  cout << std::endl << "Object summary" << std::endl;
  cout <<              "--------------" << std::endl;
  if (g_object_index < 0) {
    calc_placement(g_image_info->image_name, crush, weight);
    for (size_t loop = 0; loop < g_image_info->num_objects; loop++) {
      calc_obj_placement(loop, crush, weight);
    }
  } else {
    calc_obj_placement(g_object_index, crush, weight);
  }

  cout << std::endl << "Device summary" << std::endl;
  cout <<              "--------------" << std::endl;
  for (map<unsigned, list<string> >::iterator it = g_dev_map.begin();
       it != g_dev_map.end(); it++) {
    list<string>& lst = it->second;
    cout << setfill(' ') << setw(5) << it->first << " => " << std::endl;
    for (list<string>::iterator lit = lst.begin(); 
         lit != lst.end(); lit++) {
      cout << setfill(' ') << setw(9) << "=> " <<  *lit << std::endl;
    }
  }

  cout << std::endl << "Total summary" << std::endl;
  cout <<              "-------------" << std::endl;
  cout << "Total " << g_dev_map.size() << " devices " << std::endl 
       << "  for " << g_pool_info->rep_size << " replications " << std::endl 
       << "   of " << g_num_img_objects << " objects" << std::endl;

  return 0;
}
