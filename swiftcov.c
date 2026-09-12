/*
   +----------------------------------------------------------------------+
   | swiftcov: an Xdebug-API-compatible, block-based code coverage         |
   | engine for PHP 8.2 - 8.5.                                             |
   |                                                                      |
   | Registers under the module name "xdebug" and exposes the subset of   |
   | Xdebug's coverage API that php-code-coverage / PHPUnit probe, so it   |
   | is a drop-in coverage backend. It is NOT a debugger, profiler or     |
   | tracer. See docs/design.md for the compatibility posture and its     |
   | risks.                                                               |
   |                                                                      |
   | This source file is subject to the 2-Clause BSD license.             |
   +----------------------------------------------------------------------+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "Zend/zend_extensions.h"
#include "Zend/zend_smart_str.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifndef PHP_WIN32
#include <unistd.h>
#endif

#include "php_swiftcov.h"
#include "src/coverage.h"
#include "src/profiler.h"
#include "src/debugger.h"

ZEND_DECLARE_MODULE_GLOBALS(swiftcov)

/* INI: xdebug.mode (we honour "coverage"/"off"), so xdebug_info('mode') works.
 * Registered as a plain (non-global-bound) entry; read via INI_STR at runtime. */
PHP_INI_BEGIN()
	PHP_INI_ENTRY("xdebug.mode", "coverage", PHP_INI_SYSTEM, NULL)
	/* Memory-pressure adaptation: when 1 (default), path enumeration is capped
	 * by memory_limit and skipped under live pressure so huge suites never OOM.
	 * Set to 0 to always enumerate (Xdebug-parity fidelity). */
	PHP_INI_ENTRY("fast_xdebug.memory_guard", "1", PHP_INI_ALL, NULL)
PHP_INI_END()

/* ---- helpers ----------------------------------------------------------- */

/*
 * Heuristic mode auto-detection.
 *
 * When xdebug.mode is "auto" (or unset), fast-xdebug cannot *know* your intent
 * -- no extension can. Instead it resolves to a concrete mode set from
 * environment signals, once per request, and caches the result. Precedence:
 *
 *   1. XDEBUG_MODE env var, if set  -> used verbatim (Xdebug's own convention).
 *   2. XDEBUG_TRIGGER / XDEBUG_PROFILE env set  -> "profile".
 *   3. XDEBUG_SESSION / XDEBUG_SESSION_START set -> would be "debug" (step
 *      debugging is not implemented, so we note it and fall through).
 *   4. Running under a test runner (argv/SERVER contains phpunit/paratest, or
 *      PHPUNIT_* env)  -> "coverage".
 *   5. Fallback -> "coverage" (the safe, useful default).
 *
 * This is documented as a heuristic. An explicit non-auto xdebug.mode always
 * wins and is never second-guessed.
 */
static const char *fxd_getenv(const char *name)
{
	char *v = getenv(name);
	if (v && *v) {
		return v;
	}
	return NULL;
}

static zend_bool fxd_running_under_test_runner(void)
{
	/* env hints first (cheap, SAPI-independent) */
	if (fxd_getenv("PHPUNIT_COMPOSER_INSTALL") || fxd_getenv("PARATEST")) {
		return 1;
	}
	/* scan argv for a phpunit/paratest entrypoint */
	{
		zval *argv, *entry;
		zend_string *argv_key = zend_string_init("argv", sizeof("argv") - 1, 0);
		zval *server = NULL;

		if (Z_TYPE(PG(http_globals)[TRACK_VARS_SERVER]) == IS_ARRAY) {
			server = &PG(http_globals)[TRACK_VARS_SERVER];
		}
		if (server) {
			argv = zend_hash_find(Z_ARRVAL_P(server), argv_key);
			if (argv && Z_TYPE_P(argv) == IS_ARRAY) {
				ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(argv), entry) {
					if (Z_TYPE_P(entry) == IS_STRING) {
						const char *s = Z_STRVAL_P(entry);
						if (strstr(s, "phpunit") || strstr(s, "paratest") ||
						    strstr(s, "codecept")) {
							zend_string_release(argv_key);
							return 1;
						}
					}
				} ZEND_HASH_FOREACH_END();
			}
		}
		zend_string_release(argv_key);
	}
	return 0;
}

