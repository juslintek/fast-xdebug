/*
   +----------------------------------------------------------------------+
   | swiftcov: profiler subsystem (cachegrind output)                     |
   |                                                                      |
   | Produces Cachegrind-compatible profiles (the format KCachegrind,     |
   | qcachegrind, PhpStorm and Blackfire's importer read), collected via  |
   | zend_observer fcall begin/end -- one enter/exit hook per call rather |
   | than per opcode, which is why it is cheaper than a tracing profiler. |
   |                                                                      |
   | Exposes the Xdebug profiler userland API so existing tooling works:  |
   |   xdebug_start_trace / xdebug_stop_trace (no-op-compatible),         |
   |   xdebug_get_profiler_filename, and auto-start via xdebug.mode.      |
   +----------------------------------------------------------------------+
 */

#ifndef FXD_PROFILER_H
#define FXD_PROFILER_H

#include "php.h"

void fxd_profiler_minit(void);
void fxd_profiler_rinit(void);
void fxd_profiler_rshutdown(void);

/* Begin/stop profiling explicitly (also driven by xdebug.mode=profile). */
void fxd_profiler_start(const char *filename);
void fxd_profiler_stop(void);
zend_bool fxd_profiler_active(void);
const char *fxd_profiler_filename(void);

#endif /* FXD_PROFILER_H */
