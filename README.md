# swiftcov

**Xdebug-API-compatible PHP code coverage with a fast, block-based backend.**

> **Naming:** the product/package is **swiftcov** (Composer `juslintek/swiftcov`,
> PECL `swiftcov`, extension `swiftcov.so`, configure flag `--enable-swiftcov`).
> At **runtime** it deliberately registers as the module `xdebug` so PHPUnit /
> `php-code-coverage` detect it unchanged — see
> [Compatibility](#compatibility). Older `fast_xdebug_*` helper functions remain
> as backward-compatible aliases of the new `swiftcov_*` ones.

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
| **swiftcov** | **0.0307 s (4.1×)** | **0.0626 s (8.3×)** |

- Line coverage: **~4.8× faster than Xdebug — and faster than pcov** (0.031 s vs
  0.049 s), while pcov cannot do branch/path at all. The
  [saturation fast-path](#why-it-is-faster) is what pushes line coverage below
  pcov here.
- Branch + path: **~14× faster than Xdebug**, and — the key result — it costs
  essentially the **same as swiftcov's own line coverage** because path
  enumeration happens once at collection time, not per opcode. Xdebug pays ~6×
  more for branch than line coverage; swiftcov pays ~2×.

### Profiling — `fib(20) ×200` (call-dense)

| Engine | Time | Overhead |
|---|---|---|
| no profiler | 0.0526 s | 1.0× |
| **swiftcov** | 0.8926 s | 17.0× |
| **Xdebug 3.6** | 4.1801 s | 79.5× |

→ **~4.7× faster than Xdebug's profiler.** This workload is deliberately
pathological (almost pure function calls); on realistic code with more work
between calls the ratio improves further, because the profiler's cost is a fixed
per-call amount and everything else runs at native speed.

¹ pcov is line-only by design. Under `phpunit --path-coverage`,
`SebastianBergmann\CodeCoverage\Driver\Selector` throws
`NoCodeCoverageDriverWithPathCoverageSupportAvailableException` unless the driver
is Xdebug. **That is the gap swiftcov fills.**

> These are self-contained microbenchmarks (the original brief's private
> 741-test suite is not available here). Absolute numbers are workload- and
> machine-specific and will drift run to run; the **ratios** are the
> transferable result.

## Why it is faster

Xdebug and pcov both do work **per executed opcode** at runtime (Xdebug installs
an opcode handler on ~every opcode and, in branch mode, runs
`xdebug_branch_info_mark_reached` per branch op; pcov does a nested-hash
`file→line` probe per opline).

swiftcov:

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

**No — swiftcov uses no SIMD.** It is plain scalar C compiled at `-O2` with
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
  `./configure --enable-swiftcov-native` (default stays portable — no
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

Pick whichever row fits you — they all end with an extension that registers as
`xdebug` and is picked up automatically by PHPUnit / php-code-coverage.

| Method | Needs a compiler? | Command |
|---|---|---|
| **PIE** (recommended) | builds from source, or grabs a prebuilt binary if available | `pie install juslintek/swiftcov` |
| **Prebuilt binary** | no | download the `.so` for your PHP from [Releases](https://github.com/juslintek/swiftcov/releases), drop it in your `extension_dir` |
| **`install.sh`** (source, zero-config) | yes | `./install.sh --enable-ini` |
| **From source** (manual) | yes | `phpize && ./configure --enable-swiftcov && make && make install` |
| **PECL package** (tarball) | yes | `pecl install swiftcov-<version>.tgz` (from a Release asset) |
| **Composer** (Packagist, once submitted) | via PIE | `composer require juslintek/swiftcov` |
| **Docker image** (baked in) | no | `docker run --rm -v "$PWD":/app -w /app juslintek/swiftcov:php8.4 vendor/bin/phpunit --path-coverage` |
| **Docker `COPY --from`** (downstream image) | no | one stage in your own Dockerfile — see [docker/README.md](docker/README.md) |
| **Kubernetes** (Job/Pod) | no | `kubectl apply -f deploy/k8s/coverage-job.yaml` — see [deploy/k8s/README.md](deploy/k8s/README.md) |

The Docker and Kubernetes paths are **compiler-free** — the extension is already
compiled into the image — making swiftcov at least as easy to adopt as pcov,
while adding the branch/path coverage pcov cannot provide.

### PIE (recommended)

```sh
pie install juslintek/swiftcov
```

`composer.json` declares `"type": "php-ext"` with a `php-ext` block, and
`download-url-method: ["pre-packaged-binary", "composer-default"]` — so PIE uses
a **prebuilt release binary** when one matches your PHP, and otherwise builds
from source. PECL is deprecated in favour of PIE.

### Prebuilt binary (no compiler)

Each tagged release attaches Linux `.so` files named
`swiftcov-<version>-php<X.Y>-<nts|zts>-linux-x86_64-api<N>.so` plus a
`.sha256`. Match your PHP (`php -i | grep 'PHP Extension'` gives the API number),
verify the checksum, copy it into your `extension_dir`
(`php-config --extension-dir`) as `swiftcov.so`, and add the ini line below.

### `install.sh` (source, zero-config)

The easiest source install — it finds `php-config`/`phpize`, builds, installs to
the right `extension_dir`, and can wire up php.ini for you:

```sh
./install.sh --enable-ini        # build, install, and add the extension= line
./install.sh                     # build + install; print the ini line to add
PHP_CONFIG=/path/php-config ./install.sh   # target a specific PHP
./install.sh --native            # -O3 -march=native (self-hosted builds only)
```

### From source (manual)

Requires PHP 8.2–8.5 dev headers (`phpize`, `php-config`) and a C toolchain.

```sh
phpize
./configure --enable-swiftcov --with-php-config="$(command -v php-config)"
make -j"$(nproc)"
sudo make install
```

`--enable-swiftcov-native` tunes for the build host CPU (`-O3 -march=native`);
**don't** use it for a distributed binary — the default build is portable.

### Enable it

Load it as a **normal module** (not a Zend extension):

```ini
; php.ini
extension=swiftcov.so
```

It registers under the name `xdebug` (see [Compatibility](#compatibility)), so
PHPUnit picks it up as the coverage driver automatically. `xdebug.mode` defaults
to `coverage`; set `xdebug.mode=auto` for [heuristic detection](#auto-configuration).

> **Packagist / `composer require`:** the repository is Composer-valid and tagged;
> once it is submitted to packagist.org (a one-time login-and-paste by the owner)
> `composer require juslintek/swiftcov` works and auto-updates from tags.
> **pecl.php.net central:** now that the package name is `swiftcov` (not
> `xdebug`), a PECL submission is viable — the name no longer collides with
> Xdebug's, even though the module still *registers* as `xdebug` at runtime. See
> [`docs/publishing.md`](docs/publishing.md) for the submission runbook. PIE and
> the prebuilt binaries remain the fastest paths in the meantime.

## CI / containers / clusters

swiftcov is designed to drop into infrastructure as easily as pcov, but with
branch/path coverage. Pipeline-as-code for every common environment ships in the
repo:

### GitHub Actions

[`.github/workflows/ci.yml`](.github/workflows/ci.yml) builds and tests across
PHP 8.2–8.5 (NTS + a ZTS row), runs the identity smoke test, the `.phpt` suite,
the valgrind frameless regression, and the PHPUnit `--path-coverage` integration
harness. [`.github/workflows/release-binaries.yml`](.github/workflows/release-binaries.yml)
attaches prebuilt `.so`s + a PECL tarball to each tag, and optionally
builds/pushes the Docker image when registry secrets are present.

### GitLab CI

[`.gitlab-ci.yml`](.gitlab-ci.yml) mirrors the GitHub pipeline for GitLab:
build+test on `php:X.Y-cli` across 8.2/8.3/8.4, a valgrind stage, and a
tag-gated release stage that produces the prebuilt `.so` + `pecl package`
tarball and publishes them as a GitLab Release. Coverage reports come out as
Cobertura, which GitLab's coverage visualization consumes unchanged.

### Docker

Build a self-contained image (extension baked in, no compiler at runtime):

```sh
docker build --build-arg PHP_VERSION=8.3 -t swiftcov:php8.3 .
docker run --rm -v "$PWD":/app -w /app swiftcov:php8.3 \
  vendor/bin/phpunit --path-coverage --coverage-cobertura=cobertura.xml
```

Or add swiftcov to your own image with a single compiler-free `COPY --from`
stage:

```dockerfile
FROM php:8.3-cli
COPY --from=juslintek/swiftcov:php8.3 \
     /usr/local/lib/php/extensions/ /usr/local/lib/php/extensions/
COPY --from=juslintek/swiftcov:php8.3 \
     /usr/local/etc/php/conf.d/swiftcov.ini /usr/local/etc/php/conf.d/swiftcov.ini
```

Full details in [docker/README.md](docker/README.md).

### Kubernetes

Run coverage in-cluster with the prebuilt image:

```sh
kubectl apply -f deploy/k8s/coverage-job.yaml
```

The [`deploy/k8s/`](deploy/k8s/) manifests define a `Job` that runs
`phpunit --path-coverage` and writes Cobertura; see
[deploy/k8s/README.md](deploy/k8s/README.md).

## Usage

Just run PHPUnit as usual — path coverage now works and is fast:

```sh
php -d extension=swiftcov.so vendor/bin/phpunit \
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
`xdebug_info`, `xdebug_start_trace`, `xdebug_stop_trace`,
`xdebug_get_profiler_filename`, plus the `XDEBUG_CC_*` / `XDEBUG_FILTER_*` /
`XDEBUG_PATH_*` constants. swiftcov-specific helpers:
`swiftcov_engine()` (identify the engine), `swiftcov_resolved_mode()`
(what `auto` chose), `swiftcov_recommended_settings()` (auto-tuning advice). The
former `fast_xdebug_engine()` / `fast_xdebug_resolved_mode()` /
`fast_xdebug_recommended_settings()` remain as backward-compatible aliases.

## Auto-configuration

swiftcov can adapt to its environment so you configure less. **All of this is
heuristic and documented as such — an extension cannot truly know your intent,
so nothing here overrides an explicit setting.**

### Mode auto-detection (`xdebug.mode=auto`)

With `xdebug.mode=auto`, the effective mode is resolved once per request from
environment signals, in this order:

1. `XDEBUG_MODE` env var, if set → used verbatim (Xdebug's own convention).
2. `XDEBUG_TRIGGER` / `XDEBUG_PROFILE` set → `profile`.
3. `XDEBUG_SESSION` / `XDEBUG_SESSION_START` set → would be `debug`; since step
   debugging isn't implemented, falls back to `coverage` (never hangs an IDE).
4. Running under a test runner (argv/env mentions phpunit/paratest/codeception)
   → `coverage`.
5. Fallback → `coverage`.

An explicit `xdebug.mode` (anything other than `auto`) always wins.
`swiftcov_resolved_mode()` shows what `auto` resolved to.

### Recommended settings

`swiftcov_recommended_settings()` inspects `memory_limit`, CPU count, OPcache
and the resolved mode, and returns advisory recommendations (it changes
nothing):

```php
print_r(swiftcov_recommended_settings());
// [ memory_limit_bytes, cpu_count, opcache, resolved_mode,
//   recommended => [ 'fast_xdebug.max_paths' => 1024, 'coverage_filter' => …,
//                    'fast_xdebug.memory_guard' => 1 ],
//   notes => [ … human-readable reasoning … ] ]
```

The suggested `max_paths` scales with `memory_limit` (≤128M → 256, ≤512M → 1024,
larger/unlimited → 4096) so small-memory environments stay safe.

## Memory management

The extension is **valgrind-clean (no leaks)** — see [Development](#development).
On top of that, an automatic **memory guard** keeps large suites from running
out of memory:

- At `xdebug_start_code_coverage()`, the path-enumeration cap is derived from
  `memory_limit` (the biggest memory sink in branch/path coverage is path
  enumeration, which can produce thousands of paths for a generated file).
- During collection, if live usage crosses **85 % of `memory_limit`**, path
  enumeration for the remaining functions is **skipped** — **branches and line
  coverage are still emitted in full**, so you keep the important data and only
  lose the optional path list, instead of an OOM fatal.

It is on by default and costs one `zend_memory_usage()` read per function at
collection time (nothing on the hot per-opcode path). Disable it with
`fast_xdebug.memory_guard=0` for exact Xdebug-parity fidelity regardless of
memory.

## Tool integration

| Tool | Works? | How |
|---|---|---|
| **PHPUnit** (line/branch/path coverage) | ✅ | auto-detected as the Xdebug driver; run `--coverage-*` / `--path-coverage` as usual |
| **php-code-coverage** (Clover, Cobertura, HTML, Crap4J) | ✅ | same driver path; Cobertura carries branch data |
| **GitLab / GitHub coverage** | ✅ | consumes the Cobertura/Clover output unchanged |
| **Paratest / Codeception** | ✅ | detected by `xdebug.mode=auto`; uses the coverage path |
| **KCachegrind / qcachegrind** | ✅ | open the `cachegrind.out.*` the profiler writes |
| **PhpStorm / VS Code — profiling** | ✅ | import the Cachegrind snapshot |
| **PhpStorm / VS Code — step debugging** | ❌ | not implemented (see [Step debugging](#step-debugging)); with `xdebug.mode=debug` it prints a notice rather than half-opening a DBGp session |

## Profiling

swiftcov ships a Cachegrind profiler on the same "cheap hook" thesis: it uses
one `zend_observer` enter/exit per **call** (not per opcode), builds a call tree
with self/inclusive time and memory, and writes a Cachegrind file that
KCachegrind, qcachegrind, PhpStorm and Blackfire's importer read.

```sh
# auto-start (writes cachegrind.out.<pid> to xdebug.output_dir)
php -d extension=swiftcov.so -d xdebug.mode=profile -d xdebug.output_dir=/tmp app.php

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
swiftcov **registers under the module name `xdebug`**, reports an Xdebug
version ≥ 3.1, and answers `xdebug_info('mode')` with `['coverage']`.

This is a deliberate, documented trade-off (see
[ADR-0001](docs/decisions/0001-claim-xdebug-identity.md)): it works with every
existing tool today, at the cost of impersonating Xdebug. Consequences:

- **It cannot be loaded at the same time as real Xdebug** (both claim `xdebug`).
- `phpversion('xdebug')` returns `3.6.99-swiftcov-<version>`, and
  `xdebug_info()` includes `"engine" => "swiftcov (fast-xdebug)"`, so the
  impersonation is detectable.

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
  Xdebug vs 1.0 here). swiftcov's numbers are internally consistent and
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
phpize && ./configure --enable-swiftcov --with-php-config="$(command -v php-config)" && make -j"$(nproc)"

# .phpt tests
php run-tests.php -q -d extension="$PWD/modules/swiftcov.so" tests/*.phpt

# validate with valgrind (never trust "it didn't crash")
USE_ZEND_ALLOC=0 valgrind -q php -n -d extension="$PWD/modules/swiftcov.so" some-script.php

# end-to-end against php-code-coverage / PHPUnit
cd tests/integration && composer install
php -d extension="$PWD/../../modules/swiftcov.so" vendor/bin/phpunit --path-coverage --coverage-text
```

The day-one regression test is a branch-coverage run across a PHP 8.4+
**frameless internal call** (`in_array` with variable arguments), which is the
exact shape that segfaulted Xdebug 3.5.0–3.5.3 by reading past a
`zend_internal_function`. It is `tests/030-frameless-internal-call.phpt` and runs
under valgrind in CI.

## License

BSD-2-Clause. See [LICENSE](LICENSE).
