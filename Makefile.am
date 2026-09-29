AUTOMAKE_OPTIONS = subdir-objects

ACLOCAL_AMFLAGS=-I m4

EXTRA_DIST = \
	fonts/README.md \
	fonts/generate-builtin-fonts.py

if DBE
DBE_DIR=dbe
endif

SUBDIRS = \
	include \
	dix  \
	fb \
	mi \
	Xext \
	miext \
	os \
	randr \
	render  \
	$(DBE_DIR) \
	xfixes \
	damageext \
	kdrive

DIST_SUBDIRS = \
	include \
	dix  \
	fb \
	mi \
	Xext \
	miext \
	os \
	randr \
	render  \
	dbe \
	xfixes \
	damageext \
	kdrive
