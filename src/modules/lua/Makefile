uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')
CLANG := $(findstring clang,$(shell sh -c '$(CC) --version | head -1'))

DEPS_DIR=../../../deps

ifeq ($(uname_S),Darwin)
	SHOBJ_CFLAGS= -I. -I$(DEPS_DIR)/lua/src -I$(DEPS_DIR)/fpconv -fPIC -W -Wall -dynamic -fno-common $(OPTIMIZATION) -std=gnu11 -D_GNU_SOURCE $(CFLAGS)
	SHOBJ_LDFLAGS= -bundle -undefined dynamic_lookup $(LDFLAGS)
else
	SHOBJ_CFLAGS= -I. -I$(DEPS_DIR)/lua/src -I$(DEPS_DIR)/fpconv -fPIC -W -Wall -fno-common $(OPTIMIZATION) -std=gnu11 -D_GNU_SOURCE $(CFLAGS)
	SHOBJ_LDFLAGS= -shared $(LDFLAGS)

# Pretty-printing setup for module build
CC_COLOR="\033[32m"
LINKCOLOR_MOD="\033[36;1m"
SRC_COLOR="\033[33m"
SCOPE_COLOR="\033[38:5:250m"
BINCOLOR_MOD="\033[35m"
RESET="\033[0m"

ifndef V
QUIET_CC_MOD = @printf '    %b %b(LUA Module)%b %b\n' $(CC_COLOR)CC$(RESET) $(SCOPE_COLOR) $(RESET) $(SRC_COLOR)$<$(RESET) 1>&2;
QUIET_LINK_MOD = @printf '    %b %b\n' $(LINKCOLOR_MOD)LINK$(RESET) $(BINCOLOR_MOD)$@$(RESET) 1>&2;
endif
ifeq (clang,$(CLANG))
	SHOBJ_LDFLAGS+= -fuse-ld=lld
endif

endif

# BSD variants support
ifeq ($(uname_S),FreeBSD)
	SHOBJ_LDFLAGS+= -lm
else ifeq ($(uname_S),OpenBSD)
	SHOBJ_LDFLAGS+= -lm
else ifeq ($(uname_S),NetBSD)
	SHOBJ_LDFLAGS+= -lm
else ifeq ($(uname_S),DragonFly)
	SHOBJ_LDFLAGS+= -lm
endif

LIBS = \
    $(DEPS_DIR)/lua/src/liblua.a \
    $(DEPS_DIR)/fpconv/libfpconv.a
SRCS= $(wildcard *.c)
OBJS = \
    $(SRCS:.c=.o) \
    sha1.o \
    rand.o

# OS X 11.x doesn't have /usr/lib/libSystem.dylib and needs an explicit setting.
ifeq ($(uname_S),Darwin)
ifeq ("$(wildcard /usr/lib/libSystem.dylib)","")
	SHOBJ_LDFLAGS+= -L /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/lib -lsystem
endif
endif

all: libvalkeylua.so

libvalkeylua.so: $(OBJS) $(LIBS)
	$(QUIET_LINK_MOD)$(CC) -o $@ $(SHOBJ_LDFLAGS) $^

sha1.o: ../../sha1.c
	$(QUIET_CC_MOD)$(CC) $(SHOBJ_CFLAGS) -c $< -o $@

rand.o: ../../rand.c
	$(QUIET_CC_MOD)$(CC) $(SHOBJ_CFLAGS) -c $< -o $@

%.o: %.c
	$(QUIET_CC_MOD)$(CC) $(SHOBJ_CFLAGS) -c $< -o $@

$(DEPS_DIR)/lua/src/liblua.a:
	cd $(DEPS_DIR) && $(MAKE) lua

$(DEPS_DIR)/fpconv/libfpconv.a:
	cd $(DEPS_DIR) && $(MAKE) fpconv

clean:
	rm -f *.so $(OBJS)
