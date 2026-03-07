CCCOLOR:="\033[34m"
SRCCOLOR:="\033[33m"
ENDCOLOR:="\033[0m"

CC?=cc
TEMP:=$(CC)
QUIET_CC=@printf '    %b %b\n' $(CCCOLOR)CC$(ENDCOLOR) $(SRCCOLOR)$@$(ENDCOLOR) 1>&2;
CC=$(QUIET_CC)$(TEMP)

WARN=-Wall -W -Wno-missing-field-initializers

STD=-pedantic -std=c99

OPT?=-O3
CLANG := $(findstring clang,$(shell sh -c '$(TEMP) --version | head -1'))
ifeq ($(OPT),-O3)
        ifeq (clang,$(CLANG))
                OPT+=-flto
        else
                OPT+=-flto=auto -ffat-lto-objects
        endif
endif

CFLAGS_LOCAL=$(STD) $(OPT) $(WARN) -static -fPIC $(CFLAGS)

.PHONY: all clean

all: fast_float_strtod.o

fast_float_strtod.o: fast_float_strtod.c ../fast_float/ffc.h
	$(CC) $(CFLAGS_LOCAL) -c fast_float_strtod.c -o fast_float_strtod.o

clean:
	rm -f *.o || true;
