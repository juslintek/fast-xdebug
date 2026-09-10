--TEST--
Regression: branch coverage across a PHP 8.4+ frameless internal call (in_array)
--SKIPIF--
<?php if (!extension_loaded('xdebug') || !function_exists('fast_xdebug_engine')) echo 'skip fast-xdebug not loaded'; ?>
--FILE--
<?php
// Arguments MUST be variables: with literals the call is constant-folded and
// ZEND_JMP_FRAMELESS is never emitted, so the test would pass for the wrong
// reason. This is the exact shape that segfaulted Xdebug 3.5.0-3.5.3 when the
// coverage filter read past a zend_internal_function via the frameless path.
$needle = 'x';
$haystack = ['a', 'b'];
xdebug_start_code_coverage(XDEBUG_CC_BRANCH_CHECK);
in_array($needle, $haystack);
$data = xdebug_get_code_coverage();
xdebug_stop_code_coverage();
echo "survived\n";
var_dump(is_array($data));
?>
--EXPECT--
survived
bool(true)
