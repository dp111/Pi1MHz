#ifndef STUB_RPI_H
#define STUB_RPI_H
#include <stdio.h>
#define LOG_DEBUG(...) do { if (0) printf(__VA_ARGS__); } while (0)
#define LOG_INFO(...)  do { if (0) printf(__VA_ARGS__); } while (0)
#endif
