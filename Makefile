
status = notok;

ifndef CEPH_SRCDIR
  msg = "Kindly define CEPH_SRCDIR"
else
  ifeq ($(wildcard $(CEPH_SRCDIR)/crush),)
    msg = "CEPH_SRCDIR seems to be wrong - it should point to ceph_srcroot/src"
  else
    ifeq ($(wildcard $(CEPH_SRCDIR)/.libs/libcommon.a),)
      msg = "You have to build CEPH before building this tool"
    else
      status = ok;
    endif
  endif
endif

CFLAGS=-I$(CEPH_SRCDIR)/crush/ -I$(CEPH_SRCDIR) -I$(CEPH_SRCDIR)/include -g -Wall

%.o:%.cc
	g++ -c $^ $(CFLAGS) -o $@

LDFLAGS= -Wl,--whole-archive $(CEPH_SRCDIR)/.libs/libcommon.a -Wl,--no-whole-archive
LDLIBS=-lpthread -lcryptopp -lboost_thread -luuid -lrt 

all:check tool
	@echo "Make completed"

X_notok:
	$(error $(msg))

X_ok:

check: X_$(status)

tool:main.o
	g++ main.o $(LDFLAGS) $(LDLIBS) -o tool

clean:
	@rm -f main.o tool
