CFLAGS = -Wall -O2 -g -fPIC

LIBOBJS = wbh.o
TESTOBJS = wtest.o

all: libwbh.a libwbh.so wtest html/index.html

clean:
	rm -fr $(LIBOBJS) libwbh.a libwbh.so html latex

libwbh.a: $(LIBOBJS)
	$(AR) rcs $@ $<

libwbh.so: $(LIBOBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -shared -o $@ $<

wtest: $(TESTOBJS) libwbh.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< ./libwbh.a

html/index.html: wbh.h wbh.c wtest.c Doxyfile
	doxygen

wbh.o: wbh.h
wtest.o: wbh.h
