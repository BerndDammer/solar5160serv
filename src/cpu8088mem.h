#include <stdint.h>
//#include <inttypes.h>

uint8_t cpu_byte_read(uint32_t addr);
void cpu_byte_write(uint32_t addr, uint8_t data);

uint16_t cpu_word_read(uint32_t addr);
void cpu_word_write(uint32_t addr, uint16_t data);

uint8_t cpu_io_read(uint16_t addr);
void cpu_io_write(uint16_t addr, uint8_t data);

