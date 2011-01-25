CFLAGS = -Wall -O2 -g -fPIC

LIBOBJS = wbh.o
TESTOBJS = wtest.o

all: libwbh.a libwbh.so wtest

clean:
	rm -f $(LIBOBJS) libwbh.a libwbh.so

libwbh.a: $(LIBOBJS)
	$(AR) rcs $@ $<

libwbh.so: $(LIBOBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -shared -o $@ $<

wtest: $(TESTOBJS) libwbh.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< ./libwbh.a

wbh.o: wbh.h
wtest.o: wbh.h
