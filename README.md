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

Environment: **PHP 8.4.24 NTS, x86_64, gcc 11.5.0, `-O2` (no `-march`)**. Best of
5 runs; timing wraps only the workload (start→stop), not process startup.
Reproduce with the `bench/` scripts and the `.so`s you build.

### Coverage — `bench/workload.php`

Memoised recursion, `if/elseif` ladders, `in_array`/loop bodies, 4000 iterations.

| Engine | Line coverage | Branch + path coverage |
|---|---|---|
| no coverage | 0.0075 s (1.0×) | — |
| **pcov** | 0.0494 s (6.6×) | ❌ not supported¹ |
| **Xdebug 3.6** | 0.1463 s (19.5×) | 0.8769 s (**117×**) |
| **fast-xdebug** | **0.0307 s (4.1×)** | **0.0626 s (8.3×)** |

- Line coverage: **~4.8× faster than Xdebug — and faster than pcov** (0.031 s vs
  0.049 s), while pcov cannot do branch/path at all. The
  [saturation fast-path](#why-it-is-faster) is what pushes line coverage below
  pcov here.
- Branch + path: **~14× faster than Xdebug**, and — the key result — it costs
  essentially the **same as fast-xdebug's own line coverage** because path
  enumeration happens once at collection time, not per opcode. Xdebug pays ~6×
  more for branch than line coverage; fast-xdebug pays ~2×.

### Profiling — `fib(20) ×200` (call-dense)

| Engine | Time | Overhead |
|---|---|---|
| no profiler | 0.0526 s | 1.0× |
| **fast-xdebug** | 0.8926 s | 17.0× |
| **Xdebug 3.6** | 4.1801 s | 79.5× |

→ **~4.7× faster than Xdebug's profiler.** This workload is deliberately
pathological (almost pure function calls); on realistic code with more work
between calls the ratio improves further, because the profiler's cost is a fixed
per-call amount and everything else runs at native speed.

¹ pcov is line-only by design. Under `phpunit --path-coverage`,
`SebastianBergmann\CodeCoverage\Driver\Selector` throws
`NoCodeCoverageDriverWithPathCoverageSupportAvailableException` unless the driver
is Xdebug. **That is the gap fast-xdebug fills.**

> These are self-contained microbenchmarks (the original brief's private
> 741-test suite is not available here). Absolute numbers are workload- and
> machine-specific and will drift run to run; the **ratios** are the
> transferable result.

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
4. **Saturation fast-path.** Once every block of an `op_array` has been recorded
   (and, in branch mode, every CFG edge), re-executing it can record nothing
   new. A single-slot pointer compare then short-circuits the whole handler
   *before any hash lookup*. This is why line coverage on the benchmark dropped
   below pcov: the production code exercised by a test suite is fully covered
   early, then runs "for free" across the remaining thousands of invocations.
   (It does not help a *single* monolithic loop — a function's exit block isn't
   covered until the loop finishes — which is inherent, not a bug.)

Full rationale, C4 diagrams, and the measured baseline are in
[`docs/design.md`](docs/design.md) and [`docs/decisions/`](docs/decisions).

## Is this SIMD? Would SIMD help? What would help more?

**No — fast-xdebug uses no SIMD.** It is plain scalar C compiled at `-O2` with
`-fno-strict-aliasing` and **no `-march`**, so it targets baseline x86-64 (only
SSE2, no auto-vectorization to AVX). The speedup over Xdebug is **algorithmic,
not instruction-level**: fewer operations per executed opcode (one byte store vs
a hash probe / filter-slot read / per-line set insert), and moving branch/path
work from runtime to collection time.

**Would SIMD help? Almost certainly not, and here's the honest reasoning.** The
hot path is:

```c
block_id = analysis->op_to_block[cur_op];   // one indexed load
runtime->block_hit[block_id] = 1;           // one byte store
```

That is a **pointer-chasing, control-flow-bound, one-element-at-a-time** workload
driven by the PHP VM calling our handler once per opcode. SIMD accelerates
*data-parallel* work — the same operation over contiguous arrays. Here there is
no vector to operate on: each opcode independently touches one scalar at a
data-dependent index. The cost is dominated by the **indirect call from the VM
into the handler and the branch-mispredict/cache behaviour around it**, none of
which vector instructions address. Vectorizing a single-byte store would be
strictly slower (setup overhead for no width benefit).

There are exactly two places SIMD could *ever* apply, both minor and off the hot
path:

- **Collection-time line reconstruction** (expanding hit blocks to line arrays)
  and path enumeration — these run once per request, not per opcode, so their
  cost is already negligible in the measurements.
- **Filter prefix matching** in `xdebug_set_filter` (comparing a filename
  against include/exclude prefixes) is a `memcmp`, which glibc already
  vectorizes internally.

So hand-written intrinsics would add portability/build complexity for no
measurable win. **The bottleneck is call/dispatch overhead and memory access
patterns, not arithmetic throughput.**

### What has been done, and what could improve performance further

**Done (v0.3.0):**

- ✅ **Saturation fast-path** — the biggest realistic win for test suites (see
  point 4 above). Line coverage on the benchmark went from 0.058 s → 0.031 s
  (~1.9×), overtaking pcov.
- ✅ **Opt-in `-O3 -march=native` build** via
  `./configure --enable-fast-xdebug-native` (default stays portable — no
  `-march` — so distributed/PECL/PIE binaries run everywhere). On the benchmark
  this was within noise, consistent with the workload being dispatch/memory-
  bound rather than arithmetic-bound; it may help on other CPUs/workloads, so
  it's offered but not overclaimed.

**Considered and deliberately *not* done:**

- **Persist analysis across requests / OPcache reuse.** Tempting on paper, but
  the dominant consumer (PHPUnit on the CLI SAPI) is **one request per
  process**, so a cross-request cache buys nothing there while adding lifetime
  and stale-`mtime` risk. It would only help a long-lived FPM worker collecting
  coverage across many requests — a rare setup. Skipped rather than ship risk
  for no real-world gain.

**Remaining candidates (smaller):**

- **Branch mode: per-frame last-block slot** instead of the single-slot edge
  cache, to drop the `execute_data` comparison per opcode.
- **`likely()`/`unlikely()` + `restrict`** on the handler to straight-line the
  common path.
- **Hook fewer opcode types** when only line coverage is requested.

The theme: every remaining win is about **doing the per-opcode work less often**
(saturation, fewer hooks) or **letting the compiler specialize** — not
about vectorizing the tiny scalar store, which SIMD cannot help.

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

For a build you run on the same machine you compile on (e.g. a self-hosted CI
runner), add `--enable-fast-xdebug-native` to tune for the host CPU
(`-O3 -march=native`). **Do not** use it for a binary you distribute — it will
only run on CPUs matching the build host. The default build is portable.

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

See the [profiler numbers above](#profiling--fib20-200-call-dense):
**~4.7× faster than Xdebug's profiler**, with the gap widening on realistic
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
