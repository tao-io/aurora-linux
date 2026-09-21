// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * J713 Linux operating-temperature policy. These are not Apple safety limits.
 * PMP remains firmware-owned; this client only reads its documented records.
 */

#include <linux/cpu.h>
#include <linux/cpu_cooling.h>
#include <linux/cpufreq.h>
#include <linux/hwmon.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_qos.h>
#include <linux/soc/apple/pmp-temps.h>
#include <linux/soc/apple/pmp-cooling.h>
#include <linux/thermal.h>
#include <linux/workqueue.h>

#include "apple-pmp-confirm.h"

/* Fast raw protection and ordinary governor regulation have separate clocks.
 * No sysfs read advances either controller. Thresholds remain a development
 * policy, not an inference of Apple's limits from macOS temperature peaks.
 */
#define APPLE_PMP_HYST_MC		2000
#define APPLE_PMP_POLL_MS		5
#define APPLE_PMP_GOVERNOR_NS	100000000ULL

struct apple_pmp_cluster {
	struct cpufreq_policy *policy;
	struct thermal_cooling_device *cdev;
	struct freq_qos_request guard;
};

static struct {
	void __iomem *sram;
	struct thermal_zone_device *zone;
	struct apple_pmp_cluster clusters[2];
	struct mutex lock; /* Serializes sensor reads, mode changes and guard updates. */
	struct apple_pmp_cooling cooling;
	unsigned int guard_level;
	bool enabled;
	long cached_mc;
	int cached_error;
	u64 governor_ns;
	unsigned long long sample_count, governor_count;
	struct apple_pmp_confirmation critical;
	bool shutdown_requested;
	struct hrtimer poll_timer;
	struct work_struct poll_work;
	bool polling;
	bool debug_critical_dumped; /* DEBUG_REMOVE_IN_PROD */
} pmp_thermal;

/* Read-only cadence diagnostics: reading temperatures must not advance these. */
module_param_named(sample_count, pmp_thermal.sample_count, ullong, 0444);
module_param_named(governor_count, pmp_thermal.governor_count, ullong, 0444);

/* DEBUG_REMOVE_IN_PROD: retain the actual samples used for the decision,
 * including invalid/unavailable channels. A later read could miss the spike.
 */
struct apple_pmp_debug_snapshot {
	u64 started_ns;
	u64 finished_ns;
	long values[PMP_TEMP_CHANNELS];
	int errors[PMP_TEMP_CHANNELS];
};

static int apple_pmp_debug_sample(struct apple_pmp_debug_snapshot *snapshot,
				  long *value)
{
	bool found = false;
	int first_error = 0;
	unsigned int i;

	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->started_ns = ktime_get_ns();
	for (i = 0; i < PMP_TEMP_CHANNELS; i++) {
		int ret = apple_pmp_temp_sample(pmp_thermal.sram, i,
						&snapshot->values[i]);

		snapshot->errors[i] = ret;
		if (ret) {
			if (ret != -ENODATA && !first_error)
				first_error = ret;
			continue;
		}
		if (!found || snapshot->values[i] > *value)
			*value = snapshot->values[i];
		found = true;
	}
	snapshot->finished_ns = ktime_get_ns();
	return first_error ?: (found ? 0 : -ENODATA);
}

/* Defer slow console output; do not hold up critical-trip processing. */
static void apple_pmp_debug_dump(const struct apple_pmp_debug_snapshot *snapshot,
				 const char *reason, long value, int error)
{
	unsigned int i;

	hwmon_debug_printk(KERN_EMERG "DEBUG_REMOVE_IN_PROD apple-pmp: %s hotspot_mc=%ld error=%d sample_start_ns=%llu sample_end_ns=%llu\n",
		 reason, value, error, snapshot->started_ns, snapshot->finished_ns);
	for (i = 0; i < PMP_TEMP_CHANNELS; i++)
		hwmon_debug_printk(KERN_EMERG "DEBUG_REMOVE_IN_PROD apple-pmp: %s mc=%ld error=%d\n",
			 apple_pmp_temp_records[i].name, snapshot->values[i],
			 snapshot->errors[i]);
	for (i = 0; i < ARRAY_SIZE(pmp_thermal.clusters); i++) {
		struct cpufreq_policy *policy = pmp_thermal.clusters[i].policy;

		if (policy)
			hwmon_debug_printk(KERN_EMERG "DEBUG_REMOVE_IN_PROD apple-pmp: cpu%u min_khz=%u max_khz=%u\n",
				 policy->cpu, policy->min, policy->max);
	}
	if (IS_REACHABLE(CONFIG_HWMON))
		hwmon_debug_dump_temperatures();
}

