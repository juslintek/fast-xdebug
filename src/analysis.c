/*
   +----------------------------------------------------------------------+
   | swiftcov: compile-time op_array -> basic-block analysis              |
   |                                                                      |
   | Done ONCE per op_array (cached in an extension-reserved slot by the  |
   | caller). Jump semantics mirror Xdebug's xdebug_find_jumps so branch  |
   | numbers match; the output is a basic-block decomposition so the      |
   | runtime can record one bit per block instead of per opcode.          |
   +----------------------------------------------------------------------+
 */

#include "php.h"
#include "zend_compile.h"
#include "zend_extensions.h"
#include <limits.h>

#include "../php_swiftcov.h"
#include "coverage.h"

#define FXD_JMP_NOT_SET (INT_MAX - 1)
#define FXD_JMP_EXIT    (INT_MAX - 2)

#if ZEND_USE_ABS_JMP_ADDR
# define FXD_JMP_TARGET(node, position, base) \
	(int32_t)(((intptr_t)((node).jmp_addr) - (intptr_t)(base)) / (intptr_t)sizeof(zend_op))
#else
# define FXD_JMP_TARGET(node, position, base) \
	(int32_t)(((int32_t)((node).jmp_offset) / (int32_t)sizeof(zend_op)) + (position))
#endif

/*
 * Compute successor op-indexes for the opcode at `position`.
 * Returns 1 if the opcode is a control-flow instruction, filling
 * jumps[] and *jump_count. Mirrors Xdebug's xdebug_find_jumps().
 * FRAMELESS/internal calls are handled as first-class cases.
 */
static int fxd_find_jumps(const zend_op_array *opa, uint32_t position,
                          size_t *jump_count, int *jumps)
{
#if ZEND_USE_ABS_JMP_ADDR
	zend_op *base_address = &(opa->opcodes[0]);
#else
	void *base_address = NULL;
#endif
	zend_op opcode = opa->opcodes[position];

	*jump_count = 0;

	switch (opcode.opcode) {
		case ZEND_JMP:
			jumps[0] = FXD_JMP_TARGET(opcode.op1, position, base_address);
			*jump_count = 1;
			return 1;

		case ZEND_JMPZ:
		case ZEND_JMPNZ:
		case ZEND_JMPZ_EX:
		case ZEND_JMPNZ_EX:
			jumps[0] = position + 1;
			jumps[1] = FXD_JMP_TARGET(opcode.op2, position, base_address);
			*jump_count = 2;
			return 1;

		case ZEND_FE_FETCH_R:
		case ZEND_FE_FETCH_RW:
			jumps[0] = position + 1;
			jumps[1] = position + (opcode.extended_value / sizeof(zend_op));
			*jump_count = 2;
			return 1;

		case ZEND_FE_RESET_R:
		case ZEND_FE_RESET_RW:
			jumps[0] = position + 1;
			jumps[1] = FXD_JMP_TARGET(opcode.op2, position, base_address);
			*jump_count = 2;
			return 1;

		case ZEND_CATCH:
			*jump_count = 2;
			jumps[0] = position + 1;
			if (!(opcode.extended_value & ZEND_LAST_CATCH)) {
				jumps[1] = FXD_JMP_TARGET(opcode.op2, position, base_address);
				if (jumps[1] == jumps[0]) {
					jumps[1] = FXD_JMP_NOT_SET;
					*jump_count = 1;
				}
			} else {
				jumps[1] = FXD_JMP_EXIT;
			}
			return 1;

		case ZEND_GOTO:
			jumps[0] = FXD_JMP_TARGET(opcode.op1, position, base_address);
			*jump_count = 1;
			return 1;

		case ZEND_FAST_CALL:
			jumps[0] = FXD_JMP_TARGET(opcode.op1, position, base_address);
			jumps[1] = position + 1;
			*jump_count = 2;
			return 1;

		case ZEND_FAST_RET:
			jumps[0] = FXD_JMP_EXIT;
			*jump_count = 1;
			return 1;

#if PHP_VERSION_ID >= 80400
		case ZEND_JMP_FRAMELESS:
			/* PHP 8.4+ frameless internal call: op2 is the fallback jump target
			 * when the frameless call cannot be used. Treated as a two-way branch,
			 * exactly like a conditional jump. See docs/design.md and the frameless
			 * regression test. */
			jumps[0] = position + 1;
			jumps[1] = FXD_JMP_TARGET(opcode.op2, position, base_address);
			*jump_count = 2;
			return 1;
#endif

		case ZEND_GENERATOR_RETURN:
#if PHP_VERSION_ID < 80400
		case ZEND_EXIT:
#endif
		case ZEND_THROW:
		case ZEND_MATCH_ERROR:
		case ZEND_RETURN:
		case ZEND_RETURN_BY_REF:
			jumps[0] = FXD_JMP_EXIT;
			*jump_count = 1;
			return 1;

		case ZEND_MATCH:
		case ZEND_SWITCH_LONG:
		case ZEND_SWITCH_STRING: {
			zval *array_value = RT_CONSTANT(&opa->opcodes[position], opcode.op2);
			HashTable *myht = Z_ARRVAL_P(array_value);
			zval *val;

			ZEND_HASH_FOREACH_VAL_IND(myht, val) {
				if (*jump_count < (size_t)(FXD_BRANCH_MAX_OUTS - 2)) {
					jumps[*jump_count] = position + (val->value.lval / sizeof(zend_op));
					(*jump_count)++;
				}
			} ZEND_HASH_FOREACH_END();

			/* default case */
			jumps[*jump_count] = position + (opcode.extended_value / sizeof(zend_op));
			(*jump_count)++;

			if (opcode.opcode != ZEND_MATCH) {
				jumps[*jump_count] = position + 1; /* fall-through */
				(*jump_count)++;
			}
			return 1;
		}

		default:
			return 0;
	}
}

