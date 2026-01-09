dnl Fix Autoconf bugs by overriding broken internal Autoconf
dnl macros with backports of fixes from newer releases.
dnl
dnl The override bits of this file should be a no-op for the newest
dnl Autoconf version, which means they can be removed once the complete
dnl tree has moved to a new enough Autoconf version.
dnl
dnl The _GCC_AUTOCONF_VERSION_TEST ensures that exactly the desired
dnl Autoconf version is used.  It should be kept for consistency.

dnl Use ifdef/ifelse over m4_ifdef/m4_ifelse to be clean for 2.13.
ifdef([m4_PACKAGE_VERSION], [

dnl Provide m4_copy_force and m4_rename_force for old Autoconf versions.

m4_ifndef([m4_copy_force],
[m4_define([m4_copy_force],
[m4_ifdef([$2], [m4_undefine([$2])])m4_copy($@)])])

m4_ifndef([m4_rename_force],
[m4_define([m4_rename_force],
[m4_ifdef([$2], [m4_undefine([$2])])m4_rename($@)])])

dnl AC_DEFUN a commonly used macro so this file is picked up.
m4_copy([AC_PREREQ], [_AC_PREREQ])
AC_DEFUN([AC_PREREQ], [frob])
m4_copy_force([_AC_PREREQ], [AC_PREREQ])


dnl Ensure exactly this Autoconf version is used
m4_ifndef([_GCC_AUTOCONF_VERSION],
  [m4_define([_GCC_AUTOCONF_VERSION], [2.69])])

dnl Test for the exact version when AC_INIT is expanded.
dnl This allows to update the tree in steps (for testing)
dnl by putting
dnl   m4_define([_GCC_AUTOCONF_VERSION], [X.Y])
dnl in configure.ac before AC_INIT,
dnl without rewriting this file.
dnl Or for updating the whole tree at once with the definition above.
AC_DEFUN([_GCC_AUTOCONF_VERSION_CHECK],
[m4_if(m4_defn([_GCC_AUTOCONF_VERSION]),
  m4_defn([m4_PACKAGE_VERSION]), [],
  [m4_fatal([Please use exactly Autoconf ]_GCC_AUTOCONF_VERSION[ instead of ]m4_defn([m4_PACKAGE_VERSION])[.])])
])
dnl don't do this for libbacktrace
dnl m4_define([AC_INIT], m4_defn([AC_INIT])[
dnl _GCC_AUTOCONF_VERSION_CHECK
dnl ])


dnl Ensure we do not use a buggy M4.
m4_if(m4_index([..wi.d.], [.d.]), [-1],
  [m4_fatal(m4_do([m4 with buggy strstr detected.  Please install
GNU M4 1.4.16 or newer and set the M4 environment variable]))])


dnl Fix 2.64 cross compile detection for AVR and RTEMS
dnl by not trying to compile fopen.
m4_if(m4_defn([m4_PACKAGE_VERSION]), [2.64],
  [m4_foreach([_GCC_LANG], [C, C++, Fortran, Fortran 77],
     [m4_define([_AC_LANG_IO_PROGRAM(]_GCC_LANG[)], m4_defn([AC_LANG_PROGRAM(]_GCC_LANG[)]))])])

m4_version_prereq([2.66],, [
dnl We need AC_CHECK_DECL which works for overloaded C++ functions.

# _AC_CHECK_DECL_BODY
# -------------------
# Shell function body for AC_CHECK_DECL.
m4_define([_AC_CHECK_DECL_BODY],
[  AS_LINENO_PUSH([$[]1])
  [as_decl_name=`echo $][2|sed 's/ *(.*//'`]
  [as_decl_use=`echo $][2|sed -e 's/(/((/' -e 's/)/) 0&/' -e 's/,/) 0& (/g'`]
  AC_CACHE_CHECK([whether $as_decl_name is declared], [$[]3],
  [AC_COMPILE_IFELSE([AC_LANG_PROGRAM([$[]4],
[@%:@ifndef $[]as_decl_name
@%:@ifdef __cplusplus
  (void) $[]as_decl_use;
@%:@else
  (void) $[]as_decl_name;
@%:@endif
@%:@endif
])],
		   [AS_VAR_SET([$[]3], [yes])],
		   [AS_VAR_SET([$[]3], [no])])])
  AS_LINENO_POP
])# _AC_CHECK_DECL_BODY

# _AC_CHECK_DECLS(SYMBOL, ACTION-IF_FOUND, ACTION-IF-NOT-FOUND,
#                 INCLUDES)
# -------------------------------------------------------------
# Helper to AC_CHECK_DECLS, which generates the check for a single
# SYMBOL with INCLUDES, performs the AC_DEFINE, then expands
# ACTION-IF-FOUND or ACTION-IF-NOT-FOUND.
m4_define([_AC_CHECK_DECLS],
[AC_CHECK_DECL([$1], [ac_have_decl=1], [ac_have_decl=0], [$4])]dnl
[AC_DEFINE_UNQUOTED(AS_TR_CPP(m4_bpatsubst(HAVE_DECL_[$1],[ *(.*])),
  [$ac_have_decl],
  [Define to 1 if you have the declaration of `$1',
   and to 0 if you don't.])]dnl
[m4_ifvaln([$2$3], [AS_IF([test $ac_have_decl = 1], [$2], [$3])])])

])

dnl If flex/lex are not found, the top level configure sets LEX to
dnl "/path_to/missing flex".  When AC_PROG_LEX tries to find the flex
dnl output file, it calls $LEX to do so, but the current lightweight
dnl "missing" won't create a file.  This results in an error.
dnl Avoid calling the bulk of AC_PROG_LEX when $LEX is "missing".
AC_DEFUN_ONCE([AC_PROG_LEX],
[AC_CHECK_PROGS(LEX, flex lex, :)
case "$LEX" in
  :|*"missing "*) ;;
  *) _AC_PROG_LEX_YYTEXT_DECL ;;
esac])

])
