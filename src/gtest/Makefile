CXX ?= g++
AR ?= ar
SERVER_AR ?= $(AR)
-include ../.make-settings
OPT=-O3 -DNDEBUG
uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')

# Add -m32 for 32bit builds
ifeq ($(findstring -m32,$(CFLAGS)),-m32)
	CXX+= -m32
endif

.DEFAULT_GOAL := all
SOURCES := $(wildcard *.cpp)
ALL_OBJECTS := $(patsubst %.cpp,%.o,$(SOURCES))
DEPENDS := $(patsubst %.cpp,%.d,$(SOURCES))

# Hold new gtest test to a high standard
MORE_WARNINGS=-Wold-style-cast \
	-Wno-variadic-macros \
	-Werror \
	-Wpedantic \
	-Wextra \
	-Wall \
	-Wcast-align \
	-Wcast-qual \
	-Wframe-larger-than=32768 \
	-Wno-strict-overflow \
	-Wsign-compare \
	-Werror=float-equal \
	-Werror=missing-braces \
	-Werror=init-self \
	-Werror=write-strings \
	-Werror=address \
	-Werror=array-bounds \
	-Werror=char-subscripts \
	-Werror=enum-compare \
	-Werror=empty-body \
	-Werror=main \
	-Werror=nonnull \
	-Werror=parentheses \
	-Werror=return-type \
	-Werror=sequence-point \
	-Werror=uninitialized \
	-Werror=volatile-register-var \
	-Werror=ignored-qualifiers \
	-Wno-unused-function \
	-Wno-missing-field-initializers \
	-Wdouble-promotion \
	-Wformat=2

# Detect compiler
IS_GCC := $(if $(findstring clang,$(CXX)),,$(findstring g++,$(CXX)))

GCC_FLAGS :=

# Only add GCC-only warnings and flags
ifeq ($(IS_GCC),g++)
    # GCC supports these extra warnings and -fno-var-tracking-assignments
    MORE_WARNINGS += -Wsync-nand -Wtrampolines -Werror=logical-op -Werror=aggressive-loop-optimizations
    GCC_FLAGS += -fno-var-tracking-assignments
else
    # Clang: disable warnings that are errors in gtest/gmock
    MORE_WARNINGS += -Wno-deprecated-copy
	MORE_WARNINGS += -Wno-keyword-macro
endif

-include $(DEPENDS)

ifdef COVERAGE
	TEST_CFLAGS += -DCOVERAGE_TEST $(COVERAGE_CFLAGS)
endif

ifdef FORCE_RUN_SKIPPED_TESTS
	TEST_CFLAGS+=-DFORCE_RUN_SKIPPED_TESTS
endif

# Track compilation flags to force rebuild when they change
.PHONY: .flags
.flags:
	@echo '$(TEST_CFLAGS)' | cmp -s - $@ || echo '$(TEST_CFLAGS)' > $@

# Valkey server library dependencies
VALKEY_SERVER_LIB := ../libvalkey.a
HIREDIS_LIB := ../../deps/libvalkey/lib/libvalkey.a
JEMALLOC_LIB := ../../deps/jemalloc/lib/libjemalloc.a
HDR_HISTOGRAM_LIB := ../../deps/hdr_histogram/libhdrhistogram.a
FPCONV_LIB := ../../deps/fpconv/libfpconv.a
GTEST_LIB := ../../deps/googletest/build/googlemock/gtest/libgtest.a
GMOCK_LIB := ../../deps/googletest/build/googlemock/libgmock.a
FAST_FLOAT_LIB := ../../deps/fast_float_c_interface/fast_float_strtod.o

# On Darwin (macOS), the linker does not support -Wl,--wrap,
# so we manually define __wrap_* and __real_* for each function in wrappers.h.
# by copying all object files into the wrapped/ folder and applying symbol redefinitions.
ifeq ($(uname_S), Darwin)
WRAP_DIR := wrapped
VALKEY_SERVER_WRAP_LIB := $(WRAP_DIR)/wrappedlibvalkey.a
ENGINE_SERVER_WRAP_OBJ := $(addprefix $(WRAP_DIR)/, $(ENGINE_SERVER_OBJ))

$(WRAP_DIR):
	mkdir -p $(WRAP_DIR)

$(WRAP_DIR)/%.o: ../%.o $(WRAP_DIR) generated_wrappers.o
	@mkdir -p $(dir $@)
	@if file $< | grep -q "LLVM bitcode"; then \
		llc $< -filetype=obj -o $(WRAP_DIR)/$*_temp.o; \
		python3 generate-redefine-syms.py $(WRAP_DIR)/$*_temp.o >$(WRAP_DIR)/$*-wrap-syms; \
		llvm-objcopy --redefine-syms=$(WRAP_DIR)/$*-wrap-syms $(WRAP_DIR)/$*_temp.o $@; \
		rm $(WRAP_DIR)/$*_temp.o 2>/dev/null; \
	else \
		python3 generate-wrapper-objects.py $< >$(WRAP_DIR)/$*-wrap-syms; \
		llvm-objcopy --redefine-syms=$(WRAP_DIR)/$*-wrap-syms $< $@; \
	fi
	rm $(WRAP_DIR)/$*-wrap-syms 2>/dev/null

