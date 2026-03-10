CXX ?= g++
AR ?= ar
SERVER_AR ?= $(AR)
-include ../.make-settings
OPT=-Og -g
uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')

# Add -m32 for 32bit builds
ifeq ($(findstring -m32,$(CFLAGS)),-m32)
	CXX+= -m32
endif

.DEFAULT_GOAL := all
SOURCES := $(filter-out generated_wrappers.cpp, $(wildcard *.cpp))
ALL_OBJECTS := $(patsubst %.cpp,%.o,$(SOURCES))
DEPENDS := $(patsubst %.cpp,%.d,$(SOURCES))

# Inherit CFLAGS from parent Makefile
TEST_CFLAGS := $(CFLAGS)

# Hold new gtest test to a high standard
MORE_WARNINGS=-Wno-variadic-macros \
	-Werror \
	-Wpedantic \
	-Wextra \
	-Wall \
	-Wcast-align \
	-Wframe-larger-than=32768 \
	-Wno-strict-overflow \
	-Wsign-compare \
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

# Enable -Werror=float-equal only for non-32bit builds.
# On 32bit (-m32), GoogleTest headers trigger -Wfloat-equal
# (e.g., in gtest-printers.h), which would fail the build.
ifneq ($(findstring -m32,$(CFLAGS)),-m32)
	MORE_WARNINGS += -Werror=float-equal
endif

-include $(DEPENDS)

ifdef COVERAGE
	TEST_CFLAGS += -DCOVERAGE_TEST $(COVERAGE_CFLAGS)
endif

ifdef FORCE_RUN_SKIPPED_TESTS
	TEST_CFLAGS+=-DFORCE_RUN_SKIPPED_TESTS
endif

# Track compilation flags to force rebuild when they change
.flags: FORCE
	@echo '$(TEST_CFLAGS)' | cmp -s - $@ || echo '$(TEST_CFLAGS)' > $@

.PHONY: FORCE

# Valkey server library dependencies
VALKEY_SERVER_LIB := ../libvalkey.a
HIREDIS_LIB := ../../deps/libvalkey/lib/libvalkey.a
JEMALLOC_LIB := ../../deps/jemalloc/lib/libjemalloc.a
HDR_HISTOGRAM_LIB := ../../deps/hdr_histogram/libhdrhistogram.a
FPCONV_LIB := ../../deps/fpconv/libfpconv.a
TLS_LIBS := ../../deps/libvalkey/lib/libvalkey_tls.a -lssl -lcrypto
GTEST_CFLAGS ?=
GTEST_LIBS   ?=
ifeq ($(strip $(GTEST_LIBS)),)
	GTEST_CFLAGS := $(shell pkg-config --cflags gtest gmock 2>/dev/null)
	GTEST_LIBS   := $(shell pkg-config --libs   gtest gmock 2>/dev/null)
endif

ifeq ($(strip $(GTEST_LIBS)),)
    $(error googletest not found. Install gtest or set GTEST_CFLAGS/GTEST_LIBS explicitly)
endif
FAST_FLOAT_LIB := ../../deps/fast_float_c_interface/fast_float_strtod.o

# On Darwin (macOS), the linker does not support -Wl,--wrap,
# so we manually define __wrap_* and __real_* for each function in wrappers.h.
# by copying all object files into the wrapped/ folder and applying symbol redefinitions.
ifeq ($(uname_S), Darwin)
WRAP_DIR := wrapped
VALKEY_SERVER_WRAP_LIB := $(WRAP_DIR)/wrappedlibvalkey.a
ENGINE_SERVER_WRAP_OBJ := $(addprefix $(WRAP_DIR)/, $(ENGINE_SERVER_OBJ))

$(WRAP_DIR):
	@mkdir -p $(WRAP_DIR)

$(WRAP_DIR)/%.o: ../%.o $(WRAP_DIR) generated_wrappers.o
	@mkdir -p $(dir $@)
	@if file $< | grep -q "LLVM bitcode"; then \
		llc $< -filetype=obj -o $(WRAP_DIR)/$*_temp.o; \
		python3 generate-redefine-syms.py $(WRAP_DIR)/$*_temp.o > $(WRAP_DIR)/$*-wrap-syms; \
		llvm-objcopy --redefine-syms=$(WRAP_DIR)/$*-wrap-syms $(WRAP_DIR)/$*_temp.o $@; \
		rm -f $(WRAP_DIR)/$*_temp.o; \
	else \
		python3 generate-redefine-syms.py $< > $(WRAP_DIR)/$*-wrap-syms; \
		llvm-objcopy --redefine-syms=$(WRAP_DIR)/$*-wrap-syms $< $@; \
	fi
	@rm -f $(WRAP_DIR)/$*-wrap-syms

