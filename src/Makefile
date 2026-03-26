# Valkey Makefile
# Copyright (C) 2009 Redis Ltd.
# This file is released under the BSD license, see the COPYING file
#
# The Makefile composes the final FINAL_CFLAGS and FINAL_LDFLAGS using
# what is needed for Valkey plus the standard CFLAGS and LDFLAGS passed.
# However when building the dependencies (Jemalloc, Lua, libvalkey, ...)
# CFLAGS and LDFLAGS are propagated to the dependencies, so to pass
# flags only to be used when compiling / linking Valkey itself SERVER_CFLAGS
# and SERVER_LDFLAGS are used instead (this is the case of 'make gcov').
#
# Dependencies are stored in the Makefile.dep file. To rebuild this file
# Just use 'make dep', but this is only needed by developers.

release_hdr := $(shell sh -c './mkreleasehdr.sh')
uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')
uname_M := $(shell sh -c 'uname -m 2>/dev/null || echo not')
CLANG := $(findstring clang,$(shell sh -c '$(CC) --version | head -1'))

# Optimization flags. To override, the OPTIMIZATION variable can be passed, but
# some automatic defaults are added to it. To specify optimization flags
# explicitly without any defaults added, pass the OPT variable instead.
OPTIMIZATION?=-O3
ifeq ($(OPTIMIZATION),-O3)
	ifeq (clang,$(CLANG))
		OPTIMIZATION+=-flto
	else
		OPTIMIZATION+=-flto=auto -ffat-lto-objects
	endif
endif
ifneq ($(OPTIMIZATION),-O0)
	OPTIMIZATION+=-fno-omit-frame-pointer
endif
DEPENDENCY_TARGETS=libvalkey linenoise hdr_histogram fpconv
NODEPS:=clean distclean

# Default settings
STD=-pedantic

# Use -Wno-c11-extensions on clang, either where explicitly used or on
# platforms we can assume it's being used.
ifeq (clang,$(CLANG))
  STD+=-Wno-c11-extensions
else
ifneq (,$(findstring FreeBSD,$(uname_S)))
  STD+=-Wno-c11-extensions
endif
endif
WARN=-Wall -W -Wno-missing-field-initializers -Werror=deprecated-declarations -Wstrict-prototypes -Werror=undef
OPT=$(OPTIMIZATION)

# Detect if the compiler supports C11 _Atomic.
# NUMBER_SIGN_CHAR is a workaround to support both GNU Make 4.3 and older versions.
NUMBER_SIGN_CHAR := \#
C11_ATOMIC := $(shell sh -c 'echo "$(NUMBER_SIGN_CHAR)include <stdatomic.h>" > foo.c; \
	$(CC) -std=gnu11 -c foo.c -o foo.o > /dev/null 2>&1; \
	if [ -f foo.o ]; then echo "yes"; rm foo.o; fi; rm foo.c')
ifeq ($(C11_ATOMIC),yes)
	STD+=-std=gnu11
else
	STD+=-std=c99
endif

PREFIX?=/usr/local
INSTALL_BIN=$(PREFIX)/bin
INSTALL_LIB=$(PREFIX)/lib
INSTALL=install
PKG_CONFIG?=pkg-config

ifndef PYTHON
PYTHON := $(shell which python3 || which python)
endif

# Default allocator defaults to Jemalloc on Linux and libc otherwise
MALLOC=libc
ifeq ($(uname_S),Linux)
	MALLOC=jemalloc
endif

# To get ARM stack traces if Valkey crashes we need a special C flag.
ifneq (,$(filter aarch64 armv%,$(uname_M)))
        CFLAGS+=-funwind-tables
endif

# Backwards compatibility for selecting an allocator
ifeq ($(USE_TCMALLOC),yes)
	MALLOC=tcmalloc
endif

ifeq ($(USE_TCMALLOC_MINIMAL),yes)
	MALLOC=tcmalloc_minimal
endif

ifeq ($(USE_JEMALLOC),yes)
	MALLOC=jemalloc
endif

ifeq ($(USE_JEMALLOC),no)
	MALLOC=libc
endif

ifdef SANITIZER
ifeq ($(SANITIZER),address)
	MALLOC=libc
	CFLAGS+=-fsanitize=address -fno-sanitize-recover=all -fno-omit-frame-pointer
	LDFLAGS+=-fsanitize=address
else
ifeq ($(SANITIZER),undefined)
	MALLOC=libc
	CFLAGS+=-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
	LDFLAGS+=-fsanitize=undefined
else
ifeq ($(SANITIZER),thread)
	CFLAGS+=-fsanitize=thread -fno-sanitize-recover=all -fno-omit-frame-pointer
	LDFLAGS+=-fsanitize=thread
else
    $(error "unknown sanitizer=${SANITIZER}")
endif
endif
endif
endif

# Override default settings if possible
-include .make-settings

# For added compatibility REDIS_CFLAGS and REDIS_LDFLAGS are also supported.
ifdef REDIS_CFLAGS
    SERVER_CFLAGS := $(REDIS_CFLAGS)
