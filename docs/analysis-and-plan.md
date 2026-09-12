# fast-xdebug: repository analysis and publishable-product plan

Status: baseline verified on 2026-09 against PHP 8.2.33, 8.3.33, 8.4.24 (all NTS,
preinstalled via mise). This document is the anchor for the subsequent features
FEAT-002..FEAT-005; those features implement against Section B here.

## Verified baseline

The extension was built and exercised on all three preinstalled PHP versions.
Between versions the tree was cleaned (`make clean` + `phpize --clean`) to avoid
stale objects.

| PHP | Build | `.phpt` suite | Identity smoke test |
| --- | ----- | ------------- | ------------------- |
| 8.4.24 | OK (`modules/fast_xdebug.so`, ~265 KB) | 12/12 PASS | `fast-xdebug 0.4.0` / `phpversion("xdebug") = 3.6.99-fast-xdebug-0.4.0` |
| 8.2.33 | OK | 12/12 PASS | pass |
| 8.3.33 | OK | 12/12 PASS | pass |

Identity smoke command (all three assertions pass on every version):

```sh
php -n -d extension="$PWD/modules/fast_xdebug.so" -r '
  assert(extension_loaded("xdebug"));
  assert(version_compare(phpversion("xdebug"),"3.1",">="));
  assert(in_array("coverage", xdebug_info("mode"), true));
  echo fast_xdebug_engine();'
```

The PHPUnit end-to-end harness in `tests/integration/` was **not** run in this
environment: it needs `composer install`, which reaches packagist, and the
sandbox network is INTEGRATIONS_ONLY (no external internet). This is expected and
is not a defect. That harness is exercised by the `integration` job in
`.github/workflows/ci.yml` on GitHub's runners, where packagist is reachable.

The documented `README.md` "Development" commands were checked against what
actually built and passed; they are accurate, so `README.md` is left unchanged in
this feature.

---

## Section A: Repository analysis

### A.1 Architecture

fast-xdebug is a PHP Zend extension (C) that ships as a plain `extension=` module
(like pcov, unlike a Zend extension). Its thesis, recorded in
[`docs/decisions/0002-block-model-and-analysis-cache.md`](decisions/0002-block-model-and-analysis-cache.md),
is that coverage runtime cost drops if each `op_array` is analysed once into basic
blocks and coverage is recorded at block granularity, reconstructing
lines/branches/paths only at collection time. The moving parts:

- **`src/analysis.c`** — decomposes each `op_array` into basic blocks
  (`op -> block` map, block terminators, branch successors). Analysis is cached
  per request in a `HashTable` keyed by the `op_array`'s `opcodes` pointer, since
  `op_array->reserved[]` slots are only available to *Zend* extensions and using
  them would force the heavier Zend-extension packaging. See ADR-0002.
- **`src/coverage.c` / `src/coverage.h`** — the runtime hook. Every opcode type
  is hooked with a deliberately minimal handler (`block_hit[op_to_block[op]] = 1`)
  plus an O(1) single-slot edge cache for branch transitions. Lines, branches and
  paths are reconstructed at `xdebug_get_code_coverage()` time. In-flight frames
  (notably `{main}`, whose `RETURN` has not run at collection) are made
  line-exact via `start_floors` clamping. A saturation fast-path
  (`sat_generation` / `sat_last_opcodes` in `php_fast_xdebug.h`) short-circuits
  hot loops on a pointer compare.
- **`src/profiler.c` / `src/profiler.h`** — a Cachegrind-format profiler driven by
  `zend_observer` (function enter/leave), emitting call edges and a summary. Test:
  `tests/050-profiler-cachegrind.phpt`.
- **`src/debugger.c` / `src/debugger.h`** — a DBGp no-op boundary only; step
  debugging is deliberately not implemented (see
  [`docs/debugger-spec.md`](debugger-spec.md)). This is the largest functional gap
  versus real Xdebug and is called out honestly in `package.xml` notes.
- **`fast_xdebug.c`** — the module entry (~20 KB): the `zend_module_entry`, the
  `xdebug_*` userland functions, `xdebug_info()`, INI handling
  (`xdebug.mode` including `auto`, memory guard), version reporting, and the
  `fast_xdebug_*` sentinel functions.