/*
 * Mark all block leaders. A leader is: op 0, any jump target, the op after a
 * conditional/branch op, and any ZEND_CATCH. We do a linear scan (Xdebug does
 * a recursive walk; a linear leader scan yields the same leader set and is
 * simpler and cache-friendly).
 */
static void fxd_mark_leaders(const zend_op_array *opa, zend_bool *is_leader)
{
	uint32_t pos;
	size_t jump_count;
	int jumps[FXD_BRANCH_MAX_OUTS];

	is_leader[0] = 1;

	/* Leaders are entry points (op 0, CATCH) and jump *targets* only. This
	 * matches Xdebug's `starts` set: each jump's explicit targets (including
	 * the fall-through target position+1 that conditional jumps list in
	 * jumps[0]) become block starts, but a plain op following an unconditional
	 * terminator does not, so branch granularity matches Xdebug. */
	for (pos = 0; pos < opa->last; pos++) {
		if (opa->opcodes[pos].opcode == ZEND_CATCH) {
			is_leader[pos] = 1;
		}
		if (fxd_find_jumps(opa, pos, &jump_count, jumps)) {
			size_t i;
			for (i = 0; i < jump_count; i++) {
				int t = jumps[i];
				if (t == FXD_JMP_EXIT || t == FXD_JMP_NOT_SET) {
					continue;
				}
				if (t >= 0 && (uint32_t)t < opa->last) {
					is_leader[t] = 1;
				}
			}
		}
	}
}

static void fxd_collect_exec_lines(fxd_analysis *a, const zend_op_array *opa)
{
	/* Distinct source lines with "real" opcodes (skip VM-internal opcodes,
	 * matching pcov/Xdebug's opcode_type_filter). */
	uint32_t i;
	uint32_t last_line = 0;
	uint32_t cap = 16, count = 0;
	uint32_t *lines = emalloc(sizeof(uint32_t) * cap);

	for (i = 0; i < opa->last; i++) {
		uint8_t op = opa->opcodes[i].opcode;
		uint32_t ln = opa->opcodes[i].lineno;

		if (op == ZEND_NOP || op == ZEND_EXT_NOP || op == ZEND_RECV ||
		    op == ZEND_RECV_INIT || op == ZEND_OP_DATA || op == ZEND_TICKS ||
		    op == ZEND_FAST_CALL || op == ZEND_RECV_VARIADIC
#if PHP_VERSION_ID >= 80400
		    || op == ZEND_FREE
#endif
		) {
			continue;
		}
		if (ln == 0) {
			continue;
		}
		if (ln == last_line) {
			continue; /* cheap de-dup of adjacent identical lines */
		}
		if (count == cap) {
			cap *= 2;
			lines = erealloc(lines, sizeof(uint32_t) * cap);
		}
		lines[count++] = ln;
		last_line = ln;
	}

	a->exec_lines = lines;
	a->exec_lines_count = count;
}

