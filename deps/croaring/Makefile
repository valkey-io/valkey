STD= -std=gnu11
WARN= -Wall
OPT= -O3

# Symbols stay hidden so that they cannot clash with a copy of CRoaring
# bundled in a module.
R_CFLAGS= $(STD) $(WARN) $(OPT) $(DEBUG) $(CFLAGS) -fvisibility=hidden
R_LDFLAGS= $(LDFLAGS)
DEBUG= -g

R_CC=$(CC) $(R_CFLAGS)
R_LD=$(CC) $(R_LDFLAGS)

AR= ar
ARFLAGS= rcs

libcroaring.a: roaring.o
	$(AR) $(ARFLAGS) $@ $^

roaring.o: roaring.c roaring.h

.c.o:
	$(R_CC) -fPIC -c $<

.PHONY: clean
clean:
	rm -f *.o
	rm -f *.a