endif
ifdef REDIS_LDFLAGS
    SERVER_LDFLAGS := $(REDIS_LDFLAGS)
endif

# Special case of forcing defrag to run even though we have no Jemalloc support
ifeq ($(DEBUG_FORCE_DEFRAG), yes)
	SERVER_CFLAGS +=-DHAVE_DEFRAG -DDEBUG_FORCE_DEFRAG
endif

FINAL_CFLAGS=$(STD) $(WARN) $(OPT) $(DEBUG) $(CFLAGS) $(SERVER_CFLAGS)
FINAL_LDFLAGS=$(LDFLAGS) $(OPT) $(SERVER_LDFLAGS) $(DEBUG)
FINAL_LIBS=-lm
DEBUG=-g -ggdb

# Linux ARM32 needs -latomic at linking time
ifneq (,$(findstring armv,$(uname_M)))
        FINAL_LIBS+=-latomic
else
# Linux POWER needs -latomic at linking time
ifneq (,$(findstring ppc,$(uname_M)))
        FINAL_LIBS+=-latomic
endif
endif

ifeq ($(uname_S),SunOS)
	# SunOS
	ifeq ($(findstring -m32,$(FINAL_CFLAGS)),)
		CFLAGS+=-m64
	endif
	ifeq ($(findstring -m32,$(FINAL_LDFLAGS)),)
		LDFLAGS+=-m64
	endif
	DEBUG=-g
	DEBUG_FLAGS=-g
	export CFLAGS LDFLAGS DEBUG DEBUG_FLAGS
	INSTALL=cp -pf
	FINAL_CFLAGS+= -D__EXTENSIONS__ -D_XPG6
	FINAL_LIBS+= -ldl -lnsl -lsocket -lresolv -lpthread -lrt
	ifeq ($(USE_BACKTRACE),yes)
	    FINAL_CFLAGS+= -DUSE_BACKTRACE
	endif
else
ifeq ($(uname_S),Darwin)
	# Darwin
	FINAL_LIBS+= -ldl
	# Homebrew's OpenSSL is not linked to /usr/local to avoid
	# conflicts with the system's LibreSSL installation so it
	# must be referenced explicitly during build.
ifeq ($(uname_M),arm64)
	# Homebrew arm64 uses /opt/homebrew as HOMEBREW_PREFIX
	OPENSSL_PREFIX?=/opt/homebrew/opt/openssl
else
	# Homebrew x86/ppc uses /usr/local as HOMEBREW_PREFIX
	OPENSSL_PREFIX?=/usr/local/opt/openssl
endif
else
ifeq ($(uname_S),AIX)
        # AIX
        FINAL_LDFLAGS+= -Wl,-bexpall
        FINAL_LIBS+=-ldl -pthread -lcrypt -lbsd
else
ifeq ($(uname_S),OpenBSD)
	# OpenBSD
	FINAL_LIBS+= -lpthread
	ifeq ($(USE_BACKTRACE),yes)
	    FINAL_CFLAGS+= -DUSE_BACKTRACE -I/usr/local/include
	    FINAL_LDFLAGS+= -L/usr/local/lib
	    FINAL_LIBS+= -lexecinfo
    	endif

else
ifeq ($(uname_S),NetBSD)
	# NetBSD
	FINAL_LIBS+= -lpthread
	ifeq ($(USE_BACKTRACE),yes)
	    FINAL_CFLAGS+= -DUSE_BACKTRACE -I/usr/pkg/include
	    FINAL_LDFLAGS+= -L/usr/pkg/lib
	    FINAL_LIBS+= -lexecinfo
    	endif
else
ifeq ($(uname_S),FreeBSD)
	# FreeBSD
	FINAL_LIBS+= -lpthread -lexecinfo
else
ifeq ($(uname_S),DragonFly)
	# DragonFly
	FINAL_LIBS+= -lpthread -lexecinfo
else
ifeq ($(uname_S),OpenBSD)
	# OpenBSD
	FINAL_LIBS+= -lpthread -lexecinfo
else
ifeq ($(uname_S),NetBSD)
	# NetBSD
	FINAL_LIBS+= -lpthread -lexecinfo
else
ifeq ($(uname_S),Haiku)
	# Haiku
	FINAL_CFLAGS+= -DBSD_SOURCE
	FINAL_LDFLAGS+= -lbsd -lnetwork
	FINAL_LIBS+= -lpthread
else
	# All the other OSes (notably Linux)
	FINAL_LDFLAGS+= -rdynamic
	FINAL_LIBS+=-ldl -pthread -lrt
endif
endif
endif
endif
endif
endif
endif
endif
endif
endif

ifdef OPENSSL_PREFIX
	OPENSSL_CFLAGS=-I$(OPENSSL_PREFIX)/include
	OPENSSL_LDFLAGS=-L$(OPENSSL_PREFIX)/lib
	# Also export OPENSSL_PREFIX so it ends up in deps sub-Makefiles
	export OPENSSL_PREFIX
endif

