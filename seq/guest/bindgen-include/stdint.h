/* The only header application-engine.h includes, for bindgen's cross-target parse of the seam on
 * any build host (-nostdinc): the LP64 fixed-width types of riscv64gc-unknown-linux-musl. The
 * engine's own build uses the real libc headers; src/lib/app_engine.cpp static_asserts the
 * resulting record layouts, so a mismatch here would fail that build, not silently drift. */
#ifndef QASEQ_BINDGEN_STDINT_H
#define QASEQ_BINDGEN_STDINT_H
typedef signed char int8_t;
typedef short int16_t;
typedef int int32_t;
typedef long int64_t;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long uint64_t;
typedef unsigned long uintptr_t;
typedef long intptr_t;
#define UINT32_MAX 0xffffffffU
#define UINT64_MAX 0xffffffffffffffffUL
#endif
