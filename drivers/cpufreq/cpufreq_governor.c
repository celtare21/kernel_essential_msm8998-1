/*
 * drivers/cpufreq/cpufreq_governor.c
 *
 * CPUFREQ governors common code
 *
 * Copyright	(C) 2001 Russell King
 *		(C) 2003 Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>.
 *		(C) 2003 Jun Nakajima <jun.nakajima@intel.com>
 *		(C) 2009 Alexander Clouter <alex@digriz.org.uk>
 *		(c) 2012 Viresh Kumar <viresh.kumar@linaro.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/export.h>
#include <linux/kernel_stat.h>
#include <linux/slab.h>

#include "cpufreq_governor.h"

static DEFINE_PER_CPU(struct cpu_dbs_info, cpu_dbs);

static DEFINE_MUTEX(gov_dbs_data_mutex);

/* Common sysfs tunables */
/**
 * store_sampling_rate - update sampling rate effective immediately if needed.
 *
 * If new rate is smaller than the old, simply updating
 * dbs.sampling_rate might not be appropriate. For example, if the
 * original sampling_rate was 1 second and the requested new sampling rate is 10
 * ms because the user needs immediate reaction from ondemand governor, but not
 * sure if higher frequency will be required or not, then, the governor may
 * change the sampling rate too late; up to 1 second later. Thus, if we are
 * reducing the sampling rate, we need to make the new value effective
 * immediately.
 *
 * This must be called with dbs_data->mutex held, otherwise traversing
 * policy_dbs_list isn't safe.
 */
ssize_t store_sampling_rate(struct gov_attr_set *attr_set, const char *buf,
			    size_t count)
{
	struct dbs_data *dbs_data = to_dbs_data(attr_set);
	struct policy_dbs_info *policy_dbs;
	unsigned int rate;
	int ret;
	ret = sscanf(buf, "%u", &rate);
	if (ret != 1)
		return -EINVAL;

	dbs_data->sampling_rate = max(rate, dbs_data->min_sampling_rate);

	/*
	 * We are operating under dbs_data->mutex and so the list and its
	 * entries can't be freed concurrently.
	 */
	list_for_each_entry(policy_dbs, &attr_set->policy_list, list) {
		mutex_lock(&policy_dbs->timer_mutex);
		/*
		 * On 32-bit architectures this may race with the
		 * sample_delay_ns read in dbs_update_util_handler(), but that
		 * really doesn't matter.  If the read returns a value that's
		 * too big, the sample will be skipped, but the next invocation
		 * of dbs_update_util_handler() (when the update has been
		 * completed) will take a sample.
		 *
		 * If this runs in parallel with dbs_work_handler(), we may end
		 * up overwriting the sample_delay_ns value that it has just
		 * written, but it will be corrected next time a sample is
		 * taken, so it shouldn't be significant.
		 */
		gov_update_sample_delay(policy_dbs, 0);
		mutex_unlock(&policy_dbs->timer_mutex);
	}

	return count;
}
EXPORT_SYMBOL_GPL(store_sampling_rate);

/**
 * gov_update_cpu_data - Update CPU load data.
 * @dbs_data: Top-level governor data pointer.
 *
 * Update CPU load data for all CPUs in the domain governed by @dbs_data
 * (that may be a single policy or a bunch of them if governor tunables are
 * system-wide).
 *
 * Call under the @dbs_data mutex.
 */
void gov_update_cpu_data(struct dbs_data *dbs_data)
{
	struct policy_dbs_info *policy_dbs;

	list_for_each_entry(policy_dbs, &dbs_data->attr_set.policy_list, list) {
		unsigned int j;

		for_each_cpu(j, policy_dbs->policy->cpus) {
			struct cpu_dbs_info *j_cdbs = &per_cpu(cpu_dbs, j);

			j_cdbs->prev_cpu_idle = get_cpu_idle_time(j, &j_cdbs->prev_cpu_wall,
								  dbs_data->io_is_busy);
			if (dbs_data->ignore_nice_load)
				j_cdbs->prev_cpu_nice = kcpustat_cpu(j).cpustat[CPUTIME_NICE];
		}
	}
}
EXPORT_SYMBOL_GPL(gov_update_cpu_data);

static inline struct gov_attr_set *to_gov_attr_set(struct kobject *kobj)
{
	return container_of(kobj, struct gov_attr_set, kobj);
}

static inline struct governor_attr *to_gov_attr(struct attribute *attr)
{
	return container_of(attr, struct governor_attr, attr);
}

