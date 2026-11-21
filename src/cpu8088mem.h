#include <stdint.h>
//#include <inttypes.h>

uint8_t cpu_byte_read(uint32_t addr);
void cpu_byte_write(uint32_t addr, uint8_t data);

uint16_t cpu_word_read(uint32_t addr);
void cpu_word_write(uint32_t addr, uint16_t data);

uint8_t cpu_io_read(uint16_t addr);
void cpu_io_write(uint16_t addr, uint8_t data);

#ifndef CPU8088MEM_H
#define CPU8088MEM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 20-bit physical memory access (0x00000 .. 0xFFFFF) */
uint8_t  cpu8088_mem_read8(uint32_t phys_addr);
uint16_t cpu8088_mem_read16(uint32_t phys_addr);
void     cpu8088_mem_write8(uint32_t phys_addr, uint8_t value);
void     cpu8088_mem_write16(uint32_t phys_addr, uint16_t value);

/* 16-bit I/O port access (0x0000 .. 0xFFFF) */
uint8_t  cpu8088_io_read8(uint16_t port);
uint16_t cpu8088_io_read16(uint16_t port);
void     cpu8088_io_write8(uint16_t port, uint8_t value);
void     cpu8088_io_write16(uint16_t port, uint16_t value);

#ifdef __cplusplus
}
#endif

#endif /* CPU8088MEM_H */