# Repository guidance

This is a C PHP extension that exposes coverage and profiling through the Xdebug-compatible `xdebug` module identity. Step debugging is not implemented. Read `README.md` and `.github/workflows/ci.yml` before changing its runtime or compatibility behavior.

- Build the extension for the selected PHP ABI with `phpize`, `./configure --enable-fast-xdebug --with-php-config="$(command -v php-config)"`, and `make`. For development, load `modules/fast_xdebug.so` with `php -n`; do not run `install.sh` or `make install`, which change the host PHP installation.
- Run the `.phpt` suite with the built extension:

  ```sh
  php -n run-tests.php -q -p "$(command -v php)" -d extension="$PWD/modules/fast_xdebug.so" tests/*.phpt
  ```

- For driver integration changes, run the CI PHPUnit path-coverage harness:

  ```sh
  cd tests/integration
  composer install --no-interaction
  php -n -d extension="$PWD/../../modules/fast_xdebug.so" -d memory_limit=512M ./vendor/bin/phpunit --path-coverage --coverage-cobertura=cobertura.xml
  ```

  CI checks PHP 8.2–8.4 and verifies the generated Cobertura report. The extension CI matrix also covers PHP 8.2–8.5 NTS and PHP 8.4 ZTS, plus a Valgrind regression check.
- Keep the default build portable. `--enable-fast-xdebug-native` uses host-specific CPU tuning and must not be used for distributed binaries. Do not skip tests or weaken expected results to make a run pass.
