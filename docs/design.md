# fast-xdebug — design

A PHP Zend extension that provides **Xdebug-API-compatible code coverage**
(line, branch and path) with a block-based backend that is dramatically faster
than Xdebug for branch/path coverage, and faster than Xdebug for line coverage,
while producing output in exactly the shape `php-code-coverage` / PHPUnit
consume.

It deliberately implements **only coverage** — not the debugger, profiler or
tracer that Xdebug also ships — behind a clean internal boundary so a step
debugger could be added later.

---

## 1. Why this exists (the gap)

Measured on this project's representative workload (`bench/`, PHP 8.4.24; see
`../BASELINE.md` and the README for the full table):

| Engine | Line | Branch+Path |
|--------|------|-------------|
| pcov | 4.97× | *not supported* |
| Xdebug | 18.7× | 99.7× |
| **fast-xdebug** | **~7×** | **~7×** |

(× = slowdown vs no coverage.)

- **pcov** is fast but **line-only by design**. Under `phpunit --path-coverage`,
  `SebastianBergmann\CodeCoverage\Driver\Selector` throws
  `NoCodeCoverageDriverWithPathCoverageSupportAvailableException` unless the
  driver is Xdebug.
- **Xdebug** is complete but slow, and branch/path coverage is ~20× slower than
  pcov's line coverage.

So the market gap is **fast branch/path coverage**. That is fast-xdebug's
primary target: it makes path coverage nearly as cheap as line coverage.

## 2. Where the incumbents spend their time (measured, not guessed)

Both incumbents do work **per executed opcode** at runtime:

- **Xdebug** installs a `user_opcode_handler` on essentially every opcode
  (`src/coverage/code_coverage.c`), and for branch mode additionally runs
  `xdebug_branch_info_mark_reached` per branch opcode. Callgrind on a small
  workload attributed ~5.7–6% of *all* instructions to
  `xdebug_branch_info_mark_reached` alone; the rest of the coverage cost is the
  per-opcode handlers (read op_array, read the reserved filter slot, read
  lineno, hash/set-add per line).
- **pcov** overrides `zend_execute_ex` and does a nested-hash
  `php_pcov_has(file, line)` probe per opline.

Both are therefore **O(opcodes executed)** with non-trivial per-opcode work.

## 3. The engine

### 3.1 Compile-time analysis (once per op_array)

`src/analysis.c` splits each `op_array` into **basic blocks**:

- **Leaders** are entry points (op 0, `ZEND_CATCH`) and jump *targets*. Jump
  targets are computed by `fxd_find_jumps`, which mirrors Xdebug's
  `xdebug_find_jumps` so branch topology matches — including the PHP 8.4+
  frameless internal-call opcode `ZEND_JMP_FRAMELESS`, `ZEND_MATCH`,
  `ZEND_SWITCH_LONG/STRING`, `ZEND_CATCH` chains, `ZEND_FE_FETCH/RESET`,
  `ZEND_FAST_CALL/RET`.
- A block **ends at its first control-flow op** (its terminator). Ops after an
  unconditional terminator up to the next leader are unreachable-trailing and do
  not extend the block. This yields branch spans equivalent to Xdebug's
  `start..end` merge (`xdebug_branch_post_process`).
- For each block we precompute: `start_op/end_op`, the exact set of
  **executable lines** and the op index at which each first appears
  (`lines`, `line_ops`), successor block edges (`outs`), and `is_entry`.

The analysis is cached for the request in a `HashTable` keyed by the op_array's
`opcodes` pointer (stable for the request). This deliberately avoids depending
on being registered as a *Zend* extension (which `op_array->reserved[]` slots
require) so the extension can ship as a plain `extension=` module. See ADR-0002.

### 3.2 Runtime recording

`src/coverage.c` installs a `user_opcode_handler` on every opcode type (except
`ZEND_NOP`/`ZEND_EXT_NOP`/`ZEND_OP_DATA`). The handler is deliberately tiny:

```
block_id = analysis->op_to_block[current_op_index];
runtime->block_hit[block_id] = 1;         // one byte store
```

This is still O(opcodes executed), but the per-opcode work is **one array write
with no hash lookup, no filter-slot dereference, and no per-line set insert** —
which is why it is far cheaper than either incumbent. Lines, branches and paths
are reconstructed from `block_hit` only at collection time, so **branch/path
coverage costs the same at runtime as line coverage** (the 20× Xdebug gap
disappears).

**Branch edges.** In branch mode we additionally record the *actual edge
traversed* using an O(1) single-slot cache of the last `(frame, block)` seen:
two consecutive marks in the same frame whose block changed are a real CFG edge;
a frame change (call/return) is not. This gives edge/path `hit` values with the
same taken/not-taken pattern as Xdebug, without per-opcode hashing.

**Line exactness for in-flight frames.** Block granularity would over-report the
open block of a frame that is *currently executing* when `start`/`get` is called
(most importantly `{main}`, which has not `RETURN`ed yet). We clamp the open
block's reported lines to `[floor, frontier]`:

- `floor` = the opline each active frame was on when coverage **started**
  (so lines that ran before `start` are excluded);
- `frontier` = the opline each active frame is on at **collection**
  (so lines after the current point are excluded).

With this, line coverage is **byte-identical to both pcov and Xdebug** on the
test fixtures, including `{main}`.

### 3.3 Path enumeration and the cap

