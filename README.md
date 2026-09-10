# fast-xdebug

**Xdebug-API-compatible PHP code coverage with a fast, block-based backend.**
It makes **branch and path coverage nearly as cheap as line coverage**, closing
the ~20× gap that makes `phpunit --path-coverage` painful today — while still
being accepted, unchanged, by PHPUnit / `php-code-coverage` / GitLab / Cobertura
tooling.

> Status: working proof of concept.
> **Coverage** (line/branch/path) and **profiling** (Cachegrind) are implemented
> and measured faster than Xdebug. **Step debugging (DBGp) is not implemented** —
> it is specified in [`docs/debugger-spec.md`](docs/debugger-spec.md) and
> deliberately left as a documented no-op rather than a half-working session that
> would hang an IDE (see [Step debugging](#step-debugging)).
> Line coverage is byte-identical to pcov and Xdebug; branch/path output is in
> Xdebug's exact shape (see [Accuracy](#accuracy)).

## Measured numbers first

Representative CPU-bound workload (`bench/workload.php`: memoised recursion,
branch ladders, `in_array`/loop bodies, 4000 iterations). PHP 8.4.24 NTS,
x86_64, gcc 11.5, best of 5, coverage timed around the workload only.
Reproduce with `bench/` + the `.so`s you build.

| Engine | Line coverage | Branch + path coverage |
|---|---|---|
| no coverage | 0.0135 s (1.0×) | — |
| **pcov** | 0.0665 s (4.9×) | ❌ not supported¹ |
| **Xdebug** | 0.2528 s (18.7×) | 1.5541 s (**115×**) |
| **fast-xdebug** | **0.1001 s (7.4×)** | **0.1013 s (7.5×)** |

- fast-xdebug line coverage is **2.5× faster than Xdebug**, ~1.5× the cost of pcov.
- fast-xdebug branch+path coverage is **15× faster than Xdebug**, and costs
  essentially the same as its own line coverage.

¹ pcov is line-only by design. Under `phpunit --path-coverage`,
`SebastianBergmann\CodeCoverage\Driver\Selector` throws
`NoCodeCoverageDriverWithPathCoverageSupportAvailableException` unless the driver
is Xdebug. **That is the gap fast-xdebug fills.**

> The original brief targeted a large private suite (loyalty-hub, 741 tests) that
> is not present in this environment, so the numbers above use a self-contained
> workload. The *ratios* are the transferable result and match the brief's
> qualitative expectations (Xdebug branch coverage ≈ 20× pcov line coverage).

## Why it is faster

Xdebug and pcov both do work **per executed opcode** at runtime (Xdebug installs
an opcode handler on ~every opcode and, in branch mode, runs
`xdebug_branch_info_mark_reached` per branch op; pcov does a nested-hash
`file→line` probe per opline).

fast-xdebug:

1. **Analyses each `op_array` once** into basic blocks (jump semantics mirror
   Xdebug's, so branch topology matches — including the PHP 8.4+ frameless
   internal-call opcode).
2. At runtime does **one byte store per executed block** — no hashing, no
   per-line work.
3. **Reconstructs lines, branches and paths only at collection time**, which is
   why branch/path coverage costs the same at runtime as line coverage.

Full rationale, C4 diagrams, and the measured baseline are in
[`docs/design.md`](docs/design.md) and [`docs/decisions/`](docs/decisions).

## Install

### With PIE (recommended)

```sh
# from a checkout of this repo
pie install juslintek/fast-xdebug
```

The `composer.json` declares `"type": "php-ext"` with a `php-ext` block so PIE
can build and enable it. PIE builds C extensions from source against your PHP.

> PECL is deprecated in favour of PIE. If you want to try a **pre-release**
> build, install from a git checkout (below); PIE's support for VCS/`dev-<branch>`
> constraints on `php-ext` packages is evolving — check `pie install --help` for
> your PIE version before relying on it.

### From source

Requires PHP 8.2–8.5 dev headers (`phpize`, `php-config`) and a C toolchain.

```sh
phpize
./configure --enable-fast-xdebug --with-php-config="$(command -v php-config)"
make -j"$(nproc)"
```

Then load it as a **normal module** (not a Zend extension):

```ini
; php.ini
extension=fast_xdebug.so
```

It registers under the name `xdebug` (see [Compatibility](#compatibility)), so
PHPUnit will pick it up as the coverage driver automatically. Nothing else to
configure; `xdebug.mode` defaults to `coverage`.

## Usage

Just run PHPUnit as usual — path coverage now works and is fast:

```sh
php -d extension=fast_xdebug.so vendor/bin/phpunit \
    --path-coverage --coverage-cobertura=cobertura.xml
```

The produced Cobertura report has `branches-covered` / `branches-valid`
populated, so an existing GitLab coverage pipeline consumes it with **no change**.

Or drive the Xdebug coverage API directly:

```php
xdebug_start_code_coverage(XDEBUG_CC_UNUSED | XDEBUG_CC_DEAD_CODE | XDEBUG_CC_BRANCH_CHECK);
run_the_code();
$data = xdebug_get_code_coverage();   // Xdebug-shaped: lines + functions{branches,paths}
xdebug_stop_code_coverage();
```

Supported API surface: `xdebug_start_code_coverage`, `xdebug_stop_code_coverage`,
`xdebug_get_code_coverage`, `xdebug_code_coverage_started`, `xdebug_set_filter`,
`xdebug_info`, plus the `XDEBUG_CC_*` / `XDEBUG_FILTER_*` / `XDEBUG_PATH_*`
constants. A sentinel `fast_xdebug_engine()` lets you confirm which engine is
loaded.

## Profiling

fast-xdebug ships a Cachegrind profiler on the same "cheap hook" thesis: it uses
one `zend_observer` enter/exit per **call** (not per opcode), builds a call tree
with self/inclusive time and memory, and writes a Cachegrind file that
KCachegrind, qcachegrind, PhpStorm and Blackfire's importer read.

```sh
# auto-start (writes cachegrind.out.<pid> to xdebug.output_dir)
php -d extension=fast_xdebug.so -d xdebug.mode=profile -d xdebug.output_dir=/tmp app.php

# or drive it from userland (Xdebug-compatible API)
xdebug_start_trace('/tmp/profile.out');
run_the_code();
xdebug_stop_trace();
echo xdebug_get_profiler_filename();
```

Measured on a call-dense workload (`fib(20)` ×200), best of 3:

| | Time | Overhead |
|---|---|---|
| no profiler | 0.092 s | 1.0× |
| **fast-xdebug** | 1.28 s | 13.9× |
| **Xdebug** | 6.25 s | 67.8× |

→ **~4.9× faster than Xdebug's profiler**, and the gap widens on realistic
(less call-dense) workloads because the per-call observer cost is fixed.

## Step debugging

**Not implemented in this release.** Step debugging speaks the DBGp protocol to
an IDE, and a partial implementation is worse than none: an IDE that negotiates a
session and then hits an unimplemented command hangs. With `xdebug.mode=debug`
the extension emits a diagnostic and does nothing else, rather than opening a
half-working DBGp session. The full plan — DBGp socket server, `zend_observer`
statement callbacks, breakpoints, stack/context/eval — is written up in
[`docs/debugger-spec.md`](docs/debugger-spec.md). This is also the point where
the "register as `xdebug`" compatibility posture (see below) is most in tension:
faithfully passing an IDE's Xdebug-specific expectations is a much higher bar than
answering `php-code-coverage`'s probes.

## Compatibility

`php-code-coverage` selects its driver via `extension_loaded('xdebug')` and only
offers branch/path coverage from the Xdebug driver. To be a drop-in backend,
fast-xdebug **registers under the module name `xdebug`**, reports an Xdebug
version ≥ 3.1, and answers `xdebug_info('mode')` with `['coverage']`.

This is a deliberate, documented trade-off (see
[ADR-0001](docs/decisions/0001-claim-xdebug-identity.md)): it works with every
existing tool today, at the cost of impersonating Xdebug. Consequences:

- **It cannot be loaded at the same time as real Xdebug** (both claim `xdebug`).
- `phpversion('xdebug')` returns `3.6.99-fast-xdebug-<version>`, and
  `xdebug_info()` includes `"engine" => "fast-xdebug"`, so the impersonation is
  detectable.

The cleaner long-term path — a first-class driver accepted into
`php-code-coverage` — is the intended direction and is recorded in the ADR.

## Accuracy

- **Line coverage: byte-identical** to both pcov and Xdebug on the test fixtures,
  including the in-flight `{main}` frame.
- **Branch/path coverage:** produced in Xdebug's exact data shape
  (`functions → {branches, paths}` with `op_start/op_end/line_start/line_end/
  hit/out/out_hit` and `path/hit`). Per-function branch **topology** and the
  taken/not-taken **edge pattern** match Xdebug; branch/path **percentages** are
  not byte-identical because Xdebug's `hit` accounting under-counts some
  fully-executed constructs (a fully-run `foreach` scores branch-rate 0.25 under
  Xdebug vs 1.0 here). fast-xdebug's numbers are internally consistent and
  arguably more intuitive. Details and the exact reasons are in
  [`docs/design.md`](docs/design.md#7-known-limitations-documented-honestly).

## Supported versions

- PHP **8.2, 8.3, 8.4, 8.5**, NTS and ZTS.
- Locally built and tested on 8.2 / 8.3 / 8.4 in this repository; 8.5 is covered
  by CI.
- Built with `-fno-strict-aliasing` (as PHP core itself is) — required for
  correct `-O2` codegen; see [ADR-0002](docs/decisions/0002-block-model-and-analysis-cache.md).

## Development

```sh
# build
phpize && ./configure --enable-fast-xdebug --with-php-config="$(command -v php-config)" && make -j"$(nproc)"

# .phpt tests
php run-tests.php -q -d extension="$PWD/modules/fast_xdebug.so" tests/*.phpt

# validate with valgrind (never trust "it didn't crash")
USE_ZEND_ALLOC=0 valgrind -q php -n -d extension="$PWD/modules/fast_xdebug.so" some-script.php

# end-to-end against php-code-coverage / PHPUnit
cd tests/integration && composer install
php -d extension="$PWD/../../modules/fast_xdebug.so" vendor/bin/phpunit --path-coverage --coverage-text
```

The day-one regression test is a branch-coverage run across a PHP 8.4+
**frameless internal call** (`in_array` with variable arguments), which is the
exact shape that segfaulted Xdebug 3.5.0–3.5.3 by reading past a
`zend_internal_function`. It is `tests/030-frameless-internal-call.phpt` and runs
under valgrind in CI.

## License

BSD-2-Clause. See [LICENSE](LICENSE).
