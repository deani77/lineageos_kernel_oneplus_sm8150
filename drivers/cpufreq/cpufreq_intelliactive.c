// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2014 Paul Reioux
 * Copyright (C) 2010-2016 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Author: Mike Chan (mike@android.com)
 * Original intelliactive changes: Paul Reioux (2014)
 * SM8150 port uses the current 4.14 interactive lifecycle and load hooks.
 * See Documentation/cpu-freq/sm8150-legacy-governors.md for differences.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/cpufreq.h>
#include <linux/irq_work.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/rwsem.h>
#include <linux/sched/cpufreq.h>
#include <linux/sched/rt.h>
#include <linux/sched/task.h>
#include <linux/tick.h>
#include <linux/time.h>
#include <linux/timer.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <uapi/linux/sched/types.h>

#define CREATE_TRACE_POINTS
#include <trace/events/cpufreq_intelliactive.h>

#define gov_attr_ro(_name)						\
static struct governor_attr _name =					\
__ATTR(_name, 0444, show_##_name, NULL)

#define gov_attr_wo(_name)						\
static struct governor_attr _name =					\
__ATTR(_name, 0200, NULL, store_##_name)

#define gov_attr_rw(_name)						\
static struct governor_attr _name =					\
__ATTR(_name, 0644, show_##_name, store_##_name)

/* Separate instance required for each 'intelliactive' directory in sysfs */
struct intelliactive_tunables {
	struct gov_attr_set attr_set;

	/* Hi speed to bump to from lo speed when load burst (default max) */
	unsigned int hispeed_freq;

	/* Go to hi speed when CPU load at or above this value. */
#define DEFAULT_GO_HISPEED_LOAD 99
	unsigned long go_hispeed_load;

	/* Target load. Lower values result in higher CPU speeds. */
	spinlock_t target_loads_lock;
	unsigned int *target_loads;
	int ntarget_loads;

	/*
	 * The minimum amount of time to spend at a frequency before we can ramp
	 * down.
	 */
#define DEFAULT_MIN_SAMPLE_TIME (80 * USEC_PER_MSEC)
	unsigned long min_sample_time;

	/* The sample rate of the timer used to increase frequency */
	unsigned long sampling_rate;

	/*
	 * Wait this long before raising speed above hispeed, by default a
	 * single timer interval.
	 */
	spinlock_t above_hispeed_delay_lock;
	unsigned int *above_hispeed_delay;
	int nabove_hispeed_delay;

	/* Non-zero means indefinite speed boost active */
	int boost;
	/* Duration of a boot pulse in usecs */
	int boostpulse_duration;
	/* End time of boost pulse in ktime converted to usecs */
	u64 boostpulse_endtime;
	bool boosted;

	/*
	 * Max additional time to wait in idle, beyond sampling_rate, at speeds
	 * above minimum before wakeup to reduce speed, or -1 if unnecessary.
	 */
#define DEFAULT_TIMER_SLACK (4 * DEFAULT_SAMPLING_RATE)
	unsigned long timer_slack_delay;
	long timer_slack;
	bool io_is_busy;
	unsigned int two_phase_freq;
	unsigned int sampling_down_factor;
	bool input_boost;
};

/* Separate instance required for each 'struct cpufreq_policy' */
struct intelliactive_policy {
	struct cpufreq_policy *policy;
	struct intelliactive_tunables *tunables;
	struct list_head tunables_hook;
};

/* Separate instance required for each CPU */
struct intelliactive_cpu {
	struct update_util_data update_util;
	struct intelliactive_policy *ipolicy;

	struct irq_work irq_work;
	u64 last_sample_time;
	unsigned long next_sample_jiffies;
	bool work_in_progress;

	struct rw_semaphore enable_sem;
	struct timer_list slack_timer;

	spinlock_t load_lock; /* protects the next 4 fields */
	u64 time_in_idle;
	u64 time_in_idle_timestamp;
	u64 cputime_speedadj;
	u64 cputime_speedadj_timestamp;

	spinlock_t target_freq_lock; /*protects target freq */
	unsigned int target_freq;

	unsigned int floor_freq;
	unsigned int phase_count;
	u64 pol_floor_val_time; /* policy floor_validate_time */
	u64 loc_floor_val_time; /* per-cpu floor_validate_time */
	u64 pol_hispeed_val_time; /* policy hispeed_validate_time */
	u64 loc_hispeed_val_time; /* per-cpu hispeed_validate_time */
};

static DEFINE_PER_CPU(struct intelliactive_cpu, intelliactive_cpu);

/* Realtime thread handles frequency scaling */
static struct task_struct *speedchange_task;
static cpumask_t speedchange_cpumask;
static spinlock_t speedchange_cpumask_lock;

/* Target load. Lower values result in higher CPU speeds. */
#define DEFAULT_TARGET_LOAD 90
static unsigned int default_target_loads[] = {DEFAULT_TARGET_LOAD};

#define DEFAULT_SAMPLING_RATE (20 * USEC_PER_MSEC)
#define DEFAULT_ABOVE_HISPEED_DELAY DEFAULT_SAMPLING_RATE
static unsigned int default_above_hispeed_delay[] = {
	DEFAULT_ABOVE_HISPEED_DELAY
};

/* Iterate over intelliactive policies for tunables */
#define for_each_ipolicy(__ip)	\
	list_for_each_entry(__ip, &tunables->attr_set.policy_list, tunables_hook)

static struct intelliactive_tunables *global_tunables;
static DEFINE_MUTEX(global_tunables_lock);

static inline void update_slack_delay(struct intelliactive_tunables *tunables)
{
	tunables->timer_slack_delay = usecs_to_jiffies(
		max(tunables->timer_slack, 0L) + tunables->sampling_rate);
}

static bool timer_slack_required(struct intelliactive_cpu *icpu)
{
	struct intelliactive_policy *ipolicy = icpu->ipolicy;
	struct intelliactive_tunables *tunables = ipolicy->tunables;

	if (tunables->timer_slack < 0)
		return false;

	if (icpu->target_freq > ipolicy->policy->min)
		return true;

	return false;
}

static void gov_slack_timer_start(struct intelliactive_cpu *icpu, int cpu)
{
	struct intelliactive_tunables *tunables = icpu->ipolicy->tunables;

	icpu->slack_timer.expires = jiffies + tunables->timer_slack_delay;
	add_timer_on(&icpu->slack_timer, cpu);
}

static void gov_slack_timer_modify(struct intelliactive_cpu *icpu)
{
	struct intelliactive_tunables *tunables = icpu->ipolicy->tunables;

	mod_timer(&icpu->slack_timer, jiffies + tunables->timer_slack_delay);
}

static void slack_timer_resched(struct intelliactive_cpu *icpu, int cpu,
				bool modify)
{
	struct intelliactive_tunables *tunables = icpu->ipolicy->tunables;
	unsigned long flags;

	spin_lock_irqsave(&icpu->load_lock, flags);

	icpu->time_in_idle = get_cpu_idle_time(cpu,
					       &icpu->time_in_idle_timestamp,
					       tunables->io_is_busy);
	icpu->cputime_speedadj = 0;
	icpu->cputime_speedadj_timestamp = icpu->time_in_idle_timestamp;

	if (timer_slack_required(icpu)) {
		if (modify)
			gov_slack_timer_modify(icpu);
		else
			gov_slack_timer_start(icpu, cpu);
	}

	spin_unlock_irqrestore(&icpu->load_lock, flags);
}

static unsigned int
freq_to_above_hispeed_delay(struct intelliactive_tunables *tunables,
			    unsigned int freq)
{
	unsigned long flags;
	unsigned int ret;
	int i;

	spin_lock_irqsave(&tunables->above_hispeed_delay_lock, flags);

	for (i = 0; i < tunables->nabove_hispeed_delay - 1 &&
	     freq >= tunables->above_hispeed_delay[i + 1]; i += 2)
		;

	ret = tunables->above_hispeed_delay[i];
	spin_unlock_irqrestore(&tunables->above_hispeed_delay_lock, flags);

	return ret;
}

static unsigned int freq_to_targetload(struct intelliactive_tunables *tunables,
				       unsigned int freq)
{
	unsigned long flags;
	unsigned int ret;
	int i;

	spin_lock_irqsave(&tunables->target_loads_lock, flags);

	for (i = 0; i < tunables->ntarget_loads - 1 &&
	     freq >= tunables->target_loads[i + 1]; i += 2)
		;

	ret = tunables->target_loads[i];
	spin_unlock_irqrestore(&tunables->target_loads_lock, flags);
	return ret;
}

/*
 * If increasing frequencies never map to a lower target load then
 * choose_freq() will find the minimum frequency that does not exceed its
 * target load given the current load.
 */
static unsigned int choose_freq(struct intelliactive_cpu *icpu,
				unsigned int loadadjfreq)
{
	struct cpufreq_policy *policy = icpu->ipolicy->policy;
	struct cpufreq_frequency_table *freq_table = policy->freq_table;
	unsigned int prevfreq, freqmin = 0, freqmax = UINT_MAX, tl;
	unsigned int freq = policy->cur;
	int index;

	do {
		prevfreq = freq;
		tl = freq_to_targetload(icpu->ipolicy->tunables, freq);

		/*
		 * Find the lowest frequency where the computed load is less
		 * than or equal to the target load.
		 */

		index = cpufreq_frequency_table_target(policy, loadadjfreq / tl,
						       CPUFREQ_RELATION_L);

		freq = freq_table[index].frequency;

		if (freq > prevfreq) {
			/* The previous frequency is too low */
			freqmin = prevfreq;

			if (freq < freqmax)
				continue;

			/* Find highest frequency that is less than freqmax */
			index = cpufreq_frequency_table_target(policy,
					freqmax - 1, CPUFREQ_RELATION_H);

			freq = freq_table[index].frequency;

			if (freq == freqmin) {
				/*
				 * The first frequency below freqmax has already
				 * been found to be too low. freqmax is the
				 * lowest speed we found that is fast enough.
				 */
				freq = freqmax;
				break;
			}
		} else if (freq < prevfreq) {
			/* The previous frequency is high enough. */
			freqmax = prevfreq;

			if (freq > freqmin)
				continue;

			/* Find lowest frequency that is higher than freqmin */
			index = cpufreq_frequency_table_target(policy,
					freqmin + 1, CPUFREQ_RELATION_L);

			freq = freq_table[index].frequency;

			/*
			 * If freqmax is the first frequency above
			 * freqmin then we have already found that
			 * this speed is fast enough.
			 */
			if (freq == freqmax)
				break;
		}

		/* If same frequency chosen as previous then done. */
	} while (freq != prevfreq);

	return freq;
}

static u64 update_load(struct intelliactive_cpu *icpu, int cpu)
{
	struct intelliactive_tunables *tunables = icpu->ipolicy->tunables;
	u64 now_idle, now, active_time, delta_idle, delta_time;

	now_idle = get_cpu_idle_time(cpu, &now, tunables->io_is_busy);
	delta_idle = (now_idle - icpu->time_in_idle);
	delta_time = (now - icpu->time_in_idle_timestamp);

	if (delta_time <= delta_idle)
		active_time = 0;
	else
		active_time = delta_time - delta_idle;

	icpu->cputime_speedadj += active_time * icpu->ipolicy->policy->cur;

	icpu->time_in_idle = now_idle;
	icpu->time_in_idle_timestamp = now;

	return now;
}

/* Re-evaluate load to see if a frequency change is required or not */
static void eval_target_freq(struct intelliactive_cpu *icpu)
{
	struct intelliactive_tunables *tunables = icpu->ipolicy->tunables;
	struct cpufreq_policy *policy = icpu->ipolicy->policy;
	struct cpufreq_frequency_table *freq_table = policy->freq_table;
	u64 cputime_speedadj, now, max_fvtime;
	unsigned int new_freq, loadadjfreq, index, delta_time;
	u64 min_sample_time;
	unsigned long flags;
	int cpu_load;
	int cpu = smp_processor_id();

	spin_lock_irqsave(&icpu->load_lock, flags);
	now = update_load(icpu, smp_processor_id());
	delta_time = (unsigned int)(now - icpu->cputime_speedadj_timestamp);
	cputime_speedadj = icpu->cputime_speedadj;
	spin_unlock_irqrestore(&icpu->load_lock, flags);

	if (WARN_ON_ONCE(!delta_time))
		return;

	spin_lock_irqsave(&icpu->target_freq_lock, flags);
	do_div(cputime_speedadj, delta_time);
	loadadjfreq = (unsigned int)cputime_speedadj * 100;
	cpu_load = loadadjfreq / max(policy->cur, 1U);
	tunables->boosted = tunables->boost ||
			    now < tunables->boostpulse_endtime;

	if (cpu_load >= tunables->go_hispeed_load || tunables->boosted) {
		if (policy->cur < tunables->hispeed_freq) {
			/* Two evaluations at the intermediate OPP, then hispeed.
			 * Per-CPU state avoids coupling the three SM8150 policies.
			 */
			if (!tunables->boosted && tunables->two_phase_freq &&
			    icpu->phase_count++ < 2)
				new_freq = min(tunables->two_phase_freq,
					       tunables->hispeed_freq);
			else
				new_freq = tunables->hispeed_freq;
		} else {
			new_freq = choose_freq(icpu, loadadjfreq);

			if (new_freq < tunables->hispeed_freq)
				new_freq = tunables->hispeed_freq;
		}
	} else {
		icpu->phase_count = 0;
		new_freq = choose_freq(icpu, loadadjfreq);
		if (new_freq > tunables->hispeed_freq &&
		    policy->cur < tunables->hispeed_freq)
			new_freq = tunables->hispeed_freq;
	}

	if (policy->cur >= tunables->hispeed_freq &&
	    new_freq > policy->cur &&
	    now - icpu->pol_hispeed_val_time < freq_to_above_hispeed_delay(tunables, policy->cur)) {
		trace_cpufreq_intelliactive_notyet(cpu, cpu_load,
				icpu->target_freq, policy->cur, new_freq);
		goto exit;
	}

	icpu->loc_hispeed_val_time = now;

	index = cpufreq_frequency_table_target(policy, new_freq,
					       CPUFREQ_RELATION_L);
	new_freq = freq_table[index].frequency;

	/*
	 * Do not scale below floor_freq unless we have been at or above the
	 * floor frequency for the minimum sample time since last validated.
	 */
	min_sample_time = tunables->min_sample_time;
	if (policy->cur == policy->max)
		min_sample_time *= tunables->sampling_down_factor;
	max_fvtime = max(icpu->pol_floor_val_time, icpu->loc_floor_val_time);
	if (new_freq < icpu->floor_freq && icpu->target_freq >= policy->cur) {
		if (now - max_fvtime < min_sample_time) {
			trace_cpufreq_intelliactive_notyet(cpu, cpu_load,
				icpu->target_freq, policy->cur, new_freq);
			goto exit;
		}
	}

	/*
	 * Update the timestamp for checking whether speed has been held at
	 * or above the selected frequency for a minimum of min_sample_time,
	 * if not boosted to hispeed_freq.  If boosted to hispeed_freq then we
	 * allow the speed to drop as soon as the boostpulse duration expires
	 * (or the indefinite boost is turned off).
	 */

	if (!tunables->boosted || new_freq > tunables->hispeed_freq) {
		icpu->floor_freq = new_freq;
		if (icpu->target_freq >= policy->cur || new_freq >= policy->cur)
			icpu->loc_floor_val_time = now;
	}

	if (icpu->target_freq == new_freq &&
	    icpu->target_freq <= policy->cur) {
		trace_cpufreq_intelliactive_already(cpu, cpu_load,
			icpu->target_freq, policy->cur, new_freq);
		goto exit;
	}

	trace_cpufreq_intelliactive_target(cpu, cpu_load, icpu->target_freq,
					 policy->cur, new_freq);

	icpu->target_freq = new_freq;
	spin_unlock_irqrestore(&icpu->target_freq_lock, flags);

	spin_lock_irqsave(&speedchange_cpumask_lock, flags);
	cpumask_set_cpu(cpu, &speedchange_cpumask);
	spin_unlock_irqrestore(&speedchange_cpumask_lock, flags);

	wake_up_process(speedchange_task);
	return;

exit:
	spin_unlock_irqrestore(&icpu->target_freq_lock, flags);
}

static void cpufreq_intelliactive_update(struct intelliactive_cpu *icpu)
{
	eval_target_freq(icpu);
	slack_timer_resched(icpu, smp_processor_id(), true);
}

static void cpufreq_intelliactive_idle_end(void)
{
	struct intelliactive_cpu *icpu = &per_cpu(intelliactive_cpu,
						smp_processor_id());

	if (!down_read_trylock(&icpu->enable_sem))
		return;

	if (icpu->ipolicy) {
		/*
		 * We haven't sampled load for more than sampling_rate time, do
		 * it right now.
		 */
		if (time_after_eq(jiffies, icpu->next_sample_jiffies))
			cpufreq_intelliactive_update(icpu);
	}

	up_read(&icpu->enable_sem);
}

static void cpufreq_intelliactive_get_policy_info(struct cpufreq_policy *policy,
						unsigned int *pmax_freq,
						u64 *phvt, u64 *pfvt)
{
	struct intelliactive_cpu *icpu;
	u64 hvt = ~0ULL, fvt = 0;
	unsigned int max_freq = 0, i;

	for_each_cpu(i, policy->cpus) {
		icpu = &per_cpu(intelliactive_cpu, i);

		fvt = max(fvt, icpu->loc_floor_val_time);
		if (icpu->target_freq > max_freq) {
			max_freq = icpu->target_freq;
			hvt = icpu->loc_hispeed_val_time;
		} else if (icpu->target_freq == max_freq) {
			hvt = min(hvt, icpu->loc_hispeed_val_time);
		}
	}

	*pmax_freq = max_freq;
	*phvt = hvt;
	*pfvt = fvt;
}

static void cpufreq_intelliactive_adjust_cpu(unsigned int cpu,
					   struct cpufreq_policy *policy)
{
	struct intelliactive_cpu *icpu;
	u64 hvt, fvt;
	unsigned int max_freq;
	int i;

	cpufreq_intelliactive_get_policy_info(policy, &max_freq, &hvt, &fvt);

	for_each_cpu(i, policy->cpus) {
		icpu = &per_cpu(intelliactive_cpu, i);
		icpu->pol_floor_val_time = fvt;
	}

	if (max_freq != policy->cur) {
		__cpufreq_driver_target(policy, max_freq, CPUFREQ_RELATION_H);
		for_each_cpu(i, policy->cpus) {
			icpu = &per_cpu(intelliactive_cpu, i);
			icpu->pol_hispeed_val_time = hvt;
		}
	}

	trace_cpufreq_intelliactive_setspeed(cpu, max_freq, policy->cur);
}

static int cpufreq_intelliactive_speedchange_task(void *data)
{
	unsigned int cpu;
	cpumask_t tmp_mask;
	unsigned long flags;

again:
	set_current_state(TASK_INTERRUPTIBLE);
	spin_lock_irqsave(&speedchange_cpumask_lock, flags);

	if (cpumask_empty(&speedchange_cpumask)) {
		spin_unlock_irqrestore(&speedchange_cpumask_lock, flags);
		schedule();

		if (kthread_should_stop())
			return 0;

		spin_lock_irqsave(&speedchange_cpumask_lock, flags);
	}

	set_current_state(TASK_RUNNING);
	tmp_mask = speedchange_cpumask;
	cpumask_clear(&speedchange_cpumask);
	spin_unlock_irqrestore(&speedchange_cpumask_lock, flags);

	for_each_cpu(cpu, &tmp_mask) {
		struct intelliactive_cpu *icpu = &per_cpu(intelliactive_cpu, cpu);
		struct cpufreq_policy *policy;

		if (unlikely(!down_read_trylock(&icpu->enable_sem)))
			continue;

		if (likely(icpu->ipolicy)) {
			policy = icpu->ipolicy->policy;
			cpufreq_intelliactive_adjust_cpu(cpu, policy);
		}

		up_read(&icpu->enable_sem);
	}

	goto again;
}

static void cpufreq_intelliactive_boost(struct intelliactive_tunables *tunables)
{
	struct intelliactive_policy *ipolicy;
	struct cpufreq_policy *policy;
	struct intelliactive_cpu *icpu;
	unsigned long flags[2];
	bool wakeup = false;
	int i;

	tunables->boosted = true;

	spin_lock_irqsave(&speedchange_cpumask_lock, flags[0]);

	for_each_ipolicy(ipolicy) {
		policy = ipolicy->policy;

		for_each_cpu(i, policy->cpus) {
			icpu = &per_cpu(intelliactive_cpu, i);

			if (!down_read_trylock(&icpu->enable_sem))
				continue;

			if (!icpu->ipolicy) {
				up_read(&icpu->enable_sem);
				continue;
			}

			spin_lock_irqsave(&icpu->target_freq_lock, flags[1]);
			if (icpu->target_freq < tunables->hispeed_freq) {
				icpu->target_freq = clamp(tunables->hispeed_freq,
						 policy->min, policy->max);
				cpumask_set_cpu(i, &speedchange_cpumask);
				icpu->pol_hispeed_val_time = ktime_to_us(ktime_get());
				wakeup = true;
			}
			spin_unlock_irqrestore(&icpu->target_freq_lock, flags[1]);

			up_read(&icpu->enable_sem);
		}
	}

	spin_unlock_irqrestore(&speedchange_cpumask_lock, flags[0]);

	if (wakeup)
		wake_up_process(speedchange_task);
}

static int cpufreq_intelliactive_notifier(struct notifier_block *nb,
					unsigned long val, void *data)
{
	struct cpufreq_freqs *freq = data;
	struct intelliactive_cpu *icpu = &per_cpu(intelliactive_cpu, freq->cpu);
	unsigned long flags;

	if (val != CPUFREQ_POSTCHANGE)
		return 0;

	if (!down_read_trylock(&icpu->enable_sem))
		return 0;

	if (!icpu->ipolicy) {
		up_read(&icpu->enable_sem);
		return 0;
	}

	spin_lock_irqsave(&icpu->load_lock, flags);
	update_load(icpu, freq->cpu);
	spin_unlock_irqrestore(&icpu->load_lock, flags);

	up_read(&icpu->enable_sem);

	return 0;
}

static struct notifier_block cpufreq_notifier_block = {
	.notifier_call = cpufreq_intelliactive_notifier,
};

static unsigned int *get_tokenized_data(const char *buf, int *num_tokens)
{
	char *copy, *cursor, *token;
	unsigned int *data;
	unsigned int count = 0;

	copy = kstrdup(buf, GFP_KERNEL);
	if (!copy)
		return ERR_PTR(-ENOMEM);
	data = kcalloc(strlen(buf) + 1, sizeof(*data), GFP_KERNEL);
	if (!data) {
		kfree(copy);
		return ERR_PTR(-ENOMEM);
	}
	cursor = strim(copy);
	while ((token = strsep(&cursor, " :"))) {
		if (!*token || kstrtouint(token, 0, &data[count]))
			goto invalid;
		count++;
	}
	if (!(count & 1))
		goto invalid;
	*num_tokens = count;
	kfree(copy);
	return data;
 invalid:
	kfree(copy);
	kfree(data);
	return ERR_PTR(-EINVAL);
}

/* Interactive governor sysfs interface */
static struct intelliactive_tunables *to_tunables(struct gov_attr_set *attr_set)
{
	return container_of(attr_set, struct intelliactive_tunables, attr_set);
}

#define show_one(file_name, type)					\
static ssize_t show_##file_name(struct gov_attr_set *attr_set, char *buf) \
{									\
	struct intelliactive_tunables *tunables = to_tunables(attr_set);	\
	return sprintf(buf, type "\n", tunables->file_name);		\
}

static ssize_t show_target_loads(struct gov_attr_set *attr_set, char *buf)
{
	struct intelliactive_tunables *tunables = to_tunables(attr_set);
	unsigned long flags;
	ssize_t ret = 0;
	int i;

	spin_lock_irqsave(&tunables->target_loads_lock, flags);

	for (i = 0; i < tunables->ntarget_loads; i++)
		ret += sprintf(buf + ret, "%u%s", tunables->target_loads[i],
			       i & 0x1 ? ":" : " ");

	sprintf(buf + ret - 1, "\n");
	spin_unlock_irqrestore(&tunables->target_loads_lock, flags);

	return ret;
}

static ssize_t store_target_loads(struct gov_attr_set *attr_set,
				  const char *buf, size_t count)
{
	struct intelliactive_tunables *tunables = to_tunables(attr_set);
	unsigned int *new_target_loads;
	int i;
	unsigned long flags;
	int ntokens;

	new_target_loads = get_tokenized_data(buf, &ntokens);
	if (IS_ERR(new_target_loads))
		return PTR_ERR(new_target_loads);

	if (ntokens > 63) {
		kfree(new_target_loads);
		return -EINVAL;
	}
	for (i = 0; i < ntokens; i += 2) {
		if (!new_target_loads[i] || new_target_loads[i] > 100 ||
		    (i >= 3 && new_target_loads[i - 1] <= new_target_loads[i - 3])) {
			kfree(new_target_loads);
			return -EINVAL;
		}
	}
	spin_lock_irqsave(&tunables->target_loads_lock, flags);
	if (tunables->target_loads != default_target_loads)
		kfree(tunables->target_loads);
	tunables->target_loads = new_target_loads;
	tunables->ntarget_loads = ntokens;
	spin_unlock_irqrestore(&tunables->target_loads_lock, flags);

	return count;
}

static ssize_t show_above_hispeed_delay(struct gov_attr_set *attr_set,
					char *buf)
{
	struct intelliactive_tunables *tunables = to_tunables(attr_set);
	unsigned long flags;
	ssize_t ret = 0;
	int i;

	spin_lock_irqsave(&tunables->above_hispeed_delay_lock, flags);

	for (i = 0; i < tunables->nabove_hispeed_delay; i++)
		ret += sprintf(buf + ret, "%u%s",
			       tunables->above_hispeed_delay[i],
			       i & 0x1 ? ":" : " ");

	sprintf(buf + ret - 1, "\n");
	spin_unlock_irqrestore(&tunables->above_hispeed_delay_lock, flags);

	return ret;
}

static ssize_t store_above_hispeed_delay(struct gov_attr_set *attr_set,
					 const char *buf, size_t count)
{
	struct intelliactive_tunables *tunables = to_tunables(attr_set);
	unsigned int *new_above_hispeed_delay = NULL;
	unsigned long flags;
	int ntokens, i;

	new_above_hispeed_delay = get_tokenized_data(buf, &ntokens);
	if (IS_ERR(new_above_hispeed_delay))
		return PTR_ERR(new_above_hispeed_delay);

	if (ntokens > 63) {
		kfree(new_above_hispeed_delay);
		return -EINVAL;
	}
	for (i = 0; i < ntokens; i += 2) {
		if (new_above_hispeed_delay[i] > 1000000 ||
		    (i >= 3 && new_above_hispeed_delay[i - 1] <=
		     new_above_hispeed_delay[i - 3])) {
			kfree(new_above_hispeed_delay);
			return -EINVAL;
		}
	}
	spin_lock_irqsave(&tunables->above_hispeed_delay_lock, flags);
	if (tunables->above_hispeed_delay != default_above_hispeed_delay)
		kfree(tunables->above_hispeed_delay);
	tunables->above_hispeed_delay = new_above_hispeed_delay;
	tunables->nabove_hispeed_delay = ntokens;
	spin_unlock_irqrestore(&tunables->above_hispeed_delay_lock, flags);

	return count;
}

static ssize_t store_hispeed_freq(struct gov_attr_set *attr_set,
				  const char *buf, size_t count)
{
	struct intelliactive_tunables *tunables = to_tunables(attr_set);
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	tunables->hispeed_freq = val;

	return count;
}

static ssize_t store_go_hispeed_load(struct gov_attr_set *attr_set,
				     const char *buf, size_t count)
{
	struct intelliactive_tunables *tunables = to_tunables(attr_set);
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	if (!val || val > 100)
		return -EINVAL;

	tunables->go_hispeed_load = val;

	return count;
}

static ssize_t store_min_sample_time(struct gov_attr_set *attr_set,
				     const char *buf, size_t count)
{
	struct intelliactive_tunables *tunables = to_tunables(attr_set);
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	if (val > 1000000)
		return -EINVAL;

	tunables->min_sample_time = val;

	return count;
}

static ssize_t show_timer_rate(struct gov_attr_set *attr_set, char *buf)
{
	struct intelliactive_tunables *tunables = to_tunables(attr_set);

	return sprintf(buf, "%lu\n", tunables->sampling_rate);
}

static ssize_t store_timer_rate(struct gov_attr_set *attr_set, const char *buf,
				size_t count)
{
	struct intelliactive_tunables *tunables = to_tunables(attr_set);
	unsigned long val, val_round;
	int ret;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	if (val < 1000 || val > 1000000)
		return -EINVAL;

	val_round = jiffies_to_usecs(usecs_to_jiffies(val));
	if (val != val_round)
		pr_warn("timer_rate not aligned to jiffy. Rounded up to %lu\n",
			val_round);

	tunables->sampling_rate = val_round;
	update_slack_delay(tunables);

	return count;
}

static ssize_t store_timer_slack(struct gov_attr_set *attr_set, const char *buf,
				 size_t count)
{
	struct intelliactive_tunables *tunables = to_tunables(attr_set);
	long val;
	int ret;

	ret = kstrtol(buf, 10, &val);
	if (ret < 0)
		return ret;
	if (val < -1 || val > 1000000)
		return -EINVAL;

	tunables->timer_slack = val;
	update_slack_delay(tunables);

	return count;
}

static ssize_t store_boost(struct gov_attr_set *attr_set, const char *buf,
			   size_t count)
{
	struct intelliactive_tunables *tunables = to_tunables(attr_set);
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	tunables->boost = val;

	if (tunables->boost) {
		trace_cpufreq_intelliactive_boost("on");
		if (!tunables->boosted)
			cpufreq_intelliactive_boost(tunables);
	} else {
		tunables->boostpulse_endtime = ktime_to_us(ktime_get());
		trace_cpufreq_intelliactive_unboost("off");
	}

	return count;
}

static ssize_t store_boostpulse(struct gov_attr_set *attr_set, const char *buf,
				size_t count)
{
	struct intelliactive_tunables *tunables = to_tunables(attr_set);
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	tunables->boostpulse_endtime = ktime_to_us(ktime_get()) +
					tunables->boostpulse_duration;
	trace_cpufreq_intelliactive_boost("pulse");
	if (!tunables->boosted)
		cpufreq_intelliactive_boost(tunables);

	return count;
}

static ssize_t store_boostpulse_duration(struct gov_attr_set *attr_set,
					 const char *buf, size_t count)
{
	struct intelliactive_tunables *tunables = to_tunables(attr_set);
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	if (val > 1000000)
		return -EINVAL;

	tunables->boostpulse_duration = val;

	return count;
}

static ssize_t store_io_is_busy(struct gov_attr_set *attr_set, const char *buf,
				size_t count)
{
	struct intelliactive_tunables *tunables = to_tunables(attr_set);
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	tunables->io_is_busy = val;

	return count;
}

static ssize_t store_two_phase_freq(struct gov_attr_set *attr_set,
				    const char *buf, size_t count)
{
	struct intelliactive_tunables *tunables = to_tunables(attr_set);
	unsigned int value;

	if (kstrtouint(buf, 0, &value))
		return -EINVAL;
	tunables->two_phase_freq = value;
	return count;
}

static ssize_t store_sampling_down_factor(struct gov_attr_set *attr_set,
					 const char *buf, size_t count)
{
	struct intelliactive_tunables *tunables = to_tunables(attr_set);
	unsigned int value;

	if (kstrtouint(buf, 0, &value) || value < 1 || value > 20)
		return -EINVAL;
	tunables->sampling_down_factor = value;
	return count;
}

static ssize_t store_input_boost(struct gov_attr_set *attr_set,
				const char *buf, size_t count)
{
	struct intelliactive_tunables *tunables = to_tunables(attr_set);
	bool value;

	if (kstrtobool(buf, &value))
		return -EINVAL;
	tunables->input_boost = value;
	return count;
}

show_one(two_phase_freq, "%u");
show_one(sampling_down_factor, "%u");
show_one(input_boost, "%u");
gov_attr_rw(two_phase_freq);
gov_attr_rw(sampling_down_factor);
gov_attr_rw(input_boost);

show_one(hispeed_freq, "%u");
show_one(go_hispeed_load, "%lu");
show_one(min_sample_time, "%lu");
show_one(timer_slack, "%ld");
show_one(boost, "%u");
show_one(boostpulse_duration, "%u");
show_one(io_is_busy, "%u");

gov_attr_rw(target_loads);
gov_attr_rw(above_hispeed_delay);
gov_attr_rw(hispeed_freq);
gov_attr_rw(go_hispeed_load);
gov_attr_rw(min_sample_time);
gov_attr_rw(timer_rate);
gov_attr_rw(timer_slack);
gov_attr_rw(boost);
gov_attr_wo(boostpulse);
gov_attr_rw(boostpulse_duration);
gov_attr_rw(io_is_busy);

static struct attribute *intelliactive_attributes[] = {
	&target_loads.attr,
	&above_hispeed_delay.attr,
	&hispeed_freq.attr,
	&go_hispeed_load.attr,
	&min_sample_time.attr,
	&timer_rate.attr,
	&timer_slack.attr,
	&boost.attr,
	&boostpulse.attr,
	&boostpulse_duration.attr,
	&io_is_busy.attr,
	&two_phase_freq.attr,
	&sampling_down_factor.attr,
	&input_boost.attr,
	NULL
};

static void intelliactive_tunables_release(struct kobject *kobj)
{
	struct gov_attr_set *attr_set = container_of(kobj, struct gov_attr_set, kobj);
	struct intelliactive_tunables *tunables = to_tunables(attr_set);

	if (tunables->target_loads != default_target_loads)
		kfree(tunables->target_loads);
	if (tunables->above_hispeed_delay != default_above_hispeed_delay)
		kfree(tunables->above_hispeed_delay);
	kfree(tunables);
}

static struct kobj_type intelliactive_tunables_ktype = {
	.release = intelliactive_tunables_release,
	.default_attrs = intelliactive_attributes,
	.sysfs_ops = &governor_sysfs_ops,
};

static int cpufreq_intelliactive_idle_notifier(struct notifier_block *nb,
					     unsigned long val, void *data)
{
	if (val == IDLE_END)
		cpufreq_intelliactive_idle_end();

	return 0;
}

static struct notifier_block cpufreq_intelliactive_idle_nb = {
	.notifier_call = cpufreq_intelliactive_idle_notifier,
};

/* Interactive Governor callbacks */
struct intelliactive_governor {
	struct cpufreq_governor gov;
	unsigned int usage_count;
};

static struct intelliactive_governor intelliactive_gov;

#define CPU_FREQ_GOV_INTELLIACTIVE	(&intelliactive_gov.gov)

static void irq_work(struct irq_work *irq_work)
{
	struct intelliactive_cpu *icpu = container_of(irq_work, struct
						    intelliactive_cpu, irq_work);

	cpufreq_intelliactive_update(icpu);
	icpu->work_in_progress = false;
}

static void update_util_handler(struct update_util_data *data, u64 time,
				unsigned int flags)
{
	struct intelliactive_cpu *icpu = container_of(data,
					struct intelliactive_cpu, update_util);
	struct intelliactive_policy *ipolicy = icpu->ipolicy;
	struct intelliactive_tunables *tunables = ipolicy->tunables;
	u64 delta_ns;

	/*
	 * The irq-work may not be allowed to be queued up right now.
	 * Possible reasons:
	 * - Work has already been queued up or is in progress.
	 * - It is too early (too little time from the previous sample).
	 */
	if (icpu->work_in_progress)
		return;

	delta_ns = time - icpu->last_sample_time;
	if ((s64)delta_ns < tunables->sampling_rate * NSEC_PER_USEC)
		return;

	icpu->last_sample_time = time;
	icpu->next_sample_jiffies = usecs_to_jiffies(tunables->sampling_rate) +
				    jiffies;

	icpu->work_in_progress = true;
	irq_work_queue(&icpu->irq_work);
}

static void gov_set_update_util(struct intelliactive_policy *ipolicy)
{
	struct cpufreq_policy *policy = ipolicy->policy;
	struct intelliactive_cpu *icpu;
	int cpu;

	for_each_cpu(cpu, policy->cpus) {
		icpu = &per_cpu(intelliactive_cpu, cpu);

		icpu->last_sample_time = 0;
		icpu->next_sample_jiffies = 0;
		cpufreq_add_update_util_hook(cpu, &icpu->update_util,
					     update_util_handler);
	}
}

static inline void gov_clear_update_util(struct cpufreq_policy *policy)
{
	int i;

	for_each_cpu(i, policy->cpus)
		cpufreq_remove_update_util_hook(i);

	synchronize_sched();
}

static void icpu_cancel_work(struct intelliactive_cpu *icpu)
{
	irq_work_sync(&icpu->irq_work);
	icpu->work_in_progress = false;
	del_timer_sync(&icpu->slack_timer);
}

static struct intelliactive_policy *
intelliactive_policy_alloc(struct cpufreq_policy *policy)
{
	struct intelliactive_policy *ipolicy;

	ipolicy = kzalloc(sizeof(*ipolicy), GFP_KERNEL);
	if (!ipolicy)
		return NULL;

	ipolicy->policy = policy;

	return ipolicy;
}

static void intelliactive_policy_free(struct intelliactive_policy *ipolicy)
{
	kfree(ipolicy);
}

static struct intelliactive_tunables *
intelliactive_tunables_alloc(struct intelliactive_policy *ipolicy)
{
	struct intelliactive_tunables *tunables;

	tunables = kzalloc(sizeof(*tunables), GFP_KERNEL);
	if (!tunables)
		return NULL;

	gov_attr_set_init(&tunables->attr_set, &ipolicy->tunables_hook);
	if (!have_governor_per_policy())
		global_tunables = tunables;

	ipolicy->tunables = tunables;

	return tunables;
}

static void intelliactive_tunables_free(struct intelliactive_tunables *tunables)
{
	if (!have_governor_per_policy())
		global_tunables = NULL;
	kobject_put(&tunables->attr_set.kobj);
}

static int cpufreq_intelliactive_init(struct cpufreq_policy *policy)
{
	struct intelliactive_policy *ipolicy;
	struct intelliactive_tunables *tunables;
	int ret;

	/* State should be equivalent to EXIT */
	if (policy->governor_data)
		return -EBUSY;

	ipolicy = intelliactive_policy_alloc(policy);
	if (!ipolicy)
		return -ENOMEM;

	mutex_lock(&global_tunables_lock);

	if (global_tunables) {
		if (WARN_ON(have_governor_per_policy())) {
			ret = -EINVAL;
			goto free_int_policy;
		}

		policy->governor_data = ipolicy;
		ipolicy->tunables = global_tunables;

		gov_attr_set_get(&global_tunables->attr_set,
				 &ipolicy->tunables_hook);
		goto out;
	}

	tunables = intelliactive_tunables_alloc(ipolicy);
	if (!tunables) {
		ret = -ENOMEM;
		goto free_int_policy;
	}

	tunables->hispeed_freq = policy->max;
	tunables->two_phase_freq = policy->min +
		(policy->max - policy->min) / 2;
	tunables->sampling_down_factor = 1;
	tunables->input_boost = true;
	tunables->io_is_busy = true;
	tunables->above_hispeed_delay = default_above_hispeed_delay;
	tunables->nabove_hispeed_delay =
		ARRAY_SIZE(default_above_hispeed_delay);
	tunables->go_hispeed_load = DEFAULT_GO_HISPEED_LOAD;
	tunables->target_loads = default_target_loads;
	tunables->ntarget_loads = ARRAY_SIZE(default_target_loads);
	tunables->min_sample_time = DEFAULT_MIN_SAMPLE_TIME;
	tunables->boostpulse_duration = DEFAULT_MIN_SAMPLE_TIME;
	tunables->sampling_rate = DEFAULT_SAMPLING_RATE;
	tunables->timer_slack = DEFAULT_TIMER_SLACK;
	update_slack_delay(tunables);

	spin_lock_init(&tunables->target_loads_lock);
	spin_lock_init(&tunables->above_hispeed_delay_lock);

	policy->governor_data = ipolicy;

	ret = kobject_init_and_add(&tunables->attr_set.kobj,
				   &intelliactive_tunables_ktype,
				   get_governor_parent_kobj(policy), "%s",
				   intelliactive_gov.gov.name);
	if (ret)
		goto fail;

	/* One time initialization for governor */
	if (!intelliactive_gov.usage_count++) {
		idle_notifier_register(&cpufreq_intelliactive_idle_nb);
		cpufreq_register_notifier(&cpufreq_notifier_block,
					  CPUFREQ_TRANSITION_NOTIFIER);
	}

 out:
	mutex_unlock(&global_tunables_lock);
	return 0;

 fail:
	policy->governor_data = NULL;
	mutex_destroy(&tunables->attr_set.update_lock);
	intelliactive_tunables_free(tunables);

 free_int_policy:
	mutex_unlock(&global_tunables_lock);

	intelliactive_policy_free(ipolicy);
	pr_err("governor initialization failed (%d)\n", ret);

	return ret;
}

static void cpufreq_intelliactive_exit(struct cpufreq_policy *policy)
{
	struct intelliactive_policy *ipolicy = policy->governor_data;
	struct intelliactive_tunables *tunables = ipolicy->tunables;
	unsigned int count;

	mutex_lock(&global_tunables_lock);

	/* Last policy using the governor ? */
	if (!--intelliactive_gov.usage_count) {
		cpufreq_unregister_notifier(&cpufreq_notifier_block,
					    CPUFREQ_TRANSITION_NOTIFIER);
		idle_notifier_unregister(&cpufreq_intelliactive_idle_nb);
	}

	count = gov_attr_set_put(&tunables->attr_set, &ipolicy->tunables_hook);
	policy->governor_data = NULL;
	if (!count && !have_governor_per_policy())
		global_tunables = NULL;

	mutex_unlock(&global_tunables_lock);

	intelliactive_policy_free(ipolicy);
}

static int cpufreq_intelliactive_start(struct cpufreq_policy *policy)
{
	struct intelliactive_policy *ipolicy = policy->governor_data;
	struct intelliactive_cpu *icpu;
	unsigned int cpu;

	for_each_cpu(cpu, policy->cpus) {
		icpu = &per_cpu(intelliactive_cpu, cpu);

		icpu->target_freq = policy->cur;
		icpu->floor_freq = icpu->target_freq;
		icpu->phase_count = 0;
		icpu->pol_floor_val_time = ktime_to_us(ktime_get());
		icpu->loc_floor_val_time = icpu->pol_floor_val_time;
		icpu->pol_hispeed_val_time = icpu->pol_floor_val_time;
		icpu->loc_hispeed_val_time = icpu->pol_floor_val_time;

		down_write(&icpu->enable_sem);
		icpu->ipolicy = ipolicy;
		up_write(&icpu->enable_sem);

		slack_timer_resched(icpu, cpu, false);
	}

	gov_set_update_util(ipolicy);
	return 0;
}

static void cpufreq_intelliactive_stop(struct cpufreq_policy *policy)
{
	struct intelliactive_policy *ipolicy = policy->governor_data;
	struct intelliactive_cpu *icpu;
	unsigned int cpu;

	gov_clear_update_util(ipolicy->policy);

	for_each_cpu(cpu, policy->cpus) {
		icpu = &per_cpu(intelliactive_cpu, cpu);

		icpu_cancel_work(icpu);

		down_write(&icpu->enable_sem);
		icpu->ipolicy = NULL;
		up_write(&icpu->enable_sem);
	}
}

static void cpufreq_intelliactive_limits(struct cpufreq_policy *policy)
{
	struct intelliactive_cpu *icpu;
	unsigned int cpu;
	unsigned long flags;

	cpufreq_policy_apply_limits(policy);

	for_each_cpu(cpu, policy->cpus) {
		icpu = &per_cpu(intelliactive_cpu, cpu);

		spin_lock_irqsave(&icpu->target_freq_lock, flags);

		if (policy->max < icpu->target_freq)
			icpu->target_freq = policy->max;
		else if (policy->min > icpu->target_freq)
			icpu->target_freq = policy->min;

		spin_unlock_irqrestore(&icpu->target_freq_lock, flags);
	}
}

static struct intelliactive_governor intelliactive_gov = {
	.gov = {
		.name			= "intelliactive",
		.dynamic_switching	= true,
		.owner			= THIS_MODULE,
		.init			= cpufreq_intelliactive_init,
		.exit			= cpufreq_intelliactive_exit,
		.start			= cpufreq_intelliactive_start,
		.stop			= cpufreq_intelliactive_stop,
		.limits			= cpufreq_intelliactive_limits,
	}
};

static void cpufreq_intelliactive_nop_timer(unsigned long data)
{
	/*
	 * The purpose of slack-timer is to wake up the CPU from IDLE, in order
	 * to decrease its frequency if it is not set to minimum already.
	 *
	 * This is important for platforms where CPU with higher frequencies
	 * consume higher power even at IDLE.
	 */
}

static void intelliactive_input_workfn(struct work_struct *work)
{
	struct intelliactive_cpu *icpu;
	struct intelliactive_tunables *tunables;
	int cpu;

	mutex_lock(&global_tunables_lock);
	for_each_possible_cpu(cpu) {
		icpu = &per_cpu(intelliactive_cpu, cpu);
		if (!down_read_trylock(&icpu->enable_sem))
			continue;
		if (!icpu->ipolicy || icpu->ipolicy->policy->cpu != cpu)
			goto unlock;
		tunables = icpu->ipolicy->tunables;
		if (tunables->input_boost) {
			/* Serialize against sysfs updates and the policy list. */
			mutex_lock(&tunables->attr_set.update_lock);
			tunables->boostpulse_endtime = ktime_to_us(ktime_get()) +
				tunables->boostpulse_duration;
			cpufreq_intelliactive_boost(tunables);
			mutex_unlock(&tunables->attr_set.update_lock);
		}
 unlock:
		up_read(&icpu->enable_sem);
	}
	mutex_unlock(&global_tunables_lock);
}

static DECLARE_WORK(intelliactive_input_work, intelliactive_input_workfn);

static void intelliactive_input_event(struct input_handle *handle,
				    unsigned int type, unsigned int code,
				    int value)
{
	if (type == EV_SYN && code == SYN_REPORT)
		schedule_work(&intelliactive_input_work);
}

static int intelliactive_input_connect(struct input_handler *handler,
		struct input_dev *dev, const struct input_device_id *id)
{
	struct input_handle *handle;
	int ret;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;
	handle->dev = dev;
	handle->handler = handler;
	handle->name = "intelliactive";
	ret = input_register_handle(handle);
	if (ret)
		goto free_handle;
	ret = input_open_device(handle);
	if (ret)
		goto unregister;
	return 0;
 unregister:
	input_unregister_handle(handle);
 free_handle:
	kfree(handle);
	return ret;
}

static void intelliactive_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id intelliactive_input_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT | INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			BIT_MASK(ABS_MT_POSITION_X) | BIT_MASK(ABS_MT_POSITION_Y) },
	},
	{ },
};

