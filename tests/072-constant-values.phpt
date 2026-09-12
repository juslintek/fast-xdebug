--TEST--
XDEBUG_CC_* / XDEBUG_FILTER_* / XDEBUG_PATH_* have Xdebug's exact numeric values
--SKIPIF--
<?php if (!extension_loaded('xdebug') || !function_exists('fast_xdebug_engine')) echo 'skip fast-xdebug not loaded'; ?>
--DESCRIPTION--
php-code-coverage's XdebugDriver passes XDEBUG_CC_UNUSED|XDEBUG_CC_DEAD_CODE
(and, for path coverage, |XDEBUG_CC_BRANCH_CHECK) as the bitmask to
xdebug_start_code_coverage(), and passes XDEBUG_FILTER_CODE_COVERAGE +
XDEBUG_PATH_INCLUDE/XDEBUG_PATH_EXCLUDE to xdebug_set_filter(). If any of these
numeric values diverges from Xdebug's, the wrong coverage flags/filter mode are
selected silently. Pin them so a future change can't drift.
--FILE--
<?php
// Values taken from Xdebug's own headers (src/coverage/code_coverage.h,
// src/lib/filter.h). These MUST match exactly.
var_dump(XDEBUG_CC_UNUSED === 1);
var_dump(XDEBUG_CC_DEAD_CODE === 2);
var_dump(XDEBUG_CC_BRANCH_CHECK === 4);

var_dump(XDEBUG_FILTER_CODE_COVERAGE === 0);
var_dump(XDEBUG_PATH_INCLUDE === 1);
var_dump(XDEBUG_PATH_EXCLUDE === 0);

// The bitmask php-code-coverage actually builds for path coverage.
var_dump((XDEBUG_CC_UNUSED | XDEBUG_CC_DEAD_CODE | XDEBUG_CC_BRANCH_CHECK) === 7);
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