/* Resolve the effective mode string for this request, caching in a static so
 * the detection runs at most once. Returns a stable comma-separated string. */
static const char *fxd_resolve_mode(void)
{
	static char resolved[64];
	static zend_bool done = 0;
	const char *mode;

	if (done) {
		return resolved;
	}

	mode = INI_STR("xdebug.mode");

	if (mode && *mode && strcmp(mode, "auto") != 0) {
		/* explicit mode: honour verbatim */
		strncpy(resolved, mode, sizeof(resolved) - 1);
		resolved[sizeof(resolved) - 1] = '\0';
		done = 1;
		return resolved;
	}

	/* mode == "auto" (or unset): detect from environment */
	{
		const char *env_mode = fxd_getenv("XDEBUG_MODE");
		if (env_mode) {
			strncpy(resolved, env_mode, sizeof(resolved) - 1);
		} else if (fxd_getenv("XDEBUG_TRIGGER") || fxd_getenv("XDEBUG_PROFILE")) {
			strcpy(resolved, "profile");
		} else if (fxd_getenv("XDEBUG_SESSION") || fxd_getenv("XDEBUG_SESSION_START")) {
			/* Debug requested, but step debugging is unimplemented; fall back to
			 * coverage so the extension is still useful and no IDE hangs. */
			strcpy(resolved, "coverage");
		} else if (fxd_running_under_test_runner()) {
			strcpy(resolved, "coverage");
		} else {
			strcpy(resolved, "coverage");
		}
		resolved[sizeof(resolved) - 1] = '\0';
	}
	done = 1;
	return resolved;
}

static zend_bool fxd_mode_has(const char *needle, zend_bool default_on)
{
	const char *mode = fxd_resolve_mode();
	if (!mode || !*mode) {
		return default_on;
	}
	/* mode may be a comma list, e.g. "develop,coverage,profile" */
	return strstr(mode, needle) != NULL;
}

static zend_bool fxd_mode_has_coverage(void)
{
	return fxd_mode_has("coverage", 1);
}

static zend_bool fxd_mode_has_profile(void)
{
	return fxd_mode_has("profile", 0);
}

static zend_bool fxd_mode_has_debug(void)
{
	return fxd_mode_has("debug", 0);
}

/* ---- arginfo ----------------------------------------------------------- */

ZEND_BEGIN_ARG_INFO_EX(arginfo_fxd_start, 0, 0, 0)
	ZEND_ARG_INFO(0, options)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_fxd_void, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_fxd_set_filter, 0, 0, 3)
	ZEND_ARG_INFO(0, group)
	ZEND_ARG_INFO(0, list_type)
	ZEND_ARG_INFO(0, configuration)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_fxd_info, 0, 0, 0)
	ZEND_ARG_INFO(0, category)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_fxd_start_trace, 0, 0, 0)
	ZEND_ARG_INFO(0, traceFile)
	ZEND_ARG_INFO(0, options)
ZEND_END_ARG_INFO()

/* ---- userland functions ------------------------------------------------ */

/* xdebug_start_code_coverage([int $options = XDEBUG_CC_UNUSED | XDEBUG_CC_DEAD_CODE]) */
PHP_FUNCTION(xdebug_start_code_coverage)
{
	zend_long options = FXD_CC_UNUSED | FXD_CC_DEAD_CODE;

	ZEND_PARSE_PARAMETERS_START(0, 1)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(options)
	ZEND_PARSE_PARAMETERS_END();

	if (!fxd_mode_has_coverage()) {
		php_error_docref(NULL, E_WARNING,
			"Code coverage requires xdebug.mode to contain 'coverage'");
		RETURN_FALSE;
	}

	fxd_coverage_start(options);
	RETURN_TRUE;
}

