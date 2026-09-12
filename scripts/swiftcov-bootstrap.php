<?php
/**
 * swiftcov-bootstrap.php - advisory environment inspector.
 *
 * Prints an actionable php.ini snippet based on swiftcov_recommended_settings(),
 * which inspects the running environment (memory_limit, CPU count, OPcache).
 *
 * This helper is ADVISORY ONLY: it never mutates ini settings, writes files, or
 * changes anything. It prints what it recommends so a human (or a config
 * management step) can apply it deliberately. This mirrors swiftcov's
 * recommended-settings philosophy: no silent mutation.
 *
 * Usage:
 *   php -d extension=swiftcov.so scripts/swiftcov-bootstrap.php
 *   # or, when the extension is enabled globally:
 *   php scripts/swiftcov-bootstrap.php
 *
 * Exit codes:
 *   0  extension present, recommendations printed
 *   1  swiftcov/xdebug engine not loaded (nothing to advise)
 */

if (!extension_loaded('xdebug')) {
    fwrite(STDERR, "swiftcov (module 'xdebug') is not loaded.\n");
    fwrite(STDERR, "Load it first, e.g. `php -d extension=swiftcov.so " . basename(__FILE__) . "`.\n");
    exit(1);
}

if (!function_exists('swiftcov_recommended_settings')) {
    fwrite(STDERR, "The 'xdebug' module is loaded but is not swiftcov "
        . "(swiftcov_recommended_settings() is missing).\n");
    fwrite(STDERR, "This helper only advises for swiftcov.\n");
    exit(1);
}

$engine = function_exists('swiftcov_engine') ? swiftcov_engine() : 'unknown';
$info    = swiftcov_recommended_settings();

$recommended = $info['recommended'] ?? [];
$notes       = $info['notes'] ?? [];

echo "# swiftcov bootstrap - advisory only (nothing was changed)\n";
echo "# engine:        {$engine}\n";
echo '# resolved_mode: ' . ($info['resolved_mode'] ?? 'n/a') . "\n";

$memBytes = $info['memory_limit_bytes'] ?? null;
if ($memBytes !== null) {
    $memHuman = ($memBytes < 0) ? 'unlimited' : number_format($memBytes / (1024 * 1024), 0) . 'M';
    echo "# memory_limit:  {$memHuman}\n";
}
if (isset($info['cpu_count'])) {
    echo '# cpu_count:     ' . $info['cpu_count'] . "\n";
}
if (isset($info['opcache'])) {
    echo '# opcache:       ' . ($info['opcache'] ? 'on' : 'off') . "\n";
}

echo "\n# --- recommended php.ini snippet -------------------------------------\n";
echo "; swiftcov keeps the xdebug.* namespace for drop-in compatibility.\n";
echo "xdebug.mode = coverage\n";

foreach ($recommended as $key => $value) {
    // Ini-shaped keys (contain a dot) are emitted as ini lines; everything else
    // is emitted as an advisory comment so nothing invalid ends up in a config.
    if (is_string($key) && strpos($key, '.') !== false) {
        if (is_bool($value)) {
            $value = $value ? '1' : '0';
        }
        echo "{$key} = {$value}\n";
    } else {
        echo "; {$key}: " . (is_bool($value) ? ($value ? 'true' : 'false') : $value) . "\n";
    }
}

if (!empty($notes)) {
    echo "\n# --- notes -----------------------------------------------------------\n";
    foreach ($notes as $note) {
        echo "; - {$note}\n";
    }
}

echo "\n# Apply the lines above to a php.ini / conf.d drop-in, then run PHPUnit\n";
echo "# with --path-coverage. See docs/adoption-roadmap.md for rollout guidance.\n";

exit(0);
