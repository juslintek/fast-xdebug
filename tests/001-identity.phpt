--TEST--
Registers as "xdebug", satisfies php-code-coverage's probes
--SKIPIF--
<?php if (!extension_loaded('xdebug') || !function_exists('fast_xdebug_engine')) echo 'skip fast-xdebug not loaded'; ?>
--FILE--
<?php
var_dump(extension_loaded('xdebug'));
var_dump(version_compare(phpversion('xdebug'), '3.1', '>='));
var_dump(in_array('coverage', xdebug_info('mode'), true));
var_dump(function_exists('xdebug_start_code_coverage'));
var_dump(function_exists('xdebug_get_code_coverage'));
var_dump(function_exists('xdebug_stop_code_coverage'));
var_dump(function_exists('xdebug_code_coverage_started'));
var_dump(function_exists('xdebug_set_filter'));
echo fast_xdebug_engine(), "\n";
?>
--EXPECTF--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
swiftcov %s