# Include paths to dependencies
FINAL_CFLAGS+= -I../deps/libvalkey/include -I../deps/linenoise -I../deps/hdr_histogram -I../deps/fpconv -I../deps/fast_float

# Lua scripting engine module
LUA_MODULE_NAME:=modules/lua/libvalkeylua.so
ifeq ($(BUILD_LUA),no)
	LUA_MODULE=
	LUA_MODULE_INSTALL=
else
	LUA_MODULE=$(LUA_MODULE_NAME)
	LUA_MODULE_INSTALL=install-lua-module

	current_dir = $(shell pwd)
	FINAL_CFLAGS+=-DLUA_ENABLED -DLUA_LIB=libvalkeylua.so
ifeq ($(uname_S),Darwin)
	FINAL_LDFLAGS+= -Wl,-rpath,$(PREFIX)/lib
	FINAL_LDFLAGS+= -Wl,-rpath,$(current_dir)/modules/lua
else
	FINAL_LDFLAGS+= -Wl,-rpath,$(PREFIX)/lib:$(current_dir)/modules/lua -Wl,--disable-new-dtags
endif
endif

# Determine systemd support and/or build preference (defaulting to auto-detection)
BUILD_WITH_SYSTEMD=no
LIBSYSTEMD_LIBS=-lsystemd

# If 'USE_SYSTEMD' in the environment is neither "no" nor "yes", try to
# auto-detect libsystemd's presence and link accordingly.
ifneq ($(USE_SYSTEMD),no)
	LIBSYSTEMD_PKGCONFIG := $(shell $(PKG_CONFIG) --exists libsystemd && echo $$?)
# If libsystemd cannot be detected, continue building without support for it
# (unless a later check tells us otherwise)
ifeq ($(LIBSYSTEMD_PKGCONFIG),0)
	BUILD_WITH_SYSTEMD=yes
	LIBSYSTEMD_LIBS=$(shell $(PKG_CONFIG) --libs libsystemd)
endif
endif

# If 'USE_SYSTEMD' is set to "yes" use pkg-config if available or fall back to
# default -lsystemd.
ifeq ($(USE_SYSTEMD),yes)
	BUILD_WITH_SYSTEMD=yes
endif

ifeq ($(BUILD_WITH_SYSTEMD),yes)
	FINAL_LIBS+=$(LIBSYSTEMD_LIBS)
	FINAL_CFLAGS+= -DHAVE_LIBSYSTEMD
endif

ifeq ($(MALLOC),tcmalloc)
	FINAL_CFLAGS+= -DUSE_TCMALLOC
	FINAL_LIBS+= -ltcmalloc
endif

ifeq ($(MALLOC),tcmalloc_minimal)
	FINAL_CFLAGS+= -DUSE_TCMALLOC
	FINAL_LIBS+= -ltcmalloc_minimal
endif

ifeq ($(MALLOC),jemalloc)
	DEPENDENCY_TARGETS+= jemalloc
	FINAL_CFLAGS+= -DUSE_JEMALLOC -I../deps/jemalloc/include
	FINAL_LIBS := ../deps/jemalloc/lib/libjemalloc.a $(FINAL_LIBS)
endif

ifeq ($(USE_LTTNG),yes)
	FINAL_CFLAGS+=-DUSE_LTTNG
	FINAL_LIBS += -llttng-ust
endif

# LIBSSL & LIBCRYPTO
LIBSSL_LIBS=
LIBSSL_PKGCONFIG := $(shell $(PKG_CONFIG) --exists libssl && echo $$?)
ifeq ($(LIBSSL_PKGCONFIG),0)
	LIBSSL_LIBS=$(shell $(PKG_CONFIG) --libs libssl)
else
	LIBSSL_LIBS=-lssl
endif
LIBCRYPTO_LIBS=
LIBCRYPTO_PKGCONFIG := $(shell $(PKG_CONFIG) --exists libcrypto && echo $$?)
ifeq ($(LIBCRYPTO_PKGCONFIG),0)
	LIBCRYPTO_LIBS=$(shell $(PKG_CONFIG) --libs libcrypto)
else
	LIBCRYPTO_LIBS=-lcrypto
endif

BUILD_NO:=0
BUILD_YES:=1
BUILD_MODULE:=2
ifeq ($(BUILD_TLS),yes)
	FINAL_CFLAGS+=-DUSE_OPENSSL=$(BUILD_YES) $(OPENSSL_CFLAGS) -DBUILD_TLS_MODULE=$(BUILD_NO)
	FINAL_LDFLAGS+=$(OPENSSL_LDFLAGS)
	FINAL_LIBS += ../deps/libvalkey/lib/libvalkey_tls.a $(LIBSSL_LIBS) $(LIBCRYPTO_LIBS)
endif

