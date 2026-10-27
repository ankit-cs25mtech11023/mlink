/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MLINK_COMPAT_LINUX_COMPILER_H
#define _MLINK_COMPAT_LINUX_COMPILER_H

#include <linux/types.h>

/* One untorn access the compiler may not merge, repeat or elide (as in Linux). */
#define READ_ONCE(x)		(*(const volatile __typeof__(x) *)&(x))
#define WRITE_ONCE(x, val)	(*(volatile __typeof__(x) *)&(x) = (val))

#endif /* _MLINK_COMPAT_LINUX_COMPILER_H */