/* DEBUG_REMOVE_IN_PROD: safe logging test; changes no temperatures or trips. */
static int apple_pmp_debug_trigger(const char *val, const struct kernel_param *kp)
{
	struct apple_pmp_debug_snapshot snapshot;
	long value = 0;
	bool trigger;
	int ret = kstrtobool(val, &trigger);

	if (ret || !trigger)
		return ret ?: -EINVAL;
	/* Module parameters may be parsed before probe initializes the mutex. */
	if (!READ_ONCE(pmp_thermal.zone))
		return -ENODEV;
	guard(mutex)(&pmp_thermal.lock);
	if (!pmp_thermal.zone || !pmp_thermal.sram)
		return -ENODEV;
	ret = apple_pmp_debug_sample(&snapshot, &value);
	apple_pmp_debug_dump(&snapshot, "manual-dump-not-critical", value, ret);
	return ret;
}

static const struct kernel_param_ops apple_pmp_debug_ops = {
	.set = apple_pmp_debug_trigger,
};
module_param_cb(debug_dump_thermals, &apple_pmp_debug_ops, NULL, 0200);
MODULE_PARM_DESC(debug_dump_thermals, "DEBUG_REMOVE_IN_PROD: write Y to log temperatures without changing trips");

/* Fail closed independently of the governor, including while the zone is off. */
static int apple_pmp_guard(unsigned int level)
{
	unsigned int i;
	int ret, err = 0;

	for (i = 0; i < ARRAY_SIZE(pmp_thermal.clusters); i++) {
		struct apple_pmp_cluster *c = &pmp_thermal.clusters[i];

		unsigned int low = c->policy->cpuinfo.min_freq;
		unsigned int high = c->policy->cpuinfo.max_freq;

		/* QoS composes with user caps and governor cooling. Intermediate
		 * requests are rounded down to supported OPPs by cpufreq.
		 */
		ret = freq_qos_update_request(&c->guard, level ?
			low + (high - low) * (8 - level) / 8 :
			FREQ_QOS_MAX_DEFAULT_VALUE);
		if (ret < 0)
			err = ret;
	}
	if (!err)
		pmp_thermal.guard_level = level;
	return err;
}

