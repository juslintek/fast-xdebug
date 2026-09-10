/*
   +----------------------------------------------------------------------+
   | fast-xdebug: profiler subsystem                                      |
   |                                                                      |
   | Call-tree profiler built on zend_observer fcall begin/end. On each   |
   | call we push a frame (start time, start memory, children accumulator)|
   | and on return we compute inclusive cost, subtract it from the        |
   | parent's "children" so the parent keeps only its self cost, and fold |
   | the result into a per-function aggregate plus a per (caller->callee) |
   | edge with a call count. On request shutdown we serialise a           |
   | Cachegrind file identical in shape to Xdebug's, which KCachegrind,   |
   | qcachegrind, PhpStorm and Blackfire's importer read.                 |
   |                                                                      |
   | One enter + one exit hook per call (not per opcode) is why this is   |
   | cheaper than a per-opcode tracing profiler.                          |
   +----------------------------------------------------------------------+
 */

#include "php.h"
#include "php_ini.h"
#include "SAPI.h"
#include "zend_observer.h"
#include "zend_smart_str.h"

#include "../php_fast_xdebug.h"
#include "src/profiler.h"

#include <time.h>

/*
 * Portable nanosecond monotonic clock. We avoid Zend's zend_hrtime() because
 * its public header (Zend/zend_hrtime.h) does not exist in PHP 8.2's installed
 * dev headers; clock_gettime(CLOCK_MONOTONIC) is available on every platform
 * PHP targets and gives the same monotonic ns we need for self/inclusive time.
 */
