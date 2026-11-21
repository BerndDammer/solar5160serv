/**
 * put the cpu in hardware reset condition
 */
void cpu_init(void);
/**
 * executes one step of machine code
 */
void cpu_step(void);

#ifndef CPU8088_H
#define CPU8088_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 8088 FLAGS register bit masks */
#define CPU8088_FLAG_CF (1u << 0)  /* Carry */
#define CPU8088_FLAG_PF (1u << 2)  /* Parity */
#define CPU8088_FLAG_AF (1u << 4)  /* Auxiliary Carry */
#define CPU8088_FLAG_ZF (1u << 6)  /* Zero */
#define CPU8088_FLAG_SF (1u << 7)  /* Sign */
#define CPU8088_FLAG_TF (1u << 8)  /* Trap */
#define CPU8088_FLAG_IF (1u << 9)  /* Interrupt Enable */
#define CPU8088_FLAG_DF (1u << 10) /* Direction */
#define CPU8088_FLAG_OF (1u << 11) /* Overflow */

typedef struct {
    union {
        uint16_t ax;
        struct {
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
            uint8_t ah;
            uint8_t al;
#else
            uint8_t al;
            uint8_t ah;
#endif
        };
    };
    union {
        uint16_t bx;
        struct {
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
            uint8_t bh;
            uint8_t bl;
#else
            uint8_t bl;
            uint8_t bh;
#endif
        };
    };
    union {
        uint16_t cx;
        struct {
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
            uint8_t ch;
            uint8_t cl;
#else
            uint8_t cl;
            uint8_t ch;
#endif
        };
    };
    union {
        uint16_t dx;
        struct {
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
            uint8_t dh;
            uint8_t dl;
#else
            uint8_t dl;
            uint8_t dh;
#endif
        };
    };

    uint16_t sp;
    uint16_t bp;
    uint16_t si;
    uint16_t di;

    uint16_t cs;
    uint16_t ds;
    uint16_t ss;
    uint16_t es;

    uint16_t ip;
    uint16_t flags;

    bool halted;
    bool nmi_pending;
    bool intr_pending;
    uint8_t intr_vector;
    uint32_t cycles;
} cpu8088_t;

void     cpu8088_reset(cpu8088_t *cpu);
uint32_t cpu8088_step(cpu8088_t *cpu);
uint32_t cpu8088_run(cpu8088_t *cpu, uint32_t max_cycles);
void     cpu8088_intr(cpu8088_t *cpu, uint8_t vector);
void     cpu8088_nmi(cpu8088_t *cpu);
uint32_t cpu8088_seg_phys(uint16_t seg, uint16_t offset);

#ifdef __cplusplus
}
#endif

#endif /* CPU8088_H */