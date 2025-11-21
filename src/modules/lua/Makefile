uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')

DEPS_DIR=../../../deps

ifeq ($(uname_S),Linux)
	SHOBJ_CFLAGS= -I. -I$(DEPS_DIR)/lua/src -I$(DEPS_DIR)/fpconv -fPIC -W -Wall -fno-common $(OPTIMIZATION) -std=c99 -D_GNU_SOURCE $(CFLAGS)
	SHOBJ_LDFLAGS= -shared
else
	SHOBJ_CFLAGS= -I. -I$(DEPS_DIR)/lua/src -I$(DEPS_DIR)/fpconv -fPIC -W -Wall -dynamic -fno-common $(OPTIMIZATION) -std=c99 -D_GNU_SOURCE $(CFLAGS)
	SHOBJ_LDFLAGS= -bundle -undefined dynamic_lookup
endif

LIBS= $(DEPS_DIR)/lua/src/liblua.a $(DEPS_DIR)/fpconv/libfpconv.a
SRCS= $(wildcard *.c)
OBJS= $(SRCS:.c=.o) sha1.o rand.o

# OS X 11.x doesn't have /usr/lib/libSystem.dylib and needs an explicit setting.
ifeq ($(uname_S),Darwin)
ifeq ("$(wildcard /usr/lib/libSystem.dylib)","")
	SHOBJ_LDFLAGS+= -L /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/lib -lsystem
endif
endif

all: libluaengine.so

libluaengine.so: $(OBJS) $(LIBS)
	$(CC) -o $@ $(SHOBJ_LDFLAGS) $^

sha1.o: ../../sha1.c
	$(CC) $(SHOBJ_CFLAGS) -c $< -o $@

rand.o: ../../rand.c
	$(CC) $(SHOBJ_CFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(SHOBJ_CFLAGS) -c $< -o $@

$(DEPS_DIR)/lua/src/liblua.a:
	cd $(DEPS_DIR) && $(MAKE) lua

$(DEPS_DIR)/fpconv/libfpconv.a:
	cd $(DEPS_DIR) && $(MAKE) fpconv

clean:
	rm -f *.so $(OBJS)
