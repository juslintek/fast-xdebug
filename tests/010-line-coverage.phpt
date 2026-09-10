--TEST--
Line coverage: executed lines are >=1, unused executable lines are -1
--SKIPIF--
<?php if (!extension_loaded('xdebug') || !function_exists('fast_xdebug_engine')) echo 'skip fast-xdebug not loaded'; ?>
--FILE--
<?php
function pick(int $n): string {
    if ($n > 0) {
        return 'pos';
    }
    return 'nonpos';
}

xdebug_start_code_coverage(XDEBUG_CC_UNUSED | XDEBUG_CC_DEAD_CODE);
pick(5);
$data = xdebug_get_code_coverage();
xdebug_stop_code_coverage();

$lines = $data[__FILE__];
ksort($lines);
// line 3 (if), 4 (return pos) executed; line 6 (return nonpos) not executed
var_dump($lines[3] >= 1);   // if condition executed
var_dump($lines[4] >= 1);   // taken branch executed
var_dump($lines[6]);        // untaken branch: executable-but-not-executed
?>
--EXPECT--
bool(true)
bool(true)
int(-1)
