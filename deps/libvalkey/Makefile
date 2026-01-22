# Libvalkey Makefile
# Copyright (C) 2010-2011 Salvatore Sanfilippo <antirez at gmail dot com>
# Copyright (C) 2010-2011 Pieter Noordhuis <pcnoordhuis at gmail dot com>
# This file is released under the BSD license, see the COPYING file

SHELL := /bin/sh

SRC_DIR = src
OBJ_DIR = obj
LIB_DIR = lib
TEST_DIR = tests

INCLUDE_DIR = include/valkey

TEST_SRCS = $(TEST_DIR)/client_test.c $(TEST_DIR)/ut_parse_cmd.c $(TEST_DIR)/ut_slotmap_update.c
TEST_BINS = $(patsubst $(TEST_DIR)/%.c,$(TEST_DIR)/%,$(TEST_SRCS))

SOURCES = $(filter-out $(SRC_DIR)/tls.c $(SRC_DIR)/rdma.c, $(wildcard $(SRC_DIR)/*.c))
HEADERS = $(filter-out $(INCLUDE_DIR)/tls.h $(INCLUDE_DIR)/rdma.h, $(wildcard $(INCLUDE_DIR)/*.h))

# Allow the libvalkey provided sds and dict types to be replaced by
# compatible implementations (like Valkey's).
# A replaced type is not included in a built archive or shared library.
SDS_INCLUDE_DIR ?= $(SRC_DIR)
DICT_INCLUDE_DIR ?= $(SRC_DIR)
ifneq ($(SDS_INCLUDE_DIR),$(SRC_DIR))
  SOURCES := $(filter-out $(SRC_DIR)/sds.c, $(SOURCES))
endif
ifneq ($(DICT_INCLUDE_DIR),$(SRC_DIR))
  SOURCES := $(filter-out $(SRC_DIR)/dict.c, $(SOURCES))
endif

OBJS = $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SOURCES))

LIBNAME=libvalkey
PKGCONFNAME=$(LIB_DIR)/valkey.pc

PKGCONF_TEMPLATE = valkey.pc.in
TLS_PKGCONF_TEMPLATE = valkey_tls.pc.in
RDMA_PKGCONF_TEMPLATE = valkey_rdma.pc.in

LIBVALKEY_HEADER=$(INCLUDE_DIR)/valkey.h
LIBVALKEY_VERSION=$(shell awk '/LIBVALKEY_VERSION_(MAJOR|MINOR|PATCH)/{gsub(/"/, "", $$3); print $$3}' $(LIBVALKEY_HEADER))
LIBVALKEY_MAJOR=$(word 1,$(LIBVALKEY_VERSION))
LIBVALKEY_MINOR=$(word 2,$(LIBVALKEY_VERSION))
LIBVALKEY_PATCH=$(word 3,$(LIBVALKEY_VERSION))

# Installation related variables and target
PREFIX?=/usr/local
INCLUDE_PATH?=include/valkey
LIBRARY_PATH?=lib
PKGCONF_PATH?=pkgconfig
INSTALL_INCLUDE_PATH= $(DESTDIR)$(PREFIX)/$(INCLUDE_PATH)
INSTALL_LIBRARY_PATH= $(DESTDIR)$(PREFIX)/$(LIBRARY_PATH)
INSTALL_PKGCONF_PATH= $(INSTALL_LIBRARY_PATH)/$(PKGCONF_PATH)

# valkey-server configuration used for testing
VALKEY_PORT=56379
VALKEY_SERVER=valkey-server
define VALKEY_TEST_CONFIG
	daemonize yes
	pidfile /tmp/valkey-test-valkey.pid
	port $(VALKEY_PORT)
	bind 127.0.0.1
	unixsocket /tmp/valkey-test-valkey.sock
endef
export VALKEY_TEST_CONFIG

# Fallback to gcc when $CC is not in $PATH.
CC := $(if $(shell command -v $(firstword $(CC)) >/dev/null 2>&1 && echo OK),$(CC),gcc)

OPTIMIZATION?=-O3
WARNINGS=-Wall -Wextra -pedantic -Wstrict-prototypes -Wwrite-strings -Wno-missing-field-initializers
USE_WERROR?=1
ifeq ($(USE_WERROR),1)
  WARNINGS+=-Werror
endif
DEBUG_FLAGS?= -g -ggdb
REAL_CFLAGS=$(OPTIMIZATION) -fPIC -fvisibility=hidden $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(DEBUG_FLAGS) $(PLATFORM_FLAGS)
REAL_LDFLAGS=$(LDFLAGS)

DYLIBSUFFIX=so
STLIBSUFFIX=a
DYLIB_PATCH_NAME=$(LIBNAME).$(DYLIBSUFFIX).$(LIBVALKEY_MAJOR).$(LIBVALKEY_MINOR).$(LIBVALKEY_PATCH)
DYLIB_MAJOR_NAME=$(LIBNAME).$(DYLIBSUFFIX).$(LIBVALKEY_MAJOR)
DYLIB_ROOT_NAME=$(LIBNAME).$(DYLIBSUFFIX)
DYLIBNAME=$(LIB_DIR)/$(DYLIB_ROOT_NAME)

DYLIB_MAKE_CMD=$(CC) $(OPTIMIZATION) $(PLATFORM_FLAGS) -shared -Wl,-soname,$(DYLIB_MAJOR_NAME)
STLIBNAME=$(LIB_DIR)/$(LIBNAME).$(STLIBSUFFIX)
STLIB_MAKE_CMD=$(AR) rcs

#################### TLS variables start ####################
TLS_LIBNAME=libvalkey_tls
TLS_PKGCONFNAME=$(LIB_DIR)/valkey_tls.pc
TLS_INSTALLNAME=install-tls
TLS_DYLIB_PATCH_NAME=$(TLS_LIBNAME).$(DYLIBSUFFIX).$(LIBVALKEY_MAJOR).$(LIBVALKEY_MINOR).$(LIBVALKEY_PATCH)
TLS_DYLIB_MAJOR_NAME=$(TLS_LIBNAME).$(DYLIBSUFFIX).$(LIBVALKEY_MAJOR)
TLS_DYLIB_ROOT_NAME=$(TLS_LIBNAME).$(DYLIBSUFFIX)
TLS_DYLIBNAME=$(LIB_DIR)/$(TLS_LIBNAME).$(DYLIBSUFFIX)
TLS_STLIBNAME=$(LIB_DIR)/$(TLS_LIBNAME).$(STLIBSUFFIX)
TLS_DYLIB_MAKE_CMD=$(CC) $(OPTIMIZATION) $(PLATFORM_FLAGS) -shared -Wl,-soname,$(TLS_DYLIB_MAJOR_NAME)

USE_TLS?=0

ifeq ($(USE_TLS),1)
  TLS_SOURCES = $(SRC_DIR)/tls.c
  TLS_OBJS = $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(TLS_SOURCES))

  # This is required for test.c only
  CFLAGS+=-DVALKEY_TEST_TLS
  TLS_STLIB=$(TLS_STLIBNAME)
  TLS_DYLIB=$(TLS_DYLIBNAME)
  TLS_PKGCONF=$(TLS_PKGCONFNAME)
  TLS_INSTALL=$(TLS_INSTALLNAME)
else
  TLS_STLIB=
  TLS_DYLIB=
  TLS_PKGCONF=
  TLS_INSTALL=
endif
##################### TLS variables end #####################

#################### RDMA variables start ####################
RDMA_LIBNAME=libvalkey_rdma
RDMA_PKGCONFNAME=$(LIB_DIR)/valkey_rdma.pc
RDMA_INSTALLNAME=install-rdma
RDMA_DYLIB_PATCH_NAME=$(RDMA_LIBNAME).$(DYLIBSUFFIX).$(LIBVALKEY_MAJOR).$(LIBVALKEY_MINOR).$(LIBVALKEY_PATCH)
RDMA_DYLIB_MAJOR_NAME=$(RDMA_LIBNAME).$(DYLIBSUFFIX).$(LIBVALKEY_MAJOR)
RDMA_DYLIB_ROOT_NAME=$(RDMA_LIBNAME).$(DYLIBSUFFIX)
RDMA_DYLIBNAME=$(LIB_DIR)/$(RDMA_LIBNAME).$(DYLIBSUFFIX)
RDMA_STLIBNAME=$(LIB_DIR)/$(RDMA_LIBNAME).$(STLIBSUFFIX)
RDMA_DYLIB_MAKE_CMD=$(CC) $(OPTIMIZATION) $(PLATFORM_FLAGS) -shared -Wl,-soname,$(RDMA_DYLIB_MAJOR_NAME)

USE_RDMA?=0

ifeq ($(USE_RDMA),1)
  RDMA_SOURCES=$(SRC_DIR)/rdma.c
  RDMA_OBJS=$(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(RDMA_SOURCES))
  RDMA_LDFLAGS=-lrdmacm -libverbs
  # This is required for test.c only
  CFLAGS+=-DVALKEY_TEST_RDMA
  RDMA_STLIB=$(RDMA_STLIBNAME)
  RDMA_DYLIB=$(RDMA_DYLIBNAME)
  RDMA_PKGCONF=$(RDMA_PKGCONFNAME)
  RDMA_INSTALL=$(RDMA_INSTALLNAME)
else
  RDMA_STLIB=
  RDMA_DYLIB=
  RDMA_PKGCONF=
  RDMA_INSTALL=
endif
##################### RDMA variables end #####################

# Platform-specific overrides
uname_S := $(shell uname -s 2>/dev/null || echo not)

# This is required for test.c only
ifeq ($(TEST_ASYNC),1)
  export CFLAGS+=-DVALKEY_TEST_ASYNC
endif

ifeq ($(USE_TLS),1)
  ifndef OPENSSL_PREFIX
    ifeq ($(uname_S),Darwin)
      SEARCH_PATH1=/opt/homebrew/opt/openssl
      SEARCH_PATH2=/usr/local/opt/openssl

      ifneq ($(wildcard $(SEARCH_PATH1)),)
        OPENSSL_PREFIX=$(SEARCH_PATH1)
      else ifneq ($(wildcard $(SEARCH_PATH2)),)
        OPENSSL_PREFIX=$(SEARCH_PATH2)
      endif
    endif
  endif

  ifdef OPENSSL_PREFIX
    CFLAGS+=-I$(OPENSSL_PREFIX)/include
    TLS_LDFLAGS+=-L$(OPENSSL_PREFIX)/lib
  endif

  TLS_LDFLAGS+=-lssl -lcrypto
endif

ifeq ($(uname_S),FreeBSD)
  LDFLAGS += -lm
else ifeq ($(UNAME_S),SunOS)
  ifeq ($(shell $(CC) -V 2>&1 | grep -iq 'sun\|studio' && echo true),true)
    SUN_SHARED_FLAG = -G
  else
    SUN_SHARED_FLAG = -shared
  endif
  REAL_LDFLAGS += -ldl -lnsl -lsocket
  DYLIB_MAKE_CMD = $(CC) $(OPTIMIZATION) $(SUN_SHARED_FLAG) -o $(DYLIBNAME) -h $(DYLIB_PATCH_NAME) $(LDFLAGS)
  TLS_DYLIB_MAKE_CMD = $(CC) $(SUN_SHARED_FLAG) -o $(TLS_DYLIBNAME) -h $(TLS_DYLIB_PATCH_NAME) $(LDFLAGS) $(TLS_LDFLAGS)
else ifeq ($(uname_S),Darwin)
  DYLIBSUFFIX=dylib
  DYLIB_PATCH_NAME=$(LIBNAME).$(LIBVALKEY_MAJOR).$(LIBVALKEY_MINOR).$(LIBVALKEY_PATCH).$(DYLIBSUFFIX)
  DYLIB_MAJOR_NAME=$(LIBNAME).$(LIBVALKEY_MAJOR).$(DYLIBSUFFIX)
  DYLIB_MAKE_CMD=$(CC) -dynamiclib -Wl,-install_name,$(PREFIX)/$(LIBRARY_PATH)/$(DYLIB_PATCH_NAME) -o $(DYLIBNAME) $(LDFLAGS)
  TLS_DYLIB_PATCH_NAME=$(TLS_LIBNAME).$(LIBVALKEY_MAJOR).$(LIBVALKEY_MINOR).$(LIBVALKEY_PATCH).$(DYLIBSUFFIX)
  TLS_DYLIB_MAJOR_NAME=$(TLS_LIBNAME).$(LIBVALKEY_MAJOR).$(DYLIBSUFFIX)
  TLS_DYLIB_MAKE_CMD=$(CC) -dynamiclib -Wl,-install_name,$(PREFIX)/$(LIBRARY_PATH)/$(TLS_DYLIB_PATCH_NAME) -o $(TLS_DYLIBNAME) $(LDFLAGS) $(TLS_LDFLAGS)
  DYLIB_PLUGIN=-Wl,-undefined -Wl,dynamic_lookup
endif

all: dynamic static pkgconfig tests

$(DYLIBNAME): $(OBJS) | $(LIB_DIR)
	$(DYLIB_MAKE_CMD) -o $(DYLIBNAME) $(OBJS) $(REAL_LDFLAGS)

$(STLIBNAME): $(OBJS) | $(LIB_DIR)
	$(STLIB_MAKE_CMD) $(STLIBNAME) $(OBJS)

$(TLS_DYLIBNAME): $(TLS_OBJS)
	$(TLS_DYLIB_MAKE_CMD) $(DYLIB_PLUGIN) -o $(TLS_DYLIBNAME) $(TLS_OBJS) $(REAL_LDFLAGS) $(LDFLAGS) $(TLS_LDFLAGS)

$(TLS_STLIBNAME): $(TLS_OBJS)
	$(STLIB_MAKE_CMD) $(TLS_STLIBNAME) $(TLS_OBJS)

$(RDMA_DYLIBNAME): $(RDMA_OBJS)
	$(RDMA_DYLIB_MAKE_CMD) $(DYLIB_PLUGIN) -o $(RDMA_DYLIBNAME) $(RDMA_OBJS) $(REAL_LDFLAGS) $(LDFLAGS) $(RDMA_LDFLAGS)

$(RDMA_STLIBNAME): $(RDMA_OBJS)
	$(STLIB_MAKE_CMD) $(RDMA_STLIBNAME) $(RDMA_OBJS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) -std=c99 -pedantic $(REAL_CFLAGS) -I$(INCLUDE_DIR) -I$(SDS_INCLUDE_DIR) -I$(DICT_INCLUDE_DIR) -MMD -MP -c $< -o $@

$(OBJ_DIR)/%.o: $(TEST_DIR)/%.c | $(OBJ_DIR)
	$(CC) -std=c99 -pedantic $(REAL_CFLAGS) -I$(INCLUDE_DIR) -I$(SDS_INCLUDE_DIR) -I$(DICT_INCLUDE_DIR) -I$(SRC_DIR) -MMD -MP -c $< -o $@

$(TEST_DIR)/%: $(OBJ_DIR)/%.o $(STLIBNAME)
	$(CC) -o $@ $< $(RDMA_STLIB) $(STLIBNAME) $(TLS_STLIB) $(LDFLAGS) $(TEST_LDFLAGS)

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(LIB_DIR):
	mkdir -p $(LIB_DIR)

dynamic: $(DYLIBNAME) $(TLS_DYLIB) $(RDMA_DYLIB)

static: $(STLIBNAME) $(TLS_STLIB) $(RDMA_STLIB)

pkgconfig: $(PKGCONFNAME) $(TLS_PKGCONF) $(RDMA_PKGCONF)

-include $(OBJS:.o=.d)

TEST_LDFLAGS = $(TLS_LDFLAGS) $(RDMA_LDFLAGS)
ifeq ($(USE_TLS),1)
  TEST_LDFLAGS += -pthread
endif
ifeq ($(TEST_ASYNC),1)
    TEST_LDFLAGS += -levent
endif

tests: $(TEST_BINS)

examples: $(STLIBNAME)
	$(MAKE) -C examples

clean:
	rm -rf $(OBJ_DIR) $(LIB_DIR) $(TEST_BINS) *.gcda *.gcno *.gcov
	$(MAKE) -C examples clean

INSTALL?= cp -pPR

$(PKGCONFNAME): $(PKGCONF_TEMPLATE)
	@echo "Generating $@ for pkgconfig..."
	sed \
		-e 's|@CMAKE_INSTALL_PREFIX@|$(PREFIX)|g' \
		-e 's|@CMAKE_INSTALL_LIBDIR@|$(INSTALL_LIBRARY_PATH)|g' \
		-e 's|@PROJECT_VERSION@|$(LIBVALKEY_SONAME)|g' \
		$< > $@

$(TLS_PKGCONFNAME): $(TLS_PKGCONF_TEMPLATE)
	@echo "Generating $@ for pkgconfig..."
	sed \
		-e 's|@CMAKE_INSTALL_PREFIX@|$(PREFIX)|g' \
		-e 's|@CMAKE_INSTALL_LIBDIR@|$(INSTALL_LIBRARY_PATH)|g' \
		-e 's|@PROJECT_VERSION@|$(LIBVALKEY_SONAME)|g' \
		$< > $@

$(RDMA_PKGCONFNAME): $(RDMA_PKGCONF_TEMPLATE)
	@echo "Generating $@ for pkgconfig..."
	sed \
		-e 's|@CMAKE_INSTALL_PREFIX@|$(PREFIX)|g' \
		-e 's|@CMAKE_INSTALL_LIBDIR@|$(INSTALL_LIBRARY_PATH)|g' \
		-e 's|@PROJECT_VERSION@|$(LIBVALKEY_SONAME)|g' \
		$< > $@

install: $(DYLIBNAME) $(STLIBNAME) $(PKGCONFNAME) $(TLS_INSTALL) $(RDMA_INSTALL)
	mkdir -p $(INSTALL_INCLUDE_PATH)/adapters $(INSTALL_LIBRARY_PATH)
	$(INSTALL) $(HEADERS) $(INSTALL_INCLUDE_PATH)
	$(INSTALL) $(INCLUDE_DIR)/adapters/*.h $(INSTALL_INCLUDE_PATH)/adapters
	$(INSTALL) $(DYLIBNAME) $(INSTALL_LIBRARY_PATH)/$(DYLIB_PATCH_NAME)
	ln -sf $(DYLIB_PATCH_NAME) $(INSTALL_LIBRARY_PATH)/$(DYLIB_MAJOR_NAME)
	ln -sf $(DYLIB_MAJOR_NAME) $(INSTALL_LIBRARY_PATH)/$(DYLIB_ROOT_NAME)
	$(INSTALL) $(STLIBNAME) $(INSTALL_LIBRARY_PATH)
	mkdir -p $(INSTALL_PKGCONF_PATH)
	$(INSTALL) $(PKGCONFNAME) $(INSTALL_PKGCONF_PATH)

install-tls: $(TLS_DYLIBNAME) $(TLS_STLIBNAME) $(TLS_PKGCONFNAME)
	mkdir -p $(INSTALL_INCLUDE_PATH) $(INSTALL_LIBRARY_PATH)
	$(INSTALL) $(INCLUDE_DIR)/tls.h $(INSTALL_INCLUDE_PATH)
	$(INSTALL) $(TLS_DYLIBNAME) $(INSTALL_LIBRARY_PATH)/$(TLS_DYLIB_PATCH_NAME)
	ln -sf $(TLS_DYLIB_PATCH_NAME) $(INSTALL_LIBRARY_PATH)/$(TLS_DYLIB_MAJOR_NAME)
	ln -sf $(TLS_DYLIB_MAJOR_NAME) $(INSTALL_LIBRARY_PATH)/$(TLS_DYLIB_ROOT_NAME)
	$(INSTALL) $(TLS_STLIBNAME) $(INSTALL_LIBRARY_PATH)
	mkdir -p $(INSTALL_PKGCONF_PATH)
	$(INSTALL) $(TLS_PKGCONFNAME) $(INSTALL_PKGCONF_PATH)

install-rdma: $(RDMA_DYLIBNAME) $(RDMA_STLIBNAME) $(RDMA_PKGCONFNAME)
	mkdir -p $(INSTALL_INCLUDE_PATH) $(INSTALL_LIBRARY_PATH)
	$(INSTALL) $(INCLUDE_DIR)/rdma.h $(INSTALL_INCLUDE_PATH)
	$(INSTALL) $(RDMA_DYLIBNAME) $(INSTALL_LIBRARY_PATH)/$(RDMA_DYLIB_PATCH_NAME)
	ln -sf $(RDMA_DYLIB_PATCH_NAME) $(INSTALL_LIBRARY_PATH)/$(RDMA_DYLIB_MAJOR_NAME)
	ln -sf $(RDMA_DYLIB_MAJOR_NAME) $(INSTALL_LIBRARY_PATH)/$(RDMA_DYLIB_ROOT_NAME)
	$(INSTALL) $(RDMA_STLIBNAME) $(INSTALL_LIBRARY_PATH)
	mkdir -p $(INSTALL_PKGCONF_PATH)
	$(INSTALL) $(RDMA_PKGCONFNAME) $(INSTALL_PKGCONF_PATH)

32bit:
	@echo ""
	@echo "WARNING: if this fails under Linux you probably need to install libc6-dev-i386"
	@echo ""
	$(MAKE) CFLAGS="-m32" LDFLAGS="-m32"

32bit-vars:
	$(eval CFLAGS=-m32)
	$(eval LDFLAGS=-m32)

gprof:
	$(MAKE) CFLAGS="-pg" LDFLAGS="-pg"

gcov:
	$(MAKE) CFLAGS+="-fprofile-arcs -ftest-coverage" LDFLAGS="-fprofile-arcs"

coverage: gcov
	make check
	mkdir -p tmp/lcov
	lcov -d . -c --exclude '/usr*' -o tmp/lcov/valkey.info
	lcov -q -l tmp/lcov/valkey.info
	genhtml --legend -q -o tmp/lcov/report tmp/lcov/valkey.info

debug:
	$(MAKE) OPTIMIZATION="-O0"

.PHONY: all test check clean install 32bit 32bit-vars gprof gcov noopt
