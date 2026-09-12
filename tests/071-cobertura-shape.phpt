--TEST--
Branch data has the fields php-code-coverage's Cobertura writer consumes
--SKIPIF--
<?php if (!extension_loaded('xdebug') || !function_exists('fast_xdebug_engine')) echo 'skip fast-xdebug not loaded'; ?>
--DESCRIPTION--
php-code-coverage builds RawCodeCoverageData::fromXdebugWithPathCoverage() from
xdebug_get_code_coverage(...|XDEBUG_CC_BRANCH_CHECK), then its Cobertura report
writer reads, per function, the 'branches' (op_start/op_end/line_start/line_end/
hit/out/out_hit) and 'paths' (path/hit) arrays to populate the
branches-valid / branches-covered attributes GitLab and Cobertura consumers use.
This exercises real branching (if/else, foreach, match) at the data level, since
PHPUnit itself can't run offline.
--FILE--
<?php
function classify(int $n): string {
    $out = '';
    foreach (range(1, $n) as $i) {   // foreach loop
        if ($i % 2 === 0) {          // if/else
            $out .= 'e';
        } else {
            $out .= 'o';
        }
    }
    return match (true) {            // match
        $n > 5 => 'big',
        $n > 0 => 'small',
        default => 'none',
    };
}

xdebug_start_code_coverage(XDEBUG_CC_UNUSED | XDEBUG_CC_DEAD_CODE | XDEBUG_CC_BRANCH_CHECK);
classify(4);
$data = xdebug_get_code_coverage();
xdebug_stop_code_coverage();

$file = $data[__FILE__];
$fn = $file['functions']['classify'];

// Every branch entry must carry exactly the keys the Cobertura writer reads.
$expected = ['hit', 'line_end', 'line_start', 'op_end', 'op_start', 'out', 'out_hit'];
$all_branches_ok = true;
foreach ($fn['branches'] as $b) {
    $k = array_keys($b);
    sort($k);
    if ($k !== $expected) { $all_branches_ok = false; break; }
    // out and out_hit are parallel lists of ints
    if (!array_is_list($b['out']) || !array_is_list($b['out_hit'])) { $all_branches_ok = false; break; }
    if (count($b['out']) !== count($b['out_hit'])) { $all_branches_ok = false; break; }
    if (!is_int($b['op_start']) || !is_int($b['op_end'])) { $all_branches_ok = false; break; }
    if (!is_int($b['line_start']) || !is_int($b['line_end'])) { $all_branches_ok = false; break; }
    if (!in_array($b['hit'], [0, 1], true)) { $all_branches_ok = false; break; }
}
var_dump($all_branches_ok);

// Every path entry carries a 'path' (list of block start ops) and a 'hit' flag.
$all_paths_ok = true;
foreach ($fn['paths'] as $p) {
    $k = array_keys($p);
    sort($k);
    if ($k !== ['hit', 'path']) { $all_paths_ok = false; break; }
    if (!array_is_list($p['path'])) { $all_paths_ok = false; break; }
    if (!in_array($p['hit'], [0, 1], true)) { $all_paths_ok = false; break; }
}
var_dump($all_paths_ok);

// Reproduce the Cobertura writer's branches-valid / branches-covered tally.
$branchesValid = 0;
$branchesCovered = 0;
foreach ($fn['branches'] as $b) {
    foreach ($b['out_hit'] as $hit) {
        $branchesValid++;
        if ($hit) {
            $branchesCovered++;
        }
    }
}
var_dump($branchesValid > 0);
var_dump($branchesCovered > 0);
var_dump($branchesCovered <= $branchesValid);
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
