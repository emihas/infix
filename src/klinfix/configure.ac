AC_PREREQ(2.61)
AC_INIT([klinfix], [1.0.0],
	[https://github.com/kernelkit/infix/issues])
AM_INIT_AUTOMAKE(1.11 foreign subdir-objects)
AM_SILENT_RULES(yes)

LT_INIT

AC_CONFIG_FILES([
	Makefile
	src/Makefile
	xml/Makefile
])

AC_PROG_CC
AC_PROG_INSTALL

# Check for pkg-config first, warn if it's not installed
PKG_PROG_PKG_CONFIG

PKG_CHECK_MODULES([sysrepo], [sysrepo >= 2.2.60])
AC_CHECK_LIB([klish], [kplugin_new],, AC_MSG_ERROR([Klish not found]))

test "x$prefix" = xNONE && prefix=$ac_default_prefix
test "x$exec_prefix" = xNONE && exec_prefix='${prefix}'

KLISHLIBDIR=`eval echo $libdir/klish`
AC_SUBST(KLISHLIBDIR)

KLISHCONFDIR=`eval echo $sysconfdir/klish`
AC_SUBST(KLISHCONFDIR)

AC_OUTPUT