static int apple_pmp_sample(void)
{
	struct apple_pmp_debug_snapshot snapshot; /* DEBUG_REMOVE_IN_PROD */
	struct apple_pmp_cooling *cooling = &pmp_thermal.cooling;
	long value = 0;
	bool was_confirmed; /* DEBUG_REMOVE_IN_PROD */
	unsigned int old_level, level, i;
	int old_reason, channel = -1, ret, guard_ret;
	const char *reason;

	guard(mutex)(&pmp_thermal.lock);
	old_level = pmp_thermal.guard_level;
	old_reason = cooling->reason;
	ret = apple_pmp_debug_sample(&snapshot, &value); /* DEBUG_REMOVE_IN_PROD */
	was_confirmed = pmp_thermal.critical.confirmed; /* DEBUG_REMOVE_IN_PROD */
	apple_pmp_confirmation_sample(&pmp_thermal.critical, value, ret,
				      snapshot.finished_ns);
	apple_pmp_cooling_sample(cooling, snapshot.finished_ns, value, ret);
	/* Capture governor demand before it releases a trip; the independent
	 * QoS envelope then restores CPU capacity gradually. Convert the cooling
	 * device's own state, not policy->max (which includes this guard).
	 */
	for (i = 0; i < ARRAY_SIZE(pmp_thermal.clusters); i++) {
		struct thermal_cooling_device *cdev = pmp_thermal.clusters[i].cdev;
		unsigned long state;

		if (cdev && cdev->max_state && !cdev->ops->get_cur_state(cdev, &state))
			apple_pmp_cooling_observe(cooling, snapshot.finished_ns,
				DIV_ROUND_UP(state * 8, cdev->max_state));
	}
	level = apple_pmp_cooling_level(cooling);
	/* Never release startup/fault protection with disabled or unbound cooling. */
	if (!pmp_thermal.enabled)
		level = 8;
	guard_ret = apple_pmp_guard(level);
	if (guard_ret)
		apple_pmp_guard(8);
	for (i = 0; i < PMP_TEMP_CHANNELS; i++)
		if (!snapshot.errors[i] && snapshot.values[i] == value) {
			channel = i;
			break;
		}
	switch (cooling->reason) {
	case 0: reason = "startup"; break;
	case 1: reason = "sensor"; break;
	case 2: reason = "measured-heat"; break;
	case 3: reason = "prediction"; break;
	case 5: reason = "recovery"; break;
	default: reason = "governor"; break;
	}
	if (old_level != pmp_thermal.guard_level || old_reason != cooling->reason)
		pr_info("apple-pmp-thermal: decision ns=%llu mc=%ld channel=%d error=%d previous_mc=%ld dt_ns=%llu projected_mc=%ld reason=%s hard=%d prediction=%u level=%u qos_error=%d\n",
			snapshot.finished_ns, value, channel, ret, cooling->previous_mc,
			cooling->dt_ns, cooling->projected_mc, reason, cooling->hard,
			cooling->prediction, pmp_thermal.guard_level, guard_ret);
	pmp_thermal.cached_mc = value;
	pmp_thermal.cached_error = ret ?: guard_ret;
	if (ret) {
		pr_warn_ratelimited("apple-pmp-thermal: sensor error %d; CPU minimum caps retained\n", ret);
		return ret;
	}
	ret = guard_ret;
	/* DEBUG_REMOVE_IN_PROD: first real critical sample, before core poweroff. */
	if (value >= APPLE_PMP_CRITICAL_MC && !pmp_thermal.debug_critical_dumped) {
		pmp_thermal.debug_critical_dumped = true;
		apple_pmp_debug_dump(&snapshot, "critical-sample", value, 0);
	}
	if (!was_confirmed && pmp_thermal.critical.confirmed)
		apple_pmp_debug_dump(&snapshot, "critical-confirmed-third-sample", value, 0);
	return ret;
}

/* Thermal-core/sysfs reads observe the last raw sample without changing the
 * sample count, QoS guard, or governor cadence. The fast worker owns sampling.
 */
static int apple_pmp_get_temp(struct thermal_zone_device *zone, int *temp)
{
	guard(mutex)(&pmp_thermal.lock);
	if (!pmp_thermal.cooling.last_ns ||
	    ktime_get_ns() - pmp_thermal.cooling.last_ns > 500000000ULL)
		return -EAGAIN;
	if (pmp_thermal.cached_error)
		return pmp_thermal.cached_error;
	*temp = pmp_thermal.cached_mc;
	return 0;
}

/* The thermal core calls .critical only on upward crossings. The poll
 * worker also checks after every raw sample so sustained heat reaches sample 3.
 * Preserve the raw temperature and the normal thermal-core poweroff path.
 */
static void apple_pmp_critical(struct thermal_zone_device *zone)
{
	bool shutdown = false;

	mutex_lock(&pmp_thermal.lock);
	if (pmp_thermal.critical.confirmed && !pmp_thermal.shutdown_requested) {
		pmp_thermal.shutdown_requested = true;
		shutdown = true;
	}
	mutex_unlock(&pmp_thermal.lock);
	if (shutdown) {
		pr_emerg("apple-pmp-thermal: three >=120 C samples at least 5 ms apart; requesting shutdown\n");
		thermal_zone_device_critical(zone);
	}
}

static void apple_pmp_poll_work(struct work_struct *work)
{
	u64 now;

	apple_pmp_sample();
	pmp_thermal.sample_count++;
	apple_pmp_critical(pmp_thermal.zone);
	now = ktime_get_ns();
	if (now - pmp_thermal.governor_ns >= APPLE_PMP_GOVERNOR_NS) {
		pmp_thermal.governor_ns = now;
		pmp_thermal.governor_count++;
		thermal_zone_device_update(pmp_thermal.zone, THERMAL_EVENT_UNSPECIFIED);
	}
}

