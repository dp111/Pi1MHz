#ifndef EXCEPTIONS_H
#define EXCEPTIONS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

_Noreturn void reboot_now(void);
_Noreturn void dump_info(unsigned int *context, int offset, const char *type);

#ifdef __cplusplus
}
#endif

#endif // EXCEPTIONS_H