/* xdebug_stop_code_coverage([bool $cleanup = true]) */
PHP_FUNCTION(xdebug_stop_code_coverage)
{
	zend_bool cleanup = 1;

	ZEND_PARSE_PARAMETERS_START(0, 1)
		Z_PARAM_OPTIONAL
		Z_PARAM_BOOL(cleanup)
	ZEND_PARSE_PARAMETERS_END();

	fxd_coverage_stop();
	(void) cleanup; /* data is cleared at request shutdown regardless */
	RETURN_TRUE;
}

/* xdebug_get_code_coverage(): array */
PHP_FUNCTION(xdebug_get_code_coverage)
{
	ZEND_PARSE_PARAMETERS_NONE();
	fxd_coverage_build_return(return_value);
}

/* xdebug_code_coverage_started(): bool */
PHP_FUNCTION(xdebug_code_coverage_started)
{
	ZEND_PARSE_PARAMETERS_NONE();
	RETURN_BOOL(fxd_coverage_started());
}

/* xdebug_set_filter(int $group, int $list_type, array $configuration): void */
PHP_FUNCTION(xdebug_set_filter)
{
	zend_long group, list_type;
	zval *configuration;
	zval *entry;
	HashTable **target;

	ZEND_PARSE_PARAMETERS_START(3, 3)
		Z_PARAM_LONG(group)
		Z_PARAM_LONG(list_type)
		Z_PARAM_ARRAY(configuration)
	ZEND_PARSE_PARAMETERS_END();

	if (group != FXD_FILTER_CODE_COVERAGE) {
		/* Only the code-coverage filter group is supported. */
		return;
	}

	if (list_type == FXD_PATH_INCLUDE) {
		if (FXD_G(filter_includes)) {
			zend_hash_destroy(FXD_G(filter_includes));
			FREE_HASHTABLE(FXD_G(filter_includes));
		}
		ALLOC_HASHTABLE(FXD_G(filter_includes));
		zend_hash_init(FXD_G(filter_includes), 8, NULL, ZVAL_PTR_DTOR, 0);
		target = &FXD_G(filter_includes);
	} else {
		if (FXD_G(filter_excludes)) {
			zend_hash_destroy(FXD_G(filter_excludes));
			FREE_HASHTABLE(FXD_G(filter_excludes));
		}
		ALLOC_HASHTABLE(FXD_G(filter_excludes));
		zend_hash_init(FXD_G(filter_excludes), 8, NULL, ZVAL_PTR_DTOR, 0);
		target = &FXD_G(filter_excludes);
	}

	ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(configuration), entry) {
		if (Z_TYPE_P(entry) == IS_STRING) {
			zval copy;
			ZVAL_STR(&copy, zend_string_copy(Z_STR_P(entry)));
			zend_hash_next_index_insert(*target, &copy);
		}
	} ZEND_HASH_FOREACH_END();

	FXD_G(filter_active) = 1;
}

/* xdebug_info([string $category]): mixed
 *
 * php-code-coverage only calls xdebug_info('mode') and expects an array
 * containing 'coverage'. We support that and 'no arg' (returns an assoc
 * summary). */
