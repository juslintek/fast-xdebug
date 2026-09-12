/*
   +----------------------------------------------------------------------+
   | swiftcov: runtime coverage recording + Xdebug-shaped collection      |
   |                                                                      |
   | Runtime model: each basic block ends in exactly one control-flow     |
   | opcode (JMP*, RETURN, MATCH/SWITCH, FE_*, CATCH, THROW, ...) OR falls |
   | through at the op_array end. We install a user_opcode_handler on the |
   | block-TERMINATING opcode types only. When such an opcode executes we |
   | mark the block hit (one store) and, because we know the taken        |
   | successor, mark the branch edge. This is O(blocks executed), strictly|
   | fewer operations than Xdebug's O(opcodes executed) per-opcode handler|
   | and pcov's per-opline hash probe.                                    |
   |                                                                      |
   | Straight-line blocks that fall through at the very end of the        |
   | op_array (no terminator) are handled by also hooking ZEND_RETURN and |
   | by an op_array-entry mark via the analysis (block 0 is always an     |
   | entry). See docs/design.md.                                          |
   +----------------------------------------------------------------------+
 */

#include "php.h"
#include "zend_compile.h"
#include "zend_execute.h"
#include "zend_extensions.h"
#include "zend_exceptions.h"
#include "zend_vm.h"

#include "../php_swiftcov.h"
#include "coverage.h"

/* Per-request analysis cache: HashTable<op_array-pointer, fxd_analysis*>.
 * op_array pointers are stable for the lifetime of a request, so computing
 * the analysis once per op_array per request is correct. This avoids a
 * dependency on being loaded as a zend_extension (needed for reserved[]
 * slots) and keeps packaging as a plain module. See docs/design.md. */
static HashTable fxd_analysis_cache;
static zend_bool fxd_analysis_cache_inited = 0;

/* Returns 1 when the process is close enough to memory_limit that we should
 * stop spending memory on path enumeration. Cheap: one zend_memory_usage()
 * read compared against a precomputed 85%-of-limit threshold. Always 0 when
 * memory_guard is off or memory_limit is unlimited. */
static zend_always_inline zend_bool fxd_under_memory_pressure(void)
{
	if (!FXD_G(memory_guard) || FXD_G(memory_limit_bytes) < 0) {
		return 0;
	}
	{
		size_t used = zend_memory_usage(0);
		/* threshold = 85% of the limit */
		zend_long threshold = FXD_G(memory_limit_bytes) -
		                      (FXD_G(memory_limit_bytes) / 100 * 15);
		return (zend_long) used >= threshold;
	}
}

/* Saved previous user opcode handlers, so we chain rather than clobber. */
static user_opcode_handler_t fxd_prev_handler[256];

/* The set of opcodes that terminate a basic block (and that we hook). */
static uint8_t fxd_terminator[256];

/* Whether our opcode handlers are currently installed in the VM's global
 * user_opcode_handler table. Handlers are installed lazily only while a
 * coverage session is active (see fxd_install_handlers / fxd_remove_handlers)
 * so that processes which never start coverage -- and processes co-loaded with
 * OPcache -- never carry a permanent global opcode hook.
 *
 * THREADING: fxd_handlers_installed and fxd_prev_handler[] are file-static, so
 * they are PROCESS-GLOBAL, not per-thread. This is deliberate and correct for
 * the same reason it is unavoidable: the VM's user_opcode_handler table that
 * zend_set_user_opcode_handler() mutates is ITSELF process-global, so any
 * lazy per-session toggling of it is inherently a process-wide, shared-state
 * operation -- per-thread statics would not make the underlying table safe.
 * Under ZTS this means two threads that start/stop coverage around OVERLAPPING
 * sessions share one global opcode table: one thread's stop() can restore the
 * table while another thread's session is still active. swiftcov's coverage is
 * a single-request-per-process CLI use case (php -d extension=... run-tests, CI
 * .phpt children, coverage collectors), where at most one session is live at a
 * time, so this is not exercised; the CI ZTS row is best-effort
 * (continue-on-error). We intentionally do NOT add locking here: it would be
 * speculative complexity for a scenario swiftcov does not target, and it could
 * not fix the fundamental shared-table property anyway. If concurrent
 * multi-threaded coverage sessions ever become a target, the correct fix is a
 * mutex around install/remove plus a per-thread active-session refcount, not
 * per-thread copies of these statics. */
static zend_bool fxd_handlers_installed = 0;

/* ---- filter helpers ---------------------------------------------------- */

static zend_bool fxd_file_wanted(zend_string *filename)
{
	if (!filename) {
		return 0;
	}
	if (!FXD_G(filter_active)) {
		return 1;
	}
	/* include filter: filename must start with one of the include prefixes */
	if (FXD_G(filter_includes) && zend_hash_num_elements(FXD_G(filter_includes)) > 0) {
		zval *prefix;
		zend_bool matched = 0;
		ZEND_HASH_FOREACH_VAL(FXD_G(filter_includes), prefix) {
			if (Z_TYPE_P(prefix) == IS_STRING &&
			    ZSTR_LEN(filename) >= Z_STRLEN_P(prefix) &&
			    memcmp(ZSTR_VAL(filename), Z_STRVAL_P(prefix), Z_STRLEN_P(prefix)) == 0) {
				matched = 1;
				break;
			}
		} ZEND_HASH_FOREACH_END();
		if (!matched) {
			return 0;
		}
	}
	if (FXD_G(filter_excludes) && zend_hash_num_elements(FXD_G(filter_excludes)) > 0) {
		zval *prefix;
		ZEND_HASH_FOREACH_VAL(FXD_G(filter_excludes), prefix) {
			if (Z_TYPE_P(prefix) == IS_STRING &&
			    ZSTR_LEN(filename) >= Z_STRLEN_P(prefix) &&
			    memcmp(ZSTR_VAL(filename), Z_STRVAL_P(prefix), Z_STRLEN_P(prefix)) == 0) {
				return 0;
			}
		} ZEND_HASH_FOREACH_END();
	}
	return 1;
}