- **`php_fast_xdebug.h`** — `PHP_FAST_XDEBUG_VERSION`, `FXD_XDEBUG_COMPAT_VERSION`,
  the module globals struct, and the Xdebug-identical numeric flag/filter
  constants (`FXD_CC_*`, `FXD_PATH_*`, `FXD_MAX_PATHS`, `FXD_BRANCH_MAX_OUTS`).

### A.2 Compatibility posture (the core design tension)

There is a deliberate **decoupling of build identity from runtime identity**:

- The **build / package** name is `fast_xdebug`: `PHP_NEW_EXTENSION(fast_xdebug,…)`
  in `config.m4`, `<name>fast_xdebug</name>` and `<providesextension>fast_xdebug`
  in `package.xml`, `"extension-name": "fast_xdebug"` in `composer.json`, and the
  package name `juslintek/fast-xdebug`.
- The **runtime** `zend_module_entry` name is the literal string `"xdebug"`, and
  `phpversion("xdebug")` returns `FXD_XDEBUG_COMPAT_VERSION`
  (`3.6.99-fast-xdebug-<ver>`). This is required so `sebastian/environment`'s
  `Runtime::hasXdebug()` (`extension_loaded('xdebug')`), the `>= 3.1` version
  gate, and `xdebug_info('mode')` containing `coverage` all pass — see
  [`docs/decisions/0001-claim-xdebug-identity.md`](decisions/0001-claim-xdebug-identity.md).
- Impersonation is kept detectable: `fast_xdebug_engine()`,
  `fast_xdebug_resolved_mode()`, `fast_xdebug_recommended_settings()`, and
  `xdebug_info()['engine'] = 'fast-xdebug'`.

### A.3 Test coverage and CI

- 12 `.phpt` unit tests (`tests/001-identity.phpt` … `tests/062-memory-guard.phpt`)
  covering identity, line/branch/path shape, the frameless-internal-call
  regression (the exact shape that segfaulted Xdebug 3.5.0–3.5.3), the filter,
  start/stop state, the Cachegrind profiler, auto mode, recommended settings, and
  the memory guard.
- `tests/integration/` is a PHPUnit 11 e2e harness that runs
  `phpunit --path-coverage --coverage-cobertura` and asserts the report is
  well-formed with branch data. Requires `composer install` (offline-blocked in
  this sandbox; runs in CI).
- `.github/workflows/ci.yml`: build+test matrix PHP 8.2–8.5 NTS (blocking) plus an
  8.4 ZTS row (non-blocking), a load/identity smoke test, and a **valgrind** run on
  the frameless regression. A separate `integration` job runs the PHPUnit harness
  on 8.2–8.4.
- `.github/workflows/release-binaries.yml`: tag-triggered; builds `.so` for
  8.2–8.5 × {nts,zts}, names them descriptively with a sha256, plus a PECL `.tgz`
  and a source tarball, and attaches all to the GitHub release.

### A.4 Prioritized concrete improvements

Ordered high → low. Items marked **[later feature]** are scheduled in Section B.

1. **Packaging identity contains `xdebug`, which PECL central will reject
   [P1, later feature FEAT-002].** `package.xml` declares
   `<name>fast_xdebug</name>` on `<channel>pecl.php.net</channel>` and
   `<providesextension>fast_xdebug</providesextension>`; `composer.json` uses
   `juslintek/fast-xdebug` and `"extension-name": "fast_xdebug"`. A name that
   embeds "xdebug" invites trademark friction and is a rejection risk for a public
   PECL channel (ADR-0001 anticipates this). The fix is a distinct **package**
   name that does not contain "xdebug" while the **runtime** module keeps
   registering as `"xdebug"`. This decoupling already exists structurally, so the
   rename is mechanical but touches many files (see A.4.5, the version/name
   duplication).

2. **No GitLab CI, Docker, or Kubernetes distribution [P1, later feature
   FEAT-003/FEAT-004].** Distribution today is GitHub-only (Releases + PIE
   pre-packaged-binary + PECL tarball + `install.sh`). The user explicitly wants
   install via GitLab pipelines, Docker, and Kubernetes. None of `.gitlab-ci.yml`,
   a `Dockerfile`, or K8s manifests exist yet.

3. **The integration harness is not runnable offline / on non-GitHub CI [P2].**
   `tests/integration/` hard-depends on packagist via `composer install`. That is
   fine for GitHub CI but blocks (a) this sandbox and (b) any air-gapped or
   GitLab-runner verification. Mitigations: document the constraint (done here),
   and in a GitLab pipeline use a composer cache / mirror. It is not a code defect.

