# ADR-0001: Register under the module name `xdebug`

## Status
Accepted (revisit when a first-class php-code-coverage driver is viable).

## Context
`php-code-coverage` picks a coverage driver through `sebastian/environment`:
`Runtime::hasXdebug()` is literally `extension_loaded('xdebug')`, and
`Driver\Selector` will only use branch/path coverage from the Xdebug driver.
There is no public extension point to register a third driver without changing
`php-code-coverage` itself.

Two options:
1. Register our module as `xdebug`, report `phpversion('xdebug') >= 3.1`, and
   implement `xdebug_info('mode') -> ['coverage', ...]`. Works with every
   existing tool today, no upstream change.
2. Submit a first-class driver to `php-code-coverage` upstream. Cleaner and
   durable, but slow and outside our control, and provides no value until
   released and adopted.

## Decision
Take option 1 now, so the extension is immediately useful as a drop-in coverage
backend. Mitigate the impersonation by:
- exposing `fast_xdebug_engine()` and an `"engine" => "fast-xdebug"` field in
  `xdebug_info()` so tools/humans can distinguish the two;
- reporting the version as `3.6.99-fast-xdebug-<version>` rather than a real
  Xdebug version string.

## Consequences
- Cannot be loaded alongside real Xdebug (both would claim `xdebug`). Documented.
- Invites friction with the Xdebug project; the version sentinel and engine
  marker are there to keep the impersonation honest and detectable.
- The intended long-term direction is option 2 (upstream driver); this ADR
  should be revisited then.
