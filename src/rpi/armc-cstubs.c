/*

 Part of the Raspberry-Pi Bare Metal Tutorials
 Copyright (c) 2013-2015, Brian Sidebotham
 All rights reserved.

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:

 1. Redistributions of source code must retain the above copyright notice,
 this list of conditions and the following disclaimer.

 2. Redistributions in binary form must reproduce the above copyright notice,
 this list of conditions and the following disclaimer in the
 documentation and/or other materials provided with the distribution.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 POSSIBILITY OF SUCH DAMAGE.

 */

/* For more information about this file, please visit:
 https://sourceware.org/newlib/libc.html#Stubs

 These are the newlib C-Library stubs for the valvers Raspberry-Pi bare-metal
 tutorial */

/*
 Graceful failure is permitted by returning an error code. A minor
 complication arises here: the C library must be compatible with development
 environments that supply fully functional versions of these subroutines.
 Such environments usually return error codes in a global errno. However,
 the Red Hat newlib C library provides a macro definition for errno in the
 header file errno.h, as part of its support for reentrant routines (see
 Reentrancy).

 The bridge between these two interpretations of errno is straightforward:
 the C library routines with OS interface calls capture the errno values
 returned globally, and record them in the appropriate field of the
 reentrancy structure (so that you can query them using the errno macro from
 errno.h).

 This mechanism becomes visible when you write stub routines for OS
 interfaces. You must include errno.h, then disable the macro, like this:
 */
#include <errno.h>
#undef errno
extern int errno;

/* Required include for fstat() */
#include <sys/stat.h>

/* Required include for times() */
#include <sys/times.h>

/* Prototype for the UART write function */
#include "auxuart.h"

/* A pointer to a list of environment variables and their values. For a minimal
 environment, this empty list is adequate: */
char *__env[1] =
{ 0 };
char **environ = __env;

/* A helper function written in assembler to aid us in allocating memory */
/*extern caddr_t _get_stack_pointer(void);*/

/* Never return from _exit as there's no OS to exit to, so instead we trap
 here */
 // cppcheck-suppress unusedFunction
void _exit(int status)
{
  /* Stop the compiler complaining about unused variables by "using" it */
  (void) status;

  while (1)
  {
    /* TRAP HERE */
  }
}

/* Transfer control to a new process. Minimal implementation (for a system
 without processes): */
 // cppcheck-suppress unusedFunction
int execve(const char *name __attribute__((unused)), char * const *argv __attribute__((unused)),
           char * const *env __attribute__((unused)))
{
  errno = ENOMEM;
  return -1;
}

/* Create a new process. Minimal implementation (for a system without
 processes): */
// cppcheck-suppress unusedFunction
int fork(void)
{
  errno = EAGAIN;
  return -1;
}

/* Process-ID; this is sometimes used to generate strings unlikely to conflict
 with other processes. Minimal implementation, for a system without
 processes: */
// cppcheck-suppress unusedFunction
int getpid(void)
{
  return 1;
}

/* Send a signal. Minimal implementation: */
// cppcheck-suppress unusedFunction
int kill(int pid __attribute__((unused)), int sig __attribute__((unused)))
{
  errno = EINVAL;
  return -1;
}

/* Establish a new name for an existing file. Minimal implementation: */
// cppcheck-suppress unusedFunction
int link( const char *old __attribute__((unused)), const char *new __attribute__((unused)) )
{
  errno = EMLINK;
  return -1;
}

/* Open a file. Minimal implementation: */
// cppcheck-suppress unusedFunction
int open(const char *name __attribute__((unused)), int flags __attribute__((unused)),
         int mode __attribute__((unused)))
{
  return -1;
}

/* Increase program data space. As malloc and related functions depend on this,
 it is useful to have a working implementation. The following suffices for a
 standalone system; it exploits the symbol _end automatically defined by the
 GNU linker. */
// cppcheck-suppress unusedFunction
caddr_t _sbrk(int incr)
{
  extern char _end;
  static char* heap_end = &_end;
  char* prev_heap_end;

  prev_heap_end = heap_end;
  heap_end += incr;

  return (caddr_t) prev_heap_end;
}

/* Status of a file (by name). Minimal implementation: */
// cppcheck-suppress unusedFunction
int stat(const char *file __attribute__((unused)), struct stat *st )
{
  st->st_mode = S_IFCHR;
  return 0;
}

/* Timing information for current process. Minimal implementation: */
// cppcheck-suppress unusedFunction
// cppcheck-suppress constParameterPointer
clock_t times(struct tms *buf __attribute__((unused)))
{
  return (clock_t)-1;
}

/* Remove a file's directory entry. Minimal implementation: */
// cppcheck-suppress unusedFunction
int unlink(const char *name __attribute__((unused)))
{
  errno = ENOENT;
  return -1;
}

/* Wait for a child process. Minimal implementation: */
// cppcheck-suppress unusedFunction
int wait(const int *status __attribute__((unused)))
{
  errno = ECHILD;
  return -1;
}

void outbyte(char b)
{
  RPI_AuxMiniUartWrite(b);
}
// The following functions are marked __attribute__((used)) so that LTO
// works correctly
/* There's currently no implementation of a file system because there's no
 file system! */
// cppcheck-suppress unusedFunction
__attribute__((used)) int _close(int file __attribute__((unused)))
{
  return -1;
}

/* Status of an open file. For consistency with other minimal implementations
 in these examples, all files are regarded as character special devices. The
 sys/stat.h header file required is distributed in the include subdirectory
 for this C library. */
// cppcheck-suppress unusedFunction
__attribute__((used)) int _fstat(int file __attribute__((unused)), struct stat *st)
{
  st->st_mode = S_IFCHR;
  return 0;
}

/* Query whether output stream is a terminal. For consistency with the other
 minimal implementations, which only support output to stdout, this minimal
 implementation is suggested: */
// cppcheck-suppress unusedFunction
__attribute__((used)) int _isatty(int file __attribute__((unused)))
{
  return 1;
}

/* Set position in a file. Minimal implementation: */
// cppcheck-suppress unusedFunction
__attribute__((used)) int _lseek(int file __attribute__((unused)),
                                 int ptr __attribute__((unused)),
                                 int dir __attribute__((unused)))
{
  return 0;
}

/* Read from a file. Minimal implementation: */
// cppcheck-suppress unusedFunction
__attribute__((used)) int _read(int file __attribute__((unused)),
                                const char *ptr __attribute__((unused)),
                                int len __attribute__((unused)))
{
  return 0;
}

/* Write to a file. libc subroutines will use this system routine for output to
 all files, including stdout—so if you need to generate any output, for
 example to a serial port for debugging, you should make your minimal write
 capable of doing this. The following minimal implementation is an
 incomplete example; it relies on a outbyte subroutine (not shown; typically,
 you must write this in assembler from examples provided by your hardware
 manufacturer) to actually perform the output. */
// cppcheck-suppress unusedFunction
__attribute__((used)) int _write(int file __attribute__((unused)), const char *ptr, int len)
{
  for (int todo = 0; todo < len; todo++)
    outbyte(*ptr++);

  return len;
}

// cppcheck-suppress unusedFunction
void _getpid_r()
{
}
// cppcheck-suppress unusedFunction
void _kill_r()
{
}