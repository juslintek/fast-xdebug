--TEST--
Branch mode produces the Xdebug functions/branches/paths shape
--SKIPIF--
<?php if (!extension_loaded('xdebug') || !function_exists('fast_xdebug_engine')) echo 'skip fast-xdebug not loaded'; ?>
--FILE--
<?php
function f(int $n): int {
    if ($n > 0) {
        return $n * 2;
    }
    return -$n;
}

xdebug_start_code_coverage(XDEBUG_CC_UNUSED | XDEBUG_CC_DEAD_CODE | XDEBUG_CC_BRANCH_CHECK);
f(5);
$data = xdebug_get_code_coverage();
xdebug_stop_code_coverage();

$file = $data[__FILE__];
var_dump(array_key_exists('lines', $file));
var_dump(array_key_exists('functions', $file));

$fn = $file['functions']['f'];
var_dump(array_key_exists('branches', $fn));
var_dump(array_key_exists('paths', $fn));

// each branch carries the required keys
$branch = reset($fn['branches']);
$keys = array_keys($branch);
sort($keys);
echo implode(',', $keys), "\n";

// each path carries path + hit
$path = reset($fn['paths']);
$pkeys = array_keys($path);
sort($pkeys);
echo implode(',', $pkeys), "\n";

// the taken branch (return n*2) is hit; the fallback (return -n) is not
$hits = array_map(fn($b) => $b['hit'], $fn['branches']);
var_dump(in_array(1, $hits, true));  // at least one branch hit
var_dump(in_array(0, $hits, true));  // at least one branch not hit
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
hit,line_end,line_start,op_end,op_start,out,out_hit
hit,path
bool(true)
bool(true)
