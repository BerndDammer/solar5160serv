#include "cpu8088.h"
#include "cpu8088mem.h"

/* Segment override sentinel values */
enum {
    SEG_NONE = 0,
    SEG_ES,
    SEG_CS,
    SEG_SS,
    SEG_DS
};

typedef struct {
    uint8_t seg_override;
    bool rep_ne;
    bool rep_e;
    bool lock;
} prefixes_t;

typedef struct {
    bool is_mem;
    uint8_t reg_field;
    uint32_t mem_addr;
} modrm_t;

/* Fixed 8088 FLAGS bits: bit 1 is always 1, bits 3, 5, 12..15 are 0 */
#define FLAGS_FIXED_ONE  (1u << 1)
#define FLAGS_VALID_MASK 0x0FD5u

static uint16_t sanitize_flags(uint16_t f)
{
    return (uint16_t)((f & FLAGS_VALID_MASK) | FLAGS_FIXED_ONE);
}

uint32_t cpu8088_seg_phys(uint16_t seg, uint16_t offset)
{
    return ((((uint32_t)seg) << 4) + (uint32_t)offset) & 0xFFFFFu;
}

static uint8_t fetch8(cpu8088_t *cpu)
{
    uint32_t addr = cpu8088_seg_phys(cpu->cs, cpu->ip);
    cpu->ip = (uint16_t)(cpu->ip + 1u);
    return cpu8088_mem_read8(addr);
}

static uint16_t fetch16(cpu8088_t *cpu)
{
    uint8_t lo = fetch8(cpu);
    uint8_t hi = fetch8(cpu);
    return (uint16_t)(((uint16_t)hi << 8) | (uint16_t)lo);
}

static uint16_t get_seg_reg(const cpu8088_t *cpu, uint8_t seg_id)
{
    switch (seg_id) {
        case SEG_ES: return cpu->es;
        case SEG_CS: return cpu->cs;
        case SEG_SS: return cpu->ss;
        case SEG_DS:
        default:     return cpu->ds;
    }
}

static uint16_t effective_seg(const cpu8088_t *cpu, const prefixes_t *pfx, uint8_t default_seg)
{
    if (pfx->seg_override != SEG_NONE) {
        return get_seg_reg(cpu, pfx->seg_override);
    }
    return get_seg_reg(cpu, default_seg);
}

static uint8_t *get_reg8_ptr(cpu8088_t *cpu, uint8_t idx)
{
    switch (idx & 0x07u) {
        case 0: return &cpu->al;
        case 1: return &cpu->cl;
        case 2: return &cpu->dl;
        case 3: return &cpu->bl;
        case 4: return &cpu->ah;
        case 5: return &cpu->ch;
        case 6: return &cpu->dh;
        case 7:
        default: return &cpu->bh;
    }
}

static uint16_t *get_reg16_ptr(cpu8088_t *cpu, uint8_t idx)
{
    switch (idx & 0x07u) {
        case 0: return &cpu->ax;
        case 1: return &cpu->cx;
        case 2: return &cpu->dx;
        case 3: return &cpu->bx;
        case 4: return &cpu->sp;
        case 5: return &cpu->bp;
        case 6: return &cpu->si;
        case 7:
        default: return &cpu->di;
    }
}

static uint16_t *get_sreg_ptr(cpu8088_t *cpu, uint8_t idx)
{
    switch (idx & 0x03u) {
        case 0: return &cpu->es;
        case 1: return &cpu->cs;
        case 2: return &cpu->ss;
        case 3:
        default: return &cpu->ds;
    }
}

static void push16(cpu8088_t *cpu, uint16_t val)
{
    cpu->sp = (uint16_t)(cpu->sp - 2u);
    cpu8088_mem_write16(cpu8088_seg_phys(cpu->ss, cpu->sp), val);
}

static uint16_t pop16(cpu8088_t *cpu)
{
    uint16_t val = cpu8088_mem_read16(cpu8088_seg_phys(cpu->ss, cpu->sp));
    cpu->sp = (uint16_t)(cpu->sp + 2u);
    return val;
}

static bool parity_even8(uint8_t v)
{
    v ^= (uint8_t)(v >> 4);
    v ^= (uint8_t)(v >> 2);
    v ^= (uint8_t)(v >> 1);
    return (v & 1u) == 0u;
}

static void set_flag(cpu8088_t *cpu, uint16_t mask, bool cond)
{
    if (cond) {
        cpu->flags = sanitize_flags((uint16_t)(cpu->flags | mask));
    } else {
        cpu->flags = sanitize_flags((uint16_t)(cpu->flags & (uint16_t)(~mask)));
    }
}

static bool get_flag(const cpu8088_t *cpu, uint16_t mask)
{
    return (cpu->flags & mask) != 0u;
}

static void update_zsp8(cpu8088_t *cpu, uint8_t val)
{
    set_flag(cpu, CPU8088_FLAG_ZF, val == 0u);
    set_flag(cpu, CPU8088_FLAG_SF, (val & 0x80u) != 0u);
    set_flag(cpu, CPU8088_FLAG_PF, parity_even8(val));
}

static void update_zsp16(cpu8088_t *cpu, uint16_t val)
{
    set_flag(cpu, CPU8088_FLAG_ZF, val == 0u);
    set_flag(cpu, CPU8088_FLAG_SF, (val & 0x8000u) != 0u);
    set_flag(cpu, CPU8088_FLAG_PF, parity_even8((uint8_t)(val & 0xFFu)));
}

static uint8_t alu_add8(cpu8088_t *cpu, uint8_t a, uint8_t b, uint8_t carry_in)
{
    uint16_t sum = (uint16_t)a + (uint16_t)b + (uint16_t)carry_in;
    uint8_t res = (uint8_t)(sum & 0xFFu);

    set_flag(cpu, CPU8088_FLAG_CF, sum > 0xFFu);
    set_flag(cpu, CPU8088_FLAG_AF, (((a & 0x0Fu) + (b & 0x0Fu) + (carry_in & 0x0Fu)) & 0x10u) != 0u);
    set_flag(cpu, CPU8088_FLAG_OF, (((~(a ^ b)) & (a ^ res)) & 0x80u) != 0u);
    update_zsp8(cpu, res);
    return res;
}

static uint16_t alu_add16(cpu8088_t *cpu, uint16_t a, uint16_t b, uint16_t carry_in)
{
    uint32_t sum = (uint32_t)a + (uint32_t)b + (uint32_t)carry_in;
    uint16_t res = (uint16_t)(sum & 0xFFFFu);

    set_flag(cpu, CPU8088_FLAG_CF, sum > 0xFFFFu);
    set_flag(cpu, CPU8088_FLAG_AF, (((a & 0x0Fu) + (b & 0x0Fu) + (carry_in & 0x0Fu)) & 0x10u) != 0u);
    set_flag(cpu, CPU8088_FLAG_OF, (((~(a ^ b)) & (a ^ res)) & 0x8000u) != 0u);
    update_zsp16(cpu, res);
    return res;
}

static uint8_t alu_sub8(cpu8088_t *cpu, uint8_t a, uint8_t b, uint8_t borrow_in)
{
    uint16_t subtrahend = (uint16_t)b + (uint16_t)borrow_in;
    uint16_t diff = (uint16_t)a - subtrahend;
    uint8_t res = (uint8_t)(diff & 0xFFu);

    set_flag(cpu, CPU8088_FLAG_CF, (uint16_t)a < subtrahend);
    set_flag(cpu, CPU8088_FLAG_AF, ((int)(a & 0x0Fu) - (int)(b & 0x0Fu) - (int)(borrow_in & 0x0Fu)) < 0);
    set_flag(cpu, CPU8088_FLAG_OF, (((a ^ b) & (a ^ res)) & 0x80u) != 0u);
    update_zsp8(cpu, res);
    return res;
}