/* Build the Xdebug-style function key: "{main}", "Class::method", "Ns\\func". */
static zend_string *fxd_function_key(const zend_op_array *op_array)
{
	if (op_array->function_name == NULL) {
		return zend_string_init("{main}", sizeof("{main}") - 1, 0);
	}
	if (op_array->scope && op_array->scope->name) {
		/* Xdebug uses "Class->method" for instance methods and "Class::method"
		 * for static methods; php-code-coverage looks methods up by
		 * "Class->method" (Node/File.php), so this separator matters. */
		const char *sep = (op_array->fn_flags & ZEND_ACC_STATIC) ? "::" : "->";
		return strpprintf(0, "%s%s%s",
			ZSTR_VAL(op_array->scope->name), sep,
			ZSTR_VAL(op_array->function_name));
	}
	return zend_string_copy(op_array->function_name);
}

/* ---- per-file / per-function runtime storage --------------------------- */

static void fxd_runtime_free(zval *zv)
{
	fxd_runtime *rt = (fxd_runtime *) Z_PTR_P(zv);
	if (!rt) {
		return;
	}
	if (rt->block_hit) {
		efree(rt->block_hit);
	}
	if (rt->edge_hit) {
		efree(rt->edge_hit);
	}
	/* analysis is owned by the reserved op_array slot, not freed here */
	efree(rt);
}

static void fxd_file_free(zval *zv)
{
	fxd_file *f = (fxd_file *) Z_PTR_P(zv);
	if (!f) {
		return;
	}
	zend_hash_destroy(&f->lines);
	zend_hash_destroy(&f->functions);
	if (f->filename) {
		zend_string_release(f->filename);
	}
	efree(f);
}

static fxd_file *fxd_get_file(zend_string *filename)
{
	fxd_file *f = zend_hash_find_ptr(FXD_G(files), filename);
	if (f) {
		return f;
	}
	f = ecalloc(1, sizeof(fxd_file));
	f->filename = zend_string_copy(filename);
	zend_hash_init(&f->lines, 64, NULL, NULL, 0);
	zend_hash_init(&f->functions, 8, NULL, fxd_runtime_free, 0);
	f->has_branch_info = 0;
	zend_hash_add_ptr(FXD_G(files), filename, f);
	return f;
}

static fxd_runtime *fxd_get_runtime(fxd_file *file, zend_string *fnkey, fxd_analysis *a)
{
	fxd_runtime *rt = zend_hash_find_ptr(&file->functions, fnkey);
	if (rt) {
		return rt;
	}
	rt = ecalloc(1, sizeof(fxd_runtime));
	rt->analysis = a;
	rt->block_hit = ecalloc(a->num_blocks ? a->num_blocks : 1, sizeof(uint8_t));
	if (FXD_G(coverage_flags) & FXD_CC_BRANCH_CHECK) {
		uint32_t b, e = 0;
		rt->edge_hit = ecalloc((a->num_blocks ? a->num_blocks : 1) * FXD_BRANCH_MAX_OUTS,
		                       sizeof(uint8_t));
		for (b = 0; b < a->num_blocks; b++) {
			uint32_t oi;
			for (oi = 0; oi < a->blocks[b].outs_count; oi++) {
				if (a->blocks[b].outs[oi] != (uint32_t) -1) {
					e++;
				}
			}
		}
		rt->edges_total = e;
	}
	zend_hash_add_ptr(&file->functions, fnkey, rt);
	return rt;
}

/* ---- analysis caching in the reserved op_array slot -------------------- */

static void fxd_analysis_cache_dtor(zval *zv)
{
	fxd_analysis_free((fxd_analysis *) Z_PTR_P(zv));
}

static fxd_analysis *fxd_analysis_for(zend_op_array *op_array)
{
	fxd_analysis *a;
	zend_ulong key = (zend_ulong)(uintptr_t) op_array->opcodes;

	if (!fxd_analysis_cache_inited) {
		zend_hash_init(&fxd_analysis_cache, 256, NULL, fxd_analysis_cache_dtor, 0);
		fxd_analysis_cache_inited = 1;
	}

	a = zend_hash_index_find_ptr(&fxd_analysis_cache, key);
	if (a != NULL) {
		return a;
	}
	a = fxd_analyse_op_array(op_array);
	if (a) {
		a->function_name = fxd_function_key(op_array);
		zend_hash_index_add_ptr(&fxd_analysis_cache, key, a);
	}
	return a;
}

/* ---- the runtime handler ----------------------------------------------- */

