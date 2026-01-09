dnl
dnl Check whether _Unwind_GetIPInfo is available without doing a link
dnl test so we can use this with libstdc++-v3 and libjava.  Need to
dnl use $target to set defaults because automatic checking is not possible
dnl without a link test (and maybe even with a link test).
dnl

AC_DEFUN([GCC_CHECK_UNWIND_GETIPINFO], [
  AC_ARG_WITH(system-libunwind,
  [  --with-system-libunwind use installed libunwind])
  # If system-libunwind was not specifically set, pick a default setting.
  if test x$with_system_libunwind = x; then
    case ${target} in
      ia64-*-hpux*) with_system_libunwind=yes ;;
      *) with_system_libunwind=no ;;
    esac
  fi
  # Based on system-libunwind and target, do we have ipinfo?
  if  test x$with_system_libunwind = xyes; then
    case ${target} in
      ia64-*-*) have_unwind_getipinfo=no ;;
      *) have_unwind_getipinfo=yes ;;
    esac
  else
    # Darwin before version 9 does not have _Unwind_GetIPInfo.
    changequote(,)
    case ${target} in
      *-*-darwin[3-8]|*-*-darwin[3-8].*) have_unwind_getipinfo=no ;;
      *) have_unwind_getipinfo=yes ;;
    esac
    changequote([,])
  fi

  if test x$have_unwind_getipinfo = xyes; then
    AC_DEFINE(HAVE_GETIPINFO, 1, [Define if _Unwind_GetIPInfo is available.])
  fi
])
