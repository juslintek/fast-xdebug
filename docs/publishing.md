# Publishing runbook (owner)

How swiftcov gets released and distributed. It separates what the **CI pipelines
do automatically** from what **requires the owner's credentials + network** and
is therefore performed by hand, outside CI (or with protected CI variables).

> **Naming reminder.** The product/package is **swiftcov** everywhere it is
> published (Composer `juslintek/swiftcov`, PECL `swiftcov`, extension
> `swiftcov.so`). At **runtime** the module still registers as `xdebug` so
> `php-code-coverage` / PHPUnit detect it. This split is the whole reason a
> PECL/Packagist submission is now viable: the *package* name no longer collides
> with Xdebug's, even though the module answers to `xdebug`.

## 0. Pre-release checklist

- Bump `PHP_SWIFTCOV_VERSION` in `php_swiftcov.h`.
- Match it in `package.xml` (`<release>` / `<api>`) and add a `CHANGELOG.md`
  entry.
- Green pipeline on `main` (GitHub Actions + GitLab CI): `.phpt` suite, valgrind
  gate, and the PHPUnit `--path-coverage` integration harness.

> **Sandbox vs CI.** The Docker image build, the PHPUnit `--path-coverage`
> integration harness (`tests/integration/`), a real `pecl install` from the
> tarball, and the valgrind leak gate all require network access (pulling base
> images / Composer packages) or tooling that is not present in the offline
> development sandbox, so they are **not** run there. They run on the networked
> CI runners: the `.phpt` suite, valgrind gate, and integration matrix on every
> push / PR, and the Docker image + tarball jobs on a `v*` tag. The drop-in
> compatibility and leak-freeness claims rest on those CI gates.

## 1. Tag a release — *automated in CI*

```sh
git tag vX.Y.Z
git push origin vX.Y.Z
```

Pushing a `v*` tag triggers:

- **GitHub** — `.github/workflows/release-binaries.yml`:
  - builds prebuilt `swiftcov-<ver>-php<X.Y>-<nts|zts>-linux-x86_64-api<N>.so`
    (+ `.sha256`) for PHP 8.2–8.5, NTS and ZTS, and uploads them to the GitHub
    Release;
  - builds a `pecl package` tarball + a source tarball and attaches them;
  - **optionally** builds/pushes the Docker image *if* `DOCKERHUB_USERNAME` /
    `DOCKERHUB_TOKEN` secrets are set (skips cleanly otherwise).
- **GitLab** — `.gitlab-ci.yml` `release` stage: builds the `.so` + `pecl`
  tarball and publishes them as a GitLab Release via `release-cli`.

Everything above runs on the CI runners with no owner interaction beyond pushing
the tag.

## 2. Packagist (Composer) — *one-time, owner-credentialed*

`composer.json` is Composer-valid (`"type": "php-ext"`). One-time:

1. Log in to <https://packagist.org> as the package owner.
2. **Submit** `https://github.com/juslintek/swiftcov` (or the GitLab mirror).
3. Enable the GitHub/GitLab **auto-update webhook** so new tags publish
   automatically.

After that, `composer require juslintek/swiftcov` resolves and PIE
(`pie install juslintek/swiftcov`) can install using the prebuilt release
binaries via the `pre-packaged-binary` download method.

*Requires: Packagist login. Network: yes. Not run in CI.*

## 3. PIE consumption — *no owner action*

PIE reads the same `composer.json`. Once the package is on Packagist and a tag
has release binaries attached, `pie install juslintek/swiftcov` fetches a
prebuilt `.so` matching the user's PHP, or builds from source as a fallback.
Nothing to publish separately.

## 4. Docker images — *owner-credentialed + network*

CI can build/push automatically when registry secrets exist (see step 1). To do
it manually:

```sh
# build per PHP version
docker build --build-arg PHP_VERSION=8.2 -t juslintek/swiftcov:php8.2 .
docker build --build-arg PHP_VERSION=8.3 -t juslintek/swiftcov:php8.3 .
docker build --build-arg PHP_VERSION=8.4 -t juslintek/swiftcov:php8.4 .

# push (needs `docker login` to the registry)
docker push juslintek/swiftcov:php8.2
docker push juslintek/swiftcov:php8.3
docker push juslintek/swiftcov:php8.4
```

