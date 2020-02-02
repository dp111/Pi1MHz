#ifndef RPI_H
#define RPI_H

#ifdef DEBUG
#define LOG_DEBUG(...) printf(__VA_ARGS__)
#else
#define LOG_DEBUG(...)
#endif

#define LOG_INFO(...) printf(__VA_ARGS__)

#define LOG_WARN(...) printf(__VA_ARGS__)

/* Pi 2/3 Multicore options */
#if defined(RPI2) || defined(RPI3)

/* Indicate the platform has multiple cores */
#define HAS_MULTICORE

#endif

/* Put large arrays in no init section saves bss time */

#define NOINIT_SECTION __attribute__ ((section (".noinit")))

#ifndef __ASSEMBLER__
typedef void (*func_ptr)();

#ifdef HAS_MULTICORE
int _get_core(void);
void start_core(int core, func_ptr func);
#endif

#endif

#endif