TLS_MODULE=
TLS_MODULE_NAME:=valkey-tls$(PROG_SUFFIX).so
TLS_MODULE_CFLAGS:=$(FINAL_CFLAGS)
ifeq ($(BUILD_TLS),module)
	FINAL_CFLAGS+=-DUSE_OPENSSL=$(BUILD_MODULE) $(OPENSSL_CFLAGS)
	TLS_CLIENT_LIBS = ../deps/libvalkey/lib/libvalkey_tls.a $(LIBSSL_LIBS) $(LIBCRYPTO_LIBS)
	TLS_MODULE=$(TLS_MODULE_NAME)
	TLS_MODULE_CFLAGS+=-DUSE_OPENSSL=$(BUILD_MODULE) $(OPENSSL_CFLAGS) -DBUILD_TLS_MODULE=$(BUILD_MODULE)
endif

RDMA_LIBS=
RDMA_PKGCONFIG := $(shell $(PKG_CONFIG) --exists librdmacm libibverbs && echo $$?)
ifeq ($(RDMA_PKGCONFIG),0)
	RDMA_LIBS=$(shell $(PKG_CONFIG) --libs librdmacm libibverbs)
else
	RDMA_LIBS=-lrdmacm -libverbs
endif

ifeq ($(BUILD_RDMA),yes)
	FINAL_CFLAGS+=-DUSE_RDMA=$(BUILD_YES) -DBUILD_RDMA_MODULE=$(BUILD_NO)
	FINAL_LIBS += ../deps/libvalkey/lib/libvalkey_rdma.a $(RDMA_LIBS)
endif

RDMA_MODULE=
RDMA_MODULE_NAME:=valkey-rdma$(PROG_SUFFIX).so
RDMA_MODULE_CFLAGS:=$(FINAL_CFLAGS)
ifeq ($(BUILD_RDMA),module)
	FINAL_CFLAGS+=-DUSE_RDMA=$(BUILD_MODULE)
	RDMA_CLIENT_LIBS = ../deps/libvalkey/lib/libvalkey_rdma.a
	RDMA_MODULE=$(RDMA_MODULE_NAME)
	RDMA_MODULE_CFLAGS+=-DUSE_RDMA=$(BUILD_MODULE) -DBUILD_RDMA_MODULE=$(BUILD_MODULE) $(RDMA_LIBS)
endif

ifndef V
    define MAKE_INSTALL
        @printf '    %b %b\n' $(LINKCOLOR)INSTALL$(ENDCOLOR) $(BINCOLOR)$(1)$(ENDCOLOR) 1>&2
        @$(INSTALL) $(1) $(2)
    endef

    define INSTALL_REDIS_SYMLINK
        @printf '    %b %b %b %b %b %b %b\n' \
        $(LINKCOLOR)INSTALL SYMLINK$(ENDCOLOR) \
        $(BINCOLOR)$(subst $(ENGINE_NAME),redis,$(1))$(ENDCOLOR) -\> $(BINCOLOR)$(1)$(ENDCOLOR) 1>&2
        @ln -sf $(1) $(2)/$(subst $(ENGINE_NAME),redis,$(1))
    endef
else
    define MAKE_INSTALL
        $(INSTALL) $(1) $(2)
    endef

    define INSTALL_REDIS_SYMLINK
        ln -sf $(1) $(2)/$(subst $(ENGINE_NAME),redis,$(1))
    endef
endif

# Determine install/uninstall Redis symlinks for compatibility when
# installing/uninstalling Valkey binaries (defaulting to `yes`)
USE_REDIS_SYMLINKS?=yes
ifeq ($(USE_REDIS_SYMLINKS),yes)
	MAYBE_INSTALL_REDIS_SYMLINK=$(INSTALL_REDIS_SYMLINK)
	MAYBE_UNINSTALL_REDIS_SYMLINK=@rm -f $(1)/$(subst $(ENGINE_NAME),redis,$(2))
else
	MAYBE_INSTALL_REDIS_SYMLINK=
	MAYBE_UNINSTALL_REDIS_SYMLINK=
endif

SERVER_CC=$(QUIET_CC)$(CC) $(FINAL_CFLAGS) -I.
SERVER_AR=$(QUIET_AR)$(AR)
SERVER_LD=$(QUIET_LINK)$(CC) $(FINAL_LDFLAGS)
ENGINE_INSTALL=$(QUIET_INSTALL)$(INSTALL)

CCCOLOR="\033[34m"
LINKCOLOR="\033[34;1m"
SRCCOLOR="\033[33m"
BINCOLOR="\033[37;1m"
MAKECOLOR="\033[32;1m"
ENDCOLOR="\033[0m"

ifndef V
QUIET_CC = @printf '    %b %b\n' $(CCCOLOR)CC$(ENDCOLOR) $(SRCCOLOR)$@$(ENDCOLOR) 1>&2;
QUIET_GEN = @printf '    %b %b\n' $(CCCOLOR)GEN$(ENDCOLOR) $(SRCCOLOR)$@$(ENDCOLOR) 1>&2;
QUIET_LINK = @printf '    %b %b\n' $(LINKCOLOR)LINK$(ENDCOLOR) $(BINCOLOR)$@$(ENDCOLOR) 1>&2;
QUIET_INSTALL = @printf '    %b %b\n' $(LINKCOLOR)INSTALL$(ENDCOLOR) $(BINCOLOR)$@$(ENDCOLOR) 1>&2;
QUIET_AR = @printf '    %b %b\n' $(CCCOLOR)ARCHIVE$(ENDCOLOR) $(BINCOLOR)$@$(ENDCOLOR) 1>&2;
endif

