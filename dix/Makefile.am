noinst_LTLIBRARIES = libdix.la

AM_CFLAGS = $(DIX_CFLAGS) \
	-DVENDOR_STRING=\""@VENDOR_STRING@"\" \
	-DVENDOR_RELEASE="@VENDOR_RELEASE@"

if NO_XFONT
FONT_BACKEND_SRCS = \
	fontstubs.c \
	embedded-font.c \
	embedded-font-data.c \
	embedded-font.h

check_PROGRAMS = embedded-font-data-test
TESTS = $(check_PROGRAMS)
embedded_font_data_test_SOURCES = \
	tests/embedded-font-data-test.c \
	embedded-font.h
embedded_font_data_test_LDADD = libdix.la
endif

libdix_la_SOURCES = 	\
	atom.c		\
	colormap.c	\
	cursor.c	\
	devices.c	\
	dispatch.c	\
	dispatch.h	\
	dixfonts.c	\
	$(FONT_BACKEND_SRCS) \
	dixutils.c	\
	events.c	\
	extension.c	\
	ffs.c		\
	gc.c		\
	globals.c	\
	glyphcurs.c	\
	grabs.c		\
	initatoms.c	\
	lifecycle.c	\
	lifecycle.h	\
	main.c		\
	pixmap.c	\
	privates.c	\
	property.c	\
	resource.c	\
	swaprep.c	\
	swapreq.c	\
	tables.c	\
	window.c	\
	strcasecmp.c
