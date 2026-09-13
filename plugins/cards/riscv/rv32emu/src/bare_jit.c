/* SRZ80's private RV32EMU embedding. Native arithmetic uses the upstream
 * tier-1 generator; memory always uses the embedding callbacks. No userspace
 * RAM, ELF loader, LLVM worker, or interpreter block chaining is involved. */
#include "riscv_private.h"
#include "jit.h"
#include <stdlib.h>

#if RV32_HAS(JIT)
#define BARE_BLOCK_INSNS 64
#define BARE_CACHE_BITS 10

typedef struct bare_block {
    block_t block; /* first: cache callbacks receive block_t */
    rv_insn_t ir[BARE_BLOCK_INSNS];
    uint32_t words[BARE_BLOCK_INSNS];
    branch_history_table_t bht;
    uint64_t validated;
} bare_block_t;

typedef struct {
    uint64_t epoch;
    uint64_t native_blocks;
    uint64_t instructions;
    uint32_t blocks;
} bare_jit_t;

static bool memory_op(uint16_t op)
{
    return op == rv_insn_lb || op == rv_insn_lh || op == rv_insn_lw ||
           op == rv_insn_lbu || op == rv_insn_lhu || op == rv_insn_sb ||
           op == rv_insn_sh || op == rv_insn_sw;
}

static bool branches(uint16_t op)
{
    switch (op) {
#define _(inst, branch, len, translatable, regs) IIF(branch)(case rv_insn_##inst:, )
        RV_INSN_LIST
#undef _
        return true;
    default: return false;
    }
}

static bool translatable(uint16_t op)
{
    /* Keep traps in the reference stepper; its callbacks establish precise
       stop reasons. They are cold, and need no native code generation. */
    if (op == rv_insn_ecall || op == rv_insn_ebreak)
        return false;
    switch (op) {
#define _(inst, branch, len, translatable, regs) IIF(translatable)(case rv_insn_##inst:, )
        RV_INSN_LIST
#undef _
        return true;
    default: return false;
    }
}

static void free_block(void *block) { free(block); }

void rv_bare_jit_destroy(riscv_t *rv)
{
    if (!rv->bare_jit) return;
    if (rv->jit_state) jit_state_exit(rv->jit_state);
    if (rv->block_cache) {
        clear_cache_hot(rv->block_cache, free_block);
        cache_free(rv->block_cache);
    }
    free(rv->bare_jit);
    rv->bare_jit = NULL;
    rv->jit_state = NULL;
    rv->block_cache = NULL;
}

static bool initialize(riscv_t *rv)
{
    rv->bare_jit = calloc(1, sizeof(bare_jit_t));
    if (!rv->bare_jit) return false;
    rv->jit_state = jit_state_init(4 * 1024 * 1024);
    rv->block_cache = cache_create(BARE_CACHE_BITS);
    if (!rv->jit_state || !rv->block_cache) {
        rv_bare_jit_destroy(rv);
        return false;
    }
    return true;
}

static void invalidate(riscv_t *rv)
{
    /* Generated code contains pointers to IR for callback memory operations;
       invalidate code before releasing any of those owned IR records. */
    jit_invalidate(rv);
    clear_cache_hot(rv->block_cache, free_block);
    cache_free(rv->block_cache);
    rv->block_cache = cache_create(BARE_CACHE_BITS);
    ((bare_jit_t *) rv->bare_jit)->blocks = 0;
}

static bare_block_t *decode_block(riscv_t *rv)
{
    bare_block_t *entry = calloc(1, sizeof(*entry));
    if (!entry) return NULL;
    block_t *block = &entry->block;
    block->pc_start = block->pc_end = rv->PC;
    block->translatable = true;
    memset(entry->bht.PC, 0xff, sizeof(entry->bht.PC));
    for (uint32_t i = 0; i < BARE_BLOCK_INSNS; ++i) {
        rv_insn_t *ir = &entry->ir[i];
        const uint32_t word = rv->io.mem_ifetch(rv, block->pc_end);
        if (rv->halt || !rv_decode(ir, word) || !translatable(ir->opcode))
            break;
        entry->words[i] = word;
        ir->pc = block->pc_end;
        if (ir->opcode == rv_insn_jalr) ir->branch_table = &entry->bht;
        if (i) entry->ir[i - 1].next = ir;
        block->n_insn++;
        block->pc_end += 4;
        block->ir_tail = ir;
        // Memory callbacks and branches return to the bounded dispatcher.
        if (memory_op(ir->opcode) || branches(ir->opcode)) break;
    }
    if (!block->n_insn) { free(entry); return NULL; }
    block->ir_head = entry->ir;
    block->cycle_cost = block->n_insn;
    entry->validated = ((bare_jit_t *) rv->bare_jit)->epoch;
    return entry;
}

