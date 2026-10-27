/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Just enough of the kernel's types/compiler vocabulary for driver/mlink_ring.c
 * and driver/mlink_shm.h to build unchanged on Zephyr (D19).
 */
#ifndef _MLINK_COMPAT_LINUX_TYPES_H
#define _MLINK_COMPAT_LINUX_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/toolchain.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

#ifndef static_assert
#define static_assert(expr, ...) _Static_assert(expr, #expr)
#endif
#ifndef __aligned
#define __aligned(x) __attribute__((__aligned__(x)))
#endif
#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

#endif /* _MLINK_COMPAT_LINUX_TYPES_H */
