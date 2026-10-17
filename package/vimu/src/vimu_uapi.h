/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Sample layout returned by read() on /dev/vimu.
 * Shared between the driver and userspace (telemd).
 */
#ifndef _UAPI_VIMU_H
#define _UAPI_VIMU_H

#include <linux/types.h>

struct vimu_sample {
	__u64 timestamp_ns;	/* CLOCK_MONOTONIC at sampling time */
	__u32 seq;		/* increments by one per sample, wraps */
	__s16 accel_mg[3];	/* x, y, z in milli-g */
	__s16 gyro_mdps[3];	/* x, y, z in milli-deg/s */
	__u32 reserved;
	__u32 pad;
};

#endif