$(VALKEY_SERVER_WRAP_LIB): $(VALKEY_SERVER_LIB) $(ENGINE_SERVER_WRAP_OBJ) generated_wrappers.o
	$(SERVER_AR) rcs $@ $(ENGINE_SERVER_WRAP_OBJ)
endif

# Ensure Valkey server library is built
$(VALKEY_SERVER_LIB): make-prerequisites
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

LD_LIBS := $(HIREDIS_LIB) \
	$(JEMALLOC_LIB) \
	$(GTEST_LIBS) \
	$(HDR_HISTOGRAM_LIB) \
	$(FPCONV_LIB)

# Add libbacktrace support if enabled
ifeq ($(USE_LIBBACKTRACE),yes)
    TEST_CFLAGS += -DUSE_LIBBACKTRACE
    ifdef LIBBACKTRACE_PREFIX
        TEST_CFLAGS += -I$(LIBBACKTRACE_PREFIX)/include
        LDFLAGS += -L$(LIBBACKTRACE_PREFIX)/lib
    endif
    LD_LIBS += -lbacktrace
endif

# Add fast_float support if enabled
ifeq ($(USE_FAST_FLOAT),yes)
	LD_LIBS += $(FAST_FLOAT_LIB)
endif

# Add TLS libraries if enabled
ifeq ($(BUILD_TLS),yes)
	LD_LIBS += $(TLS_LIBS)
endif

# Compile C++ test files, recompile if generated_wrappers or compiler flags change
%.o: %.cpp generated_wrappers.cpp .flags
	$(CXX) -MD -MP -std=c++17 -faligned-new -Wno-write-strings -fpermissive $(GCC_FLAGS) $(OPT) $(DEBUG) $(TEST_CFLAGS) -Wall -Wno-deprecated-declarations -c \
	-isystem .. -I ../../deps/lua/src/ -I ../../deps/hdr_histogram/ -I ../../deps/fpconv/ $(JEMALLOC_CFLAGS) \
	$(GTEST_CFLAGS) $<

$(ALL_OBJECTS): generated_wrappers.o
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
generated_wrappers.cpp: make-prerequisites wrappers.h generate-wrappers.py
	./generate-wrappers.py

generated_wrappers.o: generated_wrappers.cpp

# Generate the single test binary in parent directory.
ifeq ($(uname_S), Darwin)
valkey-unit-gtests: $(ALL_OBJECTS) generated_wrappers.o $(VALKEY_SERVER_WRAP_LIB) $(FINAL_LIBS)
	$(CXX) -g $(LDFLAGS) -fno-lto \
		-Wl,-flat_namespace -Wl,-undefined,dynamic_lookup \
		-Wall -Wno-deprecated-declarations \
		-o $@ $^ $(LD_LIBS) \
		-lm -pthread -ldl
else
valkey-unit-gtests: $(ALL_OBJECTS) generated_wrappers.o $(VALKEY_SERVER_LIB) $(FINAL_LIBS)
	$(CXX) -g $(LDFLAGS) -fno-lto \
		$(OVERRIDES) \
		-Wall -Wno-deprecated-declarations \
		-o $@ $^ $(LD_LIBS) \
		-lrt -lm -pthread -ldl $(if $(COVERAGE),-lgcov)
endif

.PHONY: all
all: valkey-unit-gtests

.PHONY: test-unit
test-unit: valkey-unit-gtests
	python3 ../../deps/gtest-parallel/gtest_parallel.py ./valkey-unit-gtests
	@printf '\033[32mAll UNIT TESTS PASSED!\033[0m\n'

.PHONY: valgrind
valgrind: valkey-unit-gtests
	valgrind $(VALGRIND_ARGS) ./valkey-unit-gtests

clean:
	rm -rf *.o valkey-unit-gtests ../valkey-unit-gtests generated_* $(DEPENDS) .flags wrapped

distclean: clean
	$(MAKE) -C .. distclean

.PHONY: clean distclean
