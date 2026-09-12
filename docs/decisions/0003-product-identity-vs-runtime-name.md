# ADR-0003: Distinct product/package identity (`swiftcov`) vs runtime name (`xdebug`)

## Status
Accepted.

## Context
The extension needs to be *publishable* to the PHP ecosystem's distribution
channels (PECL, PIE, Packagist) and installable "as native as possible" in
CI/CD, Docker and Kubernetes. Two hard constraints pull in opposite directions:

1. **Tool detection requires the runtime name `xdebug`.** As recorded in
   ADR-0001, `php-code-coverage` selects a coverage driver via
   `Runtime::hasXdebug()` == `extension_loaded('xdebug')`, gates branch/path
   coverage on the Xdebug driver, and checks `phpversion('xdebug') >= 3.1` and
   `xdebug_info('mode')` containing `'coverage'`. There is no public extension
   point to register a third driver. So at runtime the `zend_module_entry` name
   MUST be the literal string `"xdebug"`.

2. **A publishable package name must NOT be `xdebug` (or contain it).** The
   previous build/package name `fast_xdebug` literally contains "xdebug", which
   is exactly why PECL central would reject it and why it creates trademark
   friction with the Xdebug project. PECL/PIE/Packagist accept a distinct
   product name.

These are two *different* identities: the compile-time/package identity and the
runtime module identity. They were already decoupled in the code (the module
entry name string was independent of the `PHP_NEW_EXTENSION` name).

## Decision
Give the project a distinct, publishable product/package name **`swiftcov`**
that does not contain "xdebug", while keeping the runtime module name `"xdebug"`.

- Build/package identity is `swiftcov`: `--enable-swiftcov`,
  `PHP_NEW_EXTENSION(swiftcov)`, `HAVE_SWIFTCOV`, source `swiftcov.c` +
  `php_swiftcov.h`, artifact `modules/swiftcov.so`, composer
  `juslintek/swiftcov` with `php-ext.extension-name = swiftcov`, PECL
  `<name>swiftcov</name>` / `<providesextension>swiftcov</providesextension>`.
- Runtime identity is unchanged: `zend_module_entry` name stays `"xdebug"`,
  `phpversion('xdebug')` stays `3.6.99-swiftcov-<version>`, and the userland
  `xdebug_*` functions keep their exact names.
- The sentinel helpers gain canonical `swiftcov_*` names
  (`swiftcov_engine()` etc.) and the old `fast_xdebug_*` helpers are retained
  as documented BC aliases. `xdebug_info()['engine']` reports
  `"swiftcov (fast-xdebug)"`.

## Consequences
- The package is now nameable and submittable on pecl.php.net / PIE /
  Packagist because its name is not "xdebug". Submission still requires review,
  and the fact that the runtime still answers as `xdebug` (per ADR-0001) remains
  the reviewer-discussion point; this ADR does not change that posture.
- Because the runtime name is still `xdebug`, swiftcov **still cannot be loaded
  alongside real Xdebug** (both would claim `xdebug`). Unchanged from ADR-0001.
- All install docs, the build artifact, the configure flag and the CI/release
  tooling now refer to `swiftcov`. Anything that referenced the old
  `fast_xdebug_*` helper functions keeps working via the BC aliases.
- References ADR-0001 (why the runtime name is `xdebug`) and ADR-0002.