static ssize_t governor_show(struct kobject *kobj, struct attribute *attr,
			     char *buf)
{
	struct governor_attr *gattr = to_gov_attr(attr);

	return gattr->show(to_gov_attr_set(kobj), buf);
}

static ssize_t governor_store(struct kobject *kobj, struct attribute *attr,
			      const char *buf, size_t count)
{
	struct gov_attr_set *attr_set = to_gov_attr_set(kobj);
	struct governor_attr *gattr = to_gov_attr(attr);
	int ret = -EBUSY;

	mutex_lock(&attr_set->update_lock);

	if (attr_set->usage_count)
		ret = gattr->store(attr_set, buf, count);

	mutex_unlock(&attr_set->update_lock);

	return ret;
}

/*
 * Sysfs Ops for accessing governor attributes.
 *
 * All show/store invocations for governor specific sysfs attributes, will first
 * call the below show/store callbacks and the attribute specific callback will
 * be called from within it.
 */
static const struct sysfs_ops governor_sysfs_ops = {
	.show	= governor_show,
	.store	= governor_store,
};

unsigned int dbs_update(struct cpufreq_policy *policy)
{
	struct policy_dbs_info *policy_dbs = policy->governor_data;
	struct dbs_data *dbs_data = policy_dbs->dbs_data;
	unsigned int ignore_nice = dbs_data->ignore_nice_load;
	unsigned int max_load = 0;
	unsigned int sampling_rate, io_busy, j;

	/*
	 * Sometimes governors may use an additional multiplier to increase
	 * sample delays temporarily.  Apply that multiplier to sampling_rate
	 * so as to keep the wake-up-from-idle detection logic a bit
	 * conservative.
	 */
	sampling_rate = dbs_data->sampling_rate * policy_dbs->rate_mult;
	/*
	 * For the purpose of ondemand, waiting for disk IO is an indication
	 * that you're performance critical, and not that the system is actually
	 * idle, so do not add the iowait time to the CPU idle time then.
	 */
	io_busy = dbs_data->io_is_busy;

	/* Get Absolute Load */
	for_each_cpu(j, policy->cpus) {
		struct cpu_dbs_info *j_cdbs = &per_cpu(cpu_dbs, j);
		u64 cur_wall_time, cur_idle_time;
		unsigned int idle_time, wall_time;
		unsigned int load;

		cur_idle_time = get_cpu_idle_time(j, &cur_wall_time, io_busy);

		wall_time = cur_wall_time - j_cdbs->prev_cpu_wall;
		j_cdbs->prev_cpu_wall = cur_wall_time;

		idle_time = cur_idle_time - j_cdbs->prev_cpu_idle;
		j_cdbs->prev_cpu_idle = cur_idle_time;

		if (ignore_nice) {
			u64 cur_nice = kcpustat_cpu(j).cpustat[CPUTIME_NICE];

			idle_time += cputime_to_usecs(cur_nice - j_cdbs->prev_cpu_nice);
			j_cdbs->prev_cpu_nice = cur_nice;
		}

		if (unlikely(!wall_time || wall_time < idle_time))
			continue;

		/*
		 * If the CPU had gone completely idle, and a task just woke up
		 * on this CPU now, it would be unfair to calculate 'load' the
		 * usual way for this elapsed time-window, because it will show
		 * near-zero load, irrespective of how CPU intensive that task
		 * actually is. This is undesirable for latency-sensitive bursty
		 * workloads.
		 *
		 * To avoid this, we reuse the 'load' from the previous
		 * time-window and give this task a chance to start with a
		 * reasonably high CPU frequency. (However, we shouldn't over-do
		 * this copy, lest we get stuck at a high load (high frequency)
		 * for too long, even when the current system load has actually
		 * dropped down. So we perform the copy only once, upon the
		 * first wake-up from idle.)
		 *
		 * Detecting this situation is easy: the governor's utilization
		 * update handler would not have run during CPU-idle periods.
		 * Hence, an unusually large 'wall_time' (as compared to the
		 * sampling rate) indicates this scenario.
		 *
		 * prev_load can be zero in two cases and we must recalculate it
		 * for both cases:
		 * - during long idle intervals
		 * - explicitly set to zero
		 */
		if (unlikely(wall_time > (2 * sampling_rate) &&
			     j_cdbs->prev_load)) {
			load = j_cdbs->prev_load;

			/*
			 * Perform a destructive copy, to ensure that we copy
			 * the previous load only once, upon the first wake-up
			 * from idle.
			 */
			j_cdbs->prev_load = 0;
		} else {
			load = 100 * (wall_time - idle_time) / wall_time;
			j_cdbs->prev_load = load;
		}

		if (load > max_load)
			max_load = load;
	}
	return max_load;
}
EXPORT_SYMBOL_GPL(dbs_update);

