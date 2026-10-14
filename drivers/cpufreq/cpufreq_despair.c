// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2015 Matthew Alex <matthewalex@outlook.com>
 *  Copyright (C)  2001 Russell King
 *            (C)  2003 Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>.
 *                      Jun Nakajima <jun.nakajima@intel.com>
 *            (C)  2009 Alexander Clouter <alex@digriz.org.uk>
 *
 * Adapted for SM8150 from deani77/kernel_huawei_angler_old (n7.1.1).
 * Original despair author: Matthew Alex (2015).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/slab.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include "cpufreq_governor.h"

struct cs_policy_dbs_info {
	struct policy_dbs_info policy_dbs;
	unsigned int down_skip;
	unsigned int requested_freq;
	unsigned int twostep_counter;
	u64 twostep_time;
};

static inline struct cs_policy_dbs_info *to_dbs_info(struct policy_dbs_info *policy_dbs)
{
	return container_of(policy_dbs, struct cs_policy_dbs_info, policy_dbs);
}

struct cs_dbs_tuners {
	unsigned int down_threshold;
	unsigned int freq_step;
	unsigned int twostep_threshold;
};

/* Conservative governor macros */
#define DEF_FREQUENCY_UP_THRESHOLD		(95)
#define DEF_FREQUENCY_DOWN_THRESHOLD		(35)
#define DEF_FREQUENCY_STEP			(5)
#define DEF_SAMPLING_DOWN_FACTOR		(1)
#define MAX_SAMPLING_DOWN_FACTOR		(10)

static inline unsigned int get_freq_step(struct cs_dbs_tuners *cs_tuners,
					 struct cpufreq_policy *policy)
{
	unsigned int freq_step = (cs_tuners->freq_step * policy->max) / 100;

	/* max freq cannot be less than 100. But who knows... */
	if (unlikely(freq_step == 0))
		freq_step = DEF_FREQUENCY_STEP;

	return freq_step;
}

/*
 * Every sampling_rate, we check, if current idle time is less than 20%
 * (default), then we try to increase frequency. Every sampling_rate *
 * sampling_down_factor, we check, if current idle time is more than 80%
 * (default), then we try to decrease frequency
 *
 * Frequency updates happen at minimum steps of 5% (default) of maximum
 * frequency
 */
static unsigned int cs_dbs_update(struct cpufreq_policy *policy)
{
	struct policy_dbs_info *policy_dbs = policy->governor_data;
	struct cs_policy_dbs_info *info = to_dbs_info(policy_dbs);
	struct dbs_data *dbs = policy_dbs->dbs_data;
	struct cs_dbs_tuners *tuners = dbs->tuners;
	unsigned int load = dbs_update(policy), step, requested;
	u64 now = ktime_to_us(ktime_get());

	if (!tuners->freq_step)
		return dbs->sampling_rate;

	requested = clamp(info->requested_freq, policy->min, policy->max);
	step = get_freq_step(tuners, policy);
	/* The donor's two-step state was shared between CPU policies. */
	if (load > tuners->twostep_threshold) {
		if (load >= dbs->up_threshold) {
			info->down_skip = 0;
			info->twostep_counter = 0;
		} else if (info->twostep_counter++ < 2) {
			info->twostep_time = now;
			step = max(step / 2, 1U);
		} else {
			info->twostep_counter = 0;
			return dbs->sampling_rate;
		}
		requested += min(step, policy->max - requested);
		__cpufreq_driver_target(policy, requested, CPUFREQ_RELATION_H);
	} else if (++info->down_skip >= dbs->sampling_down_factor) {
		info->down_skip = 0;
		if (load < tuners->down_threshold) {
			if (now - info->twostep_time >= 150000)
				info->twostep_counter = 0;
			requested -= min(step, requested - policy->min);
			__cpufreq_driver_target(policy, requested, CPUFREQ_RELATION_L);
		}
	}
	info->requested_freq = requested;
	return dbs->sampling_rate;
}

/************************** sysfs interface ************************/

static ssize_t store_sampling_down_factor(struct gov_attr_set *attr_set,
					  const char *buf, size_t count)
{
	struct dbs_data *dbs_data = to_dbs_data(attr_set);
	unsigned int input;
	int ret;

	ret = kstrtouint(buf, 0, &input);

	if (ret || input > MAX_SAMPLING_DOWN_FACTOR || input < 1)
		return -EINVAL;

	dbs_data->sampling_down_factor = input;
	return count;
}

static ssize_t store_up_threshold(struct gov_attr_set *attr_set,
				  const char *buf, size_t count)
{
	struct dbs_data *dbs_data = to_dbs_data(attr_set);
	struct cs_dbs_tuners *cs_tuners = dbs_data->tuners;
	unsigned int input;
	int ret;

	ret = kstrtouint(buf, 0, &input);

	if (ret || input > 100 || input <= cs_tuners->twostep_threshold)
		return -EINVAL;

	dbs_data->up_threshold = input;
	return count;
}

static ssize_t store_down_threshold(struct gov_attr_set *attr_set,
				    const char *buf, size_t count)
{
	struct dbs_data *dbs_data = to_dbs_data(attr_set);
	struct cs_dbs_tuners *cs_tuners = dbs_data->tuners;
	unsigned int input;
	int ret;

	ret = kstrtouint(buf, 0, &input);

	/* cannot be lower than 1 otherwise freq will not fall */
	if (ret || input < 1 || input > 100 ||
			input >= cs_tuners->twostep_threshold)
		return -EINVAL;

	cs_tuners->down_threshold = input;
	return count;
}