static zend_always_inline void fxd_mark(zend_execute_data *execute_data)
{
	zend_op_array *op_array;
	fxd_analysis *a;
	fxd_file *file;
	fxd_runtime *rt;
	zend_string *fnkey;
	uint32_t cur_op, block_id;

	if (!FXD_G(coverage_active)) {
		return;
	}
	if (!execute_data || !execute_data->func || !ZEND_USER_CODE(execute_data->func->type)) {
		return;
	}

	op_array = &execute_data->func->op_array;

	/* Saturation fast-path (pointer compare, no hash lookup): a hot loop
	 * re-enters the same op_array millions of times. Once its analysis is fully
	 * recorded for this session, there is nothing new to mark, so a single
	 * compare against the last-known-saturated opcodes pointer short-circuits
	 * the ENTIRE handler -- no filter check, no analysis/file/runtime hash
	 * lookups, no store. This is the dominant cost in hot loops. */
	if (FXD_G(sat_last_opcodes) == (void *) op_array->opcodes) {
		return;
	}

	if (!fxd_file_wanted(op_array->filename)) {
		return;
	}

	a = fxd_analysis_for(op_array);
	if (!a || a->num_blocks == 0) {
		return;
	}

	/* Already saturated this session (e.g. the single-slot cache was pointing at
	 * a different op_array): re-prime the slot and skip. */
	if (a->sat_saturated && a->sat_generation == FXD_G(sat_generation)) {
		FXD_G(sat_last_opcodes) = (void *) op_array->opcodes;
		return;
	}

	cur_op = (uint32_t)(execute_data->opline - op_array->opcodes);
	if (cur_op >= a->num_ops) {
		return;
	}
	block_id = a->op_to_block[cur_op];

	fnkey = a->function_name ? a->function_name : op_array->filename;
	file = fxd_get_file(op_array->filename);
	rt = fxd_get_runtime(file, fnkey, a);

	/* Branch mode: record the actual edge traversed. We keep a single-slot
	 * cache of the last (frame, block) we saw. Because the VM executes opcodes
	 * sequentially, two consecutive marks in the SAME frame whose block changed
	 * represent a real CFG edge; a change of frame (call/return) is not an edge.
	 * This is O(1) per opcode and needs no hashing. Edges into a block via a
	 * call-return are naturally excluded because execute_data differs. */
	if ((FXD_G(coverage_flags) & FXD_CC_BRANCH_CHECK) && rt->edge_hit) {
		file->has_branch_info = 1;
		if (FXD_G(edge_last_frame) == (void *) execute_data &&
		    FXD_G(edge_last_rt) == (void *) rt &&
		    FXD_G(edge_last_block) != block_id) {
			uint32_t pb = FXD_G(edge_last_block);
			if (pb < a->num_blocks) {
				uint32_t oi;
				for (oi = 0; oi < a->blocks[pb].outs_count; oi++) {
					if (a->blocks[pb].outs[oi] == block_id) {
						if (!rt->edge_hit[pb * FXD_BRANCH_MAX_OUTS + oi]) {
							rt->edge_hit[pb * FXD_BRANCH_MAX_OUTS + oi] = 1;
							rt->edges_hit++;
						}
						break;
					}
				}
			}
		}
		FXD_G(edge_last_frame) = (void *) execute_data;
		FXD_G(edge_last_rt) = (void *) rt;
		FXD_G(edge_last_block) = block_id;
	}

	/* One store: this block executed. Because we hook every opcode, each
	 * executed block is observed directly at one of its opcodes; no fall-through
	 * reconstruction or in-flight-frame flushing is required. */
	if (!rt->block_hit[block_id]) {
		rt->block_hit[block_id] = 1;
		rt->blocks_hit++;

		/* Mark the analysis saturated once every block (and, in branch mode,
		 * every real edge) has been seen. We only re-evaluate on a *new* block
		 * hit, so this costs nothing on the hot repeated path. */
		if (rt->blocks_hit >= a->num_blocks) {
			zend_bool branch = (FXD_G(coverage_flags) & FXD_CC_BRANCH_CHECK) != 0;
			if (!branch || rt->edges_hit >= rt->edges_total) {
				a->sat_saturated = 1;
				a->sat_generation = FXD_G(sat_generation);
				FXD_G(sat_last_opcodes) = (void *) op_array->opcodes;
			}
		}
	} else if ((FXD_G(coverage_flags) & FXD_CC_BRANCH_CHECK) && rt->edge_hit &&
	           !(a->sat_saturated && a->sat_generation == FXD_G(sat_generation)) &&
	           rt->blocks_hit >= a->num_blocks &&
	           rt->edges_hit >= rt->edges_total) {
		/* Branch mode: all blocks were already hit but the final edge may only
		 * have completed on this (repeat) block visit -- saturate now. */
		a->sat_saturated = 1;
		a->sat_generation = FXD_G(sat_generation);
		FXD_G(sat_last_opcodes) = (void *) op_array->opcodes;
	}
}

static int fxd_opcode_handler(zend_execute_data *execute_data)
{
	uint8_t opcode;

	/* Harden against unexpected VM state that opcache/JIT edge cases can
	 * produce: never dereference execute_data or its opline blindly. If there
	 * is no frame or no opline, there is nothing to attribute -- just dispatch
	 * to the VM so execution proceeds safely. */
	if (!execute_data || !execute_data->opline) {
		/* Known, deliberate asymmetry with the normal path below: fxd_prev_handler
		 * is keyed by opcode, and here the opcode is unreadable (no opline), so we
		 * cannot look up or invoke a previously-chained handler for this one
		 * dispatch. Any handler another extension chained ahead of us is therefore
		 * skipped in this pathological edge case. Dispatching straight to the VM is
		 * the only safe choice; this state should be near-zero in practice (it is
		 * exactly the corrupt-frame condition this guard exists to survive). */
		return ZEND_USER_OPCODE_DISPATCH;
	}

	opcode = execute_data->opline->opcode;

	fxd_mark(execute_data);

	if (fxd_prev_handler[opcode]) {
		return fxd_prev_handler[opcode](execute_data);
	}
	return ZEND_USER_OPCODE_DISPATCH;
}

/* ---- MINIT / lifecycle ------------------------------------------------- */