#if defined(PHP_WIN32)
# include <windows.h>
static uint64_t fxd_hrtime(void)
{
	LARGE_INTEGER f, c;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&c);
	return (uint64_t)((double)c.QuadPart * 1000000000.0 / (double)f.QuadPart);
}
#else
static uint64_t fxd_hrtime(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
#endif

/* One aggregated function record. Keyed by "fl\nfn" identity string. */
typedef struct _fxd_prof_func {
	zend_string *fl;          /* file (or "php:internal") */
	zend_string *fn;          /* function name */
	uint32_t     lineno;      /* definition line (0 for internal) */
	uint64_t     self_time;   /* ns, self only */
	int64_t      self_mem;    /* bytes, self only */
	uint32_t     called;      /* invocation count */
	/* outgoing edges: HashTable<callee-key-string, fxd_prof_edge*> */
	HashTable    edges;
} fxd_prof_func;

typedef struct _fxd_prof_edge {
	zend_string *callee_key;  /* "fl\nfn" of callee */
	zend_string *callee_fl;
	zend_string *callee_fn;
	uint32_t     callee_line; /* call site line */
	uint32_t     count;
	uint64_t     time;        /* inclusive ns attributed through this edge */
	int64_t      mem;
} fxd_prof_edge;

/* A live stack frame. */
typedef struct _fxd_prof_frame {
	uint64_t     t_start;
	int64_t      m_start;
	uint64_t     children_time; /* time consumed by callees */
	int64_t      children_mem;
	zend_string *key;           /* this function's identity key */
	zend_string *fl;
	zend_string *fn;
	uint32_t     lineno;
	uint32_t     call_line;     /* line in the caller where this was invoked */
} fxd_prof_frame;

static zend_bool     fxd_prof_active = 0;
static char         *fxd_prof_file = NULL;      /* output path */
static HashTable     fxd_prof_funcs;            /* key -> fxd_prof_func* */
static zend_bool     fxd_prof_funcs_inited = 0;
static fxd_prof_frame *fxd_prof_stack = NULL;
static uint32_t      fxd_prof_depth = 0;
static uint32_t      fxd_prof_cap = 0;
static uint64_t      fxd_prof_total_time = 0;
static int64_t       fxd_prof_total_mem = 0;

/* ---- identity ---------------------------------------------------------- */

static void fxd_prof_identity(zend_execute_data *ex, zend_string **fl,
                              zend_string **fn, uint32_t *lineno)
{
	zend_function *func = ex->func;

	if (func->common.function_name) {
		if (func->common.scope) {
			const char *sep = (func->common.fn_flags & ZEND_ACC_STATIC) ? "::" : "->";
			*fn = strpprintf(0, "%s%s%s",
				ZSTR_VAL(func->common.scope->name), sep,
				ZSTR_VAL(func->common.function_name));
		} else {
			*fn = zend_string_copy(func->common.function_name);
		}
	} else {
		*fn = zend_string_init("{main}", sizeof("{main}") - 1, 0);
	}

	if (ZEND_USER_CODE(func->type) && func->op_array.filename) {
		*fl = zend_string_copy(func->op_array.filename);
		*lineno = func->op_array.line_start;
	} else {
		*fl = zend_string_init("php:internal", sizeof("php:internal") - 1, 0);
		*lineno = 0;
	}
}

static zend_string *fxd_prof_key(zend_string *fl, zend_string *fn)
{
	return strpprintf(0, "%s\n%s", ZSTR_VAL(fl), ZSTR_VAL(fn));
}

/* ---- aggregate storage ------------------------------------------------- */

static void fxd_prof_edge_dtor(zval *zv)
{
	fxd_prof_edge *e = Z_PTR_P(zv);
	zend_string_release(e->callee_key);
	zend_string_release(e->callee_fl);
	zend_string_release(e->callee_fn);
	efree(e);
}

static void fxd_prof_func_dtor(zval *zv)
{
	fxd_prof_func *f = Z_PTR_P(zv);
	zend_string_release(f->fl);
	zend_string_release(f->fn);
	zend_hash_destroy(&f->edges);
	efree(f);
}

static fxd_prof_func *fxd_prof_get_func(zend_string *key, zend_string *fl,
                                        zend_string *fn, uint32_t lineno)
{
	fxd_prof_func *f = zend_hash_find_ptr(&fxd_prof_funcs, key);
	if (f) {
		return f;
	}
	f = ecalloc(1, sizeof(fxd_prof_func));
	f->fl = zend_string_copy(fl);
	f->fn = zend_string_copy(fn);
	f->lineno = lineno;
	zend_hash_init(&f->edges, 8, NULL, fxd_prof_edge_dtor, 0);
	zend_hash_add_ptr(&fxd_prof_funcs, key, f);
	return f;
}

static void fxd_prof_add_edge(fxd_prof_func *caller, zend_string *callee_key,
                              zend_string *callee_fl, zend_string *callee_fn,
                              uint32_t call_line, uint64_t time, int64_t mem)
{
	fxd_prof_edge *e = zend_hash_find_ptr(&caller->edges, callee_key);
	if (!e) {
		e = ecalloc(1, sizeof(fxd_prof_edge));
		e->callee_key = zend_string_copy(callee_key);
		e->callee_fl = zend_string_copy(callee_fl);
		e->callee_fn = zend_string_copy(callee_fn);
		e->callee_line = call_line;
		zend_hash_add_ptr(&caller->edges, callee_key, e);
	}
	e->count++;
	e->time += time;
	e->mem += mem;
}

/* ---- observer hooks ---------------------------------------------------- */

static void fxd_prof_begin(zend_execute_data *ex)
{
	fxd_prof_frame *fr;

	if (!fxd_prof_active) {
		return;
	}
	if (fxd_prof_depth == fxd_prof_cap) {
		fxd_prof_cap = fxd_prof_cap ? fxd_prof_cap * 2 : 256;
		fxd_prof_stack = erealloc(fxd_prof_stack, sizeof(fxd_prof_frame) * fxd_prof_cap);
	}
	fr = &fxd_prof_stack[fxd_prof_depth++];
	fr->t_start = fxd_hrtime();
	fr->m_start = (int64_t) zend_memory_usage(0);
	fr->children_time = 0;
	fr->children_mem = 0;
	fxd_prof_identity(ex, &fr->fl, &fr->fn, &fr->lineno);
	fr->key = fxd_prof_key(fr->fl, fr->fn);
	/* line in the caller where the call happened (for cfn call-site) */
	fr->call_line = (ex->prev_execute_data && ex->prev_execute_data->opline)
	                ? ex->prev_execute_data->opline->lineno : 0;
}

static void fxd_prof_end(zend_execute_data *ex, zval *retval)
{
	fxd_prof_frame *fr;
	uint64_t incl_time;
	int64_t incl_mem;
	uint64_t self_time;
	int64_t self_mem;
	fxd_prof_func *f;
	(void) ex; (void) retval;

	if (!fxd_prof_active || fxd_prof_depth == 0) {
		return;
	}
	fr = &fxd_prof_stack[--fxd_prof_depth];

	incl_time = fxd_hrtime() - fr->t_start;
	incl_mem = (int64_t) zend_memory_usage(0) - fr->m_start;
	self_time = incl_time - fr->children_time;
	self_mem = incl_mem - fr->children_mem;

	f = fxd_prof_get_func(fr->key, fr->fl, fr->fn, fr->lineno);
	f->self_time += self_time;
	f->self_mem += self_mem;
	f->called++;

	fxd_prof_total_time += self_time;
	fxd_prof_total_mem += self_mem;

	/* attribute inclusive cost to the caller as a child + record the edge */
	if (fxd_prof_depth > 0) {
		fxd_prof_frame *parent = &fxd_prof_stack[fxd_prof_depth - 1];
		fxd_prof_func *pf;
		parent->children_time += incl_time;
		parent->children_mem += incl_mem;
		pf = fxd_prof_get_func(parent->key, parent->fl, parent->fn, parent->lineno);
		fxd_prof_add_edge(pf, fr->key, fr->fl, fr->fn, fr->call_line,
		                  incl_time, incl_mem);
	}

	zend_string_release(fr->key);
	zend_string_release(fr->fl);
	zend_string_release(fr->fn);
}

static zend_observer_fcall_handlers fxd_prof_observer_init(zend_execute_data *ex)
{
	(void) ex;
	if (fxd_prof_active) {
		return (zend_observer_fcall_handlers){fxd_prof_begin, fxd_prof_end};
	}
	return (zend_observer_fcall_handlers){NULL, NULL};
}

/* ---- cachegrind serialisation ------------------------------------------ */

/* time is stored in ns; Xdebug's unit is 10ns. Convert to match. */
static uint64_t fxd_ns_to_10ns(uint64_t ns) { return ns / 10; }

static void fxd_prof_write(void)
{
	FILE *fp;
	zend_string *key;
	fxd_prof_func *f;
	smart_str buf = {0};

	if (!fxd_prof_file) {
		return;
	}
	fp = fopen(fxd_prof_file, "wb");
	if (!fp) {
		return;
	}

	smart_str_appends(&buf, "version: 1\n");
	smart_str_appends(&buf, "creator: fast-xdebug " PHP_FAST_XDEBUG_VERSION "\n");
	smart_str_appends(&buf, "cmd: ");
	smart_str_appends(&buf, sapi_module.name ? sapi_module.name : "php");
	smart_str_appends(&buf, "\npart: 1\npositions: line\n\n");
	smart_str_appends(&buf, "events: Time_(10ns) Memory_(bytes)\n\n");

	ZEND_HASH_FOREACH_STR_KEY_PTR(&fxd_prof_funcs, key, f) {
		zend_string *ekey;
		fxd_prof_edge *e;
		(void) key;

		smart_str_appends(&buf, "fl=");
		smart_str_append(&buf, f->fl);
		smart_str_appendc(&buf, '\n');
		smart_str_appends(&buf, "fn=");
		smart_str_append(&buf, f->fn);
		smart_str_appendc(&buf, '\n');

		/* self cost line: <lineno> <time_10ns> <mem_bytes> */
		smart_str_append_long(&buf, (zend_long) f->lineno);
		smart_str_appendc(&buf, ' ');
		smart_str_append_long(&buf, (zend_long) fxd_ns_to_10ns(f->self_time));
		smart_str_appendc(&buf, ' ');
		smart_str_append_long(&buf, (zend_long) f->self_mem);
		smart_str_appendc(&buf, '\n');

		ZEND_HASH_FOREACH_STR_KEY_PTR(&f->edges, ekey, e) {
			(void) ekey;
			smart_str_appends(&buf, "cfl=");
			smart_str_append(&buf, e->callee_fl);
			smart_str_appendc(&buf, '\n');
			smart_str_appends(&buf, "cfn=");
			smart_str_append(&buf, e->callee_fn);
			smart_str_appendc(&buf, '\n');
			smart_str_appends(&buf, "calls=");
			smart_str_append_long(&buf, (zend_long) e->count);
			smart_str_appends(&buf, " 0 0\n");
			/* call-site line + inclusive cost through this edge */
			smart_str_append_long(&buf, (zend_long) e->callee_line);
			smart_str_appendc(&buf, ' ');
			smart_str_append_long(&buf, (zend_long) fxd_ns_to_10ns(e->time));
			smart_str_appendc(&buf, ' ');
			smart_str_append_long(&buf, (zend_long) e->mem);
			smart_str_appendc(&buf, '\n');
		} ZEND_HASH_FOREACH_END();

		smart_str_appendc(&buf, '\n');
	} ZEND_HASH_FOREACH_END();

	smart_str_appends(&buf, "summary: ");
	smart_str_append_long(&buf, (zend_long) fxd_ns_to_10ns(fxd_prof_total_time));
	smart_str_appendc(&buf, ' ');
	smart_str_append_long(&buf, (zend_long) fxd_prof_total_mem);
	smart_str_appends(&buf, "\n\n");

	if (buf.s) {
		fwrite(ZSTR_VAL(buf.s), 1, ZSTR_LEN(buf.s), fp);
	}
	fclose(fp);
	smart_str_free(&buf);
}

/* ---- default output path ----------------------------------------------- */

static char *fxd_prof_default_path(void)
{
	const char *dir = INI_STR("xdebug.output_dir");
	char *path;
	if (!dir || !*dir) {
		dir = "/tmp";
	}
	path = malloc(strlen(dir) + 64);
	sprintf(path, "%s/cachegrind.out.%d", dir, (int) getpid());
	return path;
}

/* ---- public API -------------------------------------------------------- */

void fxd_profiler_start(const char *filename)
{
	if (!fxd_prof_funcs_inited) {
		zend_hash_init(&fxd_prof_funcs, 256, NULL, fxd_prof_func_dtor, 0);
		fxd_prof_funcs_inited = 1;
	}
	if (fxd_prof_file) {
		free(fxd_prof_file);
		fxd_prof_file = NULL;
	}
	if (filename && *filename) {
		fxd_prof_file = strdup(filename);
	} else {
		fxd_prof_file = fxd_prof_default_path();
	}
	fxd_prof_depth = 0;
	fxd_prof_total_time = 0;
	fxd_prof_total_mem = 0;
	fxd_prof_active = 1;
}

void fxd_profiler_stop(void)
{
	if (!fxd_prof_active) {
		return;
	}
	fxd_prof_active = 0;
	fxd_prof_write();
}

zend_bool fxd_profiler_active(void)
{
	return fxd_prof_active;
}

const char *fxd_profiler_filename(void)
{
	return fxd_prof_file;
}

void fxd_profiler_minit(void)
{
	zend_observer_fcall_register(fxd_prof_observer_init);
}

void fxd_profiler_rinit(void)
{
	fxd_prof_active = 0;
	fxd_prof_depth = 0;
}

void fxd_profiler_rshutdown(void)
{
	/* auto-flush if still active (script ended without explicit stop) */
	if (fxd_prof_active) {
		fxd_profiler_stop();
	}
	if (fxd_prof_funcs_inited) {
		zend_hash_destroy(&fxd_prof_funcs);
		fxd_prof_funcs_inited = 0;
	}
	if (fxd_prof_stack) {
		efree(fxd_prof_stack);
		fxd_prof_stack = NULL;
		fxd_prof_cap = 0;
	}
	if (fxd_prof_file) {
		free(fxd_prof_file);
		fxd_prof_file = NULL;
	}
}