PHP_FUNCTION(xdebug_info)
{
	char *category = NULL;
	size_t category_len = 0;

	ZEND_PARSE_PARAMETERS_START(0, 1)
		Z_PARAM_OPTIONAL
		Z_PARAM_STRING_OR_NULL(category, category_len)
	ZEND_PARSE_PARAMETERS_END();

	if (category && category_len == 4 && strncmp(category, "mode", 4) == 0) {
		array_init(return_value);
		if (fxd_mode_has_coverage()) {
			add_next_index_string(return_value, "coverage");
		}
		if (fxd_mode_has_profile()) {
			add_next_index_string(return_value, "profile");
		}
		if (fxd_mode_has_debug()) {
			add_next_index_string(return_value, "debug");
		}
		return;
	}

	/* Default: minimal assoc summary. */
	array_init(return_value);
	{
		zval mode;
		array_init(&mode);
		if (fxd_mode_has_coverage()) {
			add_next_index_string(&mode, "coverage");
		}
		if (fxd_mode_has_profile()) {
			add_next_index_string(&mode, "profile");
		}
		if (fxd_mode_has_debug()) {
			add_next_index_string(&mode, "debug");
		}
		add_assoc_zval(return_value, "mode", &mode);
		add_assoc_string(return_value, "engine", "swiftcov (fast-xdebug)");
		add_assoc_string(return_value, "version", (char *) FXD_XDEBUG_COMPAT_VERSION);
	}
}

/* --- profiler userland API (Xdebug-compatible) --- */

/* xdebug_start_trace([?string $traceFile = null, int $options = 0]): ?string
 * We map Xdebug's trace/profile entrypoint onto the cachegrind profiler. */
PHP_FUNCTION(xdebug_start_trace)
{
	char *file = NULL;
	size_t file_len = 0;
	zend_long options = 0;

	ZEND_PARSE_PARAMETERS_START(0, 2)
		Z_PARAM_OPTIONAL
		Z_PARAM_STRING_OR_NULL(file, file_len)
		Z_PARAM_LONG(options)
	ZEND_PARSE_PARAMETERS_END();
	(void) options;

	fxd_profiler_start(file);
	if (fxd_profiler_filename()) {
		RETURN_STRING(fxd_profiler_filename());
	}
	RETURN_NULL();
}

/* xdebug_stop_trace(): void */
PHP_FUNCTION(xdebug_stop_trace)
{
	ZEND_PARSE_PARAMETERS_NONE();
	fxd_profiler_stop();
}

/* xdebug_get_profiler_filename(): string|false */
PHP_FUNCTION(xdebug_get_profiler_filename)
{
	ZEND_PARSE_PARAMETERS_NONE();
	if (fxd_profiler_active() && fxd_profiler_filename()) {
		RETURN_STRING(fxd_profiler_filename());
	}
	RETURN_FALSE;
}

/* xdebug_get_tracefile_name(): string|false (alias for the profiler file) */
PHP_FUNCTION(xdebug_get_tracefile_name)
{
	ZEND_PARSE_PARAMETERS_NONE();
	if (fxd_profiler_filename()) {
		RETURN_STRING(fxd_profiler_filename());
	}
	RETURN_FALSE;
}

/* Parse a PHP shorthand byte value ("512M", "1G", "-1") to bytes.
 * Returns -1 for unlimited. */
static zend_long fxd_parse_bytes(const char *v)
{
	zend_long n;
	char last;
	size_t len;
	if (!v || !*v) {
		return -1;
	}
	n = ZEND_STRTOL(v, NULL, 10);
	len = strlen(v);
	last = v[len - 1];
	switch (last) {
		case 'g': case 'G': n *= 1024; /* fallthrough */
		case 'm': case 'M': n *= 1024; /* fallthrough */
		case 'k': case 'K': n *= 1024; break;
		default: break;
	}
	return n;
}

static long fxd_cpu_count(void)
{
#if defined(_SC_NPROCESSORS_ONLN)
	long n = sysconf(_SC_NPROCESSORS_ONLN);
	return n > 0 ? n : 1;
#else
	return 1;
#endif
}

/*
 * swiftcov_recommended_settings(): array
 *
 * Advisory only -- inspects the environment (memory_limit, CPU count, opcache,
 * the requested mode) and returns recommended settings plus the reasoning, so a
 * user or a bootstrap script can apply them. It does NOT change anything by
 * itself. Everything here is a heuristic and labelled as such.
 *
 * fast_xdebug_recommended_settings() is a documented BC alias that shares this
 * implementation via fxd_build_recommended_settings().
 */
