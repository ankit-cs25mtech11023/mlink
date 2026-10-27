/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * driver/mlink_ctrl.h defines its ioctl number with _IOR(); Zephyr never uses
 * the ioctl (there is no /dev/mlink_ctrl on the modem side), so the macro only
 * has to exist, not mean anything.
 */
#ifndef _MLINK_COMPAT_LINUX_IOCTL_H
#define _MLINK_COMPAT_LINUX_IOCTL_H

#define _IOR(type, nr, size)	0

#endif /* _MLINK_COMPAT_LINUX_IOCTL_H */
