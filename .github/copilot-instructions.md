create a simulation of the intel cpu 8088.
implement the functions defined in #file:../src/cpu8088.h in #file:../src/cpu8088.c.
for io and memory read and write use the functions defined in #file:../src/cpu8088mem.h and #file:../src/cpu8088mem.c and dont implement them.

correct guards in header files

# general project rules above

## Environement and compiler
- **Language:** C99 / C11 (strict without C++-Extensions)
- **Compiler:** arm-none-eabi-gcc / Pico c/c++ sdk 2.3.1
- **Hardware Platform:** Raspberry Pi Pico 1 / Bare-Metal / no RTOS 

- **No Heap:** never use `malloc`, `calloc`, `realloc` or `free`. 
- **No Recursion:**
- **Datentypen:** prefer fix data size variables `<stdint.h>` (`uint8_t`, `int16_t`, `uint32_t`) and `<stdbool.h>`

- no `printf` or `scanf`
- correct or add guards in header files
