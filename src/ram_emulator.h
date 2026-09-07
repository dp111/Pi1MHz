#include <stdbool.h>

bool ram_emulator_jim_init_loaded(void);
void rampage_emulator_init( uint8_t instance , uint8_t address);
void rambyte_emulator_init( uint8_t instance , uint8_t address);

void ram_emulator_page_restore(void);