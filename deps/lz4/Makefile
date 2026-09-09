STD=
WARN= -Wall
OPT= -Os

R_CFLAGS= $(STD) $(WARN) $(OPT) $(DEBUG) $(CFLAGS) -DXXH_NAMESPACE=VALKEY_LZ4_
R_LDFLAGS= $(LDFLAGS)
DEBUG= -g

R_CC=$(CC) $(R_CFLAGS)
R_LD=$(CC) $(R_LDFLAGS)

AR= ar
ARFLAGS= rcs

liblz4.a: lz4.o lz4hc.o lz4frame.o xxhash.o
	$(AR) $(ARFLAGS) $@ $^

lz4.o: lz4.c lz4.h
lz4hc.o: lz4hc.c lz4hc.h lz4.h
lz4frame.o: lz4frame.c lz4frame.h lz4.h lz4hc.h xxhash.h
xxhash.o: xxhash.c xxhash.h

.c.o:
	$(R_CC) -fPIC -c $<

.PHONY: clean
clean:
	rm -f *.o
	rm -f *.a
