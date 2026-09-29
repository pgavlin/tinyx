if KDRIVEVESA
VESA_SUBDIRS = vesa
endif

if KDRIVEFBDEV
FBDEV_SUBDIRS = fbdev
endif

if KDRIVELINUX
LINUX_SUBDIRS = linux
endif

SUBDIRS =			\
	src			\
	$(LINUX_SUBDIRS)	\
	memory			\
	$(FBDEV_SUBDIRS)	\
	$(VESA_SUBDIRS)

DIST_SUBDIRS = vesa fbdev memory src linux
