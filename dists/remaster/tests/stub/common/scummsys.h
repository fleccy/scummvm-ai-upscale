// Minimal stand-in for ScummVM's common/scummsys.h, for the stand-alone remaster tests (no configure step needed).
#ifndef COMMON_SCUMMSYS_H
#define COMMON_SCUMMSYS_H
#include <cstddef>
#include <cstdint>
typedef uint8_t byte;
typedef uint8_t uint8;
typedef int8_t int8;
typedef uint16_t uint16;
typedef int16_t int16;
typedef uint32_t uint32;
typedef int32_t int32;
typedef uint64_t uint64;
typedef int64_t int64;
typedef unsigned int uint;
#endif
