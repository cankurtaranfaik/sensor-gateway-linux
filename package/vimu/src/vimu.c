// SPDX-License-Identifier: GPL-2.0
/*
 * vimu - virtual IMU platform driver
 *
 * Produces accelerometer and gyro samples from an hrtimer at the rate set by
 * the "sample-rate-hz" DT property and hands them to userspace through a
 * misc character device. The data is synthetic (a low frequency vibration
 * plus noise on top of 1 g, roughly what a sensor bolted to a running motor
 * sees), the point is the plumbing: DT binding, timer, fifo, blocking read,
 * poll and sysfs, the same path a real SPI/I2C IMU driver ends up using.
 *
 * Samples are produced only while the device is open. If the reader falls
 * behind, the oldest sample is dropped and counted in sysfs "dropped".
 */
#include <linux/fs.h>
#include <linux/hrtimer.h>
#include <linux/kfifo.h>
#include <linux/ktime.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/wait.h>

#include "vimu_uapi.h"

#define VIMU_FIFO_LEN		256	/* must be a power of two */
#define VIMU_DEFAULT_RATE_HZ	100
#define VIMU_MAX_RATE_HZ	2000
#define VIMU_READ_BATCH		16

struct vimu {
	struct miscdevice misc;
	struct hrtimer timer;
	ktime_t period;
	u32 rate_hz;

	/* touched only from the timer callback */
	u32 seq;
	u32 phase;
	u32 rng;

	spinlock_t lock;	/* protects fifo and dropped */
	DECLARE_KFIFO(fifo, struct vimu_sample, VIMU_FIFO_LEN);
	u32 dropped;

	wait_queue_head_t wq;
	atomic_t users;
};

/* first quarter of a sine wave, 64 steps per period, scaled to 1000 */
static const s16 quarter_sine[17] = {
	0, 98, 195, 290, 383, 471, 556, 634, 707,
	773, 831, 882, 924, 957, 981, 995, 1000,
};

static s16 vimu_sin(u32 idx)
{
	idx &= 63;
	if (idx < 16)
		return quarter_sine[idx];
	if (idx < 32)
		return quarter_sine[32 - idx];
	if (idx < 48)
		return -quarter_sine[idx - 32];
	return -quarter_sine[64 - idx];
}

/* xorshift32, returns roughly -8..7 */
static s16 vimu_noise(struct vimu *v)
{
	u32 x = v->rng;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	v->rng = x;
	return (s16)(x & 0xf) - 8;
}

static void vimu_make_sample(struct vimu *v, struct vimu_sample *s)
{
	u32 ph = v->phase++;

	memset(s, 0, sizeof(*s));
	s->timestamp_ns = ktime_get_ns();
	s->seq = v->seq++;

	/* +-50 mg vibration on x/y, gravity on z */
	s->accel_mg[0] = vimu_sin(ph) / 20 + vimu_noise(v);
	s->accel_mg[1] = vimu_sin(ph + 16) / 20 + vimu_noise(v);
	s->accel_mg[2] = 1000 + vimu_noise(v);

	/* +-250 mdps wobble on x/y, z is just noise */
	s->gyro_mdps[0] = vimu_sin(ph * 2) / 4 + vimu_noise(v);
	s->gyro_mdps[1] = vimu_sin(ph * 2 + 16) / 4 + vimu_noise(v);
	s->gyro_mdps[2] = vimu_noise(v);
}

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

static struct vimu *file_to_vimu(struct file *f)
{
	/* misc_open() stores the miscdevice in private_data */
	return container_of(f->private_data, struct vimu, misc);
}

static int vimu_open(struct inode *inode, struct file *f)
{
	struct vimu *v = file_to_vimu(f);
	unsigned long flags;

	if (atomic_inc_return(&v->users) == 1) {
		spin_lock_irqsave(&v->lock, flags);
		kfifo_reset(&v->fifo);
		spin_unlock_irqrestore(&v->lock, flags);
		hrtimer_start(&v->timer, v->period, HRTIMER_MODE_REL);
	}
	return 0;
}

static int vimu_release(struct inode *inode, struct file *f)
{
	struct vimu *v = file_to_vimu(f);

	if (atomic_dec_and_test(&v->users))
		hrtimer_cancel(&v->timer);
	return 0;
}

static bool vimu_has_data(struct vimu *v)
{
	unsigned long flags;
	bool ret;

	spin_lock_irqsave(&v->lock, flags);
	ret = !kfifo_is_empty(&v->fifo);
	spin_unlock_irqrestore(&v->lock, flags);
	return ret;
}

