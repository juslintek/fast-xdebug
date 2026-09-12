#!/usr/bin/env sh
# swiftcov — headache-free source install.
#
# Detects php-config, builds the extension, installs the .so into PHP's
# extension_dir, and tells you the exact line to add to php.ini (or appends it
# for you with --enable-ini). Works on any PHP 8.2–8.5 with dev headers.
# swiftcov registers at runtime as 'xdebug' so existing tools detect it.
#
# Usage:
#   ./install.sh                 # build + install, print the ini line
#   ./install.sh --enable-ini    # also append the extension= line to php.ini
#   ./install.sh --native        # build with -O3 -march=native (self-hosted only)
#   PHP_CONFIG=/path/php-config ./install.sh   # target a specific PHP
set -eu

ENABLE_INI=0
CONFIGURE_EXTRA=""
for arg in "$@"; do
  case "$arg" in
    --enable-ini) ENABLE_INI=1 ;;
    --native)     CONFIGURE_EXTRA="$CONFIGURE_EXTRA --enable-swiftcov-native" ;;
    -h|--help)
      sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'
      exit 0 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

# 1. Locate php-config.
PHP_CONFIG="${PHP_CONFIG:-$(command -v php-config || true)}"
if [ -z "$PHP_CONFIG" ]; then
  echo "error: php-config not found. Install PHP dev headers (php-dev / php-devel)" >&2
  echo "       or set PHP_CONFIG=/path/to/php-config" >&2
  exit 1
fi
PHP_BIN="$("$PHP_CONFIG" --php-binary 2>/dev/null || true)"
PHPIZE="$(dirname "$PHP_BIN")/phpize"
[ -x "$PHPIZE" ] || PHPIZE="$(command -v phpize || true)"
if [ -z "$PHPIZE" ] || [ ! -x "$PHPIZE" ]; then
  echo "error: phpize not found next to php-config or on PATH" >&2
  exit 1
fi

PHP_VERSION="$("$PHP_CONFIG" --version)"
EXT_DIR="$("$PHP_CONFIG" --extension-dir)"
echo ">> Building swiftcov for PHP $PHP_VERSION"
echo ">> extension_dir: $EXT_DIR"

# 2. Build.
"$PHPIZE" >/dev/null
# shellcheck disable=SC2086
./configure --enable-swiftcov $CONFIGURE_EXTRA --with-php-config="$PHP_CONFIG" >/dev/null
make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"

# 3. Install.
make install >/dev/null 2>&1 || {
  # make install may need root; fall back to a manual copy.
  cp modules/swiftcov.so "$EXT_DIR/" 2>/dev/null || {
    echo "error: could not copy swiftcov.so to $EXT_DIR (try sudo)" >&2
    exit 1
  }
}
echo ">> Installed swiftcov.so to $EXT_DIR"

# 4. Wire up php.ini.
INI_LINE="extension=swiftcov.so"
INI_FILE="$(php --ini 2>/dev/null | awk -F': ' '/Loaded Configuration File/ {print $2}')"
if [ "$ENABLE_INI" = "1" ] && [ -n "$INI_FILE" ] && [ "$INI_FILE" != "(none)" ]; then
  if grep -q "swiftcov.so" "$INI_FILE" 2>/dev/null; then
    echo ">> $INI_FILE already loads swiftcov.so"
  else
    printf '\n%s\n' "$INI_LINE" >> "$INI_FILE"
    echo ">> Added '$INI_LINE' to $INI_FILE"
  fi
else
  echo ""
  echo ">> Add this line to your php.ini to enable it:"
  echo "     $INI_LINE"
  [ -n "$INI_FILE" ] && [ "$INI_FILE" != "(none)" ] && echo "   (your php.ini: $INI_FILE — or re-run with --enable-ini)"
fi

echo ""
echo ">> Verify:  php -m | grep -i xdebug   (swiftcov registers as 'xdebug')"
echo ">> It defaults to coverage mode; set xdebug.mode=auto for heuristic detection."
