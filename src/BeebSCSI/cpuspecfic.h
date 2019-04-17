#if 0
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <avr/interrupt.h>
#else
#define pgm_read_byte *
#define pgm_read_word *
#define PSTR
#define sei()
#define cli()
#define PROGMEM
#endif
