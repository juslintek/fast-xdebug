# Adoption roadmap: putting swiftcov into infrastructure

This document is the plan for adopting swiftcov as the coverage engine in real
CI/CD and container infrastructure, and for migrating off legacy Xdebug or pcov.
It complements the design rationale in
[docs/design.md](design.md), the identity decisions in
[ADR-0001](decisions/0001-claim-xdebug-identity.md) and
[ADR-0003](decisions/0003-product-identity-vs-runtime-name.md), and the verified
consumer matrix in [docs/tool-compatibility.md](tool-compatibility.md).

The positioning is simple: **as native and hassle-free as pcov to drop in, but
with the branch/path coverage that pcov cannot produce**, and faster than
legacy Xdebug for coverage.

## 1. Compatibility-first rollout (drop-in, no pipeline change)

swiftcov registers at runtime as the module `xdebug`
([ADR-0001](decisions/0001-claim-xdebug-identity.md)). Every consumer that gates
on `extension_loaded('xdebug')`, `phpversion('xdebug') >= 3.1`, and
`xdebug_info('mode')` containing `coverage` selects it unchanged. The full,
per-probe list is verified in
[docs/tool-compatibility.md](tool-compatibility.md).

Because the reports it feeds php-code-coverage are **Cobertura- and
Clover-compatible with populated `branches-valid` / `branches-covered`**, a
GitLab or GitHub/Codecov/Coveralls coverage pipeline consumes the output with no
change. The migration is therefore a *swap of the coverage driver*, not a
pipeline rewrite:

1. Remove pcov / Xdebug from the image or ini.
2. Enable swiftcov (`extension=swiftcov.so`, `xdebug.mode=coverage`).
3. Keep the existing `phpunit --coverage-*` / `--path-coverage` invocation.

The rest of this repo ships the one-line adoption paths so step 2 is trivial:

- **GitHub composite action:**
  [`.github/actions/setup-swiftcov`](../.github/actions/setup-swiftcov/action.yml)
  ```yaml
  - uses: juslintek/swiftcov/.github/actions/setup-swiftcov@v0.5.0
    with:
      php-version: '8.4'
      mode: coverage
  ```
- **GitHub reusable workflow:**
  [`.github/workflows/reusable-coverage.yml`](../.github/workflows/reusable-coverage.yml)
  ```yaml
  jobs:
    coverage:
      uses: juslintek/swiftcov/.github/workflows/reusable-coverage.yml@v0.5.0
      with:
        php-version: '8.4'
  ```
- **GitLab include template:**
  [`ci/templates/swiftcov.gitlab-ci.yml`](../ci/templates/swiftcov.gitlab-ci.yml)
  ```yaml
  include:
    - project: 'juslintek/swiftcov'
      ref: v0.5.0
      file: '/ci/templates/swiftcov.gitlab-ci.yml'

  coverage:
    extends: .swiftcov-coverage
  ```
- **Docker / Kubernetes:** bake `swiftcov.so` into a `php:<ver>-cli` base (see
  [docker/README.md](../docker/README.md)) or run the in-cluster coverage `Job`
  (see [deploy/k8s/README.md](../deploy/k8s/README.md)).
- **Bootstrap helper:** run
  [`scripts/swiftcov-bootstrap.php`](../scripts/swiftcov-bootstrap.php) inside a
  target environment to print an advisory ini snippet tuned to its
  memory_limit / CPU / OPcache. It never mutates anything.

## 2. Risk controls

- **Cannot co-load with real Xdebug.** Because swiftcov claims the `xdebug`
  module name, loading it alongside a genuine Xdebug build is a hard conflict
  (two extensions registering the same module). The rollout must *replace*
  Xdebug, not sit beside it. Audit images/ini for an existing `xdebug.so` before
  enabling swiftcov.
- **memory_guard is on by default.** Branch/path enumeration can be expensive on
  large files; swiftcov adapts the path cap under memory pressure automatically
  (`fast_xdebug.memory_guard`, `fast_xdebug.max_paths`). Keep the guard on in
  memory-constrained runners; the bootstrap helper suggests a cap for the
  detected `memory_limit`.