static uint16_t alu_sub16(cpu8088_t *cpu, uint16_t a, uint16_t b, uint16_t borrow_in)
{
    uint32_t subtrahend = (uint32_t)b + (uint32_t)borrow_in;
    uint32_t diff = (uint32_t)a - subtrahend;
    uint16_t res = (uint16_t)(diff & 0xFFFFu);

    set_flag(cpu, CPU8088_FLAG_CF, (uint32_t)a < subtrahend);
    set_flag(cpu, CPU8088_FLAG_AF, ((int)(a & 0x0Fu) - (int)(b & 0x0Fu) - (int)(borrow_in & 0x0Fu)) < 0);
    set_flag(cpu, CPU8088_FLAG_OF, (((a ^ b) & (a ^ res)) & 0x8000u) != 0u);
    update_zsp16(cpu, res);
    return res;
}

static void alu_logic8(cpu8088_t *cpu, uint8_t res)
{
    set_flag(cpu, CPU8088_FLAG_CF, false);
    set_flag(cpu, CPU8088_FLAG_OF, false);
    set_flag(cpu, CPU8088_FLAG_AF, false);
    update_zsp8(cpu, res);
}

static void alu_logic16(cpu8088_t *cpu, uint16_t res)
{
    set_flag(cpu, CPU8088_FLAG_CF, false);
    set_flag(cpu, CPU8088_FLAG_OF, false);
    set_flag(cpu, CPU8088_FLAG_AF, false);
    update_zsp16(cpu, res);
}

static modrm_t decode_modrm(cpu8088_t *cpu, const prefixes_t *pfx)
{
    modrm_t m;
    uint8_t modrm = fetch8(cpu);
    uint8_t mod = (uint8_t)((modrm >> 6) & 0x03u);
    uint8_t rm = (uint8_t)(modrm & 0x07u);
    uint16_t offset = 0u;
    uint8_t def_seg = SEG_DS;

    m.reg_field = (uint8_t)((modrm >> 3) & 0x07u);
    m.mem_addr = 0u;

    if (mod == 3u) {
        m.is_mem = false;
        return m;
    }

    m.is_mem = true;

    switch (rm) {
        case 0: offset = (uint16_t)(cpu->bx + cpu->si); def_seg = SEG_DS; break;
        case 1: offset = (uint16_t)(cpu->bx + cpu->di); def_seg = SEG_DS; break;
        case 2: offset = (uint16_t)(cpu->bp + cpu->si); def_seg = SEG_SS; break;
        case 3: offset = (uint16_t)(cpu->bp + cpu->di); def_seg = SEG_SS; break;
        case 4: offset = cpu->si;                      def_seg = SEG_DS; break;
        case 5: offset = cpu->di;                      def_seg = SEG_DS; break;
        case 6:
            if (mod == 0u) {
                offset = fetch16(cpu);
                def_seg = SEG_DS;
            } else {
                offset = cpu->bp;
                def_seg = SEG_SS;
            }
            break;
        case 7:
        default:
            offset = cpu->bx;
            def_seg = SEG_DS;
            break;
    }

    if (mod == 1u) {
        int8_t disp8 = (int8_t)fetch8(cpu);
        offset = (uint16_t)((int32_t)offset + (int32_t)disp8);
    } else if (mod == 2u) {
        int16_t disp16 = (int16_t)fetch16(cpu);
        offset = (uint16_t)((int32_t)offset + (int32_t)disp16);
    }

    m.mem_addr = cpu8088_seg_phys(effective_seg(cpu, pfx, def_seg), offset);
    return m;
}

static uint8_t read_rm8(const cpu8088_t *cpu, const modrm_t *m)
{
    if (m->is_mem) {
        return cpu8088_mem_read8(m->mem_addr);
    }
    return *get_reg8_ptr((cpu8088_t *)(uintptr_t)cpu, (uint8_t)(m->mem_addr & 0x07u));
}

/* Note: when mod == 3, store rm index in m->mem_addr for uniform helpers */
static modrm_t decode_modrm_full(cpu8088_t *cpu, const prefixes_t *pfx)
{
    uint32_t saved_ip = cpu->ip;
    uint8_t modrm_byte = cpu8088_mem_read8(cpu8088_seg_phys(cpu->cs, saved_ip));
    modrm_t m = decode_modrm(cpu, pfx);
    if (!m.is_mem) {
        m.mem_addr = (uint32_t)(modrm_byte & 0x07u);
    }
    return m;
}

static uint8_t get_rm8(cpu8088_t *cpu, const modrm_t *m)
{
    if (m->is_mem) {
        return cpu8088_mem_read8(m->mem_addr);
    }
    return *get_reg8_ptr(cpu, (uint8_t)(m->mem_addr & 0x07u));
}

static void put_rm8(cpu8088_t *cpu, const modrm_t *m, uint8_t val)
{
    if (m->is_mem) {
        cpu8088_mem_write8(m->mem_addr, val);
    } else {
        *get_reg8_ptr(cpu, (uint8_t)(m->mem_addr & 0x07u)) = val;
    }
}

static uint16_t get_rm16(cpu8088_t *cpu, const modrm_t *m)
{
    if (m->is_mem) {
        return cpu8088_mem_read16(m->mem_addr);
    }
    return *get_reg16_ptr(cpu, (uint8_t)(m->mem_addr & 0x07u));
}

static void put_rm16(cpu8088_t *cpu, const modrm_t *m, uint16_t val)
{
    if (m->is_mem) {
        cpu8088_mem_write16(m->mem_addr, val);
    } else {
        *get_reg16_ptr(cpu, (uint8_t)(m->mem_addr & 0x07u)) = val;
    }
}

static void service_interrupt(cpu8088_t *cpu, uint8_t vector)
{
    uint32_t ivt_addr = ((uint32_t)vector) * 4u;
    uint16_t new_ip = cpu8088_mem_read16(ivt_addr);
    uint16_t new_cs = cpu8088_mem_read16(ivt_addr + 2u);

    push16(cpu, sanitize_flags(cpu->flags));
    push16(cpu, cpu->cs);
    push16(cpu, cpu->ip);

    set_flag(cpu, CPU8088_FLAG_IF, false);
    set_flag(cpu, CPU8088_FLAG_TF, false);

    cpu->cs = new_cs;
    cpu->ip = new_ip;
    cpu->halted = false;
}

static bool eval_jcc(const cpu8088_t *cpu, uint8_t cc)
{
    bool cf = get_flag(cpu, CPU8088_FLAG_CF);
    bool zf = get_flag(cpu, CPU8088_FLAG_ZF);
    bool sf = get_flag(cpu, CPU8088_FLAG_SF);
    bool of = get_flag(cpu, CPU8088_FLAG_OF);
    bool pf = get_flag(cpu, CPU8088_FLAG_PF);

    switch (cc & 0x0Fu) {
        case 0x0: return of;                     /* JO */
        case 0x1: return !of;                    /* JNO */
        case 0x2: return cf;                     /* JB / JNAE / JC */
        case 0x3: return !cf;                    /* JNB / JAE / JNC */
        case 0x4: return zf;                     /* JE / JZ */
        case 0x5: return !zf;                    /* JNE / JNZ */
        case 0x6: return cf || zf;               /* JBE / JNA */
        case 0x7: return !cf && !zf;             /* JNBE / JA */
        case 0x8: return sf;                     /* JS */
        case 0x9: return !sf;                    /* JNS */
        case 0xA: return pf;                     /* JP / JPE */
        case 0xB: return !pf;                    /* JNP / JPO */
        case 0xC: return sf != of;               /* JL / JNGE */
        case 0xD: return sf == of;               /* JNL / JGE */
        case 0xE: return zf || (sf != of);       /* JLE / JNG */
        case 0xF:
        default:  return !zf && (sf == of);      /* JNLE / JG */
    }
}

