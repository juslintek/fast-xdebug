--TEST--
fast_xdebug_recommended_settings scales the path cap with memory_limit
--SKIPIF--
<?php if (!extension_loaded('xdebug') || !function_exists('fast_xdebug_recommended_settings')) echo 'skip fast-xdebug not loaded'; ?>
--INI--
memory_limit=64M
--FILE--
<?php
$r = fast_xdebug_recommended_settings();
var_dump(is_array($r));
var_dump($r['memory_limit_bytes'] === 64 * 1024 * 1024);
var_dump($r['cpu_count'] >= 1);
// 64M is "low": path cap must be clamped to 256
var_dump($r['recommended']['fast_xdebug.max_paths'] === 256);
var_dump(is_array($r['notes']));
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