static enum hrtimer_restart apple_pmp_poll_timer(struct hrtimer *timer)
{
	/* Sampling/QoS may sleep, so never perform either in timer context.
	 * One work item coalesces missed ticks; no burst of stale confirmations.
	 */
	queue_work(system_highpri_wq, &pmp_thermal.poll_work);
	hrtimer_forward_now(timer, ms_to_ktime(APPLE_PMP_POLL_MS));
	return HRTIMER_RESTART;
}

static int apple_pmp_change_mode(struct thermal_zone_device *zone,
				 enum thermal_device_mode mode)
{
	guard(mutex)(&pmp_thermal.lock);
	pmp_thermal.enabled = mode == THERMAL_DEVICE_ENABLED;
	apple_pmp_cooling_init(&pmp_thermal.cooling);
	return apple_pmp_guard(8);
}

static bool apple_pmp_should_bind(struct thermal_zone_device *zone,
				  const struct thermal_trip *trip,
				  struct thermal_cooling_device *cdev,
				  struct cooling_spec *spec)
{
	unsigned int i;

	if (trip->type != THERMAL_TRIP_PASSIVE)
		return false;
	if (!strcmp(cdev->type, "asahi-g16g")) {
		spec->lower = 0;
		spec->upper = cdev->max_state;
		return true;
	}

	for (i = 0; i < ARRAY_SIZE(pmp_thermal.clusters); i++) {
		struct apple_pmp_cluster *c = &pmp_thermal.clusters[i];

		if (cdev != c->cdev)
			continue;
		spec->lower = 0;
		spec->upper = cdev->max_state;
		return true;
	}
	return false;
}

static const struct thermal_zone_device_ops apple_pmp_thermal_ops = {
	.get_temp = apple_pmp_get_temp,
	.critical = apple_pmp_critical,
	.change_mode = apple_pmp_change_mode,
	.should_bind = apple_pmp_should_bind,
};

static const struct thermal_trip apple_pmp_trips[] = {
	{
		.temperature = APPLE_PMP_PASSIVE_MC,
		.hysteresis = APPLE_PMP_HYST_MC,
		.type = THERMAL_TRIP_PASSIVE,
	},
	{
		.temperature = APPLE_PMP_TARGET_MC,
		.hysteresis = 4000,
		.type = THERMAL_TRIP_PASSIVE,
	},
	{
		.temperature = APPLE_PMP_CRITICAL_MC,
		.type = THERMAL_TRIP_CRITICAL,
	},
};

static const struct thermal_zone_params apple_pmp_params = {
	.governor_name = "step_wise",
	.no_hwmon = true, /* Keep the existing six-channel hwmon ABI. */
};

static int apple_pmp_check_bindings(struct thermal_trip *trip, void *data)
{
	struct thermal_zone_device *zone = data;
	unsigned int i;

	if (trip->type != THERMAL_TRIP_PASSIVE)
		return 0;

	for (i = 0; i < ARRAY_SIZE(pmp_thermal.clusters); i++)
		if (!thermal_trip_is_bound_to_cdev(zone, trip, pmp_thermal.clusters[i].cdev))
			return -ENODEV;
	return 0;
}

static void apple_pmp_thermal_cleanup(void)
{
	unsigned int i;

	if (pmp_thermal.polling) {
		hrtimer_cancel(&pmp_thermal.poll_timer);
		cancel_work_sync(&pmp_thermal.poll_work);
		pmp_thermal.polling = false;
	}
	if (pmp_thermal.zone) {
		thermal_zone_device_unregister(pmp_thermal.zone);
		pmp_thermal.zone = NULL;
	}
	for (i = 0; i < ARRAY_SIZE(pmp_thermal.clusters); i++) {
		struct apple_pmp_cluster *c = &pmp_thermal.clusters[i];

		cpufreq_cooling_unregister(c->cdev);
		if (freq_qos_request_active(&c->guard))
			freq_qos_remove_request(&c->guard);
		if (c->policy)
			cpufreq_cpu_put(c->policy);
	}
	if (pmp_thermal.sram)
		iounmap(pmp_thermal.sram);
}