ifneq (, $(findstring LOG_REQ_RES, $(SERVER_CFLAGS)))
	COMMANDS_DEF_FILENAME=commands_with_reply_schema
	GEN_COMMANDS_FLAGS=--with-reply-schema
else
	COMMANDS_DEF_FILENAME=commands
	GEN_COMMANDS_FLAGS=
endif

ENGINE_NAME=valkey
SERVER_NAME=$(ENGINE_NAME)-server$(PROG_SUFFIX)
ENGINE_SENTINEL_NAME=$(ENGINE_NAME)-sentinel$(PROG_SUFFIX)
ENGINE_TRACE_OBJ = \
    trace/trace.o \
    trace/trace_aof.o \
    trace/trace_cluster.o \
    trace/trace_commands.o \
    trace/trace_db.o \
    trace/trace_rdb.o \
    trace/trace_server.o
ENGINE_SERVER_OBJ = \
    acl.o \
    adlist.o \
    ae.o \
    allocator_defrag.o \
    anet.o \
    aof.o \
    bio.o \
    bitops.o \
    blocked.o \
    call_reply.o \
    childinfo.o \
    cluster.o \
    cluster_legacy.o \
    cluster_migrateslots.o \
    cluster_slot_stats.o \
    commandlog.o \
    commands.o \
    config.o \
    connection.o \
    crc16.o \
    crc16_slottable.o \
    crc64.o \
    crccombine.o \
    crcspeed.o \
    db.o \
    debug.o \
    defrag.o \
    dict.o \
    entry.o \
    eval.o \
    evict.o \
    expire.o \
    fifo.o \
    functions.o \
    geo.o \
    geohash.o \
    geohash_helper.o \
    hashtable.o \
    hyperloglog.o \
    intset.o \
    io_threads.o \
    kvstore.o \
    latency.o \
    lazyfree.o \
    listpack.o \
    localtime.o \
    logreqres.o \
    lolwut.o \
    lolwut5.o \
    lolwut6.o \
    lolwut9.o \
    lrulfu.o \
    lzf_c.o \
    lzf_d.o \
    memory_prefetch.o \
    memtest.o \
    module.o \
    monotonic.o \
    mt19937-64.o \
    multi.o \
    mutexqueue.o \
    networking.o \
    notify.o \
    object.o \
    pqsort.o \
    pubsub.o \
    quicklist.o \
    rand.o \
    rax.o \
    rdb.o \
    rdma.o \
    release.o \
    replication.o \
    resp_parser.o \
    rio.o \
    script.o \
    scripting_engine.o \
    sds.o \
    sentinel.o \
    server.o \
    setcpuaffinity.o \
    setproctitle.o \
    sha1.o \
    sha256.o \
    siphash.o \
    socket.o \
    sort.o \
    sparkline.o \
    strl.o \
    syncio.o \
    syscheck.o \
    t_hash.o \
    t_list.o \
    t_set.o \
    t_stream.o \
    t_string.o \
    t_zset.o \
    threads_mngr.o \
    timeout.o \
    tls.o \
    tracking.o \
    unix.o \
    util.o \
    valkey-check-aof.o \
    valkey-check-rdb.o \
    valkey_strtod.o \
    vector.o \
    vset.o \
    ziplist.o \
    zipmap.o \
    zmalloc.o
ENGINE_SERVER_OBJ+=$(ENGINE_TRACE_OBJ)
ENGINE_CLI_NAME=$(ENGINE_NAME)-cli$(PROG_SUFFIX)
ENGINE_CLI_OBJ = \
    adlist.o \
    ae.o \
    anet.o \
    cli_commands.o \
    cli_common.o \
    crc16.o \
    crc64.o \
    crccombine.o \
    crcspeed.o \
    dict.o \
    monotonic.o \
    mt19937-64.o \
    release.o \
    sds.o \
    serverassert.o \
    sha256.o \
    siphash.o \
    strl.o \
    util.o \
    valkey-cli.o \
    valkey_strtod.o \
    zmalloc.o
ENGINE_BENCHMARK_NAME=$(ENGINE_NAME)-benchmark$(PROG_SUFFIX)
ENGINE_BENCHMARK_OBJ = \
    adlist.o \
    ae.o \
    anet.o \
    cli_common.o \
    crc16.o \
    crc16_slottable.o \
    crc64.o \
    crccombine.o \
    crcspeed.o \
    dict.o \
    fuzzer_client.o \
    fuzzer_command_generator.o \
    monotonic.o \
    mt19937-64.o \
    release.o \
    sds.o \
    serverassert.o \
    sha256.o \
    siphash.o \
    strl.o \
    util.o \
    valkey-benchmark.o \
    valkey_strtod.o \
    zmalloc.o
