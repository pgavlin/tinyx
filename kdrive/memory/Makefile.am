AM_CPPFLAGS = \
	-I$(top_srcdir)/dix \
	@KDRIVE_INCS@ \
	@KDRIVE_CFLAGS@

noinst_LIBRARIES = libmemory.a
noinst_PROGRAMS = Xmemory tinyx-embed-example
check_PROGRAMS = tinyx-api-test tinyx-font-pattern-test tinyx-retained-client-test
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

tinyx_font_pattern_test_SOURCES = font-pattern-test.c meminit.c
tinyx_font_pattern_test_LDADD = \
	libmemory.a \
	@KDRIVE_LIBS@ \
	$(top_builddir)/dix/libdix.la \
	@XSERVER_LIBS@
tinyx_font_pattern_test_DEPENDENCIES = libmemory.a @KDRIVE_LIBS@

tinyx_retained_client_test_SOURCES = retained-client-test.c meminit.c
tinyx_retained_client_test_LDADD = \
	libmemory.a \
	@KDRIVE_LIBS@ \
	$(top_builddir)/dix/libdix.la \
	@XSERVER_LIBS@
tinyx_retained_client_test_DEPENDENCIES = libmemory.a @KDRIVE_LIBS@