static int apple_pmp_add_cluster(unsigned int index, unsigned int cpu,
				 unsigned long members)
{
	struct apple_pmp_cluster *c = &pmp_thermal.clusters[index];
	unsigned int n;
	int ret;

	c->policy = cpufreq_cpu_get(cpu);
	if (!c->policy)
		return -EPROBE_DEFER;
	for_each_possible_cpu(n)
		if (cpumask_test_cpu(n, c->policy->related_cpus) !=
		    (n < BITS_PER_LONG && !!(members & BIT(n))))
			return -EINVAL;
	if (!c->policy->freq_table)
		return -EINVAL;

	ret = freq_qos_add_request(&c->policy->constraints, &c->guard,
				   FREQ_QOS_MAX, c->policy->cpuinfo.min_freq);
	if (ret < 0)
		return ret;
	c->cdev = cpufreq_cooling_register(c->policy);
	if (IS_ERR(c->cdev)) {
		ret = PTR_ERR(c->cdev);
		c->cdev = NULL;
		return ret;
	}
	return 0;
}

static int __init apple_pmp_thermal_init(void)
{
	struct thermal_zone_device *zone;
	struct device_node *np;
	struct platform_device *report;
	int ret;

	/* Board policy is opt-in; never guess the sensor layout on another Mac. */
	if (!of_machine_is_compatible("apple,j713"))
		return -ENODEV;
	np = of_find_compatible_node(NULL, NULL, "apple,t8132-pmp-v2-report");
	if (!np)
		return -ENODEV;
	report = of_find_device_by_node(np);
	ret = of_device_is_available(np) && report && device_is_bound(&report->dev) ?
		0 : -EPROBE_DEFER;
	if (report)
		put_device(&report->dev);
	of_node_put(np);
	if (ret)
		return ret;

	mutex_init(&pmp_thermal.lock);
	apple_pmp_cooling_init(&pmp_thermal.cooling);
	pmp_thermal.guard_level = 8;
	/* Independent read-only mapping permits insmod without rebinding PMP. */
	pmp_thermal.sram = ioremap(PMP_TEMP_SRAM_BASE, PMP_TEMP_SRAM_SIZE);
	if (!pmp_thermal.sram)
		return -ENOMEM;
	ret = apple_pmp_add_cluster(0, 0, BIT(0) | GENMASK(9, 7));
	if (ret)
		goto fail;
	ret = apple_pmp_add_cluster(1, 1, GENMASK(6, 1));
	if (ret)
		goto fail;
	zone = thermal_zone_device_register_with_trips("apple-pmp-die", apple_pmp_trips,
						       ARRAY_SIZE(apple_pmp_trips), NULL,
						       &apple_pmp_thermal_ops, &apple_pmp_params,
						       0, 0);
	pmp_thermal.zone = zone;
	if (IS_ERR(pmp_thermal.zone)) {
		ret = PTR_ERR(pmp_thermal.zone);
		pmp_thermal.zone = NULL;
		goto fail;
	}
	/* Trip array is immutable; the binding query takes the zone lock itself. */
	ret = for_each_thermal_trip(pmp_thermal.zone, apple_pmp_check_bindings,
				    pmp_thermal.zone);
	if (ret)
		goto fail;
	ret = thermal_zone_device_enable(pmp_thermal.zone);
	if (ret)
		goto fail;
	INIT_WORK(&pmp_thermal.poll_work, apple_pmp_poll_work);
	hrtimer_setup(&pmp_thermal.poll_timer, apple_pmp_poll_timer,
		      CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	pmp_thermal.polling = true;
	hrtimer_start(&pmp_thermal.poll_timer, ms_to_ktime(APPLE_PMP_POLL_MS),
		      HRTIMER_MODE_REL);
	pr_info("apple-pmp-thermal: J713 provisional policy: CPU/GPU cooling from 106 C, target 110 C, full protection 115 C, three >=120 C samples for shutdown, 5 ms sampling/100 ms governor\n");
	return 0;
fail:
	apple_pmp_thermal_cleanup();
	return ret;
}

static void __exit apple_pmp_thermal_exit(void)
{
	pr_warn("apple-pmp-thermal: removing Linux thermal policy and its CPU caps\n");
	apple_pmp_thermal_cleanup();
}

module_init(apple_pmp_thermal_init);
module_exit(apple_pmp_thermal_exit);

MODULE_DESCRIPTION("Apple J713 PMP package CPU/GPU thermal policy");
MODULE_LICENSE("Dual MIT/GPL");