fxd_analysis *fxd_analyse_op_array(zend_op_array *op_array)
{
	fxd_analysis *a;
	zend_bool *is_leader;
	uint32_t pos, block_id;
	size_t jump_count;
	int jumps[FXD_BRANCH_MAX_OUTS];

	if (op_array->last == 0 || op_array->opcodes == NULL) {
		return NULL;
	}

	a = pecalloc(1, sizeof(fxd_analysis), 0);
	a->num_ops = op_array->last;
	a->op_to_block = pecalloc(a->num_ops, sizeof(uint32_t), 0);

	is_leader = ecalloc(a->num_ops, sizeof(zend_bool));
	fxd_mark_leaders(op_array, is_leader);

	/* Count blocks and assign ids by scanning leaders in order. */
	a->num_blocks = 0;
	for (pos = 0; pos < a->num_ops; pos++) {
		if (is_leader[pos]) {
			a->num_blocks++;
		}
	}
	a->blocks = pecalloc(a->num_blocks, sizeof(fxd_block), 0);

	/* Assign each op to its block; fill start/end/line info and the exact set
	 * of executable lines belonging to each block (filtered like exec_lines).
	 *
	 * A block spans from its leader (a jump target or entry) up to and
	 * INCLUDING the first control-flow op (its terminator); ops after that
	 * terminator, until the next leader, are unreachable-in-this-block trailing
	 * ops (e.g. after an unconditional JMP) and are still mapped for op->block
	 * completeness but do not extend end_op/lines. This yields branch spans
	 * equivalent to Xdebug's start..end merge (xdebug_branch_post_process). */
	block_id = (uint32_t)-1;
	{
		zend_bool closed = 0;
		for (pos = 0; pos < a->num_ops; pos++) {
			fxd_block *b;
			uint8_t op = op_array->opcodes[pos].opcode;
			uint32_t ln = op_array->opcodes[pos].lineno;
			size_t jc; int jmps[FXD_BRANCH_MAX_OUTS];
			zend_bool line_relevant;

			if (is_leader[pos]) {
				block_id++;
				closed = 0;
				a->blocks[block_id].start_op = pos;
				a->blocks[block_id].start_line = ln;
				a->blocks[block_id].is_entry = (pos == 0) || (op == ZEND_CATCH);
			}
			a->op_to_block[pos] = block_id;
			b = &a->blocks[block_id];

			if (!closed) {
				b->end_op = pos;
				b->end_line = ln;

				line_relevant = !(op == ZEND_NOP || op == ZEND_EXT_NOP ||
				                  op == ZEND_RECV || op == ZEND_RECV_INIT ||
				                  op == ZEND_OP_DATA || op == ZEND_TICKS ||
				                  op == ZEND_FAST_CALL || op == ZEND_RECV_VARIADIC
#if PHP_VERSION_ID >= 80400
				                  || op == ZEND_FREE
#endif
				                 ) && ln != 0;
				if (line_relevant &&
				    (b->lines_count == 0 || b->lines[b->lines_count - 1] != ln)) {
					b->lines = erealloc(b->lines, sizeof(uint32_t) * (b->lines_count + 1));
					b->line_ops = erealloc(b->line_ops, sizeof(uint32_t) * (b->lines_count + 1));
					b->line_ops[b->lines_count] = pos;
					b->lines[b->lines_count] = ln;
					b->lines_count++;
				}

				/* Close the block at the first control-flow op. */
				if (fxd_find_jumps(op_array, pos, &jc, jmps)) {
					closed = 1;
				}
			}
		}
	}

	/* Fill successor edges per block, in terms of successor *block* leaders. */
	for (block_id = 0; block_id < a->num_blocks; block_id++) {
		fxd_block *b = &a->blocks[block_id];
		uint32_t last = b->end_op;

		if (fxd_find_jumps(op_array, last, &jump_count, jumps)) {
			size_t i;
			b->has_terminator = 1;
			for (i = 0; i < jump_count && b->outs_count < FXD_BRANCH_MAX_OUTS; i++) {
				int t = jumps[i];
				if (t == FXD_JMP_EXIT) {
					b->outs[b->outs_count++] = (uint32_t)-1; /* sentinel: exit */
				} else if (t == FXD_JMP_NOT_SET) {
					continue;
				} else if (t >= 0 && (uint32_t)t < a->num_ops) {
					b->outs[b->outs_count++] = a->op_to_block[t];
				}
			}
		} else if (last + 1 < a->num_ops) {
			/* fall-through to next block */
			b->has_terminator = 0;
			b->outs[b->outs_count++] = a->op_to_block[last + 1];
		} else {
			/* last block, no explicit terminator (e.g. implicit return) */
			b->has_terminator = 0;
		}
	}

	/* Mark blocks whose leader is reached only by fall-through from the
	 * previous block (i.e. the previous block has no hooked terminator). */
	for (block_id = 1; block_id < a->num_blocks; block_id++) {
		a->blocks[block_id].entered_by_fallthrough =
			!a->blocks[block_id - 1].has_terminator;
	}

	fxd_collect_exec_lines(a, op_array);

	efree(is_leader);

	if (op_array->filename) {
		a->filename = zend_string_copy(op_array->filename);
	}

	return a;
}

void fxd_analysis_free(fxd_analysis *a)
{
	if (!a) {
		return;
	}
	if (a->filename) {
		zend_string_release(a->filename);
	}
	if (a->function_name) {
		zend_string_release(a->function_name);
	}
	if (a->blocks) {
		uint32_t i;
		for (i = 0; i < a->num_blocks; i++) {
			if (a->blocks[i].lines) {
				efree(a->blocks[i].lines);
			}
			if (a->blocks[i].line_ops) {
				efree(a->blocks[i].line_ops);
			}
		}
		pefree(a->blocks, 0);
	}
	if (a->op_to_block) {
		pefree(a->op_to_block, 0);
	}
	if (a->exec_lines) {
		efree(a->exec_lines);
	}
	pefree(a, 0);
}