$(VALKEY_SERVER_WRAP_LIB): $(VALKEY_SERVER_LIB) $(ENGINE_SERVER_WRAP_OBJ) generated_wrappers.o
	$(SERVER_AR) rcs $@ $(ENGINE_SERVER_WRAP_OBJ)
endif

# Ensure GoogleTest is built
../../deps/googletest/build/googlemock/gtest/libgtest.a ../../deps/googletest/build/googlemock/libgmock.a:
	cd ../../deps && $(MAKE) gtest

# Ensure Valkey server library is built
$(VALKEY_SERVER_LIB):
	cd .. && $(MAKE) libvalkey.a

# Ensure parent prerequisites are built
.PHONY: make-prerequisites
make-prerequisites:
	cd .. && $(MAKE) .make-prerequisites

# Read MALLOC / USE_JEMALLOC setting from parent Makefile
JEMALLOC_CFLAGS := -DUSE_JEMALLOC -isystem../../deps/jemalloc/include
ifeq ($(MALLOC),libc)
	JEMALLOC_CFLAGS :=
	JEMALLOC_LIB :=
endif

# Default target to compile against Valkey, LUA, jemalloc (as Valkey does) as well as gmock
%.o: %.cpp make-prerequisites generated_wrappers.o .flags
	$(CXX) -MD -MP -std=c++14 -faligned-new -Wno-write-strings -fpermissive $(GCC_FLAGS) $(OPT) $(DEBUG) $(TEST_CFLAGS) -Wall -Wno-deprecated-declarations -c \
	-isystem .. -I ../../deps/lua/src/ -I ../../deps/hdr_histogram/ -I ../../deps/fpconv/ $(JEMALLOC_CFLAGS) \
	-I ../../deps/googletest/googletest/include -I ../../deps/googletest/googlemock/include $<

$(ALL_OBJECTS): OPT := $(OPT) $(MORE_WARNINGS)

# Obtain the list of function calls for which we are generating mocks using the gmock library.  We assume they are defined in
# wrappers.h and begin with the prefix '__wrap_' (which is required by the linker).  These command line arguments are then
# passed in to g++ during linking.
ifneq ($(wildcard wrappers.h),)
CMD:=egrep '__wrap_.*\(.*;' wrappers.h | sed 's/__wrap_/@/' | sed 's/[^@]*@\([^(]*\)(.*/\1/' | sort | uniq | xargs -I{} echo "-Wl,--wrap={}" | tr "\n" " "
OVERRIDES:=$(shell $(CMD))
else
OVERRIDES:=
endif

# Generate a new set of wrappers every time our header changes.
generated_wrappers.cpp: wrappers.h generate-wrappers.py
	./generate-wrappers.py

generated_wrappers.o: generated_wrappers.cpp

LD_LIBS := $(HIREDIS_LIB) \
    $(JEMALLOC_LIB) \
    $(GMOCK_LIB) \
    $(GTEST_LIB) \
    $(HDR_HISTOGRAM_LIB) \
    $(FPCONV_LIB)

# Add fast_float support if enabled
ifeq ($(USE_FAST_FLOAT),yes)
    LD_LIBS += $(FAST_FLOAT_LIB)
endif

# Generate the single test binary in parent directory.
ifeq ($(uname_S), Darwin)
valkey-unit-gtests: $(ALL_OBJECTS) generated_wrappers.o $(VALKEY_SERVER_WRAP_LIB) $(FINAL_LIBS)
	$(CXX) -g -fno-lto \
		-Wl,-flat_namespace -Wl,-undefined,dynamic_lookup \
		-Wall -Wno-deprecated-declarations \
		-o $@ $^ $(LD_LIBS) \
		-lm -pthread -ldl
else
valkey-unit-gtests: $(ALL_OBJECTS) generated_wrappers.o $(VALKEY_SERVER_LIB) $(FINAL_LIBS)
	$(CXX) -g \
		-Wl,--allow-multiple-definition \
		$(OVERRIDES) \
		-Wall -Wno-deprecated-declarations \
		-o $@ $^ $(LD_LIBS) \
		-lrt -lm -pthread -ldl -lgcov
endif

.PHONY: all
all: valkey-unit-gtests

.PHONY: test-gtest
test-gtest: valkey-unit-gtests
	python3 ../../deps/gtest-parallel/gtest_parallel.py ./valkey-unit-gtests

.PHONY: valgrind
valgrind: valkey-unit-gtests
	valgrind $(VALGRIND_ARGS) ./valkey-unit-gtests

clean:
	rm -rf *.o valkey-unit-gtests ../valkey-unit-gtests generated_* $(DEPENDS) .flags wrapped

distclean: clean
	$(MAKE) -C .. distclean

.PHONY: clean distclean