static void fxd_build_recommended_settings(zval *return_value)
{
	zend_long mem_bytes;
	long cpus;
	zend_bool opcache_on;
	zval recs, notes;

	mem_bytes = fxd_parse_bytes(INI_STR("memory_limit"));
	cpus = fxd_cpu_count();
	opcache_on = zend_hash_str_find(&module_registry, "zend opcache",
	                                sizeof("zend opcache") - 1) != NULL;

	array_init(return_value);
	array_init(&recs);
	array_init(&notes);

	/* Path enumeration cap: branch/path coverage can explode on large files.
	 * Scale the cap with available memory so small-memory environments stay
	 * safe while big ones keep full fidelity. */
	{
		zend_long cap;
		if (mem_bytes < 0) {
			cap = 4096;            /* unlimited memory: full Xdebug-parity cap */
			add_next_index_string(&notes,
				"memory_limit is unlimited; using the full path cap (4096)");
		} else if (mem_bytes <= (128 * 1024 * 1024)) {
			cap = 256;
			add_next_index_string(&notes,
				"low memory_limit (<=128M): reduced path cap to 256 to avoid OOM");
		} else if (mem_bytes <= (512 * 1024 * 1024)) {
			cap = 1024;
			add_next_index_string(&notes,
				"moderate memory_limit (<=512M): path cap 1024");
		} else {
			cap = 4096;
			add_next_index_string(&notes, "ample memory: full path cap (4096)");
		}
		add_assoc_long(&recs, "fast_xdebug.max_paths", cap);
	}

	/* Coverage filtering: strongly recommended for large codebases to avoid
	 * analysing vendor code you don't measure. */
	add_assoc_string(&recs, "coverage_filter",
		"use xdebug_set_filter(XDEBUG_FILTER_CODE_COVERAGE, XDEBUG_PATH_INCLUDE, [src_dir]) "
		"to skip vendor/framework code");

	/* Memory adaptation is on by default; report it. */
	add_assoc_bool(&recs, "fast_xdebug.memory_guard", 1);
	add_next_index_string(&notes,
		"memory_guard adapts path enumeration under pressure automatically");

	/* opcache advice. */
	if (!opcache_on) {
		add_next_index_string(&notes,
			"OPcache not detected; enabling opcache.enable_cli speeds up repeated runs");
	} else {
		add_next_index_string(&notes, "OPcache detected: good");
	}

	add_assoc_long(return_value, "memory_limit_bytes", mem_bytes);
	add_assoc_long(return_value, "cpu_count", cpus);
	add_assoc_bool(return_value, "opcache", opcache_on);
	add_assoc_string(return_value, "resolved_mode", (char *) fxd_resolve_mode());
	add_assoc_zval(return_value, "recommended", &recs);
	add_assoc_zval(return_value, "notes", &notes);
}

/* swiftcov_recommended_settings(): array  -- canonical name. */
PHP_FUNCTION(swiftcov_recommended_settings)
{
	ZEND_PARSE_PARAMETERS_NONE();
	fxd_build_recommended_settings(return_value);
}

/* fast_xdebug_recommended_settings(): array  -- BC alias. */
PHP_FUNCTION(fast_xdebug_recommended_settings)
{
	ZEND_PARSE_PARAMETERS_NONE();
	fxd_build_recommended_settings(return_value);
}

/*
 * swiftcov_engine(): string  -- canonical product sentinel. Lets tools
 * distinguish us from real Xdebug. fast_xdebug_engine() is a BC alias.
 */
PHP_FUNCTION(swiftcov_engine)
{
	ZEND_PARSE_PARAMETERS_NONE();
	RETURN_STRING("swiftcov " PHP_SWIFTCOV_VERSION);
}

