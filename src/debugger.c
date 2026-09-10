/*
   +----------------------------------------------------------------------+
   | fast-xdebug: step debugger (DBGp) -- boundary + scaffold             |
   |                                                                      |
   | Intentionally does not start a DBGp session yet. See debugger.h and  |
   | docs/debugger-spec.md for why a partial debugger is worse than none, |
   | and for the implementation plan.                                     |
   +----------------------------------------------------------------------+
 */

#include "php.h"
#include "php_ini.h"

#include "../php_fast_xdebug.h"
#include "src/debugger.h"

static zend_bool fxd_dbg_warned = 0;

void fxd_debugger_minit(void)
{
	/* No observer/hook installed here yet: the debugger requires per-line
	 * statement callbacks + a DBGp socket server, which are specified in
	 * docs/debugger-spec.md and not implemented in this release. */
}

void fxd_debugger_rinit(void)
{
	fxd_dbg_warned = 0;
}

void fxd_debugger_rshutdown(void)
{
}

void fxd_debugger_maybe_start(void)
{
	/* Honest behaviour: tell the operator the debugger is not implemented
	 * instead of half-opening a DBGp session that would hang their IDE. */
	if (!fxd_dbg_warned) {
		fxd_dbg_warned = 1;
		php_error_docref(NULL, E_NOTICE,
			"fast-xdebug: xdebug.mode=debug requested, but step debugging is "
			"not implemented in this release (coverage and profiling are). "
			"See docs/debugger-spec.md.");
	}
}