4. **Version string is duplicated across four files [P2, later feature].**
   `PHP_FAST_XDEBUG_VERSION` in `php_fast_xdebug.h` (`0.4.0`), `package.xml`
   (`<release>`/`<api>` = `0.4.0`), `CHANGELOG.md`, and the effective compat
   string `FXD_XDEBUG_COMPAT_VERSION`. There is no single source of truth or a
   check that they agree, so a bump can silently drift. A release checklist and/or
   a CI assertion that these match should be added alongside the rename.

5. **`composer.json` keyword and package name embed `xdebug` [P2, later
   feature].** `"name": "juslintek/fast-xdebug"` and the `keywords` list include
   `xdebug`; the Packagist listing name should track the renamed product. (Keeping
   `xdebug` as a *keyword* for discoverability is fine; the *package name* is the
   problem.)

6. **Step debugging (DBGp) is unimplemented [P3, out of scope of publishing].**
   `src/debugger.c` is a no-op boundary. This is a known, documented gap; it does
   not block coverage/profiler adoption but does mean the product is not a full
   Xdebug replacement. Worth stating prominently in the renamed product's README.

7. **README install matrix references `juslintek/fast-xdebug` and the
   `.so` name `fast_xdebug.so` [P3, later feature].** After the rename these paths
   and the Packagist/PIE instructions in `README.md` must be updated in lockstep
   so users are not sent to a dead package name.

8. **`-march=native` guardrails are good; keep them [no action, positive
   finding].** `config.m4` correctly gates `-O3 -march=native` behind an opt-in
   `--enable-fast-xdebug-native` and probes compiler support, and applies
   `-fno-strict-aliasing` to the whole extension (required for correct `-O2`
   codegen on some 8.2/8.3 toolchains). Distribution binaries must **not** use the
   native flag; the release workflow correctly omits it.

---

## Section B: Publishable-product plan (user requests 2–5)

This maps the user's four remaining asks to concrete, feature-sized work and, for
each channel, states what is **achievable as pipeline-as-code + docs in this
sandbox** versus what is **blocked** and requires the owner's credentials and real
network outside the sandbox.

### B.1 Distinct-product identity (user request: "rename so it can be published to PECL/PIE"; request 4: "under a different product, but compile as xdebug")

Design: **package/product identity renamed away from `xdebug`; runtime module
identity stays `"xdebug"`.** This preserves every tool integration (ADR-0001)
while making the package publishable and trademark-clean.

- Pick a product name with **no `xdebug` substring** (e.g. a "swift/fast coverage"
  style name). Apply it to: `composer.json` `name` + `extension-name`,
  `package.xml` `<name>` + `<providesextension>`, `config.m4`
  `PHP_NEW_EXTENSION(...)`, `config.w32`, the source file names/`php_*.h` guard if
  the build extension name changes, `install.sh`, and every doc/CI reference to
  `fast_xdebug.so` / `juslintek/fast-xdebug`.
- **Do not change** `fast_xdebug_module_entry`'s runtime `name = "xdebug"`, the
  `FXD_XDEBUG_COMPAT_VERSION` reporting, or the `xdebug_*` userland function names.
  The `fast_xdebug_*` sentinels can be renamed to the new product but should keep
  an alias for anyone already probing them.
- Add a **single version source of truth** and a CI check that
  `php_*.h`, `package.xml`, `composer.json` and `CHANGELOG.md` agree (improvement
  A.4.4).
- **Achievable here:** the full rename, the version-sync check, and updated docs
  land as code on the branch. **Blocked here:** actually registering the PECL
  channel name / requesting a `pecl.php.net` account and running
  `pecl channel-discover` + upload — needs the owner's PECL credentials and
  network.

### B.2 Distribution automation (user request 3: "installable via GitLab/GitHub pipelines or Docker/Kubernetes … as native and hassle-free as possible vs pcov")

Deliver, as pipeline-as-code and docs:

1. **GitLab CI (`.gitlab-ci.yml`)** — build+`.phpt` matrix across PHP 8.2–8.5, the
   identity smoke test, and (where a composer mirror/cache is available) the
   PHPUnit integration harness. Mirrors the GitHub `ci.yml` semantics. A
   release/tag stage that publishes `.so` artifacts to the GitLab package/generic
   registry.