static void exec_alu8_op(cpu8088_t *cpu, uint8_t op, uint8_t *dst, uint8_t src, bool write_dst)
{
    uint8_t res = 0u;
    switch (op & 0x07u) {
        case 0: res = alu_add8(cpu, *dst, src, 0u); break;                 /* ADD */
        case 1:                                                             /* OR */
            res = (uint8_t)(*dst | src);
            alu_logic8(cpu, res);
            break;
        case 2: res = alu_add8(cpu, *dst, src, get_flag(cpu, CPU8088_FLAG_CF) ? 1u : 0u); break; /* ADC */
        case 3: res = alu_sub8(cpu, *dst, src, get_flag(cpu, CPU8088_FLAG_CF) ? 1u : 0u); break; /* SBB */
        case 4:                                                             /* AND */
            res = (uint8_t)(*dst & src);
            alu_logic8(cpu, res);
            break;
        case 5: res = alu_sub8(cpu, *dst, src, 0u); break;                 /* SUB */
        case 6:                                                             /* XOR */
            res = (uint8_t)(*dst ^ src);
            alu_logic8(cpu, res);
            break;
        case 7:                                                             /* CMP */
        default:
            (void)alu_sub8(cpu, *dst, src, 0u);
            return;
    }
    if (write_dst) {
        *dst = res;
    }
}

static void exec_alu16_op(cpu8088_t *cpu, uint8_t op, uint16_t *dst, uint16_t src, bool write_dst)
{
    uint16_t res = 0u;
    switch (op & 0x07u) {
        case 0: res = alu_add16(cpu, *dst, src, 0u); break;                /* ADD */
        case 1:                                                             /* OR */
            res = (uint16_t)(*dst | src);
            alu_logic16(cpu, res);
            break;
        case 2: res = alu_add16(cpu, *dst, src, get_flag(cpu, CPU8088_FLAG_CF) ? 1u : 0u); break; /* ADC */
        case 3: res = alu_sub16(cpu, *dst, src, get_flag(cpu, CPU8088_FLAG_CF) ? 1u : 0u); break; /* SBB */
        case 4:                                                             /* AND */
            res = (uint16_t)(*dst & src);
            alu_logic16(cpu, res);
            break;
        case 5: res = alu_sub16(cpu, *dst, src, 0u); break;                /* SUB */
        case 6:                                                             /* XOR */
            res = (uint16_t)(*dst ^ src);
            alu_logic16(cpu, res);
            break;
        case 7:                                                             /* CMP */
        default:
            (void)alu_sub16(cpu, *dst, src, 0u);
            return;
    }
    if (write_dst) {
        *dst = res;
    }
}

static void exec_string_op(cpu8088_t *cpu, uint8_t opcode, const prefixes_t *pfx)
{
    bool is_word = (opcode & 0x01u) != 0u;
    int32_t step = get_flag(cpu, CPU8088_FLAG_DF) ? (is_word ? -2 : -1) : (is_word ? 2 : 1);
    bool has_rep = pfx->rep_e || pfx->rep_ne;

    for (;;) {
        if (has_rep && cpu->cx == 0u) {
            break;
        }

        uint32_t ds_si = cpu8088_seg_phys(effective_seg(cpu, pfx, SEG_DS), cpu->si);
        uint32_t es_di = cpu8088_seg_phys(cpu->es, cpu->di);

        switch (opcode) {
            case 0xA4u: /* MOVSB */
                cpu8088_mem_write8(es_di, cpu8088_mem_read8(ds_si));
                cpu->si = (uint16_t)((int32_t)cpu->si + step);
                cpu->di = (uint16_t)((int32_t)cpu->di + step);
                break;
            case 0xA5u: /* MOVSW */
                cpu8088_mem_write16(es_di, cpu8088_mem_read16(ds_si));
                cpu->si = (uint16_t)((int32_t)cpu->si + step);
                cpu->di = (uint16_t)((int32_t)cpu->di + step);
                break;
            case 0xA6u: /* CMPSB */
                (void)alu_sub8(cpu, cpu8088_mem_read8(ds_si), cpu8088_mem_read8(es_di), 0u);
                cpu->si = (uint16_t)((int32_t)cpu->si + step);
                cpu->di = (uint16_t)((int32_t)cpu->di + step);
                break;
            case 0xA7u: /* CMPSW */
                (void)alu_sub16(cpu, cpu8088_mem_read16(ds_si), cpu8088_mem_read16(es_di), 0u);
                cpu->si = (uint16_t)((int32_t)cpu->si + step);
                cpu->di = (uint16_t)((int32_t)cpu->di + step);
                break;
            case 0xAAu: /* STOSB */
                cpu8088_mem_write8(es_di, cpu->al);
                cpu->di = (uint16_t)((int32_t)cpu->di + step);
                break;
            case 0xABu: /* STOSW */
                cpu8088_mem_write16(es_di, cpu->ax);
                cpu->di = (uint16_t)((int32_t)cpu->di + step);
                break;
            case 0xACu: /* LODSB */
                cpu->al = cpu8088_mem_read8(ds_si);
                cpu->si = (uint16_t)((int32_t)cpu->si + step);
                break;
            case 0xADu: /* LODSW */
                cpu->ax = cpu8088_mem_read16(ds_si);
                cpu->si = (uint16_t)((int32_t)cpu->si + step);
                break;
            case 0xAEu: /* SCASB */
                (void)alu_sub8(cpu, cpu->al, cpu8088_mem_read8(es_di), 0u);
                cpu->di = (uint16_t)((int32_t)cpu->di + step);
                break;
            case 0xAFu: /* SCASW */
                (void)alu_sub16(cpu, cpu->ax, cpu8088_mem_read16(es_di), 0u);
                cpu->di = (uint16_t)((int32_t)cpu->di + step);
                break;
            default:
                return;
        }

        if (!has_rep) {
            break;
        }

        cpu->cx = (uint16_t)(cpu->cx - 1u);

        if (opcode == 0xA6u || opcode == 0xA7u || opcode == 0xAEu || opcode == 0xAFu) {
            bool zf = get_flag(cpu, CPU8088_FLAG_ZF);
            if (pfx->rep_e && !zf) {
                break;
            }
            if (pfx->rep_ne && zf) {
                break;
            }
        }

        if (cpu->cx == 0u) {
            break;
        }
    }
}

void cpu8088_reset(cpu8088_t *cpu)
{
    if (cpu == NULL) {
        return;
    }

    cpu->ax = 0u;
    cpu->bx = 0u;
    cpu->cx = 0u;
    cpu->dx = 0u;
    cpu->sp = 0u;
    cpu->bp = 0u;
    cpu->si = 0u;
    cpu->di = 0u;

    cpu->cs = 0xFFFFu;
    cpu->ds = 0x0000u;
    cpu->ss = 0x0000u;
    cpu->es = 0x0000u;
    cpu->ip = 0x0000u;
    cpu->flags = FLAGS_FIXED_ONE;

    cpu->halted = false;
    cpu->nmi_pending = false;
    cpu->intr_pending = false;
    cpu->intr_vector = 0u;
    cpu->cycles = 0u;
}

void cpu8088_intr(cpu8088_t *cpu, uint8_t vector)
{
    if (cpu == NULL) {
        return;
    }
    cpu->intr_pending = true;
    cpu->intr_vector = vector;
}

void cpu8088_nmi(cpu8088_t *cpu)
{
    if (cpu == NULL) {
        return;
    }
    cpu->nmi_pending = true;
}