Downstream users then get a compiler-free install via `COPY --from=...` (see
[../docker/README.md](../docker/README.md)).

*Requires: registry login (`DOCKERHUB_USERNAME`/`DOCKERHUB_TOKEN` or
`docker login`). Network: yes (pulls `php:<ver>-cli` base). Not runnable in an
offline sandbox.*

## 5. PECL submission — *owner-credentialed + review*

Now that the package name is `swiftcov` (not `xdebug`), central PECL submission
is feasible. It is a manual, reviewed process:

1. Ensure `package.xml` validates: `pecl package-validate package.xml`.
2. Build the tarball: `pecl package package.xml` → `swiftcov-<ver>.tgz`.
3. Register for a PECL account and propose the package at
   <https://pecl.php.net/account-request.php> / the "new package" workflow.
4. Go through PECL review. **Caveat for reviewers:** the *package* is `swiftcov`,
   but the *runtime module* deliberately registers as `xdebug` for tool
   compatibility (see
   [ADR-0001](decisions/0001-claim-xdebug-identity.md)). Disclose this up front;
   it is detectable via `swiftcov_engine()` and
   `xdebug_info()['engine'] === "swiftcov (fast-xdebug)"`.
5. On acceptance, `pecl install swiftcov` works; tag-built `.tgz` assets on the
   GitHub/GitLab releases cover the interim.

*Requires: PECL account + manual review. Network: yes. Not run in CI.*

### Expected `pecl install` behaviour with the two-identity design

`package.xml` declares `<providesextension>swiftcov</providesextension>`, but the
built module registers its `zend_module_entry` name as the literal `xdebug` (this
is deliberate — it is what makes `extension_loaded('xdebug')` true so
php-code-coverage / PHPUnit detect it). PECL derives the *installed* extension
name from the loaded module entry, so its post-install "provides" check compares
the promised name (`swiftcov`) against the runtime name (`xdebug`) and they do
not match. Concretely, expect this at the end of an otherwise-successful install:

```text
install ok: channel://pecl.php.net/swiftcov-X.Y.Z
Warning: channel://pecl.php.net/swiftcov-X.Y.Z: this package does not provide
extension "swiftcov" (it provides "xdebug")
```

This is expected and harmless: `swiftcov.so` is compiled and copied to the
extension directory correctly, and once enabled (`extension=swiftcov.so`) the
module loads and answers to `xdebug` as intended. The message is a naming
notice, not a build or install failure — verify the install by loading the
extension and checking `php -d extension=swiftcov.so -r "var_dump(extension_loaded('xdebug'), swiftcov_engine());"`
rather than by PECL's provides check.

We keep `<providesextension>swiftcov</providesextension>` (rather than switching
it to `xdebug`) on purpose: declaring `xdebug` would silence the warning but
reintroduce the exact `xdebug`-in-package-metadata collision the rename set out
to avoid. Documenting the expected notice is the more defensible trade-off. On
strict PECL versions that treat the mismatch as an error rather than a warning,
install the `.so` from the tag-built release assets (GitHub/GitLab) or via the
Docker image instead.

> This concrete behaviour was documented from PECL's known post-install
> provides-check semantics; it cannot be exercised from the offline development
> sandbox (see below). A real `pecl install` from the built tarball runs as part
> of the release verification on a networked host / CI runner.

## Automated vs manual — summary

| Step | Automated in CI | Needs owner creds + network |
|---|---|---|
| Build/test/valgrind/integration | ✅ (push / PR) | — |
| Prebuilt `.so` + PECL tarball on tag | ✅ (tag push) | — |
| GitLab Release assets on tag | ✅ (tag push) | — |
| Docker build/push on tag | ✅ *if secrets set* | ✅ otherwise / to push |
| Packagist submission + webhook | — | ✅ (one-time) |
| PIE install | ✅ (uses Packagist + release binaries) | — |
| Docker manual build/push | — | ✅ |
| PECL central submission | — | ✅ (reviewed) |

Nothing here uploads to a registry from the development sandbox — that
environment is network-restricted by design. All live publishing happens from
the owner's machine or from CI runners holding the credentials.
