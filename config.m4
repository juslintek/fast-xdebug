PHP_ARG_ENABLE([fast_xdebug],
  [whether to enable fast-xdebug coverage engine],
  [AS_HELP_STRING([--enable-fast-xdebug],
    [Enable fast-xdebug (Xdebug-compatible code coverage)])],
  [no])

if test "$PHP_FAST_XDEBUG" != "no"; then
  AC_DEFINE(HAVE_FAST_XDEBUG, 1, [ Have fast-xdebug support ])

  dnl PHP core and the Zend headers rely on type-punning that is undefined
  dnl behaviour under strict aliasing; PHP itself is always built with
  dnl -fno-strict-aliasing. Without it, GCC's -O2 miscompiles the coverage
  dnl hot path on some PHP 8.2/8.3 toolchains (observed: coverage silently
  dnl records nothing). Apply it to the WHOLE extension (all source dirs),
  dnl matching PHP's own build contract.
  CFLAGS="$CFLAGS -fno-strict-aliasing"

  PHP_NEW_EXTENSION(fast_xdebug,
    [fast_xdebug.c src/analysis.c src/coverage.c src/profiler.c src/debugger.c],
    $ext_shared,, [-fno-strict-aliasing])
  PHP_ADD_BUILD_DIR([$ext_builddir/src], 1)
fi
