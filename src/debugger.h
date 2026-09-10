/*
   +----------------------------------------------------------------------+
   | fast-xdebug: step debugger (DBGp) -- boundary + scaffold             |
   |                                                                      |
   | Step debugging speaks the DBGp protocol over a socket to an IDE      |
   | (PhpStorm, VS Code). Implementing it faithfully is a large,          |
   | protocol-exact subsystem; a partial implementation is WORSE than     |
   | none, because an IDE that negotiates a session and then hits         |
   | unimplemented commands hangs or misbehaves.                          |
   |                                                                      |
   | This translation unit therefore defines the clean internal boundary  |
   | and lifecycle hooks the rest of the extension calls, but deliberately|
   | does NOT open a listening session that would pretend to be a working |
   | debugger. See docs/debugger-spec.md for the full DBGp plan.          |
   +----------------------------------------------------------------------+
 */

#ifndef FXD_DEBUGGER_H
#define FXD_DEBUGGER_H

#include "php.h"

void fxd_debugger_minit(void);
void fxd_debugger_rinit(void);
void fxd_debugger_rshutdown(void);

/* Honoured only when xdebug.mode contains "debug". Currently a safe no-op that
 * emits a diagnostic rather than half-opening a DBGp session. */
void fxd_debugger_maybe_start(void);

#endif /* FXD_DEBUGGER_H */
