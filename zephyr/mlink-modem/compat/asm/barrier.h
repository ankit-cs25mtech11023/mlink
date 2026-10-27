/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The Linux virt_* barriers in C11 terms. The peer is another VM, so these
 * must be real CPU barriers even on a uniprocessor Zephyr build (which is
 * exactly why Linux has virt_* next to smp_*).
 */
#ifndef _MLINK_COMPAT_ASM_BARRIER_H
#define _MLINK_COMPAT_ASM_BARRIER_H

#define virt_load_acquire(p)		__atomic_load_n(p, __ATOMIC_ACQUIRE)
#define virt_store_release(p, v)	__atomic_store_n(p, v, __ATOMIC_RELEASE)
/* Full fence: orders an earlier store before a later load (the SB kick pattern). */
#define virt_mb()			__atomic_thread_fence(__ATOMIC_SEQ_CST)
#define virt_wmb()			__atomic_thread_fence(__ATOMIC_RELEASE)
#define virt_rmb()			__atomic_thread_fence(__ATOMIC_ACQUIRE)

#endif /* _MLINK_COMPAT_ASM_BARRIER_H */