ENGINE_CHECK_RDB_NAME=$(ENGINE_NAME)-check-rdb$(PROG_SUFFIX)
ENGINE_CHECK_AOF_NAME=$(ENGINE_NAME)-check-aof$(PROG_SUFFIX)
ENGINE_LIB_NAME=lib$(ENGINE_NAME).a
ENGINE_UNIT_GTESTS:=$(ENGINE_NAME)-unit-gtests$(PROG_SUFFIX)
ALL_SOURCES=$(sort $(patsubst %.o,%.c,$(ENGINE_SERVER_OBJ) $(ENGINE_CLI_OBJ) $(ENGINE_BENCHMARK_OBJ)))

USE_LIBBACKTRACE?=no
ifeq ($(USE_LIBBACKTRACE),yes)
	FINAL_CFLAGS += -DUSE_LIBBACKTRACE
	ifdef LIBBACKTRACE_PREFIX
		FINAL_CFLAGS += -I$(LIBBACKTRACE_PREFIX)/include
		FINAL_LDFLAGS += -L$(LIBBACKTRACE_PREFIX)/lib
	endif
	FINAL_LIBS += -lbacktrace
endif

DEPENDENCY_TARGETS+= gtest-parallel

ALL_BUILD_PREREQUISITES=$(SERVER_NAME) $(ENGINE_SENTINEL_NAME) $(ENGINE_CLI_NAME) $(ENGINE_BENCHMARK_NAME) $(ENGINE_CHECK_RDB_NAME) $(ENGINE_CHECK_AOF_NAME) $(TLS_MODULE) $(RDMA_MODULE)

all: $(ALL_BUILD_PREREQUISITES)
	@echo ""
	@echo "Hint: It's a good idea to run 'make test' ;)"
	@echo ""

Makefile.dep:
	-$(SERVER_CC) -MM $(ALL_SOURCES) > Makefile.dep 2> /dev/null || true

ifeq (0, $(words $(findstring $(MAKECMDGOALS), $(NODEPS))))
-include Makefile.dep
endif

.PHONY: all

all-with-unit-tests: all $(ENGINE_UNIT_GTESTS)
.PHONY: all

persist-settings: distclean
	echo STD=$(STD) >> .make-settings
	echo WARN=$(WARN) >> .make-settings
	echo OPT=$(OPT) >> .make-settings
	echo PROG_SUFFIX=$(PROG_SUFFIX) >> .make-settings
	echo MALLOC=$(MALLOC) >> .make-settings
	echo BUILD_TLS=$(BUILD_TLS) >> .make-settings
	echo BUILD_RDMA=$(BUILD_RDMA) >> .make-settings
	echo USE_SYSTEMD=$(USE_SYSTEMD) >> .make-settings
	echo BUILD_LUA=$(BUILD_LUA) >> .make-settings
	echo USE_LIBBACKTRACE=$(USE_LIBBACKTRACE) >> .make-settings
	echo LIBBACKTRACE_PREFIX=$(LIBBACKTRACE_PREFIX) >> .make-settings
	echo CFLAGS=$(CFLAGS) >> .make-settings
	echo LDFLAGS=$(LDFLAGS) >> .make-settings
	echo SERVER_CFLAGS=$(SERVER_CFLAGS) >> .make-settings
	echo SERVER_LDFLAGS=$(SERVER_LDFLAGS) >> .make-settings
	echo PREV_FINAL_CFLAGS=$(FINAL_CFLAGS) >> .make-settings
	echo PREV_FINAL_LDFLAGS=$(FINAL_LDFLAGS) >> .make-settings
	echo ENGINE_SERVER_OBJ=$(ENGINE_SERVER_OBJ) >> .make-settings
	-(cd ../deps && $(MAKE) $(DEPENDENCY_TARGETS))

.PHONY: persist-settings

# Prerequisites target
.make-prerequisites:
	@touch $@

# Clean everything, persist settings and build dependencies if anything changed
ifneq ($(strip $(PREV_FINAL_CFLAGS)), $(strip $(FINAL_CFLAGS)))
.make-prerequisites: persist-settings
endif

ifneq ($(strip $(PREV_FINAL_LDFLAGS)), $(strip $(FINAL_LDFLAGS)))
.make-prerequisites: persist-settings
endif

# valkey-server
$(SERVER_NAME): $(ENGINE_SERVER_OBJ) $(LUA_MODULE)
	$(SERVER_LD) -o $@ $(ENGINE_SERVER_OBJ) ../deps/libvalkey/lib/libvalkey.a ../deps/hdr_histogram/libhdrhistogram.a ../deps/fpconv/libfpconv.a $(FINAL_LIBS)

# Valkey static library, used to compile against for unit testing
$(ENGINE_LIB_NAME): $(ENGINE_SERVER_OBJ)
	$(SERVER_AR) rcs $@ $^

# valkey-sentinel
$(ENGINE_SENTINEL_NAME): $(SERVER_NAME)
	$(ENGINE_INSTALL) $(SERVER_NAME) $(ENGINE_SENTINEL_NAME)

