# Step debugger (DBGp) — specification and plan

Status: **not implemented.** This document is the design so it can be built
without re-discovering the protocol, and records *why* it is intentionally a
no-op today.

## Why a partial debugger is worse than none

IDEs (PhpStorm, VS Code + PHP Debug) drive step debugging over **DBGp**, a
line-oriented protocol Xdebug pioneered. The IDE:

1. connects (or receives a connection) on a TCP port,
2. negotiates features (`feature_set`/`feature_get`),
3. sets breakpoints (`breakpoint_set`),
4. and issues `run`/`step_into`/`step_over`/`step_out`, reading `stack_get`,
   `context_get`, `property_get`, `eval`, etc.

If the extension opens a session and then does not honour a command the IDE
depends on, the IDE typically **hangs waiting for a response** or drops into an
error state — a strictly worse experience than the debugger simply being absent.
Because fast-xdebug also *reports itself as Xdebug*, IDEs will apply
Xdebug-specific behaviour and quirks, raising the fidelity bar further.

Therefore: with `xdebug.mode=debug`, fast-xdebug emits an `E_NOTICE` and does
**not** open a listening session.

## Architecture when implemented

Reuse the same cheap-hook philosophy as coverage/profiling:

- **Statement stepping** via `zend_observer`'s statement/opline hook (PHP 8.0+
  exposes per-statement observation without overriding `zend_execute_ex`), gated
  so it is only armed while a debug session is attached and stepping.
- **Breakpoints**: a map keyed by `(file, line)` and by function name; on each
  observed statement, O(1) lookup; only pause when the current position matches.
- **DBGp server**: a small blocking socket server (connect out to
  `xdebug.client_host:client_port`, default `localhost:9003`, matching Xdebug 3),
  speaking the `length\0payload\0` DBGp framing with XML payloads.
- **State inspection**: `stack_get` from `EG(current_execute_data)` chain;
  `context_get`/`property_get` by walking `CV`s and symbol tables; `eval` via
  `zend_eval_string` in the paused frame's scope.

### Command coverage needed for a usable MVP

`feature_get`/`feature_set`, `status`, `stack_get`, `context_names`,
`context_get`, `property_get`, `breakpoint_set` (line + call/return),
`breakpoint_remove`, `run`, `step_into`, `step_over`, `step_out`, `stop`,
`eval`, `source`.

### INI surface (Xdebug-compatible names)

`xdebug.mode=debug`, `xdebug.start_with_request`, `xdebug.client_host`,
`xdebug.client_port`, `xdebug.idekey`.

## Performance thesis

The step debugger's cost is dominated by the per-statement observer while
stepping; when *running* (not single-stepping) between breakpoints, the observer
should fall back to a breakpoint-only check (no per-statement work on lines with
no breakpoint), which is where a faster-than-Xdebug design would come from.
Coverage and profiling already demonstrate the pattern.

## Scope decision

The clean internal boundary exists today (`src/debugger.{c,h}` + the
`fxd_debugger_*` lifecycle hooks) so this can be built as a self-contained
subsystem without touching coverage or profiling. Shipping it also warrants
revisiting ADR-0001 (the "register as `xdebug`" posture), because faithful IDE
compatibility is the case where impersonating Xdebug is both hardest and most
likely to mislead users.