bool rv_step_bare_jit(riscv_t *rv, uint32_t budget)
{
    if (!rv || !rv->bare_mode) return false;
    if (!rv->bare_jit && !initialize(rv)) return false;
    bare_jit_t *jit = rv->bare_jit;
    ++jit->epoch;
    uint32_t executed = 0;
    while (executed < budget && !rv->halt) {
        bare_block_t *entry = cache_get(rv->block_cache, rv->PC, false);
        if (entry && entry->validated != jit->epoch) {
            bool unchanged = true;
            for (uint32_t i = 0; i < entry->block.n_insn && !rv->halt; ++i) {
                if (rv->io.mem_ifetch(rv, entry->ir[i].pc) != entry->words[i]) {
                    unchanged = false;
                    break;
                }
            }
            if (!unchanged) {
                invalidate(rv);
                if (!rv->block_cache) return false;
                entry = NULL;
            } else entry->validated = jit->epoch;
        }
        if (rv->halt) break;
        if (!entry) {
            if (jit->blocks == (1u << BARE_CACHE_BITS)) {
                invalidate(rv);
                if (!rv->block_cache) return false;
            }
            entry = decode_block(rv);
            if (!entry) {
                if (!rv->halt) rv_step_debug(rv);
                ++executed;
                continue;
            }
            cache_put(rv->block_cache, rv->PC, entry);
            ++jit->blocks;
        }
        if (rv->halt) break;
        if (!entry->block.hot && !jit_translate(rv, &entry->block)) return false;
        struct jit_state *state = rv->jit_state;
        ((exec_block_func_t) state->buf)(rv, (uintptr_t)(state->buf + entry->block.offset));
        ++jit->native_blocks;
        executed += entry->block.n_insn;
        rv->csr_cycle += entry->block.n_insn;
        rv->X[0] = 0;
    }
    jit->instructions += executed;
    return true;
}

uint64_t rv_bare_jit_instructions(const riscv_t *rv)
{
    return rv->bare_jit ? ((const bare_jit_t *) rv->bare_jit)->instructions : 0;
}

uint64_t rv_bare_jit_native_blocks(const riscv_t *rv)
{
    return rv->bare_jit ? ((const bare_jit_t *) rv->bare_jit)->native_blocks : 0;
}

/* Called by emitted code only after spilling registers. Loads to x0 still
 * perform the bus transaction. Store boundaries invalidate validation stamps,
 * making self-modified code visible before the next compiled block executes. */
void rv_bare_jit_memory(riscv_t *rv, const rv_insn_t *ir)
{
    const uint32_t address = rv->X[ir->rs1] + ir->imm;
    uint32_t value = 0;
    bool store = false;
    rv->PC = ir->pc;
    switch (ir->opcode) {
    case rv_insn_lb: value = (int32_t)(int8_t)rv->io.mem_read_b(rv, address); break;
    case rv_insn_lbu: value = rv->io.mem_read_b(rv, address); break;
    case rv_insn_lh: value = (int32_t)(int16_t)rv->io.mem_read_s(rv, address); break;
    case rv_insn_lhu: value = rv->io.mem_read_s(rv, address); break;
    case rv_insn_lw: value = rv->io.mem_read_w(rv, address); break;
    case rv_insn_sb: rv->io.mem_write_b(rv, address, rv->X[ir->rs2]); store = true; break;
    case rv_insn_sh: rv->io.mem_write_s(rv, address, rv->X[ir->rs2]); store = true; break;
    case rv_insn_sw: rv->io.mem_write_w(rv, address, rv->X[ir->rs2]); store = true; break;
    default: abort();
    }
    if (store) ++((bare_jit_t *)rv->bare_jit)->epoch;
    else if (ir->rd && !rv->halt) rv->X[ir->rd] = value;
    if (!rv->halt) rv->PC = ir->pc + 4;
}
#endif
