# Tool compatibility matrix

swiftcov registers at runtime as the module `xdebug` (see
[ADR-0001](decisions/0001-claim-xdebug-identity.md)), so the ecosystem of tools
that consume Xdebug's API detect and drive it unchanged. This document
enumerates each consumer, the **exact runtime probe or call it makes**,
swiftcov's result, and a status.

Because this repository builds and tests offline (composer/packagist is
unreachable in CI's sandbox), each probe is verified by **reproducing the exact
call in the `.phpt` suite** rather than by installing and running the tool. The
"Verified by" column names the test that exercises the probe.

Status legend:

- **works** — swiftcov answers the probe exactly as Xdebug does.
- **gap-closed** — a function/constant was added specifically because a named
  consumer calls it; justified below.
- **intentionally-unsupported** — swiftcov deliberately does not implement this;
  rationale given so the gap is a documented decision, not an accident.

## Coverage: the primary supported surface

| Tool | What it calls / probes | swiftcov result | Status | Verified by |
|---|---|---|---|---|
| **sebastian/environment** `Runtime::hasXdebug()` | `extension_loaded('xdebug')` | `true` | works | `001-identity.phpt`, `070-tool-probes.phpt` |
| **sebastian/environment** `Runtime::getXdebugVersion()` | `phpversion('xdebug')` | `3.6.99-swiftcov-<ver>` (so `version_compare(…, '3.1', '>=')` is true) | works | `070-tool-probes.phpt` |
| **php-code-coverage** `Driver\Selector` | selects `XdebugDriver` when `extension_loaded('xdebug')` and `phpversion('xdebug') >= 3.1` | driver selected | works | `070-tool-probes.phpt` |
| **php-code-coverage** `Driver\XdebugDriver::__construct()` | `xdebug_info('mode')` is a list containing `'coverage'` | `['coverage', …]` (list) | works | `070-tool-probes.phpt` |
| **php-code-coverage** `XdebugDriver` (constants) | `XDEBUG_CC_UNUSED`, `XDEBUG_CC_DEAD_CODE`, `XDEBUG_CC_BRANCH_CHECK`, `XDEBUG_FILTER_CODE_COVERAGE`, `XDEBUG_PATH_INCLUDE`, `XDEBUG_PATH_EXCLUDE` defined with Xdebug's numeric values | defined, values `1/2/4` and `0/1/0` | works | `072-constant-values.phpt` |
| **php-code-coverage** `XdebugDriver::start()` | `xdebug_start_code_coverage(XDEBUG_CC_UNUSED | XDEBUG_CC_DEAD_CODE [| XDEBUG_CC_BRANCH_CHECK])` | starts recording | works | `010-line-coverage.phpt`, `070-tool-probes.phpt` |
| **php-code-coverage** `XdebugDriver::stop()` | `xdebug_get_code_coverage()` then `xdebug_stop_code_coverage()` | returns per-file line map (line-exact with Xdebug) | works | `010-line-coverage.phpt`, `011-line-coverage-main.phpt` |
| **php-code-coverage** `Driver::isActive()` | `xdebug_code_coverage_started()` | toggles `false → true → false` | works | `041-started.phpt`, `070-tool-probes.phpt` |
| **php-code-coverage** `Filter` integration | `xdebug_set_filter(XDEBUG_FILTER_CODE_COVERAGE, XDEBUG_PATH_INCLUDE, [dirs])` | restricts coverage to included prefixes | works | `040-filter.phpt` |
| **php-code-coverage** path coverage (`RawCodeCoverageData::fromXdebugWithPathCoverage`) | `xdebug_get_code_coverage(…|XDEBUG_CC_BRANCH_CHECK)` returns per-file `functions => { branches, paths }` | correct shape | works | `020-branch-shape.phpt`, `070-tool-probes.phpt` |
| **php-code-coverage** Cobertura report writer | reads per-branch `op_start`/`op_end`/`line_start`/`line_end`/`hit`/`out`/`out_hit` and per-path `path`/`hit`; tallies `branches-valid`/`branches-covered` | all fields present with correct types | works | `071-cobertura-shape.phpt` |
| **php-code-coverage** Clover report writer | consumes the same line + branch data | works via the shared driver output | works | `071-cobertura-shape.phpt` (branch tally) |

## Test runners

| Tool | What it calls / probes | swiftcov result | Status | Verified by |
|---|---|---|---|---|
| **PHPUnit** (`--coverage-clover`, `--coverage-cobertura`, `--coverage-html`, `--path-coverage`) | drives php-code-coverage's `XdebugDriver` (the coverage rows above) | full line + branch/path coverage | works | coverage rows above |
| **Paratest** | forks PHPUnit workers; each worker resolves the same `XdebugDriver`; relies on `xdebug.mode` containing `coverage` | detected; `xdebug.mode=auto` resolves to `coverage` under a test runner | works | `060-auto-mode.phpt` |
| **Codeception** | uses php-code-coverage under the hood; same driver/probe path; test-runner autodetection via argv `codecept` | works via the coverage path | works | `060-auto-mode.phpt` |
| **Infection** (mutation testing) | reads php-code-coverage's line + (optionally) path coverage to select mutations; no Xdebug-specific probe beyond the driver | works via php-code-coverage output | works | coverage rows above |
| **symfony/error-handler**, **PHPUnit error handling** | when `extension_loaded('xdebug')`, call `xdebug_is_debugger_active()` before adjusting error display | returns `false` (no step debugger) | gap-closed | `070-tool-probes.phpt` |
| **composer/xdebug-handler** | `extension_loaded('xdebug')` + reads the `xdebug.mode` ini to decide whether to restart PHP without Xdebug | detects the extension and honours `xdebug.mode` | works | `060-auto-mode.phpt` |

## Profiling

| Tool | What it consumes | swiftcov result | Status | Verified by |
|---|---|---|---|---|
| **KCachegrind / QCachegrind** | a `cachegrind.out.*` file with `fl=`/`fn=`/`calls=`/summary lines | profiler writes a valid Cachegrind file | works | `050-profiler-cachegrind.phpt` |
| **PhpStorm / Blackfire importer** | the same Cachegrind format | works | works | `050-profiler-cachegrind.phpt` |
| **userland profiler API** | `xdebug_start_trace()` / `xdebug_stop_trace()` / `xdebug_get_profiler_filename()` / `xdebug_get_tracefile_name()` | mapped onto the Cachegrind profiler | works | `050-profiler-cachegrind.phpt` |

## CI coverage consumers

| Tool | What it consumes | swiftcov result | Status |
|---|---|---|---|
| **GitLab coverage** | Cobertura XML `branches-valid` / `branches-covered` from php-code-coverage | populated from the branch data above | works |
| **GitHub / Codecov / Coveralls** | Clover or Cobertura line + branch coverage | produced unchanged by php-code-coverage | works |

## Intentionally unsupported

| Capability | What a tool calls / needs | swiftcov result | Status | Rationale |
|---|---|---|---|---|
| **Step debugging (DBGp)** — PhpStorm / VS Code / `xdebug.mode=debug` | a DBGp socket server negotiating the Xdebug debugger protocol | not implemented; `xdebug.mode=debug` prints a diagnostic and does nothing else | intentionally-unsupported | A partial DBGp implementation is worse than none: an IDE that negotiates a session then hits an unimplemented command hangs. swiftcov is a coverage + profiling engine. The full plan is in [`docs/debugger-spec.md`](debugger-spec.md). |
| **`xdebug_is_enabled()`** (legacy, Xdebug < 3) | some very old tooling probed it | not added | intentionally-unsupported | No current, named consumer calls it — modern tools gate on `extension_loaded('xdebug')` + `xdebug.mode`. Adding it would be speculative API surface. Will be added if a real consumer is identified. |
| **`xdebug_get_function_stack()`** | Xdebug's rich exception/stack-trace formatting | not added | intentionally-unsupported | No coverage/profiling consumer calls it; it belongs to Xdebug's develop/trace mode, which swiftcov does not implement. Adding it would be speculative surface with no honest data to return. |
| **`xdebug_break()` / breakpoint helpers** | step-debugging entrypoints | not added | intentionally-unsupported | Depends on the DBGp debugger, which is intentionally unsupported (above). |

## Notes on the "register as xdebug" posture

swiftcov keeps three deliberate tells so it can be distinguished from real
Xdebug by anything that wants to:

- `xdebug_info()` (no argument) includes `'engine' => 'swiftcov (fast-xdebug)'`.
- `swiftcov_engine()` (and the BC alias `fast_xdebug_engine()`) returns
  `swiftcov <version>`.
- `phpversion('xdebug')` embeds `-swiftcov-` in the version string.

The impersonation is scoped to exactly the probes coverage tooling makes; it is
**not** a claim to implement all of Xdebug. Everything swiftcov does not
implement is listed under *Intentionally unsupported* above, and gated behind an
honest, non-fatal response where a tool would otherwise call an undefined
function.
