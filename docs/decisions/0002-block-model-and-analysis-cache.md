# ADR-0002: Basic-block model, per-request analysis cache, hook-every-opcode

## Status
Accepted.

## Context
The thesis is that coverage runtime cost can be reduced by analysing each
`op_array` once and recording coverage at block granularity, instead of doing
work per executed opcode as Xdebug and pcov do.

Two sub-decisions had to be made empirically:

### Runtime hook
A terminator-only hook (hook just the block-ending opcodes and reconstruct the
fall-through run) was implemented first. It failed to observe the blocks of an
**in-flight** frame — most importantly `{main}`, whose `RETURN` has not executed
when `xdebug_get_code_coverage()` is called — and could not reconstruct the
executed-block set exactly. Instrumentation (diffing against pcov/Xdebug) showed
this directly.

We therefore **hook every opcode type**, but keep the handler to a single array
store (`block_hit[op_to_block[op]] = 1`). This is O(opcodes) like the incumbents
but with far less per-opcode work, and measured ~7× vs no-coverage (vs Xdebug
18.7× line / 99.7× branch).

### Analysis cache location
`op_array->reserved[]` slots (the natural place to cache per-op_array analysis)
are only allocated for **Zend** extensions. To keep packaging as a plain
`extension=` module (like pcov), we instead cache analysis in a per-request
`HashTable` keyed by the op_array's `opcodes` pointer, which is stable for the
request.

## Decision
- Hook every opcode with a minimal handler; reconstruct lines/branches/paths at
  collection time from `block_hit` (+ an O(1) edge cache for branch mode).
- Cache analysis per request in a hashtable keyed by `opcodes` pointer.
- Achieve line-exactness for in-flight frames via `start_floors`/`frontiers`
  clamping of the open block's reported line range.

## Consequences
- Runtime cost is the same for line and branch/path modes (path enumeration is
  at collection time), which is the whole performance win.
- Analysis is recomputed once per op_array per request (not persisted across
  requests). A future optimisation could key a cache on file identity + mtime or
  piggy-back opcache's persisted op_arrays (noted in design.md, not implemented).
- Because we do not use `reserved[]`, the extension can ship as a normal module,
  simplifying installation and PIE packaging.