/*
 * Hook one opcode handler on every block-leader-reachable opcode. A
 * user_opcode_handler can only be keyed by opcode *type*, not position, so to
 * observe block entry exactly (which the terminator-only scheme cannot do for
 * in-flight frames -- see git history / docs/design.md "Why not terminator
 * only") we hook every opcode type. The handler is deliberately tiny: it
 * resolves the current block via a precomputed op->block map and does a single
 * byte store. This is O(opcodes executed) like the incumbents, but the
 * per-opcode work is one array write with no hash lookup, no filter-slot
 * dereference and no per-line set insert, which is why it is still far cheaper.
 */
/*
 * Re-select the VM handler for every opline of one op_array.
 *
 * zend_set_user_opcode_handler() only changes the global user-opcode table;
 * op_arrays that were ALREADY COMPILED before that call still have their
 * per-opline ->handler pointers baked in and would bypass our hook. Because we
 * install lazily (at fxd_coverage_start, after the script and its functions
 * are compiled), we must walk each already-compiled op_array and ask the VM to
 * recompute each opline's handler from the current user-opcode table via
 * zend_vm_set_opcode_handler(). On remove the user table has been restored, so
 * the same walk resets the oplines to their non-hooked handlers. This is the
 * same op_array-patching pcov performs when it toggles coverage. Only real
 * user op_arrays with opcodes are patched.
 *
 * OPcache-co-load safety (the exact hazard that motivated this whole fix):
 * when OPcache caches a script in shared memory, its op_arrays are flagged
 * ZEND_ACC_IMMUTABLE and their opcode arrays live in the SHM segment shared
 * across every worker process. With opcache.protect_memory=1 that SHM is mapped
 * READ-ONLY, so zend_vm_set_opcode_handler() -- which writes opline->handler --
 * faults: verified locally, an unguarded write here SIGSEGVs (exit 139) under
 * `-d opcache.enable_cli=1 -d opcache.protect_memory=1`. Even with protection
 * off, writing there mutates VM handler pointers that other workers execute.
 * We therefore NEVER write into an op_array that could be SHM-backed; the
 * check lives in fxd_op_array_shm_unsafe() and covers BOTH (a) ZEND_ACC_IMMUTABLE
 * functions/methods/classes (how pcov guards) and (b) the top-level {main}
 * op_array whenever OPcache is loaded, because a cached {main}'s opcodes can be
 * served from SHM WITHOUT the IMMUTABLE flag (verified locally: with only the
 * IMMUTABLE guard, writing {main}'s oplines still SIGSEGVs under protect_memory).
 *
 * TRADEOFF (documented, deliberate): code whose op_array is SHM-backed keeps its
 * plain, baked-in opline handlers -- it does not route through swiftcov's
 * dispatcher -- so its coverage is not recorded while co-loaded with OPcache.
 * This is the price of never writing to SHM. Registering the hook at MINIT (so
 * OPcache would bake our dispatcher in) is NOT an option: it is exactly the
 * permanent global hook whose co-load with OPcache produced the original CI
 * SIGSEGV (also reproduced locally at exit 139). The CI gate is unaffected: each
 * run-tests.php FILE/SKIPIF child runs its script ONCE in a fresh process, so
 * {main} is freshly compiled (heap, writable) when start() runs and is covered
 * normally; a script is only served from SHM on a SUBSEQUENT run. Users who need
 * full coverage under co-loaded OPcache disable OPcache for the coverage run
 * (the standard practice for pcov/Xdebug too). Crash-freedom is the hard
 * requirement; missing coverage for SHM-cached code is a graceful degradation. */
/* Cached "is Zend OPcache loaded in this process?" answer. OPcache presence is
 * fixed for the process lifetime, so we resolve it once. When OPcache is
 * loaded, the top-level script op_array ({main}) may have its opcodes served
 * from the read-only/shared SHM segment even though it is NOT flagged
 * ZEND_ACC_IMMUTABLE (only named functions/methods/classes get that flag), so
 * we must not write into it either. See fxd_op_array_shm_unsafe(). */
static int fxd_opcache_loaded = -1; /* -1 = unresolved, 0 = no, 1 = yes */

static zend_bool fxd_opcache_is_loaded(void)
{
	if (fxd_opcache_loaded < 0) {
		fxd_opcache_loaded = zend_get_extension("Zend OPcache") ? 1 : 0;
	}
	return fxd_opcache_loaded != 0;
}

/* Return non-zero when writing into op_array->opcodes could touch OPcache's
 * shared/read-only SHM. Two cases:
 *   - ZEND_ACC_IMMUTABLE: named functions/methods/classes that OPcache cached
 *     in SHM (pcov guards exactly this way);
 *   - the top-level {main} script when OPcache is loaded: its opcodes can be
 *     served straight from SHM without the IMMUTABLE flag, and there is no
 *     portable, opcache-symbol-free way to prove otherwise, so we treat it as
 *     unsafe whenever OPcache is present. */
static zend_bool fxd_op_array_shm_unsafe(const zend_op_array *op_array)
{
	if (op_array->fn_flags & ZEND_ACC_IMMUTABLE) {
		return 1;
	}
	if (op_array->function_name == NULL && fxd_opcache_is_loaded()) {
		return 1;
	}
	return 0;
}

static void fxd_patch_op_array(zend_op_array *op_array)
{
	uint32_t i;
	if (!op_array || op_array->type != ZEND_USER_FUNCTION || !op_array->opcodes) {
		return;
	}
	if (fxd_op_array_shm_unsafe(op_array)) {
		return;
	}
	for (i = 0; i < op_array->last; i++) {
		zend_vm_set_opcode_handler(&op_array->opcodes[i]);
	}
}

/* Walk every compiled user op_array reachable from the engine (top-level
 * functions, class methods) plus the op_arrays of all live frames, re-selecting
 * their opline handlers so an install/remove toggle takes effect on code that
 * was compiled before the toggle. SHM-backed op_arrays (immutable functions and
 * the {main} script under co-loaded OPcache) are skipped by fxd_patch_op_array()
 * to avoid writing into shared/read-only memory; see the tradeoff note there. */
