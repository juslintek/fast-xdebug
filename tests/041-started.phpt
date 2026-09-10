--TEST--
xdebug_code_coverage_started reflects start/stop state
--SKIPIF--
<?php if (!extension_loaded('xdebug') || !function_exists('fast_xdebug_engine')) echo 'skip fast-xdebug not loaded'; ?>
--FILE--
<?php
var_dump(xdebug_code_coverage_started());
xdebug_start_code_coverage(XDEBUG_CC_UNUSED);
var_dump(xdebug_code_coverage_started());
xdebug_stop_code_coverage();
var_dump(xdebug_code_coverage_started());
?>
--EXPECT--
bool(false)
bool(true)
bool(false)
