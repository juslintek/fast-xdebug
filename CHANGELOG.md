# Changelog

All notable changes to fast-xdebug are documented here. Versions are alpha;
the API surface tracks the subset of Xdebug's coverage/profiler API that
php-code-coverage / PHPUnit / Cachegrind tooling consume.

## [0.3.0] - 2026-09-11

### Added
- **Saturation fast-path.** Once every basic block of an `op_array` has been
  recorded (and, in branch mode, every CFG edge), a single-slot pointer compare
  short-circuits the per-opcode handler before any hash lookup. Line coverage on
  the benchmark dropped from 0.058 s to 0.031 s (~1.9× faster), overtaking pcov,
  because production code exercised by a test suite is fully covered early and
  then runs effectively for free. Does not help a single monolithic loop (a
  function's exit block isn't covered until the loop ends) — this is inherent.
- **Opt-in optimized build** `./configure --enable-fast-xdebug-native`
  (`-O3 -march=native -mtune=native`, with a compiler probe + fallback). The
  default build stays portable (no `-march`) so distributed binaries run on any
  CPU.

### Notes
- Line coverage remains byte-identical to pcov and Xdebug; branch/path output
  keeps Xdebug's exact shape. Validated valgrind-clean (including the frameless
  internal-call and saturation paths) on PHP 8.2/8.3/8.4.

## [0.2.0]

### Added
- **Cachegrind profiler** via `zend_observer` fcall begin/end (one enter/exit
  per call, not per opcode). Emits a Cachegrind file that KCachegrind,
  qcachegrind, PhpStorm and Blackfire's importer read; ~4.7× faster than
  Xdebug's profiler on a call-dense workload. Userland API:
  `xdebug_start_trace`, `xdebug_stop_trace`, `xdebug_get_profiler_filename`;
  auto-starts on `xdebug.mode=profile`.
- Clean `src/debugger.{c,h}` boundary. **Step debugging (DBGp) is specified in
  `docs/debugger-spec.md` but intentionally not implemented** — with
  `xdebug.mode=debug` the extension emits a notice rather than opening a
  half-working session that would hang an IDE.

## [0.1.0]

### Added
- Initial release: Xdebug-API-compatible line, branch and path **code coverage**
  with a block-based backend. Analyses each `op_array` into basic blocks once,
  records one byte per executed block at runtime, reconstructs
  lines/branches/paths at collection time — so branch/path coverage costs about
  the same at runtime as line coverage (~14× faster than Xdebug branch mode).
- Registers under the module name `xdebug` to be a drop-in coverage backend for
  php-code-coverage/PHPUnit (see `docs/decisions/0001-claim-xdebug-identity.md`).
- Built with `-fno-strict-aliasing` (as PHP core requires). PHP 8.2–8.5, NTS/ZTS.
- `.phpt` suite including the PHP 8.4+ frameless internal-call regression; PIE
  `composer.json`; PECL `package.xml`; CI matrix.
