/*
   +----------------------------------------------------------------------+
   | swiftcov: coverage engine internal contract                          |
   +----------------------------------------------------------------------+
 */

#ifndef FXD_COVERAGE_H
#define FXD_COVERAGE_H

#include "php.h"
#include "zend_compile.h"

/*
 * Compile-time analysis of a single op_array.
 *
 * We split the op_array into basic blocks once (at first execution of the
 * op_array, cached in an extension-reserved slot). At runtime we only need
 * to record which blocks were entered; lines and branch/path coverage are
 * reconstructed from the analysis at collection time.
 */

typedef struct _fxd_block {
	uint32_t start_op;                 /* first opline index of the block */
	uint32_t end_op;                   /* last opline index of the block */
	uint32_t start_line;               /* lineno of start_op */
	uint32_t end_line;                 /* lineno of end_op */
	uint32_t outs[FXD_BRANCH_MAX_OUTS];/* successor block leader op-indexes */
	uint32_t outs_count;
	zend_bool is_entry;                /* is this an entry point (function start / catch) */
	uint32_t *lines;                   /* exact executable line numbers of this block's ops */
	uint32_t *line_ops;                /* op index at which each lines[] entry first appears */
	uint32_t  lines_count;
	/* True iff this block's leader is reached only by falling through from the
	 * textually-previous block (its last op is not a hooked control-flow op).
	 * Used at runtime to reconstruct the fall-through run when we observe a
	 * terminator: we walk backwards marking blocks joined by fall-through. */
	zend_bool entered_by_fallthrough;
	/* True iff this block ends in a control-flow opcode we hook at runtime. */
	zend_bool has_terminator;
} fxd_block;

typedef struct _fxd_analysis {
	uint32_t   num_ops;
	uint32_t   num_blocks;
	fxd_block *blocks;                 /* array[num_blocks], indexed by block id */
	uint32_t  *op_to_block;            /* array[num_ops]: op index -> block id (leader map) */
	/* Distinct executable lines in this op_array (for CC_UNUSED reporting). */
	uint32_t  *exec_lines;             /* sorted unique line numbers */
	uint32_t   exec_lines_count;
	/* Identity used for opcache-safe caching / debugging. */
	zend_string *filename;
	zend_string *function_name;        /* "{main}", "Class::method", "Ns\\func" */
	/* Saturation fast-path (per coverage session, guarded by sat_generation):
	 * once every recordable block/edge of this op_array has been seen, the
	 * per-opcode handler can skip it entirely -- the dominant win for hot loops.
	 * Validity is scoped to a session via sat_generation == FXD_G(sat_generation);
	 * a stale generation means "not saturated in the current session". */
	uint32_t   sat_generation;
	zend_bool  sat_saturated;
} fxd_analysis;

/*
 * Runtime hit tracking for one analyzed op_array instance.
 * block_hit[i] != 0  => block i was entered.
 * edge_hit is a flat bitmap of size num_blocks * FXD_BRANCH_MAX_OUTS.
 */
typedef struct _fxd_runtime {
	fxd_analysis *analysis;
	uint8_t      *block_hit;           /* array[num_blocks] */
	uint8_t      *edge_hit;            /* array[num_blocks * FXD_BRANCH_MAX_OUTS] */
	uint32_t      blocks_hit;          /* distinct blocks marked */
	uint32_t      edges_hit;           /* distinct edges marked (branch mode) */
	uint32_t      edges_total;         /* total real (non-exit) edges in the CFG */
} fxd_runtime;

/* Per-file aggregate of everything observed for a filename this request. */
typedef struct _fxd_file {
	zend_string *filename;
	/* line -> hit count (>0 means executed). Reconstructed at collect time. */
	HashTable    lines;                /* HashTable<line:long, count:long> */
	/* function name -> fxd_runtime* (owns analysis+hit buffers) */
	HashTable    functions;
	zend_bool    has_branch_info;
} fxd_file;

/* analysis.c */
fxd_analysis *fxd_analyse_op_array(zend_op_array *op_array);
void          fxd_analysis_free(fxd_analysis *a);

/* coverage.c */
void fxd_coverage_minit(void);
void fxd_coverage_rinit(void);
void fxd_coverage_rshutdown(void);

void fxd_coverage_start(zend_long flags);
void fxd_coverage_stop(void);
zend_bool fxd_coverage_started(void);
void fxd_coverage_build_return(zval *return_value);

#endif /* FXD_COVERAGE_H */