static void fxd_patch_all_op_arrays(void)
{
	zend_function *func;
	zend_class_entry *ce;
	zend_execute_data *ex;

	ZEND_HASH_FOREACH_PTR(EG(function_table), func) {
		if (func->type == ZEND_USER_FUNCTION) {
			fxd_patch_op_array(&func->op_array);
		}
	} ZEND_HASH_FOREACH_END();

	ZEND_HASH_FOREACH_PTR(EG(class_table), ce) {
		zend_function *m;
		if (!ce->function_table.nNumUsed) {
			continue;
		}
		ZEND_HASH_FOREACH_PTR(&ce->function_table, m) {
			if (m->type == ZEND_USER_FUNCTION && m->op_array.scope == ce) {
				fxd_patch_op_array(&m->op_array);
			}
		} ZEND_HASH_FOREACH_END();
	} ZEND_HASH_FOREACH_END();

	/* The currently-running frames (e.g. the {main} op_array that called
	 * xdebug_start_code_coverage) are not necessarily in the function table. */
	ex = EG(current_execute_data);
	while (ex) {
		if (ex->func && ex->func->type == ZEND_USER_FUNCTION) {
			fxd_patch_op_array(&ex->func->op_array);
		}
		ex = ex->prev_execute_data;
	}
}

/*
 * Install our opcode hook. LAZY and idempotent: called only inside
 * fxd_coverage_start() (before coverage_active is set) and undone by
 * fxd_remove_handlers() on stop / request shutdown. Installing here rather than
 * at MINIT means (a) processes/scripts that never start a coverage session
 * install ZERO opcode handlers -- so swiftcov cannot crash when co-loaded with
 * the system Zend OPcache on the first opcode of a SKIPIF/FILE child -- and
 * (b) fxd_prev_handler[] captures whatever chained handler is in place AFTER
 * opcache has fully initialised, so we correctly chain to it.
 */
static void fxd_install_handlers(void)
{
	int op;
	if (fxd_handlers_installed) {
		/* Already live: never re-capture fxd_prev_handler (a start() without a
		 * matching stop() must not record our own handler as the "previous"
		 * one, which would create an infinite chain). */
		return;
	}
	memset(fxd_prev_handler, 0, sizeof(fxd_prev_handler));
	for (op = 0; op < 256; op++) {
		/* Skip opcodes that never carry meaningful coverage / are hot no-ops. */
		if (op == ZEND_NOP || op == ZEND_EXT_NOP || op == ZEND_OP_DATA) {
			continue;
		}
		fxd_prev_handler[op] = zend_get_user_opcode_handler((uint8_t) op);
		zend_set_user_opcode_handler((uint8_t) op, fxd_opcode_handler);
	}
	/* Route already-compiled op_arrays through the freshly-registered hook. */
	fxd_patch_all_op_arrays();
	fxd_handlers_installed = 1;
}

/*
 * Undo fxd_install_handlers(): restore each opcode's previously-saved handler
 * pointer (including NULL) so any other extension's chained handler is put
 * back exactly as it was, leaving the VM's opcode table clean once the
 * coverage session ends. Uses the SAME op-skip set as install so the tables
 * stay symmetric, and is a no-op when nothing is installed.
 */
static void fxd_remove_handlers(void)
{
	int op;
	if (!fxd_handlers_installed) {
		return;
	}
	for (op = 0; op < 256; op++) {
		if (op == ZEND_NOP || op == ZEND_EXT_NOP || op == ZEND_OP_DATA) {
			continue;
		}
		zend_set_user_opcode_handler((uint8_t) op, fxd_prev_handler[op]);
	}
	/* Reset already-compiled op_arrays to their non-hooked handlers now that the
	 * global user-opcode table has been restored. */
	fxd_patch_all_op_arrays();
	memset(fxd_prev_handler, 0, sizeof(fxd_prev_handler));
	fxd_handlers_installed = 0;
}

void fxd_coverage_minit(void)
{
	/* Deliberately do NOT install opcode handlers at MINIT: installation is
	 * lazy (fxd_coverage_start) so that a process which never starts a coverage
	 * session -- including run-tests.php SKIPIF/FILE probes and swiftcov
	 * co-loaded with OPcache -- carries no global opcode hook. We keep the
	 * one-time zeroing of the saved-handler / terminator tables here. */
	memset(fxd_prev_handler, 0, sizeof(fxd_prev_handler));
	memset(fxd_terminator, 0, sizeof(fxd_terminator));
}

void fxd_coverage_rinit(void)
{
	FXD_G(coverage_active) = 0;
	FXD_G(coverage_flags) = 0;
	FXD_G(files) = NULL;
	FXD_G(filter_active) = 0;
	FXD_G(filter_includes) = NULL;
	FXD_G(filter_excludes) = NULL;
}

void fxd_coverage_rshutdown(void)
{
	/* Defensive: a request that started coverage but never stopped it (script
	 * fatal / exit) must not leave our handler installed into the next request
	 * served by the same process (CLI-server / fpm worker reuse). No-op when
	 * nothing is installed. */
	fxd_remove_handlers();

	if (fxd_analysis_cache_inited) {
		zend_hash_destroy(&fxd_analysis_cache);
		fxd_analysis_cache_inited = 0;
	}
	if (FXD_G(files)) {
		zend_hash_destroy(FXD_G(files));
		FREE_HASHTABLE(FXD_G(files));
		FXD_G(files) = NULL;
	}
	if (FXD_G(filter_includes)) {
		zend_hash_destroy(FXD_G(filter_includes));
		FREE_HASHTABLE(FXD_G(filter_includes));
		FXD_G(filter_includes) = NULL;
	}
	if (FXD_G(filter_excludes)) {
		zend_hash_destroy(FXD_G(filter_excludes));
		FREE_HASHTABLE(FXD_G(filter_excludes));
		FXD_G(filter_excludes) = NULL;
	}
	if (FXD_G(start_floors)) {
		zend_hash_destroy(FXD_G(start_floors));
		FREE_HASHTABLE(FXD_G(start_floors));
		FXD_G(start_floors) = NULL;
	}
}