# valkey-check-rdb
$(ENGINE_CHECK_RDB_NAME): $(SERVER_NAME)
	$(ENGINE_INSTALL) $(SERVER_NAME) $(ENGINE_CHECK_RDB_NAME)

# valkey-check-aof
$(ENGINE_CHECK_AOF_NAME): $(SERVER_NAME)
	$(ENGINE_INSTALL) $(SERVER_NAME) $(ENGINE_CHECK_AOF_NAME)

# valkey-tls.so
$(TLS_MODULE_NAME): $(SERVER_NAME)
	$(QUIET_CC)$(CC) $(LDFLAGS) -o $@ tls.c -shared -fPIC $(TLS_MODULE_CFLAGS) $(TLS_CLIENT_LIBS)

# valkey-rdma.so
$(RDMA_MODULE_NAME): $(SERVER_NAME)
	$(QUIET_CC)$(CC) $(LDFLAGS) -o $@ rdma.c -shared -fPIC $(RDMA_MODULE_CFLAGS)

# engine_lua.so
$(LUA_MODULE_NAME): .make-prerequisites
	cd modules/lua && $(MAKE) OPTIMIZATION="$(OPTIMIZATION)"

# valkey-cli
$(ENGINE_CLI_NAME): $(ENGINE_CLI_OBJ)
	$(SERVER_LD) -o $@ $^ ../deps/libvalkey/lib/libvalkey.a ../deps/linenoise/linenoise.o ../deps/fpconv/libfpconv.a $(FINAL_LIBS) $(TLS_CLIENT_LIBS) $(RDMA_CLIENT_LIBS)

# valkey-benchmark
$(ENGINE_BENCHMARK_NAME): $(ENGINE_BENCHMARK_OBJ)
	$(SERVER_LD) -o $@ $^ ../deps/libvalkey/lib/libvalkey.a ../deps/hdr_histogram/libhdrhistogram.a ../deps/fpconv/libfpconv.a $(FINAL_LIBS) $(TLS_CLIENT_LIBS) $(RDMA_CLIENT_LIBS)

DEP = $(ENGINE_SERVER_OBJ:%.o=%.d) $(ENGINE_CLI_OBJ:%.o=%.d) $(ENGINE_BENCHMARK_OBJ:%.o=%.d)
-include $(DEP)

# Because the jemalloc.h header is generated as a part of the jemalloc build,
# building it should complete before building any other object. Instead of
# depending on a single artifact, build all dependencies first.
%.o: %.c .make-prerequisites
	$(SERVER_CC) -MMD -o $@ -c $<

trace/%.o: trace/%.c .make-prerequisites
	$(SERVER_CC) -Itrace -MMD -o $@ -c $<