static ssize_t store_ignore_nice_load(struct gov_attr_set *attr_set,
				      const char *buf, size_t count)
{
	struct dbs_data *dbs_data = to_dbs_data(attr_set);
	unsigned int input;
	int ret;

	ret = kstrtouint(buf, 0, &input);
	if (ret)
		return -EINVAL;

	if (input > 1)
		input = 1;

	if (input == dbs_data->ignore_nice_load) /* nothing to do */
		return count;

	dbs_data->ignore_nice_load = input;

	/* we need to re-evaluate prev_cpu_idle */
	gov_update_cpu_data(dbs_data);

	return count;
}

static ssize_t store_freq_step(struct gov_attr_set *attr_set, const char *buf,
			       size_t count)
{
	struct dbs_data *dbs_data = to_dbs_data(attr_set);
	struct cs_dbs_tuners *cs_tuners = dbs_data->tuners;
	unsigned int input;
	int ret;

	ret = kstrtouint(buf, 0, &input);

	if (ret)
		return -EINVAL;

	if (input > 100)
		input = 100;

	/*
	 * no need to test here if freq_step is zero as the user might actually
	 * want this, they would be crazy though :)
	 */
	cs_tuners->freq_step = input;
	return count;
}

static ssize_t store_twostep_threshold(struct gov_attr_set *attr_set,
				       const char *buf, size_t count)
{
	struct dbs_data *dbs = to_dbs_data(attr_set);
	struct cs_dbs_tuners *tuners = dbs->tuners;
	unsigned int value;

	if (kstrtouint(buf, 0, &value) || value >= dbs->up_threshold ||
	    value <= tuners->down_threshold)
		return -EINVAL;
	tuners->twostep_threshold = value;
	return count;
}

gov_show_one(cs, twostep_threshold);
gov_attr_rw(twostep_threshold);

gov_show_one_common(sampling_rate);
gov_show_one_common(sampling_down_factor);
gov_show_one_common(up_threshold);
gov_show_one_common(ignore_nice_load);
gov_show_one(cs, down_threshold);
gov_show_one(cs, freq_step);

gov_attr_rw(sampling_rate);
gov_attr_rw(sampling_down_factor);
gov_attr_rw(up_threshold);
gov_attr_rw(ignore_nice_load);
gov_attr_rw(down_threshold);
gov_attr_rw(freq_step);

static struct attribute *cs_attributes[] = {
	&sampling_rate.attr,
	&sampling_down_factor.attr,
	&up_threshold.attr,
	&down_threshold.attr,
	&ignore_nice_load.attr,
	&freq_step.attr,
	&twostep_threshold.attr,
	NULL
};

/************************** sysfs end ************************/

static struct policy_dbs_info *cs_alloc(void)
{
	struct cs_policy_dbs_info *dbs_info;

	dbs_info = kzalloc(sizeof(*dbs_info), GFP_KERNEL);
	return dbs_info ? &dbs_info->policy_dbs : NULL;
}

static void cs_free(struct policy_dbs_info *policy_dbs)
{
	kfree(to_dbs_info(policy_dbs));
}

static int cs_init(struct dbs_data *dbs_data)
{
	struct cs_dbs_tuners *tuners;

	tuners = kzalloc(sizeof(*tuners), GFP_KERNEL);
	if (!tuners)
		return -ENOMEM;

	tuners->down_threshold = DEF_FREQUENCY_DOWN_THRESHOLD;
	tuners->freq_step = DEF_FREQUENCY_STEP;
	tuners->twostep_threshold = 65;
	dbs_data->up_threshold = DEF_FREQUENCY_UP_THRESHOLD;
	dbs_data->sampling_down_factor = DEF_SAMPLING_DOWN_FACTOR;
	dbs_data->ignore_nice_load = 0;
	dbs_data->tuners = tuners;

	return 0;
}

static void cs_exit(struct dbs_data *dbs_data)
{
	kfree(dbs_data->tuners);
}

static void cs_start(struct cpufreq_policy *policy)
{
	struct cs_policy_dbs_info *dbs_info = to_dbs_info(policy->governor_data);

	dbs_info->down_skip = 0;
	dbs_info->twostep_counter = 0;
	dbs_info->twostep_time = 0;
	dbs_info->requested_freq = policy->cur;
}

static struct dbs_governor cs_governor = {
	.gov = CPUFREQ_DBS_GOVERNOR_INITIALIZER("despair"),
	.kobj_type = { .default_attrs = cs_attributes },
	.gov_dbs_update = cs_dbs_update,
	.alloc = cs_alloc,
	.free = cs_free,
	.init = cs_init,
	.exit = cs_exit,
	.start = cs_start,
};

#define CPU_FREQ_GOV_DESPAIR	(&cs_governor.gov)

static int __init cpufreq_gov_dbs_init(void)
{
	return cpufreq_register_governor(CPU_FREQ_GOV_DESPAIR);
}

static void __exit cpufreq_gov_dbs_exit(void)
{
	cpufreq_unregister_governor(CPU_FREQ_GOV_DESPAIR);
}

MODULE_AUTHOR("Alexander Clouter <alex@digriz.org.uk>");
MODULE_AUTHOR("Matthew Alex");
MODULE_DESCRIPTION("SM8150 despair governor port");
MODULE_LICENSE("GPL");

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_DESPAIR
struct cpufreq_governor *cpufreq_default_governor(void)
{
	return CPU_FREQ_GOV_DESPAIR;
}

fs_initcall(cpufreq_gov_dbs_init);
#else
module_init(cpufreq_gov_dbs_init);
#endif
module_exit(cpufreq_gov_dbs_exit);
