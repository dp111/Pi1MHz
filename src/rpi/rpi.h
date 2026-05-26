#ifndef RPI_H
#define RPI_H

#ifdef DEBUG
#define LOG_DEBUG(...) printf(__VA_ARGS__)
#else
#define LOG_DEBUG(...)
#endif

#define LOG_INFO(...) printf(__VA_ARGS__)

#define LOG_WARN(...) printf(__VA_ARGS__)


/* Put large arrays in no init section saves bss time */

#define NOINIT_SECTION __attribute__ ((section (".noinit")))

#ifndef __ASSEMBLER__
typedef void (*func_ptr)();

#if (__ARM_ARCH >= 7 ) 
int _get_core(void);
void start_core(int core, func_ptr func);
#endif

#endif

#endif
