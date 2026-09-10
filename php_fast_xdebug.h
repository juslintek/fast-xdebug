/*
   +----------------------------------------------------------------------+
   | fast-xdebug: an Xdebug-API-compatible code coverage engine           |
   |                                                                      |
   | This source file is subject to the 2-Clause BSD license.             |
   +----------------------------------------------------------------------+
 */

#ifndef PHP_FAST_XDEBUG_H
#define PHP_FAST_XDEBUG_H

extern zend_module_entry fast_xdebug_module_entry;
#define phpext_fast_xdebug_ptr &fast_xdebug_module_entry

#define PHP_FAST_XDEBUG_VERSION "0.1.0"

/*
 * Reported Xdebug-compatible version. sebastian/environment and
 * php-code-coverage gate on phpversion('xdebug') >= 3.1 and on
 * xdebug_info('mode') containing 'coverage'. We advertise a 3.x
 * version so those checks pass. See docs/design.md ("Compatibility
 * posture") for the rationale and its risks.
 */
#define FXD_XDEBUG_COMPAT_VERSION "3.6.99-fast-xdebug-" PHP_FAST_XDEBUG_VERSION

#ifdef PHP_WIN32
#	define PHP_FAST_XDEBUG_API __declspec(dllexport)
#elif defined(__GNUC__) && __GNUC__ >= 4
#	define PHP_FAST_XDEBUG_API __attribute__ ((visibility("default")))
#else
#	define PHP_FAST_XDEBUG_API
#endif

#ifdef ZTS
#include "TSRM.h"
#endif

/* Coverage option flags -- identical numeric values to Xdebug's. */
#define FXD_CC_UNUSED        1
#define FXD_CC_DEAD_CODE     2
#define FXD_CC_BRANCH_CHECK  4

/* Filter constants -- identical numeric values to Xdebug's. */
#define FXD_FILTER_CODE_COVERAGE  0
#define FXD_PATH_INCLUDE          1
#define FXD_PATH_EXCLUDE          0

/* Caps, matching Xdebug's src/coverage/branch_info.h. */
#define FXD_MAX_PATHS        4096
#define FXD_BRANCH_MAX_OUTS  64

ZEND_BEGIN_MODULE_GLOBALS(fast_xdebug)
	/* whether xdebug_start_code_coverage() is currently active */
	zend_bool  coverage_active;
	/* flags passed to the current start_code_coverage() call */
	zend_long  coverage_flags;
	/* per-request line/branch hit data: HashTable<filename, fxd_file*> */
	HashTable *files;
	/* filter: NULL = no filter; otherwise list of path prefixes to include */
	HashTable *filter_includes;
	HashTable *filter_excludes;
	zend_bool  filter_active;
	/* map<analysis-ptr, op-index> of the opline each active frame was on when
	 * coverage was (re)started, so the block that was mid-execution at start
	 * does not report lines that ran before start. */
	HashTable *start_floors;
	/* single-slot cache for O(1) edge (branch transition) recording */
	void      *edge_last_frame;
	void      *edge_last_rt;
	uint32_t   edge_last_block;
ZEND_END_MODULE_GLOBALS(fast_xdebug)

ZEND_EXTERN_MODULE_GLOBALS(fast_xdebug)

#define FXD_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(fast_xdebug, v)

#endif /* PHP_FAST_XDEBUG_H */
