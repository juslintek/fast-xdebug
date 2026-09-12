--TEST--
Profiler writes a valid Cachegrind file with call edges and a summary
--SKIPIF--
<?php if (!extension_loaded('xdebug') || !function_exists('fast_xdebug_engine')) echo 'skip fast-xdebug not loaded'; ?>
--FILE--
<?php
function inner(int $n): int { $s = 0; for ($i = 0; $i < $n; $i++) { $s += $i; } return $s; }
function outer(int $n): int { $t = 0; for ($i = 0; $i < $n; $i++) { $t += inner(50); } return $t; }

$out = sys_get_temp_dir() . '/fxd-prof-' . getmypid() . '.out';
$name = xdebug_start_trace($out);
outer(100);
xdebug_stop_trace();

$data = file_get_contents($out);
@unlink($out);

var_dump(str_contains($data, 'creator: swiftcov'));
var_dump(str_contains($data, 'events: Time_(10ns) Memory_(bytes)'));
var_dump(str_contains($data, 'fn=inner'));
var_dump(str_contains($data, 'fn=outer'));
var_dump(str_contains($data, 'cfn=inner'));       // outer calls inner
var_dump(str_contains($data, 'calls=100 0 0'));   // 100 calls to inner
var_dump((bool) preg_match('/^summary: \d+ -?\d+$/m', $data));
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
