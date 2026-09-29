AM_CPPFLAGS = \
	@KDRIVE_INCS@ \
	@KDRIVE_CFLAGS@

noinst_LIBRARIES = libmemory.a
noinst_PROGRAMS = Xmemory

libmemory_a_SOURCES = \
	memory.c \
	memory.h

Xmemory_SOURCES = meminit.c
Xmemory_LDADD = \
	libmemory.a \
	@KDRIVE_LIBS@ \
	$(top_builddir)/dix/libdix.la \
	@XSERVER_LIBS@
Xmemory_DEPENDENCIES = libmemory.a @KDRIVE_LIBS@
