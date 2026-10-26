CC?=gcc
CXX?=g++

STLIBNAME?=../lib/libvalkey.a
TLS_STLIBNAME?=../lib/libvalkey_tls.a
INCLUDE_DIR?=../include

WARNINGS=-Wall -Wextra
CFLAGS?=-fPIC -g -O2 $(WARNINGS) -I $(INCLUDE_DIR)

USE_WERROR?=1
ifeq ($(USE_WERROR),1)
  WARNINGS+=-Werror
endif

# Define examples
EXAMPLES=example-blocking example-blocking-push example-async-libevent \
	 example-async-libev example-async-glib example-async-poll \
	 example-cluster-async example-cluster-clientside-caching-async \
	 example-cluster-simple

ifeq ($(USE_TLS),1)
  EXAMPLES+=example-blocking-tls example-async-libevent-tls \
	    example-cluster-async-tls example-cluster-tls
  TLS_LDFLAGS=-lssl -lcrypto
endif

.PHONY: all clean

all: $(EXAMPLES)

$(STLIBNAME):
	$(MAKE) -C ../

$(TLS_STLIBNAME):
	USE_TLS=1 $(MAKE) -C ../

example-blocking: blocking.c $(STLIBNAME)
	$(CC) -o $@ $(CFLAGS) $< $(STLIBNAME)

example-blocking-push: blocking-push.c $(STLIBNAME)
	$(CC) -o $@ $(CFLAGS) $< $(STLIBNAME)

example-blocking-tls: blocking-tls.c $(STLIBNAME) $(TLS_STLIBNAME)
	$(CC) -o $@ $(CFLAGS) $< $(STLIBNAME) $(TLS_STLIBNAME) $(TLS_LDFLAGS)

example-async-libevent: async-libevent.c $(STLIBNAME)
	$(CC) -o $@ $(CFLAGS) $< -levent $(STLIBNAME)

example-async-libevent-tls: async-libevent-tls.c $(STLIBNAME) $(TLS_STLIBNAME)
	$(CC) -o $@ $(CFLAGS) $< -levent $(STLIBNAME) $(TLS_STLIBNAME) $(TLS_LDFLAGS)

example-async-libev: async-libev.c $(STLIBNAME)
	$(CC) -o $@ $(CFLAGS) $< -lev $(STLIBNAME)

example-async-libhv: async-libhv.c $(STLIBNAME)
	$(CC) -o $@ $(CFLAGS) $< -lhv $(STLIBNAME)

example-async-libsdevent: async-libsdevent.c $(STLIBNAME)
	$(CC) -o $@ $(CFLAGS) $< -lsystemd $(STLIBNAME)

example-async-glib: async-glib.c $(STLIBNAME)
	$(CC) -o $@ $(CFLAGS) $< $(shell pkg-config --cflags --libs glib-2.0) $(STLIBNAME)

example-async-ivykis: async-ivykis.c $(STLIBNAME)
	$(CC) -o $@ $(CFLAGS) $< -livykis $(STLIBNAME)

example-async-macosx: async-macosx.c $(STLIBNAME)
	$(CC) -o $@ $(CFLAGS) $< -framework CoreFoundation $(STLIBNAME)

example-async-poll: async-poll.c $(STLIBNAME)
	$(CC) -o $@ $(CFLAGS) $< $(STLIBNAME)

ifndef AE_DIR
example-async-ae:
	@echo "Please specify AE_DIR (e.g. <valkey repository>/src)"
	@false
else
example-async-ae: async-ae.c $(STLIBNAME)
	$(CC) -o $@ $(CFLAGS) $(LDFLAGS) -I$(AE_DIR) $< $(AE_DIR)/ae.o $(AE_DIR)/zmalloc.o \
	$(AE_DIR)/monotonic.o $(AE_DIR)/anet.o $(AE_DIR)/serverassert.o $(AE_DIR)/../deps/jemalloc/lib/libjemalloc.a \
	-pthread $(STLIBNAME)
endif

ifndef LIBUV_DIR
# dynamic link libuv.so
example-async-libuv: async-libuv.c $(STLIBNAME)
	$(CC) -o $@ $(CFLAGS) -I$(LIBUV_DIR)/include $< -luv -lpthread -lrt $(STLIBNAME)
else
# use user provided static lib
example-async-libuv: async-libuv.c $(STLIBNAME)
	$(CC) -o $@ $(CFLAGS) -I$(LIBUV_DIR)/include $< $(LIBUV_DIR)/.libs/libuv.a -lpthread -lrt $(STLIBNAME)
endif

ifeq ($(and $(QT_MOC),$(QT_INCLUDE_DIR),$(QT_LIBRARY_DIR)),)
example-async-qt:
	@echo "Please specify QT_MOC, QT_INCLUDE_DIR AND QT_LIBRARY_DIR"
	@false
else
example-async-qt: async-qt.cpp $(STLIBNAME)
	$(QT_MOC) ../include/valkey/adapters/qt.h | \
	    $(CXX) -x c++ -o qt-adapter-moc.o -c - $(CFLAGS) -I$(QT_INCLUDE_DIR) -I$(QT_INCLUDE_DIR)/QtCore
	$(QT_MOC) async-qt.h | \
	    $(CXX) -x c++ -o qt-example-moc.o -c - $(CFLAGS) -I$(QT_INCLUDE_DIR) -I$(QT_INCLUDE_DIR)/QtCore
	$(CXX) -o $@ $(CFLAGS) $(LDFLAGS) -I$(QT_INCLUDE_DIR) -I$(QT_INCLUDE_DIR)/QtCore -L$(QT_LIBRARY_DIR) qt-adapter-moc.o qt-example-moc.o $< -pthread $(STLIBNAME) -lQt6Core
endif

example-cluster-async: cluster-async.c $(STLIBNAME)
	$(CC) -o $@ $(CFLAGS) $< -levent $(STLIBNAME)

example-cluster-async-tls: cluster-async-tls.c $(STLIBNAME) $(TLS_STLIBNAME)
	$(CC) -o $@ $(CFLAGS) $< -levent $(STLIBNAME) $(TLS_STLIBNAME) $(TLS_LDFLAGS)

example-cluster-clientside-caching-async: cluster-clientside-caching-async.c $(STLIBNAME)
	$(CC) -o $@ $(CFLAGS) $< -levent $(STLIBNAME)

example-cluster-simple: cluster-simple.c $(STLIBNAME)
	$(CC) -o $@ $(CFLAGS) $< $(STLIBNAME)

example-cluster-tls: cluster-tls.c $(STLIBNAME) $(TLS_STLIBNAME)
	$(CC) -o $@ $(CFLAGS) $< $(STLIBNAME) $(TLS_STLIBNAME) $(TLS_LDFLAGS)

clean:
	rm -f example-* *.o