/* ---- public start/stop ------------------------------------------------- */

/* Walk the active user frames, recording current opline per analysis into the
 * given table. Shared by start-floor capture and collection-frontier capture. */
static void fxd_snapshot_oplines(HashTable *table, zend_bool keep_max)
{
	zend_execute_data *ex = EG(current_execute_data);
	while (ex) {
		if (ex->func && ZEND_USER_CODE(ex->func->type) && ex->opline) {
			zend_op_array *op_array = &ex->func->op_array;
			if (fxd_file_wanted(op_array->filename)) {
				fxd_analysis *a = fxd_analysis_for(op_array);
				if (a) {
					uint32_t cur_op = (uint32_t)(ex->opline - op_array->opcodes);
					zval *ez = zend_hash_index_find(table, (zend_ulong)(uintptr_t) a);
					if (!ez) {
						zval v; ZVAL_LONG(&v, cur_op);
						zend_hash_index_update(table, (zend_ulong)(uintptr_t) a, &v);
					} else if (keep_max ? (Z_LVAL_P(ez) < (zend_long) cur_op)
					                    : (Z_LVAL_P(ez) > (zend_long) cur_op)) {
						ZVAL_LONG(ez, cur_op);
					}
				}
			}
		}
		ex = ex->prev_execute_data;
	}
}

void fxd_coverage_start(zend_long flags)
{
	if (!FXD_G(files)) {
		ALLOC_HASHTABLE(FXD_G(files));
		zend_hash_init(FXD_G(files), 64, NULL, fxd_file_free, 0);
	}
	if (!FXD_G(start_floors)) {
		ALLOC_HASHTABLE(FXD_G(start_floors));
		zend_hash_init(FXD_G(start_floors), 8, NULL, NULL, 0);
	}
	FXD_G(coverage_flags) = flags;
	/* Install the opcode handlers now, BEFORE marking the session active, so
	 * fxd_prev_handler[] captures the currently-chained handlers with opcache
	 * fully initialised and our handler is live for the whole session. */
	fxd_install_handlers();
	FXD_G(coverage_active) = 1;
	FXD_G(edge_last_frame) = NULL;
	FXD_G(edge_last_rt) = NULL;
	FXD_G(edge_last_block) = 0;
	FXD_G(sat_last_opcodes) = NULL;
	/* Invalidate all prior saturation decisions: a new session (possibly with
	 * different flags, e.g. branch vs line) must re-record from scratch. */
	FXD_G(sat_generation)++;

	/* Memory-pressure adaptation: derive the per-session path cap from
	 * memory_limit so low-memory environments stay safe, and record the limit
	 * so fxd_under_memory_pressure() can check live usage during collection.
	 * memory_guard is on by default; disable with fast_xdebug.memory_guard=0.
	 *
	 * NOTE: use PG(memory_limit), which PHP has already parsed to BYTES.
	 * zend_ini_long("memory_limit") returns the unscaled number (e.g. 512 for
	 * "512M"), which would make the pressure check fire immediately. */
	{
		zend_long lim = PG(memory_limit);
		FXD_G(memory_limit_bytes) = lim; /* bytes; < 0 means unlimited */
		FXD_G(memory_guard) = zend_ini_long("fast_xdebug.memory_guard",
		                                    sizeof("fast_xdebug.memory_guard") - 1, 0) ? 1 : 0;
		if (lim < 0) {
			FXD_G(effective_max_paths) = FXD_MAX_PATHS;
		} else if (lim <= (128 * 1024 * 1024)) {
			FXD_G(effective_max_paths) = 256;
		} else if (lim <= (512 * 1024 * 1024)) {
			FXD_G(effective_max_paths) = 1024;
		} else {
			FXD_G(effective_max_paths) = FXD_MAX_PATHS;
		}
	}
	/* Record where each active frame is now; keep the *minimum* per analysis so
	 * lines at or after the start opline in the open block are reportable. */
	fxd_snapshot_oplines(FXD_G(start_floors), 0);
}

void fxd_coverage_stop(void)
{
	FXD_G(coverage_active) = 0;
	/* Restore the VM's opcode handler table. Guarded by fxd_handlers_installed,
	 * so a double-stop or a stop-without-start is safe (no-op). */
	fxd_remove_handlers();
}

zend_bool fxd_coverage_started(void)
{
	return FXD_G(coverage_active);
}

/* ---- collection: build the Xdebug-shaped return value ------------------ */

/* Mark every line of every hit block as executed (>=1). Unused (analyzed but
 * not hit) lines are emitted as -1 when CC_UNUSED is set; dead code as -2.
 *
 * `frontiers` maps an op_array's opcodes-pointer to the highest op index that
 * has actually executed in a still-active frame. For the frontier block of an
 * in-flight frame we must not report lines that occur *after* the current
 * opline (they have not run yet), which keeps us line-exact with Xdebug even
 * though we track at block granularity. */
