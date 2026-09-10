/*
   +----------------------------------------------------------------------+
   | fast-xdebug: an Xdebug-API-compatible, block-based code coverage      |
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

#include "php_fast_xdebug.h"
#include "src/coverage.h"
#include "src/profiler.h"
#include "src/debugger.h"

ZEND_DECLARE_MODULE_GLOBALS(fast_xdebug)

/* INI: xdebug.mode (we honour "coverage"/"off"), so xdebug_info('mode') works.
 * Registered as a plain (non-global-bound) entry; read via INI_STR at runtime. */
PHP_INI_BEGIN()
	PHP_INI_ENTRY("xdebug.mode", "coverage", PHP_INI_SYSTEM, NULL)
PHP_INI_END()

/* ---- helpers ----------------------------------------------------------- */

static zend_bool fxd_mode_has(const char *needle, zend_bool default_on)
{
	const char *mode = INI_STR("xdebug.mode");
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
		add_assoc_string(return_value, "engine", "fast-xdebug");
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

/* fast_xdebug_engine(): string  -- lets tools distinguish us from real Xdebug. */
PHP_FUNCTION(fast_xdebug_engine)
{
	ZEND_PARSE_PARAMETERS_NONE();
	RETURN_STRING("fast-xdebug " PHP_FAST_XDEBUG_VERSION);
}

/* ---- function table ---------------------------------------------------- */

static const zend_function_entry fast_xdebug_functions[] = {
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
	PHP_FE(fast_xdebug_engine,           arginfo_fxd_void)
	PHP_FE_END
};

/* ---- module lifecycle -------------------------------------------------- */

static void php_fast_xdebug_globals_ctor(zend_fast_xdebug_globals *g)
{
	memset(g, 0, sizeof(*g));
}

PHP_MINIT_FUNCTION(fast_xdebug)
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

PHP_MSHUTDOWN_FUNCTION(fast_xdebug)
{
	UNREGISTER_INI_ENTRIES();
	return SUCCESS;
}

/* GINIT: zero the per-thread globals. */
static PHP_GINIT_FUNCTION(fast_xdebug)
{
#if defined(COMPILE_DL_FAST_XDEBUG) && defined(ZTS)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif
	php_fast_xdebug_globals_ctor(fast_xdebug_globals);
}

PHP_RINIT_FUNCTION(fast_xdebug)
{
#if defined(ZTS) && defined(COMPILE_DL_FAST_XDEBUG)
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

PHP_RSHUTDOWN_FUNCTION(fast_xdebug)
{
	fxd_coverage_rshutdown();
	fxd_profiler_rshutdown();
	fxd_debugger_rshutdown();
	return SUCCESS;
}

PHP_MINFO_FUNCTION(fast_xdebug)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "fast-xdebug support", "enabled");
	php_info_print_table_row(2, "Version", PHP_FAST_XDEBUG_VERSION);
	php_info_print_table_row(2, "Xdebug-compatible version", FXD_XDEBUG_COMPAT_VERSION);
	php_info_print_table_row(2, "Coverage mode active",
		fxd_mode_has_coverage() ? "yes" : "no");
	php_info_print_table_end();
}

zend_module_entry fast_xdebug_module_entry = {
	STANDARD_MODULE_HEADER,
	"xdebug",                     /* MUST be "xdebug" so extension_loaded('xdebug') is true */
	fast_xdebug_functions,
	PHP_MINIT(fast_xdebug),
	PHP_MSHUTDOWN(fast_xdebug),
	PHP_RINIT(fast_xdebug),
	PHP_RSHUTDOWN(fast_xdebug),
	PHP_MINFO(fast_xdebug),
	FXD_XDEBUG_COMPAT_VERSION,    /* phpversion('xdebug') returns this */
	PHP_MODULE_GLOBALS(fast_xdebug),
	PHP_GINIT(fast_xdebug),
	NULL,                         /* GSHUTDOWN */
	NULL,                         /* post deactivate */
	STANDARD_MODULE_PROPERTIES_EX
};

#ifdef COMPILE_DL_FAST_XDEBUG
# ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
# endif
ZEND_GET_MODULE(fast_xdebug)
#endif
