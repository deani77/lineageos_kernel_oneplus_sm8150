// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2015 Aaron Segaert <asegaert@gmail.com>
 *  Copyright (C)  2001 Russell King
 *            (C)  2003 Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>.
 *                      Jun Nakajima <jun.nakajima@intel.com>
 *            (C)  2009 Alexander Clouter <alex@digriz.org.uk>
 *
 * Adapted for SM8150 from deani77/kernel_huawei_angler_old (n7.1.1).
 * Original elementalx author: Aaron Segaert (2015).
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
	unsigned int prev_load;
};

static inline struct cs_policy_dbs_info *to_dbs_info(struct policy_dbs_info *policy_dbs)
{
	return container_of(policy_dbs, struct cs_policy_dbs_info, policy_dbs);
}

struct cs_dbs_tuners {
	unsigned int down_differential;
	unsigned int active_floor_freq;
	unsigned int powersave;
};

#define DEF_FREQUENCY_UP_THRESHOLD 90
#define DEF_SAMPLING_DOWN_FACTOR 4
#define MAX_SAMPLING_DOWN_FACTOR 20

static const unsigned int tblmap[6][12] = {

	//table 0
	{
		616400,
		757200,
		840000,
		960000,
		1248000,
		1344000,
		1478400,
		1555200,
		1632000,
		1728000,
		1824000,
		1958400,
	},

	//table 1
	{
		773040,
		899760,
		1014960,
		1072560,
		1248000,
		1344000,
		1478400,
		1555200,
		1632000,
		1728000,
		1824000,
		1958400,
	},

	//table 2
	{
		851100,
		956700,
		1052700,
		1100700,
		1350400,
		1416000,
		1550400,
		1627200,
		1740800,
		1824000,
		1920000,
		2054400,
	},

	//table 3
	{
		616400,
		757200,
		840000,
		960000,
		1248000,
		1344000,
		1478400,
		1555200,
		1555200,
		1555200,
		1555200,
		1555200,
	},

	//table 4
	{
		773040,
		899760,
		1014960,
		1072560,
		1248000,
		1344000,
		1478400,
		1555200,
		1555200,
		1555200,
		1555200,
		1555200,
	},

	//table 5
	{
		851100,
		956700,
		1052700,
		1100700,
		1350400,
		1416000,
		1550400,
		1627200,
		1627200,
		1627200,
		1627200,
		1627200,
	}

};

/* Map the donor's ramp curves onto this policy's actual frequency range.
 * No assumption about CPU number, table ordering or number of OPPs.
 */
static unsigned int ex_ramp(struct cpufreq_policy *policy,
			   unsigned int load, unsigned int average,
			   unsigned int powersave)
{
	unsigned int row, slot, target;
	u64 scaled;

	if (policy->cpuinfo.max_freq == policy->cpuinfo.min_freq)
		return policy->max;
	if (policy->cur < policy->cpuinfo.min_freq +
	    (policy->cpuinfo.max_freq - policy->cpuinfo.min_freq) / 5 &&
	    load > 95)
		return policy->max;

	row = average > 95 || load > 95 ? 2 : (average <= 85 ? 0 : 1);
	row += powersave ? 3 : 0;
	scaled = (u64)(clamp(policy->cur, policy->cpuinfo.min_freq,
			    policy->cpuinfo.max_freq) - policy->cpuinfo.min_freq) * 11;
	slot = div_u64(scaled, policy->cpuinfo.max_freq - policy->cpuinfo.min_freq);
	scaled = (u64)tblmap[row][slot] * policy->cpuinfo.max_freq;
	target = div_u64(scaled, row % 3 == 2 ? 2054400 : 1958400);
	return clamp(max(target, policy->cur), policy->min, policy->max);
}

static unsigned int cs_dbs_update(struct cpufreq_policy *policy)
{
	struct policy_dbs_info *policy_dbs = policy->governor_data;
	struct cs_policy_dbs_info *info = to_dbs_info(policy_dbs);
	struct dbs_data *dbs = policy_dbs->dbs_data;
	struct cs_dbs_tuners *tuners = dbs->tuners;
	unsigned int load = dbs_update(policy), target, floor;
	unsigned int average = (info->prev_load + load) / 2;
	unsigned int threshold = READ_ONCE(dbs->up_threshold);
	unsigned int differential = READ_ONCE(tuners->down_differential);

	/* Sysfs can change these between two reads. Never divide by zero. */
	if (threshold <= differential)
		return dbs->sampling_rate;

	info->prev_load = load;
	if (load > 85) {
		target = ex_ramp(policy, load, average, tuners->powersave);
		if (target > tuners->active_floor_freq)
			info->down_skip = 0;
		__cpufreq_driver_target(policy, target, CPUFREQ_RELATION_H);
	} else if (load < threshold - differential) {
		target = div_u64((u64)load * policy->cur,
				threshold - differential);
		floor = clamp(tuners->active_floor_freq, policy->min, policy->max);
		if (policy->cur >= floor &&
		    ++info->down_skip <= dbs->sampling_down_factor)
			target = max(target, floor);
		else
			info->down_skip = 0;
		__cpufreq_driver_target(policy, clamp(target, policy->min,
				       policy->max), CPUFREQ_RELATION_L);
	}
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

	if (ret || input > 100 || input <= cs_tuners->down_differential)
		return -EINVAL;

	dbs_data->up_threshold = input;
	return count;
}