static void fxd_build_lines(fxd_file *file, HashTable *out_lines,
                            HashTable *frontiers, HashTable *floors)
{
	fxd_runtime *rt;
	zend_bool want_unused = (FXD_G(coverage_flags) & FXD_CC_UNUSED) != 0;

	/* First: executed lines from hit blocks. */
	ZEND_HASH_FOREACH_PTR(&file->functions, rt) {
		fxd_analysis *a = rt->analysis;
		uint32_t b;
		zval *fz = frontiers ? zend_hash_index_find(frontiers,
			(zend_ulong)(uintptr_t) a) : NULL;
		zval *lz = floors ? zend_hash_index_find(floors,
			(zend_ulong)(uintptr_t) a) : NULL;
		zend_long frontier = fz ? Z_LVAL_P(fz) : -1;
		zend_long floor = lz ? Z_LVAL_P(lz) : -1;

		for (b = 0; b < a->num_blocks; b++) {
			uint32_t i;
			if (!rt->block_hit[b]) {
				continue;
			}
			/* Mark exactly the executable lines of this block's ops. For the
			 * open block of an active frame, clamp to [floor, frontier] so
			 * lines that ran before coverage started, or after the current
			 * opline, are not falsely reported -- this makes block-granularity
			 * tracking line-exact against Xdebug. */
			for (i = 0; i < a->blocks[b].lines_count; i++) {
				uint32_t ln = a->blocks[b].lines[i];
				uint32_t lop = a->blocks[b].line_ops[i];
				if (frontier >= 0 &&
				    a->blocks[b].start_op <= (uint32_t) frontier &&
				    a->blocks[b].end_op   >= (uint32_t) frontier &&
				    lop > (uint32_t) frontier) {
					continue; /* line lies past the current opline: not run yet */
				}
				if (floor >= 0 &&
				    a->blocks[b].start_op <= (uint32_t) floor &&
				    a->blocks[b].end_op   >= (uint32_t) floor &&
				    lop < (uint32_t) floor) {
					continue; /* line ran before coverage started */
				}
				zval v; ZVAL_LONG(&v, 1);
				zend_hash_index_update(out_lines, ln, &v);
			}
		}
	} ZEND_HASH_FOREACH_END();

	if (!want_unused) {
		return;
	}

	/* Then: analyzed-but-not-hit executable lines -> -1. */
	ZEND_HASH_FOREACH_PTR(&file->functions, rt) {
		fxd_analysis *a = rt->analysis;
		uint32_t i;
		for (i = 0; i < a->exec_lines_count; i++) {
			uint32_t ln = a->exec_lines[i];
			if (!zend_hash_index_exists(out_lines, ln)) {
				zval v; ZVAL_LONG(&v, -1);
				zend_hash_index_add(out_lines, ln, &v);
			}
		}
	} ZEND_HASH_FOREACH_END();
}

/* Enumerate paths through a function's block CFG, capped at FXD_MAX_PATHS.
 * Mirrors Xdebug's recursive path finder (branch_info.c). */
/* Recursively enumerate acyclic-per-edge paths from block_id, emitting each
 * leaf path (capped at FXD_MAX_PATHS). Path elements are block *start op*
 * indexes (Xdebug convention). A path is "hit" iff every edge along it was
 * observed at runtime (rt->edge_hit); the final exit edge counts as taken iff
 * the last block was entered. `outidx` records, per stacked block, which out
 * edge index leads to the next block, so hit can be computed edge-by-edge. */
static void fxd_find_paths(fxd_analysis *a, fxd_runtime *rt, uint32_t block_id,
                           uint32_t *stack, uint32_t *outidx, uint32_t depth,
                           zval *paths_out, uint32_t *paths_count)
{
	uint32_t i;
	zend_bool found = 0;

	if (*paths_count >= FXD_G(effective_max_paths)) {
		return;
	}
	if (depth >= a->num_blocks + 1) {
		return; /* guard against pathological cycles */
	}

	stack[depth] = block_id;

	for (i = 0; i < a->blocks[block_id].outs_count; i++) {
		uint32_t out = a->blocks[block_id].outs[i];
		uint32_t d;
		zend_bool cycle = 0;
		if (out == (uint32_t)-1) {
			continue; /* exit edge */
		}
		for (d = 0; d < depth; d++) {
			if (stack[d] == block_id && stack[d + 1] == out) {
				cycle = 1;
				break;
			}
		}
		if (cycle) {
			continue;
		}
		outidx[depth] = i;
		found = 1;
		fxd_find_paths(a, rt, out, stack, outidx, depth + 1, paths_out, paths_count);
	}

	if (!found) {
		zval path_container, path_arr;
		uint32_t d;
		zend_bool all_hit = 1;

		array_init(&path_arr);
		for (d = 0; d <= depth; d++) {
			add_next_index_long(&path_arr, a->blocks[stack[d]].start_op);
		}
		/* hit: every intermediate edge observed, and the terminal block entered */
		if (!rt->block_hit[stack[depth]]) {
			all_hit = 0;
		}
		if (all_hit && rt->edge_hit) {
			for (d = 0; d < depth; d++) {
				if (!rt->edge_hit[stack[d] * FXD_BRANCH_MAX_OUTS + outidx[d]]) {
					all_hit = 0;
					break;
				}
			}
		} else if (all_hit && !rt->edge_hit) {
			/* branch data requested but no edge buffer: fall back to block hits */
			for (d = 0; d <= depth; d++) {
				if (!rt->block_hit[stack[d]]) { all_hit = 0; break; }
			}
		}

		array_init(&path_container);
		add_assoc_zval(&path_container, "path", &path_arr);
		add_assoc_long(&path_container, "hit", all_hit ? 1 : 0);
		add_next_index_zval(paths_out, &path_container);
		(*paths_count)++;
	}
}

