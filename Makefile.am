# List extras to package
EXTRA_DIST = \
        autogen.sh \
        debian/postinst \
        debian/copyright \
        debian/control \
        debian/rules \
		debian/changelog \
		debian/postinst \
		src/*

# Sub directories
SUBDIRS = 

# Includes
INCLUDES = $(DEPS_CFLAGS) 

# Binary
bin_PROGRAMS = \
        hald-addon-bme
# /Binary

# Sources
hald_addon_bme_SOURCES = \
        src/hald-addon-bme.c
# /Sources

# LDADD
hald_addon_bme_LDADD = \
        $(DEPS_LIBS) $(HILDON_LIBS) $(OSSO_LIBS)
# /LDADD

deb: dist
	-mkdir $(top_builddir)/debian-build
	cd $(top_builddir)/debian-build && tar zxf ../$(top_builddir)/$(PACKAGE)-$(VERSION).tar.gz
	cd $(top_builddir)/debian-build/$(PACKAGE)-$(VERSION) && dpkg-buildpackage -rfakeroot
	rm -rf $(top_builddir)/debian-build/$(PACKAGE)-$(VERSION)
