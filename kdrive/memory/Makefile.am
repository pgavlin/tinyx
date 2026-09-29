AM_CPPFLAGS = \
	-I$(top_srcdir)/dix \
	@KDRIVE_INCS@ \
	@KDRIVE_CFLAGS@

noinst_LIBRARIES = libmemory.a
noinst_PROGRAMS = Xmemory tinyx-embed-example
check_PROGRAMS = tinyx-api-test
TESTS = $(check_PROGRAMS)

libmemory_a_SOURCES = \
	api.c \
	memory.c \
	memory.h

Xmemory_SOURCES = meminit.c
Xmemory_LDADD = \
	libmemory.a \
	@KDRIVE_LIBS@ \
	$(top_builddir)/dix/libdix.la \
	@XSERVER_LIBS@
Xmemory_DEPENDENCIES = libmemory.a @KDRIVE_LIBS@

tinyx_embed_example_SOURCES = embed-example.c meminit.c
tinyx_embed_example_LDADD = \
	libmemory.a \
	@KDRIVE_LIBS@ \
	$(top_builddir)/dix/libdix.la \
	@XSERVER_LIBS@
tinyx_embed_example_DEPENDENCIES = libmemory.a @KDRIVE_LIBS@

tinyx_api_test_SOURCES = api-test.c meminit.c
tinyx_api_test_LDADD = \
	libmemory.a \
	@KDRIVE_LIBS@ \
	$(top_builddir)/dix/libdix.la \
	@XSERVER_LIBS@
tinyx_api_test_DEPENDENCIES = libmemory.a @KDRIVE_LIBS@