uint32_t cpu8088_step(cpu8088_t *cpu)
{
    uint32_t start_cycles;
    prefixes_t pfx;
    uint8_t op;
    bool done_pfx = false;

    if (cpu == NULL) {
        return 0u;
    }

    start_cycles = cpu->cycles;

    if (cpu->nmi_pending) {
        cpu->nmi_pending = false;
        service_interrupt(cpu, 2u);
        cpu->cycles += 50u;
        return cpu->cycles - start_cycles;
    }

    if (cpu->intr_pending && get_flag(cpu, CPU8088_FLAG_IF)) {
        uint8_t vec = cpu->intr_vector;
        cpu->intr_pending = false;
        service_interrupt(cpu, vec);
        cpu->cycles += 51u;
        return cpu->cycles - start_cycles;
    }

    if (cpu->halted) {
        cpu->cycles += 2u;
        return cpu->cycles - start_cycles;
    }

    pfx.seg_override = SEG_NONE;
    pfx.rep_ne = false;
    pfx.rep_e = false;
    pfx.lock = false;
    op = 0u;

    while (!done_pfx) {
        op = fetch8(cpu);
        switch (op) {
            case 0x26u: pfx.seg_override = SEG_ES; break;
            case 0x2Eu: pfx.seg_override = SEG_CS; break;
            case 0x36u: pfx.seg_override = SEG_SS; break;
            case 0x3Eu: pfx.seg_override = SEG_DS; break;
            case 0xF0u: pfx.lock = true; break;
            case 0xF2u: pfx.rep_ne = true; break;
            case 0xF3u: pfx.rep_e = true; break;
            default:    done_pfx = true; break;
        }
    }

    /* Group ALU byte/word operations: 0x00..0x3D */
    if (op <= 0x3Du && (op & 0x07u) <= 0x05u) {
        uint8_t alu_op = (uint8_t)((op >> 3) & 0x07u);
        bool is_word = (op & 0x01u) != 0u;
        bool dir_reg_dst = (op & 0x02u) != 0u;
        bool is_acc = (op & 0x04u) != 0u;

        if (is_acc) {
            if (!is_word) {
                uint8_t imm = fetch8(cpu);
                exec_alu8_op(cpu, alu_op, &cpu->al, imm, true);
            } else {
                uint16_t imm = fetch16(cpu);
                exec_alu16_op(cpu, alu_op, &cpu->ax, imm, true);
            }
        } else {
            modrm_t m = decode_modrm_full(cpu, &pfx);
            if (!is_word) {
                uint8_t *regp = get_reg8_ptr(cpu, m.reg_field);
                uint8_t rmp = get_rm8(cpu, &m);
                if (dir_reg_dst) {
                    exec_alu8_op(cpu, alu_op, regp, rmp, true);
                } else {
                    uint8_t tmp = rmp;
                    exec_alu8_op(cpu, alu_op, &tmp, *regp, true);
                    put_rm8(cpu, &m, tmp);
                }
            } else {
                uint16_t *regp = get_reg16_ptr(cpu, m.reg_field);
                uint16_t rmp = get_rm16(cpu, &m);
                if (dir_reg_dst) {
                    exec_alu16_op(cpu, alu_op, regp, rmp, true);
                } else {
                    uint16_t tmp = rmp;
                    exec_alu16_op(cpu, alu_op, &tmp, *regp, true);
                    put_rm16(cpu, &m, tmp);
                }
            }
        }
        cpu->cycles += 4u;
        return cpu->cycles - start_cycles;
    }

    /* PUSH / POP segment registers */
    if (op == 0x06u || op == 0x0Eu || op == 0x16u || op == 0x1Eu) {
        push16(cpu, *get_sreg_ptr(cpu, (uint8_t)((op >> 3) & 0x03u)));
        cpu->cycles += 10u;
        return cpu->cycles - start_cycles;
    }
    if (op == 0x07u || op == 0x17u || op == 0x1Fu) {
        *get_sreg_ptr(cpu, (uint8_t)((op >> 3) & 0x03u)) = pop16(cpu);
        cpu->cycles += 8u;
        return cpu->cycles - start_cycles;
    }

    /* INC / DEC r16 (0x40..0x4F) */
    if (op >= 0x40u && op <= 0x4Fu) {
        uint16_t *regp = get_reg16_ptr(cpu, (uint8_t)(op & 0x07u));
        bool old_cf = get_flag(cpu, CPU8088_FLAG_CF);
        if ((op & 0x08u) == 0u) {
            *regp = alu_add16(cpu, *regp, 1u, 0u);
        } else {
            *regp = alu_sub16(cpu, *regp, 1u, 0u);
        }
        set_flag(cpu, CPU8088_FLAG_CF, old_cf);
        cpu->cycles += 2u;
        return cpu->cycles - start_cycles;
    }

    /* PUSH / POP r16 (0x50..0x5F) */
    if (op >= 0x50u && op <= 0x57u) {
        push16(cpu, *get_reg16_ptr(cpu, (uint8_t)(op & 0x07u)));
        cpu->cycles += 11u;
        return cpu->cycles - start_cycles;
    }
    if (op >= 0x58u && op <= 0x5Fu) {
        *get_reg16_ptr(cpu, (uint8_t)(op & 0x07u)) = pop16(cpu);
        cpu->cycles += 8u;
        return cpu->cycles - start_cycles;
    }

    /* Conditional jumps Jcc (0x70..0x7F) */
    if (op >= 0x70u && op <= 0x7Fu) {
        int8_t rel8 = (int8_t)fetch8(cpu);
        if (eval_jcc(cpu, op)) {
            cpu->ip = (uint16_t)((int32_t)cpu->ip + (int32_t)rel8);
            cpu->cycles += 16u;
        } else {
            cpu->cycles += 4u;
        }
        return cpu->cycles - start_cycles;
    }

    /* Immediate ALU Group 1: 0x80..0x83 */
    if (op >= 0x80u && op <= 0x83u) {
        modrm_t m = decode_modrm_full(cpu, &pfx);
        uint8_t alu_op = m.reg_field;
        if (op == 0x80u) {
            uint8_t imm8 = fetch8(cpu);
            uint8_t tmp = get_rm8(cpu, &m);
            exec_alu8_op(cpu, alu_op, &tmp, imm8, true);
            put_rm8(cpu, &m, tmp);
        } else if (op == 0x81u) {
            uint16_t imm16 = fetch16(cpu);
            uint16_t tmp = get_rm16(cpu, &m);
            exec_alu16_op(cpu, alu_op, &tmp, imm16, true);
            put_rm16(cpu, &m, tmp);
        } else if (op == 0x82u) {
            int8_t simm = (int8_t)fetch8(cpu);
            uint8_t tmp = get_rm8(cpu, &m);
            exec_alu8_op(cpu, alu_op, &tmp, (uint8_t)simm, true);
            put_rm8(cpu, &m, tmp);
        } else {
            int8_t simm = (int8_t)fetch8(cpu);
            uint16_t tmp = get_rm16(cpu, &m);
            exec_alu16_op(cpu, alu_op, &tmp, (uint16_t)(int16_t)simm, true);
            put_rm16(cpu, &m, tmp);
        }
        cpu->cycles += 4u;
        return cpu->cycles - start_cycles;
    }

    switch (op) {
        case 0x84u: { /* TEST r/m8, r8 */
            modrm_t m = decode_modrm_full(cpu, &pfx);
            alu_logic8(cpu, (uint8_t)(get_rm8(cpu, &m) & *get_reg8_ptr(cpu, m.reg_field)));
            cpu->cycles += 3u;
            break;
        }
        case 0x85u: { /* TEST r/m16, r16 */
            modrm_t m = decode_modrm_full(cpu, &pfx);
            alu_logic16(cpu, (uint16_t)(get_rm16(cpu, &m) & *get_reg16_ptr(cpu, m.reg_field)));
            cpu->cycles += 3u;
            break;
        }
        case 0x86u: { /* XCHG r/m8, r8 */
            modrm_t m = decode_modrm_full(cpu, &pfx);
            uint8_t *rp = get_reg8_ptr(cpu, m.reg_field);
            uint8_t tmp = get_rm8(cpu, &m);
            put_rm8(cpu, &m, *rp);
            *rp = tmp;
            cpu->cycles += 4u;
            break;
        }
        case 0x87u: { /* XCHG r/m16, r16 */
            modrm_t m = decode_modrm_full(cpu, &pfx);
            uint16_t *rp = get_reg16_ptr(cpu, m.reg_field);
            uint16_t tmp = get_rm16(cpu, &m);
            put_rm16(cpu, &m, *rp);
            *rp = tmp;
            cpu->cycles += 4u;
            break;
        }
        case 0x88u: { /* MOV r/m8, r8 */
            modrm_t m = decode_modrm_full(cpu, &pfx);
            put_rm8(cpu, &m, *get_reg8_ptr(cpu, m.reg_field));
            cpu->cycles += 3u;
            break;
        }
        case 0x89u: { /* MOV r/m16, r16 */
            modrm_t m = decode_modrm_full(cpu, &pfx);
            put_rm16(cpu, &m, *get_reg16_ptr(cpu, m.reg_field));
            cpu->cycles += 3u;
            break;
        }
        case 0x8Au: { /* MOV r8, r/m8 */
            modrm_t m = decode_modrm_full(cpu, &pfx);
            *get_reg8_ptr(cpu, m.reg_field) = get_rm8(cpu, &m);
            cpu->cycles += 2u;
            break;
        }
        case 0x8Bu: { /* MOV r16, r/m16 */
            modrm_t m = decode_modrm_full(cpu, &pfx);
            *get_reg16_ptr(cpu, m.reg_field) = get_rm16(cpu, &m);
            cpu->cycles += 2u;
            break;
        }
        case 0x8Cu: { /* MOV r/m16, Sreg */
            modrm_t m = decode_modrm_full(cpu, &pfx);
            put_rm16(cpu, &m, *get_sreg_ptr(cpu, m.reg_field));
            cpu->cycles += 2u;
            break;
        }
        case 0x8Du: { /* LEA r16, m */
            modrm_t m = decode_modrm_full(cpu, &pfx);
            if (m.is_mem) {
                uint16_t seg = effective_seg(cpu, &pfx, SEG_DS);
                uint32_t seg_base = (((uint32_t)seg) << 4) & 0xFFFFFu;
                *get_reg16_ptr(cpu, m.reg_field) = (uint16_t)((m.mem_addr - seg_base) & 0xFFFFu);
            }
            cpu->cycles += 2u;
            break;
        }
        case 0x8Eu: { /* MOV Sreg, r/m16 */
            modrm_t m = decode_modrm_full(cpu, &pfx);
            *get_sreg_ptr(cpu, m.reg_field) = get_rm16(cpu, &m);
            cpu->cycles += 2u;
            break;
        }
        case 0x8Fu: { /* POP r/m16 */
            modrm_t m = decode_modrm_full(cpu, &pfx);
            put_rm16(cpu, &m, pop16(cpu));
            cpu->cycles += 12u;
            break;
        }
        case 0x90u: /* NOP */
            cpu->cycles += 3u;
            break;
        case 0x91u: case 0x92u: case 0x93u:
        case 0x94u: case 0x95u: case 0x96u: case 0x97u: { /* XCHG AX, r16 */
            uint16_t *rp = get_reg16_ptr(cpu, (uint8_t)(op & 0x07u));
            uint16_t tmp = cpu->ax;
            cpu->ax = *rp;
            *rp = tmp;
            cpu->cycles += 3u;
            break;
        }
        case 0x98u: /* CBW */
            cpu->ax = (uint16_t)(int16_t)(int8_t)cpu->al;
            cpu->cycles += 2u;
            break;
        case 0x99u: /* CWD */
            cpu->dx = ((cpu->ax & 0x8000u) != 0u) ? 0xFFFFu : 0x0000u;
            cpu->cycles += 5u;
            break;
        case 0x9Au: { /* CALL far ptr16:16 */
            uint16_t disp = fetch16(cpu);
            uint16_t seg = fetch16(cpu);
            push16(cpu, cpu->cs);
            push16(cpu, cpu->ip);
            cpu->cs = seg;
            cpu->ip = disp;
            cpu->cycles += 28u;
            break;
        }
        case 0x9Bu: /* WAIT */
            cpu->cycles += 4u;
            break;
        case 0x9Cu: /* PUSHF */
            push16(cpu, sanitize_flags(cpu->flags));
            cpu->cycles += 10u;
            break;
        case 0x9Du: /* POPF */
            cpu->flags = sanitize_flags(pop16(cpu));
            cpu->cycles += 8u;
            break;
        case 0x9Eu: /* SAHF */
            cpu->flags = sanitize_flags((uint16_t)((cpu->flags & 0xFF00u) | (uint16_t)cpu->ah));
            cpu->cycles += 4u;
            break;
        case 0x9Fu: /* LAHF */
            cpu->ah = (uint8_t)(cpu->flags & 0xFFu);
            cpu->cycles += 4u;
            break;
        case 0xA0u: { /* MOV AL, moffs8 */
            uint16_t off = fetch16(cpu);
            cpu->al = cpu8088_mem_read8(cpu8088_seg_phys(effective_seg(cpu, &pfx, SEG_DS), off));
            cpu->cycles += 10u;
            break;
        }
        case 0xA1u: { /* MOV AX, moffs16 */
            uint16_t off = fetch16(cpu);
            cpu->ax = cpu8088_mem_read16(cpu8088_seg_phys(effective_seg(cpu, &pfx, SEG_DS), off));
            cpu->cycles += 10u;
            break;
        }
        case 0xA2u: { /* MOV moffs8, AL */
            uint16_t off = fetch16(cpu);
            cpu8088_mem_write8(cpu8088_seg_phys(effective_seg(cpu, &pfx, SEG_DS), off), cpu->al);
            cpu->cycles += 10u;
            break;
        }
        case 0xA3u: { /* MOV moffs16, AX */
            uint16_t off = fetch16(cpu);
            cpu8088_mem_write16(cpu8088_seg_phys(effective_seg(cpu, &pfx, SEG_DS), off), cpu->ax);
            cpu->cycles += 10u;
            break;
        }
        case 0xA4u: case 0xA5u: case 0xA6u: case 0xA7u:
        case 0xAAu: case 0xABu: case 0xACu: case 0xADu:
        case 0xAEu: case 0xAFu:
            exec_string_op(cpu, op, &pfx);
            cpu->cycles += 9u;
            break;
        case 0xA8u: /* TEST AL, imm8 */
            alu_logic8(cpu, (uint8_t)(cpu->al & fetch8(cpu)));
            cpu->cycles += 4u;
            break;
        case 0xA9u: /* TEST AX, imm16 */
            alu_logic16(cpu, (uint16_t)(cpu->ax & fetch16(cpu)));
            cpu->cycles += 4u;
            break;
        case 0xB0u: case 0xB1u: case 0xB2u: case 0xB3u:
        case 0xB4u: case 0xB5u: case 0xB6u: case 0xB7u: /* MOV r8, imm8 */
            *get_reg8_ptr(cpu, (uint8_t)(op & 0x07u)) = fetch8(cpu);
            cpu->cycles += 4u;
            break;
        case 0xB8u: case 0xB9u: case 0xBAu: case 0xBBu:
        case 0xBCu: case 0xBDu: case 0xBEu: case 0xBFu: /* MOV r16, imm16 */
            *get_reg16_ptr(cpu, (uint8_t)(op & 0x07u)) = fetch16(cpu);
            cpu->cycles += 4u;
            break;
        case 0xC0u: case 0xC1u: { /* 80186+ shift/rotate imm8 */
            modrm_t m = decode_modrm_full(cpu, &pfx);
            uint8_t cnt = (uint8_t)(fetch8(cpu) & 0x1Fu);
            if ((op & 0x01u) == 0u) {
                uint8_t v = get_rm8(cpu, &m);
                if (cnt > 0u) {
                    if (m.reg_field == 4u || m.reg_field == 6u) { /* SHL/SAL */
                        set_flag(cpu, CPU8088_FLAG_CF, ((v << (cnt - 1u)) & 0x80u) != 0u);
                        v = (uint8_t)(v << cnt);
                        update_zsp8(cpu, v);
                    } else if (m.reg_field == 5u) { /* SHR */
                        set_flag(cpu, CPU8088_FLAG_CF, ((v >> (cnt - 1u)) & 0x01u) != 0u);
                        v = (uint8_t)(v >> cnt);
                        update_zsp8(cpu, v);
                    } else if (m.reg_field == 7u) { /* SAR */
                        set_flag(cpu, CPU8088_FLAG_CF, (((int8_t)v >> (cnt - 1u)) & 0x01) != 0);
                        v = (uint8_t)((int8_t)v >> cnt);
                        update_zsp8(cpu, v);
                    }
                    put_rm8(cpu, &m, v);
                }
            } else {
                uint16_t v = get_rm16(cpu, &m);
                if (cnt > 0u) {
                    if (m.reg_field == 4u || m.reg_field == 6u) { /* SHL/SAL */
                        set_flag(cpu, CPU8088_FLAG_CF, ((v << (cnt - 1u)) & 0x8000u) != 0u);
                        v = (uint16_t)(v << cnt);
                        update_zsp16(cpu, v);
                    } else if (m.reg_field == 5u) { /* SHR */
                        set_flag(cpu, CPU8088_FLAG_CF, ((v >> (cnt - 1u)) & 0x0001u) != 0u);
                        v = (uint16_t)(v >> cnt);
                        update_zsp16(cpu, v);
                    } else if (m.reg_field == 7u) { /* SAR */
                        set_flag(cpu, CPU8088_FLAG_CF, (((int16_t)v >> (cnt - 1u)) & 0x0001) != 0);
                        v = (uint16_t)((int16_t)v >> cnt);
                        update_zsp16(cpu, v);
                    }
                    put_rm16(cpu, &m, v);
                }
            }
            cpu->cycles += 5u;
            break;
        }
        case 0xC2u: { /* RET imm16 */
            uint16_t imm = fetch16(cpu);
            cpu->ip = pop16(cpu);
            cpu->sp = (uint16_t)(cpu->sp + imm);
            cpu->cycles += 20u;
            break;
        }
        case 0xC3u: /* RET */
            cpu->ip = pop16(cpu);
            cpu->cycles += 16u;
            break;
        case 0xC4u: { /* LES r16, m16:16 */
            modrm_t m = decode_modrm_full(cpu, &pfx);
            if (m.is_mem) {
                *get_reg16_ptr(cpu, m.reg_field) = cpu8088_mem_read16(m.mem_addr);
                cpu->es = cpu8088_mem_read16(m.mem_addr + 2u);
            }
            cpu->cycles += 16u;
            break;
        }
        case 0xC5u: { /* LDS r16, m16:16 */
            modrm_t m = decode_modrm_full(cpu, &pfx);
            if (m.is_mem) {
                *get_reg16_ptr(cpu, m.reg_field) = cpu8088_mem_read16(m.mem_addr);
                cpu->ds = cpu8088_mem_read16(m.mem_addr + 2u);
            }
            cpu->cycles += 16u;
            break;
        }
        case 0xC6u: { /* MOV r/m8, imm8 */
            modrm_t m = decode_modrm_full(cpu, &pfx);
            uint8_t imm = fetch8(cpu);
            put_rm8(cpu, &m, imm);
            cpu->cycles += 4u;
            break;
        }
        case 0xC7u: { /* MOV r/m16, imm16 */
            modrm_t m = decode_modrm_full(cpu, &pfx);
            uint16_t imm = fetch16(cpu);
            put_rm16(cpu, &m, imm);
            cpu->cycles += 4u;
            break;
        }
        case 0xCAu: { /* RETF imm16 */
            uint16_t imm = fetch16(cpu);
            cpu->ip = pop16(cpu);
            cpu->cs = pop16(cpu);
            cpu->sp = (uint16_t)(cpu->sp + imm);
            cpu->cycles += 25u;
            break;
        }
        case 0xCBu: /* RETF */
            cpu->ip = pop16(cpu);
            cpu->cs = pop16(cpu);
            cpu->cycles += 26u;
            break;
        case 0xCCu: /* INT 3 */
            service_interrupt(cpu, 3u);
            cpu->cycles += 52u;
            break;
        case 0xCDu: { /* INT imm8 */
            uint8_t vec = fetch8(cpu);
            service_interrupt(cpu, vec);
            cpu->cycles += 51u;
            break;
        }
        case 0xCEu: /* INTO */
            if (get_flag(cpu, CPU8088_FLAG_OF)) {
                service_interrupt(cpu, 4u);
                cpu->cycles += 53u;
            } else {
                cpu->cycles += 4u;
            }
            break;
        case 0xCFu: /* IRET */
            cpu->ip = pop16(cpu);
            cpu->cs = pop16(cpu);
            cpu->flags = sanitize_flags(pop16(cpu));
            cpu->cycles += 32u;
            break;
        case 0xD0u: case 0xD1u: case 0xD2u: case 0xD3u: { /* Shift/Rotate by 1 or CL */
            modrm_t m = decode_modrm_full(cpu, &pfx);
            uint8_t cnt = ((op & 0x02u) != 0u) ? (uint8_t)(cpu->cl & 0x1Fu) : 1u;
            if ((op & 0x01u) == 0u) {
                uint8_t v = get_rm8(cpu, &m);
                if (cnt > 0u) {
                    if (m.reg_field == 0u) { /* ROL */
                        uint8_t c = cnt & 7u;
                        if (c > 0u) {
                            v = (uint8_t)((v << c) | (v >> (8u - c)));
                        }
                        set_flag(cpu, CPU8088_FLAG_CF, (v & 0x01u) != 0u);
                        put_rm8(cpu, &m, v);
                    } else if (m.reg_field == 1u) { /* ROR */
                        uint8_t c = cnt & 7u;
                        if (c > 0u) {
                            v = (uint8_t)((v >> c) | (v << (8u - c)));
                        }
                        set_flag(cpu, CPU8088_FLAG_CF, (v & 0x80u) != 0u);
                        put_rm8(cpu, &m, v);
                    } else if (m.reg_field == 4u || m.reg_field == 6u) { /* SHL/SAL */
                        set_flag(cpu, CPU8088_FLAG_CF, ((v << (cnt - 1u)) & 0x80u) != 0u);
                        v = (uint8_t)(v << cnt);
                        update_zsp8(cpu, v);
                        put_rm8(cpu, &m, v);
                    } else if (m.reg_field == 5u) { /* SHR */
                        set_flag(cpu, CPU8088_FLAG_CF, ((v >> (cnt - 1u)) & 0x01u) != 0u);
                        v = (uint8_t)(v >> cnt);
                        update_zsp8(cpu, v);
                        put_rm8(cpu, &m, v);
                    } else if (m.reg_field == 7u) { /* SAR */
                        set_flag(cpu, CPU8088_FLAG_CF, (((int8_t)v >> (cnt - 1u)) & 0x01) != 0);
                        v = (uint8_t)((int8_t)v >> cnt);
                        update_zsp8(cpu, v);
                        put_rm8(cpu, &m, v);
                    }
                }
            } else {
                uint16_t v = get_rm16(cpu, &m);
                if (cnt > 0u) {
                    if (m.reg_field == 0u) { /* ROL */
                        uint8_t c = cnt & 15u;
                        if (c > 0u) {
                            v = (uint16_t)((v << c) | (v >> (16u - c)));
                        }
                        set_flag(cpu, CPU8088_FLAG_CF, (v & 0x0001u) != 0u);
                        put_rm16(cpu, &m, v);
                    } else if (m.reg_field == 1u) { /* ROR */
                        uint8_t c = cnt & 15u;
                        if (c > 0u) {
                            v = (uint16_t)((v >> c) | (v << (16u - c)));
                        }
                        set_flag(cpu, CPU8088_FLAG_CF, (v & 0x8000u) != 0u);
                        put_rm16(cpu, &m, v);
                    } else if (m.reg_field == 4u || m.reg_field == 6u) { /* SHL/SAL */
                        set_flag(cpu, CPU8088_FLAG_CF, ((v << (cnt - 1u)) & 0x8000u) != 0u);
                        v = (uint16_t)(v << cnt);
                        update_zsp16(cpu, v);
                        put_rm16(cpu, &m, v);
                    } else if (m.reg_field == 5u) { /* SHR */
                        set_flag(cpu, CPU8088_FLAG_CF, ((v >> (cnt - 1u)) & 0x0001u) != 0u);
                        v = (uint16_t)(v >> cnt);
                        update_zsp16(cpu, v);
                        put_rm16(cpu, &m, v);
                    } else if (m.reg_field == 7u) { /* SAR */
                        set_flag(cpu, CPU8088_FLAG_CF, (((int16_t)v >> (cnt - 1u)) & 0x0001) != 0);
                        v = (uint16_t)((int16_t)v >> cnt);
                        update_zsp16(cpu, v);
                        put_rm16(cpu, &m, v);
                    }
                }
            }
            cpu->cycles += ((op & 0x02u) != 0u) ? (8u + (4u * cnt)) : 2u;
            break;
        }
        case 0xD4u: { /* AAM */
            uint8_t base = fetch8(cpu);
            if (base == 0u) {
                service_interrupt(cpu, 0u);
            } else {
                cpu->ah = (uint8_t)(cpu->al / base);
                cpu->al = (uint8_t)(cpu->al % base);
                update_zsp8(cpu, cpu->al);
            }
            cpu->cycles += 83u;
            break;
        }
        case 0xD5u: { /* AAD */
            uint8_t base = fetch8(cpu);
            cpu->al = (uint8_t)(((uint16_t)cpu->ah * (uint16_t)base + (uint16_t)cpu->al) & 0xFFu);
            cpu->ah = 0u;
            update_zsp8(cpu, cpu->al);
            cpu->cycles += 60u;
            break;
        }
        case 0xD7u: { /* XLATB */
            uint16_t off = (uint16_t)(cpu->bx + cpu->al);
            cpu->al = cpu8088_mem_read8(cpu8088_seg_phys(effective_seg(cpu, &pfx, SEG_DS), off));
            cpu->cycles += 11u;
            break;
        }
        case 0xE0u: { /* LOOPNE / LOOPNZ */
            int8_t rel8 = (int8_t)fetch8(cpu);
            cpu->cx = (uint16_t)(cpu->cx - 1u);
            if (cpu->cx != 0u && !get_flag(cpu, CPU8088_FLAG_ZF)) {
                cpu->ip = (uint16_t)((int32_t)cpu->ip + (int32_t)rel8);
                cpu->cycles += 19u;
            } else {
                cpu->cycles += 5u;
            }
            break;
        }
        case 0xE1u: { /* LOOPE / LOOPZ */
            int8_t rel8 = (int8_t)fetch8(cpu);
            cpu->cx = (uint16_t)(cpu->cx - 1u);
            if (cpu->cx != 0u && get_flag(cpu, CPU8088_FLAG_ZF)) {
                cpu->ip = (uint16_t)((int32_t)cpu->ip + (int32_t)rel8);
                cpu->cycles += 18u;
            } else {
                cpu->cycles += 6u;
            }
            break;
        }
        case 0xE2u: { /* LOOP */
            int8_t rel8 = (int8_t)fetch8(cpu);
            cpu->cx = (uint16_t)(cpu->cx - 1u);
            if (cpu->cx != 0u) {
                cpu->ip = (uint16_t)((int32_t)cpu->ip + (int32_t)rel8);
                cpu->cycles += 17u;
            } else {
                cpu->cycles += 5u;
            }
            break;
        }
        case 0xE3u: { /* JCXZ */
            int8_t rel8 = (int8_t)fetch8(cpu);
            if (cpu->cx == 0u) {
                cpu->ip = (uint16_t)((int32_t)cpu->ip + (int32_t)rel8);
                cpu->cycles += 18u;
            } else {
                cpu->cycles += 6u;
            }
            break;
        }
        case 0xE4u: /* IN AL, imm8 */
            cpu->al = cpu8088_io_read8((uint16_t)fetch8(cpu));
            cpu->cycles += 10u;
            break;
        case 0xE5u: /* IN AX, imm8 */
            cpu->ax = cpu8088_io_read16((uint16_t)fetch8(cpu));
            cpu->cycles += 10u;
            break;
        case 0xE6u: { /* OUT imm8, AL */
            uint16_t port = (uint16_t)fetch8(cpu);
            cpu8088_io_write8(port, cpu->al);
            cpu->cycles += 10u;
            break;
        }
        case 0xE7u: { /* OUT imm8, AX */
            uint16_t port = (uint16_t)fetch8(cpu);
            cpu8088_io_write16(port, cpu->ax);
            cpu->cycles += 10u;
            break;
        }
        case 0xE8u: { /* CALL rel16 */
            int16_t rel16 = (int16_t)fetch16(cpu);
            push16(cpu, cpu->ip);
            cpu->ip = (uint16_t)((int32_t)cpu->ip + (int32_t)rel16);
            cpu->cycles += 19u;
            break;
        }
        case 0xE9u: { /* JMP rel16 */
            int16_t rel16 = (int16_t)fetch16(cpu);
            cpu->ip = (uint16_t)((int32_t)cpu->ip + (int32_t)rel16);
            cpu->cycles += 15u;
            break;
        }
        case 0xEAu: { /* JMP far ptr16:16 */
            uint16_t disp = fetch16(cpu);
            uint16_t seg = fetch16(cpu);
            cpu->cs = seg;
            cpu->ip = disp;
            cpu->cycles += 15u;
            break;
        }
        case 0xEBu: { /* JMP rel8 */
            int8_t rel8 = (int8_t)fetch8(cpu);
            cpu->ip = (uint16_t)((int32_t)cpu->ip + (int32_t)rel8);
            cpu->cycles += 15u;
            break;
        }
        case 0xECu: /* IN AL, DX */
            cpu->al = cpu8088_io_read8(cpu->dx);
            cpu->cycles += 8u;
            break;
        case 0xEDu: /* IN AX, DX */
            cpu->ax = cpu8088_io_read16(cpu->dx);
            cpu->cycles += 8u;
            break;
        case 0xEEu: /* OUT DX, AL */
            cpu8088_io_write8(cpu->dx, cpu->al);
            cpu->cycles += 8u;
            break;
        case 0xEFu: /* OUT DX, AX */
            cpu8088_io_write16(cpu->dx, cpu->ax);
            cpu->cycles += 8u;
            break;
        case 0xF4u: /* HLT */
            cpu->halted = true;
            cpu->cycles += 2u;
            break;
        case 0xF5u: /* CMC */
            set_flag(cpu, CPU8088_FLAG_CF, !get_flag(cpu, CPU8088_FLAG_CF));
            cpu->cycles += 2u;
            break;
        case 0xF6u: { /* Group 3 r/m8 */
            modrm_t m = decode_modrm_full(cpu, &pfx);
            uint8_t v = get_rm8(cpu, &m);
            if (m.reg_field == 0u || m.reg_field == 1u) { /* TEST imm8 */
                alu_logic8(cpu, (uint8_t)(v & fetch8(cpu)));
            } else if (m.reg_field == 2u) { /* NOT */
                put_rm8(cpu, &m, (uint8_t)(~v));
            } else if (m.reg_field == 3u) { /* NEG */
                set_flag(cpu, CPU8088_FLAG_CF, v != 0u);
                put_rm8(cpu, &m, alu_sub8(cpu, 0u, v, 0u));
            } else if (m.reg_field == 4u) { /* MUL AL */
                uint16_t prod = (uint16_t)cpu->al * (uint16_t)v;
                cpu->ax = prod;
                set_flag(cpu, CPU8088_FLAG_CF, cpu->ah != 0u);
                set_flag(cpu, CPU8088_FLAG_OF, cpu->ah != 0u);
            } else if (m.reg_field == 5u) { /* IMUL AL */
                int16_t prod = (int16_t)(int8_t)cpu->al * (int16_t)(int8_t)v;
                cpu->ax = (uint16_t)prod;
                bool fits8 = (prod >= -128) && (prod <= 127);
                set_flag(cpu, CPU8088_FLAG_CF, !fits8);
                set_flag(cpu, CPU8088_FLAG_OF, !fits8);
            } else if (m.reg_field == 6u) { /* DIV r/m8 */
                if (v == 0u) {
                    service_interrupt(cpu, 0u);
                } else {
                    uint16_t q = cpu->ax / (uint16_t)v;
                    uint16_t r = cpu->ax % (uint16_t)v;
                    if (q > 0xFFu) {
                        service_interrupt(cpu, 0u);
                    } else {
                        cpu->al = (uint8_t)q;
                        cpu->ah = (uint8_t)r;
                    }
                }
            } else if (m.reg_field == 7u) { /* IDIV r/m8 */
                int8_t divisor = (int8_t)v;
                if (divisor == 0) {
                    service_interrupt(cpu, 0u);
                } else {
                    int16_t num = (int16_t)cpu->ax;
                    int16_t q = num / (int16_t)divisor;
                    int16_t r = num % (int16_t)divisor;
                    if (q < -128 || q > 127) {
                        service_interrupt(cpu, 0u);
                    } else {
                        cpu->al = (uint8_t)(int8_t)q;
                        cpu->ah = (uint8_t)(int8_t)r;
                    }
                }
            }
            cpu->cycles += 5u;
            break;
        }
        case 0xF7u: { /* Group 3 r/m16 */
            modrm_t m = decode_modrm_full(cpu, &pfx);
            uint16_t v = get_rm16(cpu, &m);
            if (m.reg_field == 0u || m.reg_field == 1u) { /* TEST imm16 */
                alu_logic16(cpu, (uint16_t)(v & fetch16(cpu)));
            } else if (m.reg_field == 2u) { /* NOT */
                put_rm16(cpu, &m, (uint16_t)(~v));
            } else if (m.reg_field == 3u) { /* NEG */
                set_flag(cpu, CPU8088_FLAG_CF, v != 0u);
                put_rm16(cpu, &m, alu_sub16(cpu, 0u, v, 0u));
            } else if (m.reg_field == 4u) { /* MUL AX */
                uint32_t prod = (uint32_t)cpu->ax * (uint32_t)v;
                cpu->ax = (uint16_t)(prod & 0xFFFFu);
                cpu->dx = (uint16_t)((prod >> 16) & 0xFFFFu);
                set_flag(cpu, CPU8088_FLAG_CF, cpu->dx != 0u);
                set_flag(cpu, CPU8088_FLAG_OF, cpu->dx != 0u);
            } else if (m.reg_field == 5u) { /* IMUL AX */
                int32_t prod = (int32_t)(int16_t)cpu->ax * (int32_t)(int16_t)v;
                cpu->ax = (uint16_t)((uint32_t)prod & 0xFFFFu);
                cpu->dx = (uint16_t)(((uint32_t)prod >> 16) & 0xFFFFu);
                bool fits16 = (prod >= -32768) && (prod <= 32767);
                set_flag(cpu, CPU8088_FLAG_CF, !fits16);
                set_flag(cpu, CPU8088_FLAG_OF, !fits16);
            } else if (m.reg_field == 6u) { /* DIV r/m16 */
                if (v == 0u) {
                    service_interrupt(cpu, 0u);
                } else {
                    uint32_t num = (((uint32_t)cpu->dx) << 16) | (uint32_t)cpu->ax;
                    uint32_t q = num / (uint32_t)v;
                    uint32_t r = num % (uint32_t)v;
                    if (q > 0xFFFFu) {
                        service_interrupt(cpu, 0u);
                    } else {
                        cpu->ax = (uint16_t)q;
                        cpu->dx = (uint16_t)r;
                    }
                }
            } else if (m.reg_field == 7u) { /* IDIV r/m16 */
                int16_t divisor = (int16_t)v;
                if (divisor == 0) {
                    service_interrupt(cpu, 0u);
                } else {
                    int32_t num = (int32_t)((((uint32_t)cpu->dx) << 16) | (uint32_t)cpu->ax);
                    int32_t q = num / (int32_t)divisor;
                    int32_t r = num % (int32_t)divisor;
                    if (q < -32768 || q > 32767) {
                        service_interrupt(cpu, 0u);
                    } else {
                        cpu->ax = (uint16_t)(int16_t)q;
                        cpu->dx = (uint16_t)(int16_t)r;
                    }
                }
            }
            cpu->cycles += 5u;
            break;
        }
        case 0xF8u: /* CLC */
            set_flag(cpu, CPU8088_FLAG_CF, false);
            cpu->cycles += 2u;
            break;
        case 0xF9u: /* STC */
            set_flag(cpu, CPU8088_FLAG_CF, true);
            cpu->cycles += 2u;
            break;
        case 0xFAu: /* CLI */
            set_flag(cpu, CPU8088_FLAG_IF, false);
            cpu->cycles += 2u;
            break;
        case 0xFBu: /* STI */
            set_flag(cpu, CPU8088_FLAG_IF, true);
            cpu->cycles += 2u;
            break;
        case 0xFCu: /* CLD */
            set_flag(cpu, CPU8088_FLAG_DF, false);
            cpu->cycles += 2u;
            break;
        case 0xFDu: /* STD */
            set_flag(cpu, CPU8088_FLAG_DF, true);
            cpu->cycles += 2u;
            break;
        case 0xFEu: { /* Group 4 INC/DEC r/m8 */
            modrm_t m = decode_modrm_full(cpu, &pfx);
            bool old_cf = get_flag(cpu, CPU8088_FLAG_CF);
            uint8_t v = get_rm8(cpu, &m);
            if (m.reg_field == 0u) {
                v = alu_add8(cpu, v, 1u, 0u);
            } else {
                v = alu_sub8(cpu, v, 1u, 0u);
            }
            set_flag(cpu, CPU8088_FLAG_CF, old_cf);
            put_rm8(cpu, &m, v);
            cpu->cycles += 3u;
            break;
        }
        case 0xFFu: { /* Group 5 INC/DEC/CALL/JMP/PUSH r/m16 */
            modrm_t m = decode_modrm_full(cpu, &pfx);
            if (m.reg_field == 0u || m.reg_field == 1u) {
                bool old_cf = get_flag(cpu, CPU8088_FLAG_CF);
                uint16_t v = get_rm16(cpu, &m);
                if (m.reg_field == 0u) {
                    v = alu_add16(cpu, v, 1u, 0u);
                } else {
                    v = alu_sub16(cpu, v, 1u, 0u);
                }
                set_flag(cpu, CPU8088_FLAG_CF, old_cf);
                put_rm16(cpu, &m, v);
                cpu->cycles += 3u;
            } else if (m.reg_field == 2u) { /* CALL r/m16 */
                uint16_t target = get_rm16(cpu, &m);
                push16(cpu, cpu->ip);
                cpu->ip = target;
                cpu->cycles += 16u;
            } else if (m.reg_field == 3u) { /* CALL m16:16 */
                if (m.is_mem) {
                    uint16_t target_ip = cpu8088_mem_read16(m.mem_addr);
                    uint16_t target_cs = cpu8088_mem_read16(m.mem_addr + 2u);
                    push16(cpu, cpu->cs);
                    push16(cpu, cpu->ip);
                    cpu->cs = target_cs;
                    cpu->ip = target_ip;
                }
                cpu->cycles += 37u;
            } else if (m.reg_field == 4u) { /* JMP r/m16 */
                cpu->ip = get_rm16(cpu, &m);
                cpu->cycles += 11u;
            } else if (m.reg_field == 5u) { /* JMP m16:16 */
                if (m.is_mem) {
                    cpu->ip = cpu8088_mem_read16(m.mem_addr);
                    cpu->cs = cpu8088_mem_read16(m.mem_addr + 2u);
                }
                cpu->cycles += 24u;
            } else if (m.reg_field == 6u) { /* PUSH r/m16 */
                push16(cpu, get_rm16(cpu, &m));
                cpu->cycles += 11u;
            }
            break;
        }
        default:
            /* Undefined / unsupported opcode: advance safely without halting */
            cpu->cycles += 4u;
            break;
    }

    return cpu->cycles - start_cycles;
}

uint32_t cpu8088_run(cpu8088_t *cpu, uint32_t max_cycles)
{
    uint32_t start_cycles;

    if (cpu == NULL || max_cycles == 0u) {
        return 0u;
    }

    start_cycles = cpu->cycles;

    while ((cpu->cycles - start_cycles) < max_cycles) {
        uint32_t elapsed = cpu->cycles - start_cycles;
        uint32_t rem = max_cycles - elapsed;
        uint32_t step_cycles = cpu8088_step(cpu);
        if (step_cycles >= rem) {
            break;
        }
    }

    return cpu->cycles - start_cycles;
}