static void gov_set_update_util(struct policy_dbs_info *policy_dbs,
				unsigned int delay_us)
{
	struct cpufreq_policy *policy = policy_dbs->policy;
	int cpu;

	gov_update_sample_delay(policy_dbs, delay_us);
	policy_dbs->last_sample_time = 0;

	for_each_cpu(cpu, policy->cpus) {
		struct cpu_dbs_info *cdbs = &per_cpu(cpu_dbs, cpu);

		cpufreq_set_update_util_data(cpu, &cdbs->update_util);
	}
}

static inline void gov_clear_update_util(struct cpufreq_policy *policy)
{
	int i;

	for_each_cpu(i, policy->cpus)
		cpufreq_set_update_util_data(i, NULL);

	synchronize_rcu();
}

static void gov_cancel_work(struct cpufreq_policy *policy)
{
	struct policy_dbs_info *policy_dbs = policy->governor_data;

	gov_clear_update_util(policy_dbs->policy);
	irq_work_sync(&policy_dbs->irq_work);
	cancel_work_sync(&policy_dbs->work);
	atomic_set(&policy_dbs->work_count, 0);
	policy_dbs->work_in_progress = false;
}

static void dbs_work_handler(struct work_struct *work)
{
	struct policy_dbs_info *policy_dbs;
	struct cpufreq_policy *policy;
	struct dbs_governor *gov;

	policy_dbs = container_of(work, struct policy_dbs_info, work);
	policy = policy_dbs->policy;
	gov = dbs_governor_of(policy);

	/*
	 * Make sure cpufreq_governor_limits() isn't evaluating load or the
	 * ondemand governor isn't updating the sampling rate in parallel.
	 */
	mutex_lock(&policy_dbs->timer_mutex);
	gov_update_sample_delay(policy_dbs, gov->gov_dbs_timer(policy));
	mutex_unlock(&policy_dbs->timer_mutex);

	/* Allow the utilization update handler to queue up more work. */
	atomic_set(&policy_dbs->work_count, 0);
	/*
	 * If the update below is reordered with respect to the sample delay
	 * modification, the utilization update handler may end up using a stale
	 * sample delay value.
	 */
	smp_wmb();
	policy_dbs->work_in_progress = false;
}

static void dbs_irq_work(struct irq_work *irq_work)
{
	struct policy_dbs_info *policy_dbs;

	policy_dbs = container_of(irq_work, struct policy_dbs_info, irq_work);
	schedule_work_on(smp_processor_id(), &policy_dbs->work);
}

static void dbs_update_util_handler(struct update_util_data *data, u64 time,
				    unsigned long util, unsigned long max)
{
	struct cpu_dbs_info *cdbs = container_of(data, struct cpu_dbs_info, update_util);
	struct policy_dbs_info *policy_dbs = cdbs->policy_dbs;
	u64 delta_ns, lst;

	/*
	 * The work may not be allowed to be queued up right now.
	 * Possible reasons:
	 * - Work has already been queued up or is in progress.
	 * - It is too early (too little time from the previous sample).
	 */
	if (policy_dbs->work_in_progress)
		return;

	/*
	 * If the reads below are reordered before the check above, the value
	 * of sample_delay_ns used in the computation may be stale.
	 */
	smp_rmb();
	lst = READ_ONCE(policy_dbs->last_sample_time);
	delta_ns = time - lst;
	if ((s64)delta_ns < policy_dbs->sample_delay_ns)
		return;

	/*
	 * If the policy is not shared, the irq_work may be queued up right away
	 * at this point.  Otherwise, we need to ensure that only one of the
	 * CPUs sharing the policy will do that.
	 */
	if (policy_dbs->is_shared) {
		if (!atomic_add_unless(&policy_dbs->work_count, 1, 1))
			return;

		/*
		 * If another CPU updated last_sample_time in the meantime, we
		 * shouldn't be here, so clear the work counter and bail out.
		 */
		if (unlikely(lst != READ_ONCE(policy_dbs->last_sample_time))) {
			atomic_set(&policy_dbs->work_count, 0);
			return;
		}
	}

	policy_dbs->last_sample_time = time;
	policy_dbs->work_in_progress = true;
	irq_work_queue(&policy_dbs->irq_work);
}

static struct policy_dbs_info *alloc_policy_dbs_info(struct cpufreq_policy *policy,
						     struct dbs_governor *gov)
{
	struct policy_dbs_info *policy_dbs;
	int j;

