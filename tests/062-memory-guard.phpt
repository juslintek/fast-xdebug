--TEST--
Memory guard degrades path enumeration under pressure but keeps branches
--SKIPIF--
<?php if (!extension_loaded('xdebug') || !function_exists('fast_xdebug_engine')) echo 'skip fast-xdebug not loaded'; ?>
--INI--
memory_limit=48M
fast_xdebug.memory_guard=1
--FILE--
<?php
function f($n) { if ($n > 0) { return $n * 2; } return -$n; }
xdebug_start_code_coverage(XDEBUG_CC_UNUSED | XDEBUG_CC_DEAD_CODE | XDEBUG_CC_BRANCH_CHECK);
f(5);
// Push memory usage above the 85% pressure threshold before collecting.
$buf = str_repeat('x', (int) (48 * 1024 * 1024 * 0.90) - memory_get_usage());
$d = xdebug_get_code_coverage();
xdebug_stop_code_coverage();
$fn = $d[__FILE__]['functions']['f'];
// Branches are always emitted; paths are dropped under pressure.
var_dump(count($fn['branches']) > 0);
var_dump(count($fn['paths']) === 0);
unset($buf);
?>
--EXPECT--
bool(true)
bool(true)
