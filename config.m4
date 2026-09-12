PHP_ARG_ENABLE([swiftcov],
  [whether to enable the swiftcov coverage engine],
  [AS_HELP_STRING([--enable-swiftcov],
    [Enable swiftcov (Xdebug-compatible code coverage)])],
  [no])

dnl Opt-in: tune the build for the CURRENT host CPU (-O3 -march=native).
dnl OFF by default because -march=native produces binaries that only run on
dnl CPUs supporting the build host's instruction set, which is wrong for a
dnl distributed/PECL/PIE binary. Enable only for a build you run yourself,
dnl e.g. a CI runner that is also the test host.
PHP_ARG_ENABLE([swiftcov_native],
  [whether to tune swiftcov for the build host CPU],
  [AS_HELP_STRING([--enable-swiftcov-native],
    [Build with -O3 -march=native (NON-portable; for self-hosted builds only)])],
  [no], [no])

if test "$PHP_SWIFTCOV" != "no"; then
  AC_DEFINE(HAVE_SWIFTCOV, 1, [ Have swiftcov support ])

  dnl PHP core and the Zend headers rely on type-punning that is undefined
  dnl behaviour under strict aliasing; PHP itself is always built with
  dnl -fno-strict-aliasing. Without it, GCC's -O2 miscompiles the coverage
  dnl hot path on some PHP 8.2/8.3 toolchains (observed: coverage silently
  dnl records nothing). Apply it to the WHOLE extension (all source dirs),
  dnl matching PHP's own build contract.
  SWIFTCOV_CFLAGS="-fno-strict-aliasing"

  if test "$PHP_SWIFTCOV_NATIVE" != "no"; then
    dnl Probe that the compiler actually accepts the flags before using them,
    dnl so a toolchain without -march=native support still builds.
    AC_MSG_CHECKING([whether $CC supports -O3 -march=native])
    save_CFLAGS="$CFLAGS"
    CFLAGS="$CFLAGS -O3 -march=native"
    AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[]], [[]])],
      [AC_MSG_RESULT([yes])
       SWIFTCOV_CFLAGS="$SWIFTCOV_CFLAGS -O3 -march=native -mtune=native"],
      [AC_MSG_RESULT([no; falling back to portable flags])])
    CFLAGS="$save_CFLAGS"
  fi

  CFLAGS="$CFLAGS $SWIFTCOV_CFLAGS"

  PHP_NEW_EXTENSION(swiftcov,
    [swiftcov.c src/analysis.c src/coverage.c src/profiler.c src/debugger.c],
    $ext_shared,, [$SWIFTCOV_CFLAGS])
  PHP_ADD_BUILD_DIR([$ext_builddir/src], 1)
fi