	/* Allocate memory for per-policy governor data. */
	policy_dbs = gov->alloc();
	if (!policy_dbs)
		return NULL;

	policy_dbs->policy = policy;
	mutex_init(&policy_dbs->timer_mutex);
	atomic_set(&policy_dbs->work_count, 0);
	init_irq_work(&policy_dbs->irq_work, dbs_irq_work);
	INIT_WORK(&policy_dbs->work, dbs_work_handler);

	/* Set policy_dbs for all CPUs, online+offline */
	for_each_cpu(j, policy->related_cpus) {
		struct cpu_dbs_info *j_cdbs = &per_cpu(cpu_dbs, j);

		j_cdbs->policy_dbs = policy_dbs;
		j_cdbs->update_util.func = dbs_update_util_handler;
	}
	return policy_dbs;
}

static void free_policy_dbs_info(struct policy_dbs_info *policy_dbs,
				 struct dbs_governor *gov)
{
	int j;

	mutex_destroy(&policy_dbs->timer_mutex);

	for_each_cpu(j, policy_dbs->policy->related_cpus) {
		struct cpu_dbs_info *j_cdbs = &per_cpu(cpu_dbs, j);

		j_cdbs->policy_dbs = NULL;
		j_cdbs->update_util.func = NULL;
	}
	gov->free(policy_dbs);
}

static void gov_attr_set_init(struct gov_attr_set *attr_set,
			      struct list_head *list_node)
{
	INIT_LIST_HEAD(&attr_set->policy_list);
	mutex_init(&attr_set->update_lock);
	attr_set->usage_count = 1;
	list_add(list_node, &attr_set->policy_list);
}

static void gov_attr_set_get(struct gov_attr_set *attr_set,
			     struct list_head *list_node)
{
	mutex_lock(&attr_set->update_lock);
	attr_set->usage_count++;
	list_add(list_node, &attr_set->policy_list);
	mutex_unlock(&attr_set->update_lock);
}

static unsigned int gov_attr_set_put(struct gov_attr_set *attr_set,
				     struct list_head *list_node)
{
	unsigned int count;

	mutex_lock(&attr_set->update_lock);
	list_del(list_node);
	count = --attr_set->usage_count;
	mutex_unlock(&attr_set->update_lock);
	if (count)
		return count;

	kobject_put(&attr_set->kobj);
	mutex_destroy(&attr_set->update_lock);
	return 0;
}

static int cpufreq_governor_init(struct cpufreq_policy *policy)
{
	struct dbs_governor *gov = dbs_governor_of(policy);
	struct dbs_data *dbs_data;
	struct policy_dbs_info *policy_dbs;
	unsigned int latency;
	int ret = 0;

	/* State should be equivalent to EXIT */
	if (policy->governor_data)
		return -EBUSY;

	policy_dbs = alloc_policy_dbs_info(policy, gov);
	if (!policy_dbs)
		return -ENOMEM;

	/* Protect gov->gdbs_data against concurrent updates. */
	mutex_lock(&gov_dbs_data_mutex);

	dbs_data = gov->gdbs_data;
	if (dbs_data) {
		if (WARN_ON(have_governor_per_policy())) {
			ret = -EINVAL;
			goto free_policy_dbs_info;
		}
		policy_dbs->dbs_data = dbs_data;
		policy->governor_data = policy_dbs;

		gov_attr_set_get(&dbs_data->attr_set, &policy_dbs->list);
		goto out;
	}

	dbs_data = kzalloc(sizeof(*dbs_data), GFP_KERNEL);
	if (!dbs_data) {
		ret = -ENOMEM;
		goto free_policy_dbs_info;
	}

	gov_attr_set_init(&dbs_data->attr_set, &policy_dbs->list);

	ret = gov->init(dbs_data, !policy->governor->initialized);
	if (ret)
		goto free_policy_dbs_info;

	/* policy latency is in ns. Convert it to us first */
	latency = policy->cpuinfo.transition_latency / 1000;
	if (latency == 0)
		latency = 1;

	/* Bring kernel and HW constraints together */
	dbs_data->min_sampling_rate = max(dbs_data->min_sampling_rate,
					  MIN_LATENCY_MULTIPLIER * latency);
	dbs_data->sampling_rate = max(dbs_data->min_sampling_rate,
				      LATENCY_MULTIPLIER * latency);

	if (!have_governor_per_policy())
		gov->gdbs_data = dbs_data;

	policy_dbs->dbs_data = dbs_data;
	policy->governor_data = policy_dbs;

	gov->kobj_type.sysfs_ops = &governor_sysfs_ops;
	ret = kobject_init_and_add(&dbs_data->attr_set.kobj, &gov->kobj_type,
				   get_governor_parent_kobj(policy),
				   "%s", gov->gov.name);
	if (!ret)
		goto out;

	/* Failure, so roll back. */
	pr_err("cpufreq: Governor initialization failed (dbs_data kobject init error %d)\n", ret);

	policy->governor_data = NULL;

	if (!have_governor_per_policy())
		gov->gdbs_data = NULL;
	gov->exit(dbs_data, !policy->governor->initialized);
	kfree(dbs_data);

free_policy_dbs_info:
	free_policy_dbs_info(policy_dbs, gov);

out:
	mutex_unlock(&gov_dbs_data_mutex);
	return ret;
}

