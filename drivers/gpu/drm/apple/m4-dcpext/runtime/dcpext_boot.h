/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Built-in desktop lifetime. Waiting reads the Type-C owner's cached snapshot;
 * it never touches PHY/DART/firmware until a supported monitor is present.
 * After the first startup attempt, uncertain hardware is never retried.
 */
#include <linux/reboot.h>

static bool enable = true;
module_param(enable, bool, 0400);
MODULE_PARM_DESC(enable, "Automatic J713 external display startup (disable for recovery)");
static bool boot_waiting;
module_param(boot_waiting, bool, 0400);
static bool boot_stopping;
static void dcpext_boot_work_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(dcpext_boot_work, dcpext_boot_work_fn);

static void dcpext_boot_work_fn(struct work_struct *work)
{
	int ret;

	if (READ_ONCE(boot_stopping))
		return;
	ret = session_atc_find_partner();
	if (ret < 0) {
		/* Controllers may not have bound yet, or neither port has a sink.
		 * No resource has been acquired and normal laptop boot continues. */
		if (ret == -ENOLINK || ret == -ENODEV || ret == -EOPNOTSUPP ||
		    ret == -EPROBE_DEFER || ret == -EAGAIN) {
			schedule_delayed_work(&dcpext_boot_work, HZ);
			return;
		}
		WRITE_ONCE(result, ret);
		WRITE_ONCE(boot_waiting, false);
		pr_err("m4_dcpext: Type-C admission failed: %d\n", ret);
		return;
	}
	WRITE_ONCE(boot_waiting, false);
	pr_info("m4_dcpext: automatic startup on USB-C%u\n", ret);
	ret = probe_init();
	if (ret)
		pr_err("m4_dcpext: startup failed: %d; no automatic firmware restart\n", ret);
}

static int dcpext_boot_shutdown(struct notifier_block *nb,
				unsigned long action, void *data)
{
	WRITE_ONCE(boot_stopping, true);
	cancel_delayed_work_sync(&dcpext_boot_work);
	if (!native_kms_worker_started)
		return NOTIFY_DONE;
	/* The existing worker owns all RPCs and DMA retirement. Never release
	 * uncertain mappings or force-stop firmware from a reboot callback. */
	WRITE_ONCE(desktop_stop, true);
	if (!wait_for_completion_timeout(&native_kms_worker_exited, 30 * HZ))
		pr_err("m4_dcpext: shutdown timed out; retaining DMA resources\n");
	else
		pr_info("m4_dcpext: kernel shutdown completed: %d\n", READ_ONCE(result));
	return NOTIFY_DONE;
}

static struct notifier_block dcpext_boot_notifier = {
	.notifier_call = dcpext_boot_shutdown,
};

static int __init dcpext_boot_init(void)
{
	int ret;

#if DCPEXT_INSTANCE == 1
	/* Preserve the existing recovery option that disables external display. */
	if (!m4_dcpext_primary_enabled())
		return 0;
#endif
	if (!enable || !of_machine_is_compatible("apple,j713") ||
	    !of_machine_is_compatible("apple,t8132"))
		return 0;
	ret = register_reboot_notifier(&dcpext_boot_notifier);
	if (ret)
		return ret;
	WRITE_ONCE(boot_waiting, true);
	pr_info("m4_dcpext%u: built-in driver waiting for a USB-C display\n", port);
	schedule_delayed_work(&dcpext_boot_work, HZ);
	return 0;
}
late_initcall(dcpext_boot_init);