/* fast_xdebug_engine(): string  -- BC alias for swiftcov_engine(). */
PHP_FUNCTION(fast_xdebug_engine)
{
	ZEND_PARSE_PARAMETERS_NONE();
	RETURN_STRING("swiftcov " PHP_SWIFTCOV_VERSION);
}

/* swiftcov_resolved_mode(): string
 * Shows the effective mode after auto-detection, so users can see what
 * xdebug.mode=auto resolved to (e.g. "coverage"). */
PHP_FUNCTION(swiftcov_resolved_mode)
{
	ZEND_PARSE_PARAMETERS_NONE();
	RETURN_STRING((char *) fxd_resolve_mode());
}

/* fast_xdebug_resolved_mode(): string  -- BC alias for swiftcov_resolved_mode(). */
PHP_FUNCTION(fast_xdebug_resolved_mode)
{
	ZEND_PARSE_PARAMETERS_NONE();
	RETURN_STRING((char *) fxd_resolve_mode());
}

/* xdebug_is_debugger_active(): bool
 *
 * Named consumer: symfony/error-handler (ErrorHandler::handleError) and
 * PHPUnit's error handling call xdebug_is_debugger_active() whenever
 * extension_loaded('xdebug') is true, to decide whether a step-debugger is
 * attached before touching error display. Because we register as "xdebug",
 * these call sites reach us; without this function they hit an
 * "undefined function" fatal. swiftcov never runs a DBGp debugger, so the
 * honest, safe answer is always false. See docs/tool-compatibility.md. */
PHP_FUNCTION(xdebug_is_debugger_active)
{
	ZEND_PARSE_PARAMETERS_NONE();
	RETURN_FALSE;
}

/* ---- function table ---------------------------------------------------- */

static const zend_function_entry swiftcov_functions[] = {
	PHP_FE(xdebug_start_code_coverage,   arginfo_fxd_start)
	PHP_FE(xdebug_stop_code_coverage,    arginfo_fxd_start)
	PHP_FE(xdebug_get_code_coverage,     arginfo_fxd_void)
	PHP_FE(xdebug_code_coverage_started, arginfo_fxd_void)
	PHP_FE(xdebug_set_filter,            arginfo_fxd_set_filter)
	PHP_FE(xdebug_info,                  arginfo_fxd_info)
	PHP_FE(xdebug_start_trace,           arginfo_fxd_start_trace)
	PHP_FE(xdebug_stop_trace,            arginfo_fxd_void)
	PHP_FE(xdebug_get_profiler_filename, arginfo_fxd_void)
	PHP_FE(xdebug_get_tracefile_name,    arginfo_fxd_void)
	/* canonical swiftcov_* sentinels */
	PHP_FE(swiftcov_engine,              arginfo_fxd_void)
	PHP_FE(swiftcov_resolved_mode,       arginfo_fxd_void)
	PHP_FE(swiftcov_recommended_settings, arginfo_fxd_void)
	/* honest "no step debugger" probe for symfony/error-handler + PHPUnit */
	PHP_FE(xdebug_is_debugger_active,    arginfo_fxd_void)
	/* fast_xdebug_* BC aliases (kept working for anything already using them) */
	PHP_FE(fast_xdebug_engine,           arginfo_fxd_void)
	PHP_FE(fast_xdebug_resolved_mode,    arginfo_fxd_void)
	PHP_FE(fast_xdebug_recommended_settings, arginfo_fxd_void)
	PHP_FE_END
};

/* ---- module lifecycle -------------------------------------------------- */

static void php_swiftcov_globals_ctor(zend_swiftcov_globals *g)
{
	memset(g, 0, sizeof(*g));
	/* Safe defaults before any coverage session configures them. */
	g->effective_max_paths = FXD_MAX_PATHS;
	g->memory_limit_bytes = -1;
	g->memory_guard = 1;
}

