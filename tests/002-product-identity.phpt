--TEST--
Product identity is swiftcov while runtime still registers as "xdebug"
--SKIPIF--
<?php if (!extension_loaded('xdebug') || !function_exists('swiftcov_engine')) echo 'skip swiftcov not loaded'; ?>
--FILE--
<?php
// Runtime identity is still "xdebug" so php-code-coverage/PHPUnit detect it.
var_dump(extension_loaded('xdebug'));
var_dump(version_compare(phpversion('xdebug'), '3.1', '>='));
var_dump(in_array('coverage', xdebug_info('mode'), true));

// Canonical product sentinels are swiftcov-branded.
var_dump(str_starts_with(swiftcov_engine(), 'swiftcov'));
var_dump(is_string(swiftcov_resolved_mode()));
var_dump(is_array(swiftcov_recommended_settings()));

// The fast_xdebug_* helpers remain as working BC aliases and are also
// swiftcov-branded now.
var_dump(function_exists('fast_xdebug_engine'));
var_dump(str_starts_with(fast_xdebug_engine(), 'swiftcov'));
var_dump(swiftcov_engine() === fast_xdebug_engine());

// xdebug_info() self-describes the product engine.
$info = xdebug_info();
var_dump(str_contains($info['engine'], 'swiftcov'));
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
