
# find the OS
uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')

# Compile flags for linux / macOS
ifeq ($(uname_S),Linux)
	SHOBJ_CFLAGS ?= -W -Wall -fno-common -g -ggdb -std=c99 -O2
	SHOBJ_LDFLAGS ?= -shared
else
	SHOBJ_CFLAGS ?= -W -Wall -dynamic -fno-common -g -ggdb -std=c99 -O2
	SHOBJ_LDFLAGS ?= -bundle -undefined dynamic_lookup
endif

# Link with the compiler driver on macOS so libSystem is resolved from the
# active SDK; raw ld can't parse newer SDKs' libSystem.tbd stubs.
ifeq ($(uname_S),Darwin)
# CFLAGS may select a non-native architecture and must reach the driver.
LD = $(CC) $(CFLAGS)
endif

.SUFFIXES: .c .so .xo .o

all: helloworld.so hellotype.so helloblock.so hellocluster.so hellotimer.so hellodict.so hellohook.so helloacl.so

.c.xo:
	$(CC) -I. $(CFLAGS) $(SHOBJ_CFLAGS) -fPIC -c $< -o $@

helloworld.xo: ../valkeymodule.h

helloworld.so: helloworld.xo
	$(LD) -o $@ $^ $(SHOBJ_LDFLAGS) $(LIBS) -lc

hellotype.xo: ../valkeymodule.h

hellotype.so: hellotype.xo
	$(LD) -o $@ $^ $(SHOBJ_LDFLAGS) $(LIBS) -lc

helloblock.xo: ../valkeymodule.h

helloblock.so: helloblock.xo
	$(LD) -o $@ $^ $(SHOBJ_LDFLAGS) $(LIBS) -lpthread -lc

hellocluster.xo: ../valkeymodule.h

hellocluster.so: hellocluster.xo
	$(LD) -o $@ $^ $(SHOBJ_LDFLAGS) $(LIBS) -lc

hellotimer.xo: ../valkeymodule.h

hellotimer.so: hellotimer.xo
	$(LD) -o $@ $^ $(SHOBJ_LDFLAGS) $(LIBS) -lc

hellodict.xo: ../valkeymodule.h

hellodict.so: hellodict.xo
	$(LD) -o $@ $^ $(SHOBJ_LDFLAGS) $(LIBS) -lc

hellohook.xo: ../valkeymodule.h

hellohook.so: hellohook.xo
	$(LD) -o $@ $^ $(SHOBJ_LDFLAGS) $(LIBS) -lc

helloacl.xo: ../valkeymodule.h

helloacl.so: helloacl.xo
	$(LD) -o $@ $^ $(SHOBJ_LDFLAGS) $(LIBS) -lc

clean:
	rm -rf *.xo *.so
