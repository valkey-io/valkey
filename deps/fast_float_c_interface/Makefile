CCCOLOR:="\033[34m"
SRCCOLOR:="\033[33m"
ENDCOLOR:="\033[0m"

CXX?=c++
# we need = instead of := so that $@ in QUIET_CXX gets evaluated in the rule and is assigned appropriate value.
TEMP:=$(CXX)
QUIET_CXX=@printf '    %b %b\n' $(CCCOLOR)C++$(ENDCOLOR) $(SRCCOLOR)$@$(ENDCOLOR) 1>&2;
CXX=$(QUIET_CXX)$(TEMP)

WARN=-Wall -W -Wno-missing-field-initializers

STD=-pedantic -std=c++11

OPT?=-O3
CLANG := $(findstring clang,$(shell sh -c '$(CC) --version | head -1'))
ifeq ($(OPT),-O3)
        ifeq (clang,$(CLANG))
                OPT+=-flto
        else
                OPT+=-flto=auto -ffat-lto-objects
        endif
endif

# 1) Today src/Makefile passes -m32 flag for explicit 32-bit build on 64-bit machine, via CFLAGS. For 32-bit build on
# 32-bit machine and 64-bit on 64-bit machine, CFLAGS are empty. No other flags are set that can conflict with C++,
# therefore let's use CFLAGS without changes for now.
# 2) FASTFLOAT_ALLOWS_LEADING_PLUS allows +inf to be parsed as inf, instead of error.
CXXFLAGS=$(STD) $(OPT) $(WARN) -static -fPIC -fno-exceptions $(CFLAGS) -D FASTFLOAT_ALLOWS_LEADING_PLUS 

.PHONY: all clean

all: fast_float_strtod.o

clean:
	rm -f *.o || true;

