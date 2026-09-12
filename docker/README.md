# swiftcov Docker images

Prebuilt-style, compiler-free coverage for your CI and local runs. The image
bakes `swiftcov.so` into a `php:<ver>-cli` base and enables it via
[`swiftcov.ini`](swiftcov.ini). At runtime the extension registers as the module
`xdebug`, so PHPUnit / `php-code-coverage` pick it up with **no configuration**,
while giving you branch/path coverage that pcov cannot.

## Build

The [root `Dockerfile`](../Dockerfile) is multi-stage: a builder compiles the
extension, the final image carries only the `.so` + ini (no C toolchain).

```sh
# default PHP (8.4)
docker build -t swiftcov:php8.4 .

# pick a PHP version with the build ARG
docker build --build-arg PHP_VERSION=8.3 -t swiftcov:php8.3 .
docker build --build-arg PHP_VERSION=8.2 -t swiftcov:php8.2 .
```

> Building pulls `php:<ver>-cli` from a registry, so it needs network access. In
> an offline / restricted sandbox this step cannot run; build it in real CI or
> on a networked machine.

## Run your test suite with coverage

Mount your project and run PHPUnit with path coverage:

```sh
docker run --rm -v "$PWD":/app -w /app swiftcov:php8.4 \
  vendor/bin/phpunit --path-coverage --coverage-cobertura=cobertura.xml
```

`xdebug.mode` defaults to `coverage`. Nothing else to configure — the produced
Cobertura report carries `branches-valid` / `branches-covered`, so a GitLab or
GitHub coverage pipeline consumes it unchanged.

## Add swiftcov to your OWN image (compiler-free, one COPY)

You do not need to rebuild from source in your project image. Copy the compiled
`.so` out of the published swiftcov image in a single stage — this is the
"as native as pcov" install path:

```dockerfile
FROM php:8.3-cli

# Pull the prebuilt extension from the matching swiftcov image.
# The extension_dir layout matches because the base tag (php:8.3-cli) matches.
COPY --from=juslintek/swiftcov:php8.3 \
     /usr/local/lib/php/extensions/ \
     /usr/local/lib/php/extensions/
COPY --from=juslintek/swiftcov:php8.3 \
     /usr/local/etc/php/conf.d/swiftcov.ini \
     /usr/local/etc/php/conf.d/swiftcov.ini

# your app...
```

Match the PHP version tag (`php8.2` / `php8.3` / `php8.4`) to your base image so
the extension ABI matches.

## Verify inside a container

```sh
docker run --rm swiftcov:php8.4 \
  php -r 'assert(extension_loaded("xdebug")); echo swiftcov_engine(), PHP_EOL;'
```

This prints `swiftcov <version>` — confirming the swiftcov engine is active even
though it answers to the `xdebug` module name.
