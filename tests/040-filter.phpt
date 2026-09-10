--TEST--
xdebug_set_filter restricts coverage to included path prefixes
--SKIPIF--
<?php if (!extension_loaded('xdebug') || !function_exists('fast_xdebug_engine')) echo 'skip fast-xdebug not loaded'; ?>
--FILE--
<?php
xdebug_set_filter(
    XDEBUG_FILTER_CODE_COVERAGE,
    XDEBUG_PATH_INCLUDE,
    [__DIR__ . '/nonexistent-prefix']
);
xdebug_start_code_coverage(XDEBUG_CC_UNUSED | XDEBUG_CC_DEAD_CODE);
$x = 1 + 1;
$data = xdebug_get_code_coverage();
xdebug_stop_code_coverage();
// This file is not under the included prefix, so it must not appear.
var_dump(isset($data[__FILE__]));
?>
--EXPECT--
bool(false)
