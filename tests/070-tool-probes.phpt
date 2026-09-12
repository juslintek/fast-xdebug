--TEST--
Reproduces the exact runtime probes sebastian/environment + php-code-coverage make
--SKIPIF--
<?php if (!extension_loaded('xdebug') || !function_exists('fast_xdebug_engine')) echo 'skip fast-xdebug not loaded'; ?>
--FILE--
<?php
// --- sebastian/environment Runtime ------------------------------------------
// Runtime::hasXdebug()  ==  extension_loaded('xdebug')
var_dump(extension_loaded('xdebug'));
// Runtime::getXdebugVersion() reads phpversion('xdebug'); Selector needs >= 3.1
var_dump(version_compare(phpversion('xdebug'), '3.1', '>='));

// --- php-code-coverage Driver\XdebugDriver constructor ----------------------
// It checks that xdebug_info('mode') is a list containing 'coverage'.
$mode = xdebug_info('mode');
var_dump(is_array($mode));
var_dump(array_is_list($mode));
var_dump(in_array('coverage', $mode, true));

// XdebugDriver references these constants directly; they must be defined.
var_dump(defined('XDEBUG_CC_UNUSED'));
var_dump(defined('XDEBUG_CC_DEAD_CODE'));
var_dump(defined('XDEBUG_CC_BRANCH_CHECK'));
var_dump(defined('XDEBUG_FILTER_CODE_COVERAGE'));
var_dump(defined('XDEBUG_PATH_INCLUDE'));
var_dump(defined('XDEBUG_PATH_EXCLUDE'));

// The five functions php-code-coverage's XdebugDriver calls.
var_dump(function_exists('xdebug_start_code_coverage'));
var_dump(function_exists('xdebug_stop_code_coverage'));
var_dump(function_exists('xdebug_get_code_coverage'));
var_dump(function_exists('xdebug_code_coverage_started'));
var_dump(function_exists('xdebug_set_filter'));

// symfony/error-handler + PHPUnit call this when extension_loaded('xdebug').
var_dump(function_exists('xdebug_is_debugger_active'));
var_dump(xdebug_is_debugger_active());

// --- XdebugDriver::start()/stop() lifecycle ---------------------------------
// isActive() before start.
var_dump(xdebug_code_coverage_started());
xdebug_start_code_coverage(XDEBUG_CC_UNUSED | XDEBUG_CC_DEAD_CODE);
var_dump(xdebug_code_coverage_started());
xdebug_stop_code_coverage();
var_dump(xdebug_code_coverage_started());

// --- Path coverage shape (Driver\Selector picks the path-coverage branch) ---
// php-code-coverage's XdebugDriver::stop() reads per-file 'functions' whose
// entries carry 'branches' and 'paths' when CC_BRANCH_CHECK was requested.
function branchy(int $n): int {
    if ($n > 0) {
        return $n;
    }
    return -$n;
}
xdebug_start_code_coverage(XDEBUG_CC_UNUSED | XDEBUG_CC_DEAD_CODE | XDEBUG_CC_BRANCH_CHECK);
branchy(3);
$data = xdebug_get_code_coverage();
xdebug_stop_code_coverage();

$file = $data[__FILE__];
var_dump(is_array($file));
var_dump(array_key_exists('lines', $file));
var_dump(array_key_exists('functions', $file));
$fn = $file['functions']['branchy'];
var_dump(array_key_exists('branches', $fn));
var_dump(array_key_exists('paths', $fn));
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
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(false)
bool(false)
bool(true)
bool(false)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