- **Branch percentages are not byte-identical to Xdebug.** Per
  [docs/design.md section 7](design.md), line coverage is byte-identical to pcov
  and Xdebug, and branch topology/shape and totals match per-function, but the
  branch-*rate* accounting differs from Xdebug's `hit_branch` transition-indexing
  (a fully-run `foreach` can read 0.25 under Xdebug vs 1.0 here). Teams that gate
  a build on an absolute branch-coverage threshold should re-baseline that
  threshold against swiftcov before enforcing it, rather than assume parity with
  a prior Xdebug number.
- **Coverage state is per-request** and ZTS caches are request-scoped; this is a
  correctness note for long-lived worker processes, not a blocker.

## 3. Phased distribution plan

Distribution is staged so the lowest-friction, most controllable channels come
first and the ecosystem-registry channels follow once the identity has settled
([ADR-0003](decisions/0003-product-identity-vs-runtime-name.md)).

1. **Prebuilt binaries + Docker images (now).** Tagged GitHub/GitLab releases
   attach a per-PHP-version `.so` plus a source tarball, and the multi-stage
   `Dockerfile` publishes `juslintek/swiftcov:php<ver>` images. This is the
   compiler-free, "as native as pcov" path and needs no registry acceptance.
2. **PIE + Packagist (next).** `composer.json` already declares `type: php-ext`
   with a `php-ext` block, so PIE can install it and Packagist can index the
   `juslintek/swiftcov` package. The package name deliberately omits the word
   "xdebug" so these registries accept it.
3. **PECL (optional, later).** A `pecl package` tarball is produced by the
   release pipeline. PECL central would reject a package literally named
   `xdebug` ([ADR-0001](decisions/0001-claim-xdebug-identity.md)); publishing as
   `swiftcov` sidesteps that, so PECL becomes an optional additional channel
   rather than a prerequisite.

Publishing to any registry needs the owner's credentials and real network
access; the pipelines produce the assets but never perform credentialed uploads
automatically. See [docs/publishing.md](publishing.md) for the manual steps.

## 4. Long-term end-state: a first-class php-code-coverage driver

The durable direction, recorded in
[ADR-0001](decisions/0001-claim-xdebug-identity.md), is to stop impersonating
`xdebug` and instead ship swiftcov as its **own first-class driver in
php-code-coverage upstream** (a `SwiftcovDriver` selected on its own merits). At
that point:

- swiftcov no longer needs to claim the `xdebug` module name;
- tools select it explicitly instead of by Xdebug-probe coincidence;
- the branch-rate semantics can be documented as swiftcov's own contract rather
  than measured against Xdebug parity.

Until that driver lands upstream, the compatibility-first posture in section 1
is what makes adoption zero-config today. The two are sequential: ship
compatibility now, contribute the native driver as the end-state.

## 5. Observability and rollback

**Detecting that swiftcov (not real Xdebug) is active**, for dashboards, smoke
tests, or gating:

```php
// canonical sentinel - returns e.g. "swiftcov 0.5.0"
echo swiftcov_engine();

// or via the info array
$info = xdebug_info();          // no argument
echo $info['engine'];           // "swiftcov (fast-xdebug)"
```

Both are cheap to assert in a CI step, so a pipeline can fail fast if it somehow
booted with the wrong (or no) coverage engine.

**Rollback** is a one-line revert because nothing above the driver changed:

- Docker/ini: remove `extension=swiftcov.so` and reinstall pcov
  (`extension=pcov.so`, `pcov.enabled=1`) or Xdebug (`xdebug.mode=coverage`).
- GitHub Action / reusable workflow: drop the `setup-swiftcov` step (or the
  `uses:` of the reusable workflow) and restore your previous `setup-php`
  `coverage:` input.
- GitLab: remove the `include:` and the `extends: .swiftcov-coverage`.

Because swiftcov emits the same Cobertura/Clover shape as pcov/Xdebug, reverting
does not change how any downstream coverage consumer reads the report; only the
producing engine changes back.
