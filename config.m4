PHP_ARG_ENABLE([fast_xdebug],
  [whether to enable fast-xdebug coverage engine],
  [AS_HELP_STRING([--enable-fast-xdebug],
    [Enable fast-xdebug (Xdebug-compatible code coverage)])],
  [no])

dnl Opt-in: tune the build for the CURRENT host CPU (-O3 -march=native).
dnl OFF by default because -march=native produces binaries that only run on
dnl CPUs supporting the build host's instruction set, which is wrong for a
dnl distributed/PECL/PIE binary. Enable only for a build you run yourself,
dnl e.g. a CI runner that is also the test host.
PHP_ARG_ENABLE([fast_xdebug_native],
  [whether to tune fast-xdebug for the build host CPU],
  [AS_HELP_STRING([--enable-fast-xdebug-native],
    [Build with -O3 -march=native (NON-portable; for self-hosted builds only)])],
  [no], [no])

if test "$PHP_FAST_XDEBUG" != "no"; then
  AC_DEFINE(HAVE_FAST_XDEBUG, 1, [ Have fast-xdebug support ])

  dnl PHP core and the Zend headers rely on type-punning that is undefined
  dnl behaviour under strict aliasing; PHP itself is always built with
  dnl -fno-strict-aliasing. Without it, GCC's -O2 miscompiles the coverage
  dnl hot path on some PHP 8.2/8.3 toolchains (observed: coverage silently
  dnl records nothing). Apply it to the WHOLE extension (all source dirs),
  dnl matching PHP's own build contract.
  FAST_XDEBUG_CFLAGS="-fno-strict-aliasing"

  if test "$PHP_FAST_XDEBUG_NATIVE" != "no"; then
    dnl Probe that the compiler actually accepts the flags before using them,
    dnl so a toolchain without -march=native support still builds.
    AC_MSG_CHECKING([whether $CC supports -O3 -march=native])
    save_CFLAGS="$CFLAGS"
    CFLAGS="$CFLAGS -O3 -march=native"
    AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[]], [[]])],
      [AC_MSG_RESULT([yes])
       FAST_XDEBUG_CFLAGS="$FAST_XDEBUG_CFLAGS -O3 -march=native -mtune=native"],
      [AC_MSG_RESULT([no; falling back to portable flags])])
    CFLAGS="$save_CFLAGS"
  fi

  CFLAGS="$CFLAGS $FAST_XDEBUG_CFLAGS"

  PHP_NEW_EXTENSION(fast_xdebug,
    [fast_xdebug.c src/analysis.c src/coverage.c src/profiler.c src/debugger.c],
    $ext_shared,, [$FAST_XDEBUG_CFLAGS])
  PHP_ADD_BUILD_DIR([$ext_builddir/src], 1)
fi
