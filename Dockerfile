# swiftcov — multi-stage image.
#
# Stage 1 (builder): compile swiftcov from source against the chosen PHP.
# Stage 2 (final):   a slim php:<ver>-cli that carries ONLY the compiled
#                    swiftcov.so + the ini, so the runtime image has no C
#                    toolchain.
#
# The build/package name is "swiftcov" (modules/swiftcov.so, --enable-swiftcov)
# but the extension registers at RUNTIME as the module "xdebug" so PHPUnit /
# php-code-coverage detect it unchanged.
#
# Build for a specific PHP:
#   docker build --build-arg PHP_VERSION=8.3 -t swiftcov:php8.3 .
#   docker build --build-arg PHP_VERSION=8.4 -t swiftcov:php8.4 .
#
# NOTE: building requires pulling php:<ver>-cli from a registry (network). In an
# offline/INTEGRATIONS_ONLY sandbox this cannot run; build it in real CI or
# locally.

ARG PHP_VERSION=8.4

# ---- builder ------------------------------------------------------------
FROM php:${PHP_VERSION}-cli AS builder

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        autoconf \
        build-essential \
        pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /usr/src/swiftcov
COPY . .

# Compile and install into the image's extension_dir. `make install` places
# swiftcov.so under `php-config --extension-dir`.
RUN set -eux; \
    phpize; \
    ./configure --enable-swiftcov --with-php-config="$(command -v php-config)"; \
    make -j"$(nproc)"; \
    make install; \
    # capture the resolved extension_dir so the final stage can COPY it
    php-config --extension-dir > /tmp/ext_dir

# ---- final --------------------------------------------------------------
FROM php:${PHP_VERSION}-cli

# Bring over just the compiled extension and drop it into this image's
# extension_dir (identical layout because the base image tag matches).
COPY --from=builder /usr/local/lib/php/extensions/ /usr/local/lib/php/extensions/

# Enable the extension + default to coverage mode.
COPY docker/swiftcov.ini /usr/local/etc/php/conf.d/swiftcov.ini

# Sanity: the module must be loadable and answer as xdebug.
RUN php -m | grep -qi xdebug \
    && php -r 'assert(extension_loaded("xdebug")); assert(in_array("coverage", xdebug_info("mode"), true)); echo swiftcov_engine(), PHP_EOL;'

CMD ["php", "-v"]
