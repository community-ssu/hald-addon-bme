dnl Process this file with autoconf to produce a configure script.

AC_INIT(haldaddonbme,0.1)
AM_CONFIG_HEADER(config.h)

AM_INIT_AUTOMAKE([1.7.9])

AC_PROG_CC
AC_STDC_HEADERS
AC_PROG_INSTALL
dnl AC_PREFIX_DEFAULT(/var/lib/install/usr)
AC_PREFIX_DEFAULT(/usr)

CFLAGS="$CFLAGS -Wall -ansi -pedantic -Wmissing-prototypes -Wmissing-declarations"

PKG_CHECK_MODULES(DEPS, gtk+-2.0 >= 2.2 glib-2.0 >= 2.2 gnome-vfs-2.0 >= 2.2  hal)
AC_SUBST(DEPS_CFLAGS)
AC_SUBST(DEPS_LIBS)

USE_NLS=yes
AC_SUBST(USE_NLS)


AC_CONFIG_FILES([
    Makefile 
    src/Makefile
])

AC_OUTPUT
