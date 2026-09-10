--TEST--
Regression: frameless internal call inside a user function under branch coverage
--SKIPIF--
<?php if (!extension_loaded('xdebug') || !function_exists('fast_xdebug_engine')) echo 'skip fast-xdebug not loaded'; ?>
--FILE--
<?php
function search(string $n, array $h): bool {
    // str_contains / in_array reached via the frameless internal-call path
    if (in_array($n, $h, true)) {
        return true;
    }
    return false;
}

$needle = 'b';
$hay = ['a', 'b', 'c'];
xdebug_start_code_coverage(XDEBUG_CC_UNUSED | XDEBUG_CC_DEAD_CODE | XDEBUG_CC_BRANCH_CHECK);
var_dump(search($needle, $hay));
var_dump(search('z', $hay));
$data = xdebug_get_code_coverage();
xdebug_stop_code_coverage();
echo "survived\n";
var_dump(isset($data[__FILE__]['functions']['search']));
?>
--EXPECT--
bool(true)
bool(false)
survived
bool(true)