PHP_MINIT_FUNCTION(swiftcov)
{
	REGISTER_INI_ENTRIES();

	REGISTER_LONG_CONSTANT("XDEBUG_CC_UNUSED",       FXD_CC_UNUSED,       CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("XDEBUG_CC_DEAD_CODE",    FXD_CC_DEAD_CODE,    CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("XDEBUG_CC_BRANCH_CHECK", FXD_CC_BRANCH_CHECK, CONST_CS | CONST_PERSISTENT);

	REGISTER_LONG_CONSTANT("XDEBUG_FILTER_CODE_COVERAGE", FXD_FILTER_CODE_COVERAGE, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("XDEBUG_PATH_INCLUDE",         FXD_PATH_INCLUDE,         CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("XDEBUG_PATH_EXCLUDE",         FXD_PATH_EXCLUDE,         CONST_CS | CONST_PERSISTENT);

	/* Trace/profile option constants (Xdebug-compatible values). */
	REGISTER_LONG_CONSTANT("XDEBUG_TRACE_APPEND",       1, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("XDEBUG_TRACE_COMPUTERIZED", 2, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("XDEBUG_TRACE_HTML",         4, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("XDEBUG_TRACE_NAKED_FILENAME", 8, CONST_CS | CONST_PERSISTENT);

	fxd_coverage_minit();
	fxd_profiler_minit();
	fxd_debugger_minit();
	return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(swiftcov)
{
	UNREGISTER_INI_ENTRIES();
	return SUCCESS;
}

/* GINIT: zero the per-thread globals. */
static PHP_GINIT_FUNCTION(swiftcov)
{
#if defined(COMPILE_DL_SWIFTCOV) && defined(ZTS)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif
	php_swiftcov_globals_ctor(swiftcov_globals);
}

PHP_RINIT_FUNCTION(swiftcov)
{
#if defined(ZTS) && defined(COMPILE_DL_SWIFTCOV)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif
	fxd_coverage_rinit();
	fxd_profiler_rinit();
	fxd_debugger_rinit();

	/* Auto-start the profiler when xdebug.mode contains "profile", matching
	 * Xdebug's behaviour (output goes to xdebug.output_dir). */
	if (fxd_mode_has_profile()) {
		fxd_profiler_start(NULL);
	}
	/* Auto-start the debugger listener when xdebug.mode contains "debug". */
	if (fxd_mode_has_debug()) {
		fxd_debugger_maybe_start();
	}
	return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(swiftcov)
{
	fxd_coverage_rshutdown();
	fxd_profiler_rshutdown();
	fxd_debugger_rshutdown();
	return SUCCESS;
}

PHP_MINFO_FUNCTION(swiftcov)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "swiftcov support", "enabled");
	php_info_print_table_row(2, "Version", PHP_SWIFTCOV_VERSION);
	php_info_print_table_row(2, "Xdebug-compatible version", FXD_XDEBUG_COMPAT_VERSION);
	php_info_print_table_row(2, "Coverage mode active",
		fxd_mode_has_coverage() ? "yes" : "no");
	php_info_print_table_end();
}

zend_module_entry swiftcov_module_entry = {
	STANDARD_MODULE_HEADER,
	"xdebug",                     /* MUST be "xdebug" so extension_loaded('xdebug') is true */
	swiftcov_functions,
	PHP_MINIT(swiftcov),
	PHP_MSHUTDOWN(swiftcov),
	PHP_RINIT(swiftcov),
	PHP_RSHUTDOWN(swiftcov),
	PHP_MINFO(swiftcov),
	FXD_XDEBUG_COMPAT_VERSION,    /* phpversion('xdebug') returns this */
	PHP_MODULE_GLOBALS(swiftcov),
	PHP_GINIT(swiftcov),
	NULL,                         /* GSHUTDOWN */
	NULL,                         /* post deactivate */
	STANDARD_MODULE_PROPERTIES_EX
};

#ifdef COMPILE_DL_SWIFTCOV
# ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
# endif
ZEND_GET_MODULE(swiftcov)
#endif