static int cpufreq_governor_exit(struct cpufreq_policy *policy)
{
	struct dbs_governor *gov = dbs_governor_of(policy);
	struct policy_dbs_info *policy_dbs = policy->governor_data;
	struct dbs_data *dbs_data = policy_dbs->dbs_data;
	unsigned int count;

	/* Protect gov->gdbs_data against concurrent updates. */
	mutex_lock(&gov_dbs_data_mutex);

	count = gov_attr_set_put(&dbs_data->attr_set, &policy_dbs->list);

	policy->governor_data = NULL;

	if (!count) {
		if (!have_governor_per_policy())
			gov->gdbs_data = NULL;

		gov->exit(dbs_data, policy->governor->initialized == 1);
		kfree(dbs_data);
	}

	free_policy_dbs_info(policy_dbs, gov);

	mutex_unlock(&gov_dbs_data_mutex);
	return 0;
}

static int cpufreq_governor_start(struct cpufreq_policy *policy)
{
	struct dbs_governor *gov = dbs_governor_of(policy);
	struct policy_dbs_info *policy_dbs = policy->governor_data;
	struct dbs_data *dbs_data = policy_dbs->dbs_data;
	unsigned int sampling_rate, ignore_nice, j;
	unsigned int io_busy;

	if (!policy->cur)
		return -EINVAL;

	policy_dbs->is_shared = policy_is_shared(policy);
	policy_dbs->rate_mult = 1;

	sampling_rate = dbs_data->sampling_rate;
	ignore_nice = dbs_data->ignore_nice_load;
	io_busy = dbs_data->io_is_busy;

	for_each_cpu(j, policy->cpus) {
		struct cpu_dbs_info *j_cdbs = &per_cpu(cpu_dbs, j);

		j_cdbs->prev_cpu_idle = get_cpu_idle_time(j, &j_cdbs->prev_cpu_wall, io_busy);
		/*
		 * Make the first invocation of dbs_update() compute the load.
		 */
		j_cdbs->prev_load = 0;

		if (ignore_nice)
			j_cdbs->prev_cpu_nice = kcpustat_cpu(j).cpustat[CPUTIME_NICE];
	}

	gov->start(policy);

	gov_set_update_util(policy_dbs, sampling_rate);
	return 0;
}

static int cpufreq_governor_stop(struct cpufreq_policy *policy)
{
	gov_cancel_work(policy);
	return 0;
}

static int cpufreq_governor_limits(struct cpufreq_policy *policy)
{
	struct policy_dbs_info *policy_dbs = policy->governor_data;

	mutex_lock(&policy_dbs->timer_mutex);

	if (policy->max < policy->cur)
		__cpufreq_driver_target(policy, policy->max, CPUFREQ_RELATION_H);
	else if (policy->min > policy->cur)
		__cpufreq_driver_target(policy, policy->min, CPUFREQ_RELATION_L);

	gov_update_sample_delay(policy_dbs, 0);

	mutex_unlock(&policy_dbs->timer_mutex);

	return 0;
}

int cpufreq_governor_dbs(struct cpufreq_policy *policy, unsigned int event)
{
	if (event == CPUFREQ_GOV_POLICY_INIT) {
		return cpufreq_governor_init(policy);
	} else if (policy->governor_data) {
		switch (event) {
		case CPUFREQ_GOV_POLICY_EXIT:
			return cpufreq_governor_exit(policy);
		case CPUFREQ_GOV_START:
			return cpufreq_governor_start(policy);
		case CPUFREQ_GOV_STOP:
			return cpufreq_governor_stop(policy);
		case CPUFREQ_GOV_LIMITS:
			return cpufreq_governor_limits(policy);
		}
	}
	return -EINVAL;
}
EXPORT_SYMBOL_GPL(cpufreq_governor_dbs);
