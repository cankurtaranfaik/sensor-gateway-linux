---
layout: post
title: "Embedded Linux without a board: Buildroot, a custom driver and measurements in QEMU"
date: 2026-10-17
---

sensor-gateway-linux is a small embedded Linux image for ARM64. It reads an IMU through a kernel driver and streams the samples over UDP, like a box next to a motor or a pump that sends vibration data to a server. Everything runs in QEMU, and every commit is built, booted and measured automatically.

Code: [github.com/cankurtaranfaik/sensor-gateway-linux](https://github.com/cankurtaranfaik/sensor-gateway-linux)

![Architecture](img/architecture.png)

## The parts

**Image.** A `BR2_EXTERNAL` tree on top of Buildroot's `qemu_aarch64_virt_defconfig`. Instead of copying the defconfig, a small fragment is appended to it, so the stock settings stay current when the Buildroot version changes. The prebuilt Bootlin toolchain keeps the build time down.

**Driver (`vimu.ko`).** The sensor is simulated, but the driver follows the same path a real SPI or I2C IMU driver would:

- it binds to the `sensgw,vimu` device tree node and reads the sample rate from `sample-rate-hz`,
- an `hrtimer` produces samples and pushes them into a `kfifo`,
- `/dev/vimu` offers blocking `read()` and `poll()`,
- if the reader falls behind, the oldest sample is dropped and counted in the sysfs `dropped` attribute.

The timer callback:

```c
static enum hrtimer_restart vimu_tick(struct hrtimer *t)
{
	struct vimu *v = container_of(t, struct vimu, timer);
	struct vimu_sample s;
	unsigned long flags;

	vimu_make_sample(v, &s);

	spin_lock_irqsave(&v->lock, flags);
	if (kfifo_is_full(&v->fifo)) {
		kfifo_skip(&v->fifo);
		v->dropped++;
	}
	kfifo_put(&v->fifo, s);
	spin_unlock_irqrestore(&v->lock, flags);

	wake_up_interruptible(&v->wq);

	hrtimer_forward_now(t, v->period);
	return HRTIMER_RESTART;
}
```

The timer only runs while the device is open, so nothing wakes the CPU when no one is reading.

**Device tree.** QEMU's `virt` machine generates its device tree at runtime, so there is no `.dts` file to edit. The build asks QEMU to dump the tree with `dumpdtb`, includes it from a small wrapper that adds the vimu node and compiles the result with `dtc`. The dump and the real boot must use the same machine options. If memory size or CPU count differ, the tree no longer matches the machine.

**Service (`telemd`).** It reads the driver in batches of 16 and puts each sample into a 38-byte frame: sequence number, two timestamps, six axes and a CRC-16. One timestamp is taken when the sample is produced, the other right before `sendto()`. The difference shows how long a sample waits between the driver and the socket. A BusyBox init script loads the driver and starts the service at boot.

**CI.** On every push, GitHub Actions builds the image, boots it in QEMU, listens on the host for 30 seconds and writes a report. If a change breaks the driver or the service, it shows up as lost frames or a longer boot.

## Results

From CI [run 39027156837](https://github.com/cankurtaranfaik/sensor-gateway-linux/actions/runs/39027156837), Linux 6.12.27, one Cortex-A53, 256 MiB RAM:

| Measurement | Value |
|---|---|
| Kernel hands over to /sbin/init | 0.53 s |
| init reaches S90telemd | 2.64 s |
| vimu driver probed | 2.68 s |
| First frame sent | 2.80 s |
| QEMU launch to first frame on the host | 2.94 s |
| Kernel Image / rootfs content | 12.5 MiB / 7.9 MiB |
| vimu.ko / telemd | 13.9 KiB / 9.8 KiB |
| Frames received in 30 s | 3002 (100.0 Hz) |
| Lost / bad frames | 0 / 0 |
| Sample interval, mean / stdev | 10.000 ms / 16 µs |
| Sample interval, min / max | 9.86 ms / 10.15 ms |
| Driver to socket latency, p50 / p99 / max | 91 / 157 / 511 µs |

![Data received on the host](img/telemetry.png)

![Driver to socket latency](img/latency.png)

QEMU runs here without KVM and translates every ARM instruction in software, so the absolute values do not describe a real Cortex-A53 board. They are useful for comparing commits: if a change makes latency or boot time worse, CI catches it.