2. **Docker** — a `Dockerfile` (and ideally per-PHP-version build args) that
   builds the extension and drops the `.so` into `extension_dir`, plus an example
   showing how to `COPY --from` the built `.so` into a user's PHP image so CI
   images get coverage without a compiler. Provide a docker-compose/example too.
3. **Kubernetes** — manifests (a Job/initContainer pattern, or a ConfigMap-mounted
   `.ini`) showing how to inject the extension into a PHP workload for
   coverage-in-cluster CI, plus notes for the more common "bake it into the image"
   path.
4. **GitHub release polish** — the existing `release-binaries.yml` already builds
   the matrix; align its artifact/`.so` names and PECL package with the renamed
   product, and add checksums/provenance notes.

- **Achievable here:** all of the above authored and **syntactically validated**
  (YAML parsed; Dockerfile lint/parse). **Blocked here:** `docker build` cannot
  pull base images (no Docker Hub), so images are delivered as Dockerfiles, not
  built/pushed; pushing to GHCR/GitLab registry needs credentials + network.

### B.3 Xdebug-ecosystem tool-support matrix (user request 4: "investigate what tools exist and bring all that support, but under a different product")

Because the runtime keeps registering as `xdebug` with `phpversion >= 3.1` and a
`coverage` mode, the following ecosystem should already work and must be
**verified/documented** under the new product name:

- **php-code-coverage / PHPUnit** — line, branch and path coverage; Cobertura,
  Clover, HTML, text reports. This is the primary path and is exercised by the
  integration harness.
- **Paratest / parallel runners** — inherit php-code-coverage behaviour.
- **Infection (mutation testing)** — consumes coverage; verify it detects the
  driver.
- **Codeception / Behat** — via php-code-coverage.
- **Cachegrind consumers** (KCachegrind/QCachegrind, `phpstorm` profiler viewer,
  webgrind) — for the `profiler.c` output.
- **CI coverage uploaders** (Codecov, Coveralls) — consume Cobertura/Clover, so no
  driver-specific work.

The matrix, with the exact detection gate each tool uses and the fast-xdebug
status, goes into the renamed product's README. **Achievable here:** the matrix +
the `.phpt`/identity evidence. **Blocked here:** live A/B against real
Xdebug/pcov and running third-party tools that need composer/network.

### B.4 Infrastructure and adoption roadmap (user request 5: "start building and planning support to put it into infrastructure … using the better/faster API")

- **Short term (this task):** rename + distribution automation + tool matrix +
  version-sync check, so the product is installable and publishable through
  pipeline-as-code the moment the owner supplies credentials.
- **Medium term:** publish prebuilt `.so` for the full PHP × {nts,zts} matrix to
  GitHub Releases *and* a GitLab generic package registry; publish the PECL tarball
  to the (renamed) PECL channel; list on Packagist so PIE's
  `pre-packaged-binary` path installs without a compiler — the "hassle-free vs
  pcov" goal. Provide a ready-to-`COPY` CI Docker image.
- **Long term:** the durable fix for the identity tension (ADR-0001, option 2):
  contribute a **first-class coverage driver** to `php-code-coverage` so the
  product can register under its own name instead of impersonating `xdebug`.
  Track the faster block-based collection API (ADR-0002) as the basis for that
  upstream driver, and consider an opcache-persisted analysis cache
  (noted in `docs/design.md`) to remove per-request re-analysis.

### B.5 What requires the owner (out of sandbox), summarized

| Action | In-sandbox deliverable | Needs owner + network |
| ------ | ---------------------- | --------------------- |
| PECL publish | renamed `package.xml`, tarball build in CI | PECL account/channel, upload |
| Packagist listing | renamed `composer.json` | Packagist submit + webhook |
| PIE prebuilt-binary | release workflow + `composer.json` `download-url-method` | tagged release with assets (owner pushes tag) |
| Docker images | `Dockerfile`(s), examples | `docker build`/push to GHCR/GitLab registry |
| Kubernetes | manifests + docs | a real cluster |
| GitLab pipeline | `.gitlab-ci.yml` | a GitLab project + runners |
| A/B benchmarks vs Xdebug/pcov | methodology + harness | real network to install competitors |
