DEPS_DIR=../../../deps
CFLAGS= -I. -I$(DEPS_DIR)/lua/src -I$(DEPS_DIR)/fpconv -fPIC -W -Wall -fno-common $(OPTIMIZATION) -std=c99 -D_GNU_SOURCE
SHOBJ_LDFLAGS= -shared
LIBS= $(DEPS_DIR)/lua/src/liblua.a $(DEPS_DIR)/fpconv/libfpconv.a
SRCS = $(wildcard *.c)
OBJS = $(SRCS:.c=.o)

all: libluaengine.so

libluaengine.so: $(LIBS) $(OBJS)
	$(LD) -o $@ $^ $(SHOBJ_LDFLAGS) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(DEPS_DIR)/lua/src/liblua.a:
	cd $(DEPS_DIR) && $(MAKE) lua

$(DEPS_DIR)/fpconv/libfpconv.a:
	cd $(DEPS_DIR) && $(MAKE) fpconv

clean:
	rm -f *.so $(OBJS)