`fxd_find_paths` enumerates acyclic-per-edge paths from each entry block,
capped at `FXD_MAX_PATHS = 4096` (identical to Xdebug's
`src/coverage/branch_info.h`), with `FXD_BRANCH_MAX_OUTS = 64`. Path counts
explode on large generated files; the cap matches Xdebug's so behaviour is
comparable.

## 4. Compatibility posture (the deliberate choice)

`php-code-coverage` selects a driver via `sebastian/environment`
`Runtime::hasXdebug()` = `extension_loaded('xdebug')`, and then probes
`phpversion('xdebug') >= 3.1` and `xdebug_info('mode')` containing `'coverage'`.
`Driver\Selector` makes Xdebug the **only** driver that offers branch/path
coverage.

There were two options (see ADR-0001):

1. **Register as `xdebug`** and satisfy those probes — works with every existing
   tool *today*, unchanged.
2. **Get a first-class driver accepted upstream** in `php-code-coverage` — a
   cleaner, more durable footing, but slower and dependent on others.

fast-xdebug currently takes **option 1** to be immediately useful, while exposing
a sentinel `fast_xdebug_engine()` and an `"engine" => "fast-xdebug"` entry in
`xdebug_info()` so tools and humans can tell the two apart. The reported
`phpversion('xdebug')` is `3.6.99-fast-xdebug-<version>`. This is a pragmatic
footing, not the end state; option 2 is the intended long-term path.

## 5. Data structure produced

`xdebug_get_code_coverage()` returns exactly the Xdebug shapes consumed by
`RawCodeCoverageData`:

- **Line mode:** `array<filename, array<line:int, hit:int>>` where `1` =
  executed, `-1` = executable but not executed (only when `XDEBUG_CC_UNUSED`),
  `-2` = dead code. Consumers treat `>= 1` as covered.
- **Branch mode (`XDEBUG_CC_BRANCH_CHECK`):**
  `array<filename, {lines, functions}>` where each function
  (`{main}`, `Class->method` for instance methods, `Class::method` for static
  methods, `Ns\func`) carries `branches` and `paths`. `BranchCoverageType` =
  `{op_start, op_end, line_start, line_end, hit, out, out_hit}`;
  `PathCoverageType` = `{path, hit}`.

The `Class->method` vs `Class::method` distinction matters: `php-code-coverage`
looks methods up by `Class->method` in `Node/File.php`. Branch `line_start`/
`line_end` are reported as the block's first/last **executable** line so that
`keepFunctionCoverageDataOnlyForLines` does not discard the branch.

## 6. C4 view

### Level 1 — System context

```
   PHPUnit / php-code-coverage
              │  (Selector -> XdebugDriver, because we answer the Xdebug probes)
              ▼
        xdebug_* userland API
              ▼
        fast-xdebug (this extension)
              ▲
   PHP / Zend VM (opcode handlers, observer)
```

External actors: the test runner (calls `xdebug_start/stop/get`), the Zend
engine (drives the opcode handler), and the report consumers (Cobertura/Clover/
HTML built by php-code-coverage). Trust boundary: none crossed — everything is
in-process.

### Level 2 — Containers

Single container: the shared object `fast_xdebug.so`, loaded as a PHP module.
No network, no files, no databases. State is per-request only.

### Level 3 — Components

| Component | File | Responsibility |
|-----------|------|----------------|
| Userland API + module lifecycle | `fast_xdebug.c` | functions, constants, INI, MINIT/RINIT, `xdebug_info`, version reporting |
| Analysis | `src/analysis.c` | op_array → basic blocks, jump semantics, per-block lines/edges |
| Runtime + collection | `src/coverage.c` | opcode handler, hit recording, edge tracking, Xdebug-shaped output |
| Contract/types | `src/coverage.h`, `php_fast_xdebug.h` | data structures, globals, caps |

Dependency rule: `coverage.c` depends on `analysis.c` (one direction); analysis
depends only on the Zend headers.

## 7. Known limitations (documented honestly)

- **Branch/path *percentages* are not byte-identical to Xdebug.** Topology,
  shape, and edge taken/not-taken patterns match per-function, and totals match,
  but Xdebug's branch-`hit` accounting under-counts some fully-executed
  constructs (e.g. a fully-run `foreach` loop reports branch-rate 0.25 under
  Xdebug vs 1.0 here). fast-xdebug's numbers are arguably more intuitive; they
  are internally consistent and derived from observed edges. If strict Xdebug
  parity is required, Xdebug's exact `hit_branch` transition-indexing (including
  its `highest_out` multiplier) would need to be replicated.
- **Line coverage IS byte-identical** to both pcov and Xdebug on the fixtures.
- `start_floors` persists for the request: a function that is on the call stack
  when `xdebug_start_code_coverage()` is called, and is *later* re-entered fresh,
  could under-report its pre-floor lines on the later call. This is rare
  (coverage is normally started from `{main}` before the code under test runs).
- Coverage state is per-request; ZTS builds are supported via module globals but
  the analysis cache and edge cache are request-scoped.

## 8. Validation

- Line coverage diffed against pcov and Xdebug on `bench/workload.php` and
  several fixtures — identical.
- End-to-end with PHPUnit 11 + php-code-coverage: `--path-coverage` runs and a
  well-formed Cobertura report with `branches-covered`/`branches-valid` is
  produced with no pipeline change.
- Valgrind-clean (no invalid reads/writes, no leaks) including the PHP 8.4+
  frameless `in_array` regression, which is the class of bug that caused an
  Xdebug 3.5 segfault.