static struct input_handler intelliactive_input_handler = {
	.event = intelliactive_input_event,
	.connect = intelliactive_input_connect,
	.disconnect = intelliactive_input_disconnect,
	.name = "intelliactive",
	.id_table = intelliactive_input_ids,
};

static int __init cpufreq_intelliactive_gov_init(void)
{
	struct sched_param param = { .sched_priority = MAX_RT_PRIO - 1 };
	struct intelliactive_cpu *icpu;
	unsigned int cpu;
	int ret;

	for_each_possible_cpu(cpu) {
		icpu = &per_cpu(intelliactive_cpu, cpu);

		init_irq_work(&icpu->irq_work, irq_work);
		spin_lock_init(&icpu->load_lock);
		spin_lock_init(&icpu->target_freq_lock);
		init_rwsem(&icpu->enable_sem);

		/* Initialize per-cpu slack-timer */
		init_timer_pinned(&icpu->slack_timer);
		icpu->slack_timer.function = cpufreq_intelliactive_nop_timer;
	}

	spin_lock_init(&speedchange_cpumask_lock);
	speedchange_task = kthread_create(cpufreq_intelliactive_speedchange_task,
					  NULL, "cfintelliactive");
	if (IS_ERR(speedchange_task))
		return PTR_ERR(speedchange_task);

	sched_setscheduler_nocheck(speedchange_task, SCHED_FIFO, &param);
	get_task_struct(speedchange_task);

	/* wake up so the thread does not look hung to the freezer */
	wake_up_process(speedchange_task);

	ret = cpufreq_register_governor(CPU_FREQ_GOV_INTELLIACTIVE);
	if (!ret) {
		ret = input_register_handler(&intelliactive_input_handler);
		if (ret)
			cpufreq_unregister_governor(CPU_FREQ_GOV_INTELLIACTIVE);
	}
	if (ret) {
		kthread_stop(speedchange_task);
		put_task_struct(speedchange_task);
	}
	return ret;
}

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_INTELLIACTIVE
struct cpufreq_governor *cpufreq_default_governor(void)
{
	return CPU_FREQ_GOV_INTELLIACTIVE;
}

fs_initcall(cpufreq_intelliactive_gov_init);
#else
module_init(cpufreq_intelliactive_gov_init);
#endif

static void __exit cpufreq_intelliactive_gov_exit(void)
{
	input_unregister_handler(&intelliactive_input_handler);
	cancel_work_sync(&intelliactive_input_work);
	cpufreq_unregister_governor(CPU_FREQ_GOV_INTELLIACTIVE);
	kthread_stop(speedchange_task);
	put_task_struct(speedchange_task);
}
module_exit(cpufreq_intelliactive_gov_exit);

MODULE_AUTHOR("Mike Chan <mike@android.com>");
MODULE_DESCRIPTION("'cpufreq_intelliactive' - A dynamic cpufreq governor for Latency sensitive workloads");
MODULE_LICENSE("GPL");
