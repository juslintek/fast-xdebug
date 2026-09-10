--TEST--
Line coverage records the in-flight {main} frame up to the collection point
--SKIPIF--
<?php if (!extension_loaded('xdebug') || !function_exists('fast_xdebug_engine')) echo 'skip fast-xdebug not loaded'; ?>
--FILE--
<?php
xdebug_start_code_coverage(XDEBUG_CC_UNUSED | XDEBUG_CC_DEAD_CODE);
$a = 1;
$b = 2;
$c = $a + $b;
$data = xdebug_get_code_coverage();
xdebug_stop_code_coverage();
$lines = $data[__FILE__];
// lines 3,4,5 ran after start and before collection
var_dump($lines[3] >= 1);
var_dump($lines[4] >= 1);
var_dump($lines[5] >= 1);
// line 2 (the start() call) ran BEFORE the floor, so it must NOT be marked
// executed (>=1). It may be reported as -1 (executable, unused) like pcov does.
var_dump(!isset($lines[2]) || $lines[2] < 1);
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