static ssize_t store_down_differential(struct gov_attr_set *attr_set,
				    const char *buf, size_t count)
{
	struct dbs_data *dbs_data = to_dbs_data(attr_set);
	struct cs_dbs_tuners *cs_tuners = dbs_data->tuners;
	unsigned int input;
	int ret;

	ret = kstrtouint(buf, 0, &input);

	/* cannot be lower than 1 otherwise freq will not fall */
	if (ret || input < 1 || input > 100 ||
			input >= dbs_data->up_threshold)
		return -EINVAL;

	cs_tuners->down_differential = input;
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

static ssize_t store_active_floor_freq(struct gov_attr_set *attr_set,
				      const char *buf, size_t count)
{
	struct cs_dbs_tuners *tuners = to_dbs_data(attr_set)->tuners;
	unsigned int value;

	if (kstrtouint(buf, 0, &value))
		return -EINVAL;
	tuners->active_floor_freq = value;
	return count;
}

static ssize_t store_powersave(struct gov_attr_set *attr_set,
			      const char *buf, size_t count)
{
	struct cs_dbs_tuners *tuners = to_dbs_data(attr_set)->tuners;
	unsigned int value;

	if (kstrtouint(buf, 0, &value) || value > 1)
		return -EINVAL;
	tuners->powersave = value;
	return count;
}

gov_show_one_common(sampling_rate);
gov_show_one_common(sampling_down_factor);
gov_show_one_common(up_threshold);
gov_show_one_common(ignore_nice_load);
gov_show_one(cs, down_differential);
gov_show_one(cs, active_floor_freq);
gov_show_one(cs, powersave);

gov_attr_rw(sampling_rate);
gov_attr_rw(sampling_down_factor);
gov_attr_rw(up_threshold);
gov_attr_rw(ignore_nice_load);
gov_attr_rw(down_differential);
gov_attr_rw(active_floor_freq);
gov_attr_rw(powersave);

static struct attribute *cs_attributes[] = {
	&sampling_rate.attr,
	&sampling_down_factor.attr,
	&up_threshold.attr,
	&down_differential.attr,
	&ignore_nice_load.attr,
	&active_floor_freq.attr,
	&powersave.attr,
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

	tuners->down_differential = 20;
	tuners->active_floor_freq = 960000;
	tuners->powersave = 0;
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
	dbs_info->prev_load = 0;
	dbs_info->requested_freq = policy->cur;
}

static struct dbs_governor cs_governor = {
	.gov = CPUFREQ_DBS_GOVERNOR_INITIALIZER("elementalx"),
	.kobj_type = { .default_attrs = cs_attributes },
	.gov_dbs_update = cs_dbs_update,
	.alloc = cs_alloc,
	.free = cs_free,
	.init = cs_init,
	.exit = cs_exit,
	.start = cs_start,
};

#define CPU_FREQ_GOV_ELEMENTALX	(&cs_governor.gov)

static int __init cpufreq_gov_dbs_init(void)
{
	return cpufreq_register_governor(CPU_FREQ_GOV_ELEMENTALX);
}

static void __exit cpufreq_gov_dbs_exit(void)
{
	cpufreq_unregister_governor(CPU_FREQ_GOV_ELEMENTALX);
}

MODULE_AUTHOR("Alexander Clouter <alex@digriz.org.uk>");
MODULE_AUTHOR("Aaron Segaert");
MODULE_DESCRIPTION("SM8150 elementalx governor port");
MODULE_LICENSE("GPL");

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_ELEMENTALX
struct cpufreq_governor *cpufreq_default_governor(void)
{
	return CPU_FREQ_GOV_ELEMENTALX;
}

fs_initcall(cpufreq_gov_dbs_init);
#else
module_init(cpufreq_gov_dbs_init);
#endif
module_exit(cpufreq_gov_dbs_exit);