static void fxd_build_function(fxd_runtime *rt, zval *func_out)
{
	fxd_analysis *a = rt->analysis;
	zval branches, paths;
	uint32_t b;

	array_init(&branches);
	array_init(&paths);

	/* Emit one branch per basic block, keyed by start_op (Xdebug keys branches
	 * by the op index of the block start). */
	for (b = 0; b < a->num_blocks; b++) {
		zval branch, out, out_hit;
		uint32_t i;
		fxd_block *blk = &a->blocks[b];

		/* Report the branch's line span in terms of the block's *executable*
		 * lines. php-code-coverage drops any branch whose range(line_start,
		 * line_end) contains a line it does not consider executable
		 * (RawCodeCoverageData::keepFunctionCoverageDataOnlyForLines), so
		 * spanning declaration/blank/brace-only lines would silently discard
		 * the branch. Using the first/last executable op line keeps the span
		 * inside the executable set. */
		uint32_t line_start = blk->start_line, line_end = blk->end_line;
		if (blk->lines_count > 0) {
			uint32_t lo = blk->lines[0], hi = blk->lines[0], k;
			for (k = 1; k < blk->lines_count; k++) {
				if (blk->lines[k] < lo) lo = blk->lines[k];
				if (blk->lines[k] > hi) hi = blk->lines[k];
			}
			line_start = lo;
			line_end = hi;
		}

		array_init(&branch);
		add_assoc_long(&branch, "op_start", blk->start_op);
		add_assoc_long(&branch, "op_end", blk->end_op);
		add_assoc_long(&branch, "line_start", line_start);
		add_assoc_long(&branch, "line_end", line_end);
		add_assoc_long(&branch, "hit", rt->block_hit[b] ? 1 : 0);

		array_init(&out);
		array_init(&out_hit);
		for (i = 0; i < blk->outs_count; i++) {
			uint32_t target = blk->outs[i];
			long target_op = (target == (uint32_t)-1) ? 0 : (long)a->blocks[target].start_op;
			int edge = 0;
			add_index_long(&out, i, target_op);
			if (target == (uint32_t)-1) {
				/* exit edge: taken iff this block was entered (the frame left
				 * via this block's terminator) */
				edge = rt->block_hit[b] ? 1 : 0;
			} else if (rt->edge_hit) {
				/* precise: the transition b->target was actually observed */
				edge = rt->edge_hit[b * FXD_BRANCH_MAX_OUTS + i] ? 1 : 0;
			}
			add_index_long(&out_hit, i, edge);
		}
		add_assoc_zval(&branch, "out", &out);
		add_assoc_zval(&branch, "out_hit", &out_hit);

		add_index_zval(&branches, blk->start_op, &branch);
	}

	/* Paths. Path enumeration is the one part of coverage that can blow up in
	 * memory (thousands of paths per generated file). Under memory pressure we
	 * skip it for this function -- branches (the cheaper, bounded data) are
	 * still emitted, so line/branch coverage stay intact and only the optional
	 * path list is degraded. This keeps a huge suite from OOMing. */
	if (!fxd_under_memory_pressure()) {
		uint32_t *stack = ecalloc(a->num_blocks + 2, sizeof(uint32_t));
		uint32_t *outidx = ecalloc(a->num_blocks + 2, sizeof(uint32_t));
		uint32_t paths_count = 0;
		uint32_t entry;
		for (entry = 0; entry < a->num_blocks; entry++) {
			if (a->blocks[entry].is_entry) {
				fxd_find_paths(a, rt, entry, stack, outidx, 0, &paths, &paths_count);
			}
		}
		efree(stack);
		efree(outidx);
	}

	add_assoc_zval(func_out, "branches", &branches);
	add_assoc_zval(func_out, "paths", &paths);
}

void fxd_coverage_build_return(zval *return_value)
{
	zend_bool branch_mode = (FXD_G(coverage_flags) & FXD_CC_BRANCH_CHECK) != 0;
	zend_string *fname;
	fxd_file *file;
	HashTable frontiers;

	array_init(return_value);

	if (!FXD_G(files)) {
		return;
	}

	zend_hash_init(&frontiers, 8, NULL, NULL, 0);
	if (FXD_G(coverage_active)) {
		/* frontier = highest opline reached per analysis in active frames */
		fxd_snapshot_oplines(&frontiers, 1);
	}



	ZEND_HASH_FOREACH_STR_KEY_PTR(FXD_G(files), fname, file) {
		zval lines;
		array_init(&lines);
		fxd_build_lines(file, Z_ARRVAL(lines), &frontiers, FXD_G(start_floors));

		if (branch_mode && file->has_branch_info) {
			zval file_info, functions;
			zend_string *fnkey;
			fxd_runtime *rt;

			array_init(&file_info);
			array_init(&functions);

			ZEND_HASH_FOREACH_STR_KEY_PTR(&file->functions, fnkey, rt) {
				zval func_out;
				array_init(&func_out);
				fxd_build_function(rt, &func_out);
				add_assoc_zval_ex(&functions, ZSTR_VAL(fnkey), ZSTR_LEN(fnkey), &func_out);
			} ZEND_HASH_FOREACH_END();

			add_assoc_zval_ex(&file_info, "lines", sizeof("lines") - 1, &lines);
			add_assoc_zval_ex(&file_info, "functions", sizeof("functions") - 1, &functions);
			add_assoc_zval_ex(return_value, ZSTR_VAL(fname), ZSTR_LEN(fname), &file_info);
		} else {
			add_assoc_zval_ex(return_value, ZSTR_VAL(fname), ZSTR_LEN(fname), &lines);
		}
	} ZEND_HASH_FOREACH_END();

	zend_hash_destroy(&frontiers);
}