static ssize_t vimu_read(struct file *f, char __user *buf, size_t len,
			 loff_t *off)
{
	struct vimu *v = file_to_vimu(f);
	struct vimu_sample batch[VIMU_READ_BATCH];
	size_t max = min_t(size_t, len / sizeof(batch[0]), VIMU_READ_BATCH);
	unsigned long flags;
	unsigned int n;
	int ret;

	if (!max)
		return -EINVAL;

	for (;;) {
		spin_lock_irqsave(&v->lock, flags);
		n = kfifo_out(&v->fifo, batch, max);
		spin_unlock_irqrestore(&v->lock, flags);
		if (n)
			break;

		if (f->f_flags & O_NONBLOCK)
			return -EAGAIN;

		ret = wait_event_interruptible(v->wq, vimu_has_data(v));
		if (ret)
			return ret;
	}

	if (copy_to_user(buf, batch, n * sizeof(batch[0])))
		return -EFAULT;

	return n * sizeof(batch[0]);
}

static __poll_t vimu_poll(struct file *f, poll_table *wait)
{
	struct vimu *v = file_to_vimu(f);

	poll_wait(f, &v->wq, wait);
	return vimu_has_data(v) ? EPOLLIN | EPOLLRDNORM : 0;
}

static const struct file_operations vimu_fops = {
	.owner = THIS_MODULE,
	.open = vimu_open,
	.release = vimu_release,
	.read = vimu_read,
	.poll = vimu_poll,
	.llseek = noop_llseek,
};

static struct vimu *dev_to_vimu(struct device *dev)
{
	struct miscdevice *misc = dev_get_drvdata(dev);

	return container_of(misc, struct vimu, misc);
}

static ssize_t rate_hz_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	return sysfs_emit(buf, "%u\n", dev_to_vimu(dev)->rate_hz);
}
static DEVICE_ATTR_RO(rate_hz);

static ssize_t dropped_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct vimu *v = dev_to_vimu(dev);
	unsigned long flags;
	u32 dropped;

	spin_lock_irqsave(&v->lock, flags);
	dropped = v->dropped;
	spin_unlock_irqrestore(&v->lock, flags);

	return sysfs_emit(buf, "%u\n", dropped);
}
static DEVICE_ATTR_RO(dropped);

static struct attribute *vimu_attrs[] = {
	&dev_attr_rate_hz.attr,
	&dev_attr_dropped.attr,
	NULL,
};
ATTRIBUTE_GROUPS(vimu);

static int vimu_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct vimu *v;
	int ret;

	v = devm_kzalloc(dev, sizeof(*v), GFP_KERNEL);
	if (!v)
		return -ENOMEM;

	if (of_property_read_u32(dev->of_node, "sample-rate-hz", &v->rate_hz))
		v->rate_hz = VIMU_DEFAULT_RATE_HZ;
	if (!v->rate_hz || v->rate_hz > VIMU_MAX_RATE_HZ)
		return dev_err_probe(dev, -EINVAL,
				     "sample-rate-hz %u out of range\n",
				     v->rate_hz);

	v->period = ns_to_ktime(NSEC_PER_SEC / v->rate_hz);
	v->rng = 0x2545f491;
	spin_lock_init(&v->lock);
	INIT_KFIFO(v->fifo);
	init_waitqueue_head(&v->wq);
	atomic_set(&v->users, 0);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
	hrtimer_setup(&v->timer, vimu_tick, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
#else
	hrtimer_init(&v->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	v->timer.function = vimu_tick;
#endif

	v->misc.minor = MISC_DYNAMIC_MINOR;
	v->misc.name = "vimu";
	v->misc.fops = &vimu_fops;
	v->misc.groups = vimu_groups;
	v->misc.parent = dev;

	platform_set_drvdata(pdev, v);

	ret = misc_register(&v->misc);
	if (ret)
		return ret;

	dev_info(dev, "virtual IMU ready, %u Hz\n", v->rate_hz);
	return 0;
}

static void vimu_remove(struct platform_device *pdev)
{
	struct vimu *v = platform_get_drvdata(pdev);

	misc_deregister(&v->misc);
	hrtimer_cancel(&v->timer);
}

static const struct of_device_id vimu_of_match[] = {
	{ .compatible = "sensgw,vimu" },
	{ }
};
MODULE_DEVICE_TABLE(of, vimu_of_match);

static struct platform_driver vimu_driver = {
	.probe = vimu_probe,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
	.remove = vimu_remove,
#else
	.remove_new = vimu_remove,
#endif
	.driver = {
		.name = "vimu",
		.of_match_table = vimu_of_match,
	},
};
module_platform_driver(vimu_driver);

MODULE_AUTHOR("Faik Cankurtaran");
MODULE_DESCRIPTION("Virtual IMU platform driver");
MODULE_LICENSE("GPL");