# The following files are checked in and don't normally need to be rebuilt. They
# are built only if python is available and their prereqs are modified.
ifneq (,$(PYTHON))
$(COMMANDS_DEF_FILENAME).def: commands/*.json ../utils/generate-command-code.py
	$(QUIET_GEN)$(PYTHON) ../utils/generate-command-code.py $(GEN_COMMANDS_FLAGS)

fmtargs.h: ../utils/generate-fmtargs.py
	$(QUITE_GEN)sed '/Everything below this line/,$$d' $@ > $@.tmp
	$(QUITE_GEN)$(PYTHON) ../utils/generate-fmtargs.py >> $@.tmp
	$(QUITE_GEN)mv $@.tmp $@
endif

commands.c: $(COMMANDS_DEF_FILENAME).def

clean:
	rm -rf $(SERVER_NAME) $(ENGINE_SENTINEL_NAME) $(ENGINE_CLI_NAME) $(ENGINE_BENCHMARK_NAME) $(ENGINE_CHECK_RDB_NAME) $(ENGINE_CHECK_AOF_NAME) $(ENGINE_UNIT_GTESTS) $(ENGINE_LIB_NAME) trace/*.o trace/*.d *.o *.gcda *.gcno *.gcov valkey.info lcov-html Makefile.dep *.so
	rm -f $(DEP)
	-(cd modules/lua && $(MAKE) clean)

.PHONY: clean

distclean: clean
	-(cd ../deps && $(MAKE) distclean)
	-(cd modules && $(MAKE) clean)
	-(cd ../tests/modules && $(MAKE) clean)
	-(cd unit && $(MAKE) clean)
	-(rm -f .make-*)
	-(find unit -type d -name __pycache__ -exec rm -rf {} +)

.PHONY: distclean

test: $(ALL_BUILD_PREREQUISITES)
	@(cd ..; VALKEY_PROG_SUFFIX="$(PROG_SUFFIX)" ./runtest)

test-unit:
	@(cd unit && $(MAKE) test-unit)

# valkey-unit-gtests
$(ENGINE_UNIT_GTESTS): $(ENGINE_LIB_NAME)
	@(cd unit && $(MAKE) $(ENGINE_UNIT_GTESTS))

test-modules: $(SERVER_NAME)
	@(cd ..; ./runtest-moduleapi)

test-sentinel: $(ENGINE_SENTINEL_NAME) $(ENGINE_CLI_NAME)
	@(cd ..; ./runtest-sentinel)

test-cluster: $(SERVER_NAME) $(ENGINE_CLI_NAME)
	@(cd ..; ./runtest-cluster)

check: test

lcov:
	@lcov --version
	$(MAKE) gcov
	@(set -e; cd ..; ./runtest)
	$(MAKE) test-unit COVERAGE=1 COVERAGE_CFLAGS="-fprofile-arcs -ftest-coverage"
	@geninfo -o valkey.info .
	@genhtml --legend -o lcov-html valkey.info

.PHONY: lcov

bench: $(ENGINE_BENCHMARK_NAME)
	./$(ENGINE_BENCHMARK_NAME)

32bit:
	@echo ""
	@echo "WARNING: if it fails under Linux you probably need to install libc6-dev-i386 and libstdc++-11-dev-i386-cross"
	@echo ""
	$(MAKE) all-with-unit-tests CFLAGS="-m32" LDFLAGS="-m32"

gcov:
	$(MAKE) SERVER_CFLAGS="-fprofile-arcs -ftest-coverage -DCOVERAGE_TEST" SERVER_LDFLAGS="-fprofile-arcs -ftest-coverage"

noopt:
	$(MAKE) OPTIMIZATION="-O0"

valgrind:
	$(MAKE) OPTIMIZATION="-O0" MALLOC="libc"

helgrind:
	$(MAKE) OPTIMIZATION="-O0" MALLOC="libc" CFLAGS="-D__ATOMIC_VAR_FORCE_SYNC_MACROS" SERVER_CFLAGS="-I/usr/local/include" SERVER_LDFLAGS="-L/usr/local/lib"

install: all $(LUA_MODULE_INSTALL)
	@mkdir -p $(INSTALL_BIN)
	$(call MAKE_INSTALL,$(SERVER_NAME),$(INSTALL_BIN))
	$(call MAKE_INSTALL,$(ENGINE_BENCHMARK_NAME),$(INSTALL_BIN))
	$(call MAKE_INSTALL,$(ENGINE_CLI_NAME),$(INSTALL_BIN))
	@ln -sf $(SERVER_NAME) $(INSTALL_BIN)/$(ENGINE_CHECK_RDB_NAME)
	@ln -sf $(SERVER_NAME) $(INSTALL_BIN)/$(ENGINE_CHECK_AOF_NAME)
	@ln -sf $(SERVER_NAME) $(INSTALL_BIN)/$(ENGINE_SENTINEL_NAME)
	$(call MAYBE_INSTALL_REDIS_SYMLINK,$(SERVER_NAME),$(INSTALL_BIN))
	$(call MAYBE_INSTALL_REDIS_SYMLINK,$(ENGINE_CLI_NAME),$(INSTALL_BIN))
	$(call MAYBE_INSTALL_REDIS_SYMLINK,$(ENGINE_BENCHMARK_NAME),$(INSTALL_BIN))
	$(call MAYBE_INSTALL_REDIS_SYMLINK,$(ENGINE_CHECK_RDB_NAME),$(INSTALL_BIN))
	$(call MAYBE_INSTALL_REDIS_SYMLINK,$(ENGINE_CHECK_AOF_NAME),$(INSTALL_BIN))
	$(call MAYBE_INSTALL_REDIS_SYMLINK,$(ENGINE_SENTINEL_NAME),$(INSTALL_BIN))

install-lua-module: $(LUA_MODULE)
	@mkdir -p $(INSTALL_LIB)
	$(call MAKE_INSTALL,$(LUA_MODULE),$(INSTALL_LIB))

uninstall:
	@rm -f $(INSTALL_BIN)/{$(SERVER_NAME),$(ENGINE_BENCHMARK_NAME),$(ENGINE_CLI_NAME),$(ENGINE_CHECK_RDB_NAME),$(ENGINE_CHECK_AOF_NAME),$(ENGINE_SENTINEL_NAME)}
	$(call MAYBE_UNINSTALL_REDIS_SYMLINK,$(INSTALL_BIN),$(SERVER_NAME))
	$(call MAYBE_UNINSTALL_REDIS_SYMLINK,$(INSTALL_BIN),$(ENGINE_CLI_NAME))
	$(call MAYBE_UNINSTALL_REDIS_SYMLINK,$(INSTALL_BIN),$(ENGINE_BENCHMARK_NAME))
	$(call MAYBE_UNINSTALL_REDIS_SYMLINK,$(INSTALL_BIN),$(ENGINE_CHECK_RDB_NAME))
	$(call MAYBE_UNINSTALL_REDIS_SYMLINK,$(INSTALL_BIN),$(ENGINE_CHECK_AOF_NAME))
	$(call MAYBE_UNINSTALL_REDIS_SYMLINK,$(INSTALL_BIN),$(ENGINE_SENTINEL_NAME))
