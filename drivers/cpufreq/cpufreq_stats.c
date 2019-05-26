/*
 *  drivers/cpufreq/cpufreq_stats.c
 *
 *  Copyright (C) 2003-2004 Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>.
 *  (C) 2004 Zou Nan hai <nanhai.zou@intel.com>.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/atomic.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/cputime.h>
#include <linux/hashtable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/proc_fs.h>
#include <linux/profile.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/uaccess.h>

#define UID_HASH_BITS 10

DECLARE_HASHTABLE(uid_hash_table, UID_HASH_BITS);

static spinlock_t cpufreq_stats_lock;

static DEFINE_SPINLOCK(task_time_in_state_lock); /* task->time_in_state */
static DEFINE_SPINLOCK(task_concurrent_active_time_lock);
	/* task->concurrent_active_time */
static DEFINE_SPINLOCK(task_concurrent_policy_time_lock);
	/* task->concurrent_policy_time */
static DEFINE_SPINLOCK(uid_lock); /* uid_hash_table */

struct uid_entry {
	uid_t uid;
	unsigned int max_state;
	struct hlist_node hash;
	struct rcu_head rcu;
	atomic64_t *concurrent_active_time;
	atomic64_t *concurrent_policy_time;
	u64 time_in_state[0];
};

struct cpufreq_stats {
	unsigned int total_trans;
	unsigned long long last_time;
	unsigned int max_state;
	int prev_states;
	atomic_t curr_state;
	u64 *time_in_state;
	unsigned int *freq_table;
};

static int cpufreq_max_state;
static int cpufreq_last_max_state;
static unsigned int *cpufreq_states;
static bool cpufreq_stats_initialized;
static struct proc_dir_entry *uid_cpupower;

/* STOPSHIP: uid_cpupower_enable is used to enable/disable concurrent_*_time
 * This varible will be used in P/H experiments and should be removed before
 * launch.
 *
 * Because it is being used to test performance and power, it should have a
 * minimum impact on both. For these performance reasons, it will not be guarded
 * by a lock or protective barriers. This limits what it can safely
 * enable/disable.
 *
 * It is safe to check it before updating any concurrent_*_time stats. If there
 * are changes uid_cpupower_enable state while we are updating the stats, we
 * will simply ignore the changes until the next attempt to update the stats.
 * This may result in a couple ms where the uid_cpupower_enable is in one state
 * and the code is acting in another. Since the P/H experiments are done over
 * the course of many days, a couple ms delay should not be an issue.
 *
 * It is not safe to delete the associated proc files without additional locking
 * mechanisms that would hurt performance. Leaving the files empty but intact
 * will not have any impact on the P/H experiments provided that userspace does
 * not attempt to read them. Since the P/H experiment will also disable the code
 * that reads these files from userspace, this is not a concern.
 */
static char uid_cpupower_enable;

struct cpufreq_stats_attribute {
	struct attribute attr;

	ssize_t (*show)(struct cpufreq_stats *, char *);
};

/* Caller must hold rcu_read_lock() */
static struct uid_entry *find_uid_entry_rcu(uid_t uid)
{
	struct uid_entry *uid_entry;

	hash_for_each_possible_rcu(uid_hash_table, uid_entry, hash, uid) {
		if (uid_entry->uid == uid)
			return uid_entry;
	}
	return NULL;
}

/* Caller must hold uid lock */
static struct uid_entry *find_uid_entry(uid_t uid)
{
	struct uid_entry *uid_entry;

	hash_for_each_possible(uid_hash_table, uid_entry, hash, uid) {
		if (uid_entry->uid == uid)
			return uid_entry;
	}
	return NULL;
}

/* Caller must hold uid lock */
static struct uid_entry *find_or_register_uid(uid_t uid)
{
	struct uid_entry *uid_entry;
	struct uid_entry *temp;
	atomic64_t *times;
	unsigned int max_state = READ_ONCE(cpufreq_max_state);
	size_t alloc_size = sizeof(*uid_entry) + max_state *
		sizeof(uid_entry->time_in_state[0]);

	uid_entry = find_uid_entry(uid);
	if (uid_entry) {
		if (uid_entry->max_state == max_state)
			return uid_entry;
		/* uid_entry->time_in_state is too small to track all freqs, so
		 * expand it.
		 */
		temp = __krealloc(uid_entry, alloc_size, GFP_ATOMIC);
		if (!temp)
			return uid_entry;
		temp->max_state = max_state;
		memset(temp->time_in_state + uid_entry->max_state, 0,
		       (max_state - uid_entry->max_state) *
		       sizeof(uid_entry->time_in_state[0]));
		if (temp != uid_entry) {
			hlist_replace_rcu(&uid_entry->hash, &temp->hash);
			kfree_rcu(uid_entry, rcu);
		}
		return temp;
	}

	uid_entry = kzalloc(alloc_size, GFP_ATOMIC);
	if (!uid_entry)
		return NULL;
	/* Single allocation for both active & policy time arrays  */
	times = kcalloc(num_possible_cpus() * 2, sizeof(atomic64_t),
			GFP_ATOMIC);
	if (!times) {
		kfree(uid_entry);
		return NULL;
	}

	uid_entry->uid = uid;
	uid_entry->max_state = max_state;
	uid_entry->concurrent_active_time = times;
	uid_entry->concurrent_policy_time = times + num_possible_cpus();

	hash_add_rcu(uid_hash_table, &uid_entry->hash, uid);

	return uid_entry;
}

static int single_uid_time_in_state_show(struct seq_file *m, void  *ptr)
{
	struct uid_entry *uid_entry;
	unsigned int i;
	u64 time;
	uid_t uid = from_kuid_munged(current_user_ns(), *(kuid_t *)m->private);

	if (uid == overflowuid)
		return -EINVAL;
	if (!cpufreq_stats_initialized)
		return 0;

	rcu_read_lock();
	uid_entry = find_uid_entry_rcu(uid);

	if (!uid_entry) {
		rcu_read_unlock();
		return 0;
	}

	for (i = 0; i < uid_entry->max_state; ++i) {
		time = cputime_to_clock_t(uid_entry->time_in_state[i]);
		seq_write(m, &time, sizeof(time));
	}

	rcu_read_unlock();

	return 0;
}

static void *uid_seq_start(struct seq_file *seq, loff_t *pos)
{
	if (!cpufreq_stats_initialized)
		return NULL;

	if (*pos >= HASH_SIZE(uid_hash_table))
		return NULL;

	return &uid_hash_table[*pos];
}

static void *uid_seq_next(struct seq_file *seq, void *v, loff_t *pos)
{
	(*pos)++;

	if (*pos >= HASH_SIZE(uid_hash_table))
		return NULL;

	return &uid_hash_table[*pos];
}

static void uid_seq_stop(struct seq_file *seq, void *v) { }

static int uid_time_in_state_seq_show(struct seq_file *m, void *v)
{
	struct uid_entry *uid_entry;
	struct cpufreq_policy *last_policy = NULL;
	int i;

	if (!cpufreq_stats_initialized)
		return 0;

	if (v == uid_hash_table) {
		seq_puts(m, "uid:");
		for_each_possible_cpu(i) {
			struct cpufreq_frequency_table *table, *pos;
			struct cpufreq_policy *policy;

			policy = cpufreq_cpu_get(i);
			if (!policy)
				continue;
			table = cpufreq_frequency_get_table(i);

			/* Assumes cpus are colocated within a policy */
			if (table && last_policy != policy) {
				last_policy = policy;
				cpufreq_for_each_valid_entry(pos, table)
					seq_printf(m, " %d", pos->frequency);
			}
			cpufreq_cpu_put(policy);
		}
		seq_putc(m, '\n');
	}

	rcu_read_lock();

	hlist_for_each_entry_rcu(uid_entry, (struct hlist_head *)v, hash) {
		if (uid_entry->max_state)
			seq_printf(m, "%d:", uid_entry->uid);

		for (i = 0; i < uid_entry->max_state; ++i) {
			seq_printf(m, " %lu", (unsigned long)cputime_to_clock_t(
					   uid_entry->time_in_state[i]));
		}
		if (uid_entry->max_state)
			seq_putc(m, '\n');
	}

	rcu_read_unlock();
	return 0;
}

/*
 * time_in_state is an array of u32's in the following format:
 * [n, uid0, time0a, time0b, ..., time0n,
 *     uid1, time1a, time1b, ..., time1n,
 *     uid2, time2a, time2b, ..., time2n, etc.]
 * where n is the total number of frequencies
 */
static int time_in_state_seq_show(struct seq_file *m, void *v)
{
	struct uid_entry *uid_entry;
	u32 cpufreq_max_state_u32 = READ_ONCE(cpufreq_max_state);
	u32 uid, time;
	int i;

	if (!cpufreq_stats_initialized)
		return 0;

	if (v == uid_hash_table)
		seq_write(m, &cpufreq_max_state_u32,
			  sizeof(cpufreq_max_state_u32));

	rcu_read_lock();

	hlist_for_each_entry_rcu(uid_entry, (struct hlist_head *)v, hash) {
		if (uid_entry->max_state) {
			uid = (u32) uid_entry->uid;
			seq_write(m, &uid, sizeof(uid));
		}

		for (i = 0; i < uid_entry->max_state; ++i) {
			time = (u32)
				cputime_to_clock_t(uid_entry->time_in_state[i]);
			seq_write(m, &time, sizeof(time));
		}
	}

	rcu_read_unlock();
	return 0;
}

/*
 * concurrent_active_time is an array of u32's in the following format:
 * [n, uid0, time0a, time0b, ..., time0n,
 *     uid1, time1a, time1b, ..., time1n,
 *     uid2, time2a, time2b, ..., time2n, etc.]
 * where n is the total number of cpus (num_possible_cpus)
 */
static int concurrent_active_time_seq_show(struct seq_file *m, void *v)
{
	struct uid_entry *uid_entry;
	u32 uid, time, num_possible_cpus = num_possible_cpus();
	int i;

	if (!cpufreq_stats_initialized || !uid_cpupower_enable)
		return 0;

	if (v == uid_hash_table)
		seq_write(m, &num_possible_cpus, sizeof(num_possible_cpus));

	rcu_read_lock();

	hlist_for_each_entry_rcu(uid_entry, (struct hlist_head *)v, hash) {
		uid = (u32) uid_entry->uid;
		seq_write(m, &uid, sizeof(uid));

		for (i = 0; i < num_possible_cpus; ++i) {
			time = (u32) cputime_to_clock_t(
				atomic64_read(
					&uid_entry->concurrent_active_time[i]));
			seq_write(m, &time, sizeof(time));
		}
	}

	rcu_read_unlock();
	return 0;
}

/*
 * concurrent_policy_time is an array of u32's in the following format:
 * [n, x0, ..., xn, uid0, time0a, time0b, ..., time0n,
 *                  uid1, time1a, time1b, ..., time1n,
 *                  uid2, time2a, time2b, ..., time2n, etc.]
 * where n is the number of policies
 * xi is the number cpus on a particular policy
 */
static int concurrent_policy_time_seq_show(struct seq_file *m, void *v)
{
	struct uid_entry *uid_entry;
	struct cpufreq_policy *policy;
	struct cpufreq_policy *last_policy = NULL;
	u32 buf[num_possible_cpus()];
	u32 uid, time;
	int i, cnt = 0, num_possible_cpus = num_possible_cpus();

	if (!cpufreq_stats_initialized || !uid_cpupower_enable)
		return 0;

	if (v == uid_hash_table) {
		for_each_possible_cpu(i) {
			policy = cpufreq_cpu_get(i);
			if (!policy)
				continue;
			if (policy != last_policy) {
				cnt++;
				if (last_policy)
					cpufreq_cpu_put(last_policy);
				last_policy = policy;
				buf[cnt] = 0;
			} else {
				cpufreq_cpu_put(policy);
			}
			++buf[cnt];
		}
		if (last_policy)
			cpufreq_cpu_put(last_policy);

		buf[0] = (u32) cnt;
		seq_write(m, buf, (cnt + 1) * sizeof(*buf));
	}
	rcu_read_lock();
	hlist_for_each_entry_rcu(uid_entry, (struct hlist_head *)v, hash) {
		uid = (u32) uid_entry->uid;
		seq_write(m, &uid, sizeof(uid));

		for (i = 0; i < num_possible_cpus; ++i) {
			time = (u32) cputime_to_clock_t(
				atomic64_read(
					&uid_entry->concurrent_policy_time[i]));
			seq_write(m, &time, sizeof(time));
		}
	}
	rcu_read_unlock();
	return 0;
}

static int uid_cpupower_enable_show(struct seq_file *m, void *v)
{
	seq_putc(m, uid_cpupower_enable);
	seq_putc(m, '\n');

	return 0;
}

static ssize_t uid_cpupower_enable_write(struct file *file,
	const char __user *buffer, size_t count, loff_t *ppos)
{
	char enable;

	if (count >= sizeof(enable))
		count = sizeof(enable);

	if (copy_from_user(&enable, buffer, count))
		return -EFAULT;

	if (enable == '0')
		uid_cpupower_enable = 0;
	else if (enable == '1')
		uid_cpupower_enable = 1;
	else
		return -EINVAL;

	return count;
}

static int cpufreq_stats_update(struct cpufreq_stats *stats)
{
	unsigned long long cur_time = get_jiffies_64();

	spin_lock(&cpufreq_stats_lock);
	stats->time_in_state[atomic_read(&stats->curr_state)] +=
		cur_time - stats->last_time;
	stats->last_time = cur_time;
	spin_unlock(&cpufreq_stats_lock);
	return 0;
}

void cpufreq_task_stats_init(struct task_struct *p)
{
	unsigned long flags;
	spin_lock_irqsave(&task_time_in_state_lock, flags);
	p->time_in_state = NULL;
	spin_unlock_irqrestore(&task_time_in_state_lock, flags);
	WRITE_ONCE(p->max_state, 0);
	spin_lock_irqsave(&task_concurrent_active_time_lock, flags);
	p->concurrent_active_time = NULL;
	spin_unlock_irqrestore(&task_concurrent_active_time_lock, flags);
	spin_lock_irqsave(&task_concurrent_policy_time_lock, flags);
	p->concurrent_policy_time = NULL;
	spin_unlock_irqrestore(&task_concurrent_policy_time_lock, flags);
}

void cpufreq_task_stats_alloc(struct task_struct *p)
{
	size_t alloc_size;
	void *temp;
	unsigned long flags;

	if (!cpufreq_stats_initialized)
		return;

	/* We use one array to avoid multiple allocs per task */
	WRITE_ONCE(p->max_state, cpufreq_max_state);

	alloc_size = p->max_state * sizeof(p->time_in_state[0]);
	temp = kzalloc(alloc_size, GFP_ATOMIC);

	spin_lock_irqsave(&task_time_in_state_lock, flags);
	p->time_in_state = temp;
	spin_unlock_irqrestore(&task_time_in_state_lock, flags);

	alloc_size = num_possible_cpus() * sizeof(u64);
	temp = kzalloc(alloc_size, GFP_ATOMIC);

	spin_lock_irqsave(&task_concurrent_active_time_lock, flags);
	p->concurrent_active_time = temp;
	spin_unlock_irqrestore(&task_concurrent_active_time_lock, flags);

	temp = kzalloc(alloc_size, GFP_ATOMIC);

	spin_lock_irqsave(&task_concurrent_policy_time_lock, flags);
	p->concurrent_policy_time = temp;
	spin_unlock_irqrestore(&task_concurrent_policy_time_lock, flags);
}

int proc_time_in_state_show(struct seq_file *m, struct pid_namespace *ns,
	struct pid *pid, struct task_struct *p)
{
	int i;
	cputime_t cputime;
	unsigned long flags;

	if (!cpufreq_stats_initialized || !p->time_in_state)
		return 0;

	spin_lock(&cpufreq_stats_lock);
	for (i = 0; i < p->max_state; ++i) {
		cputime = 0;
		spin_lock_irqsave(&task_time_in_state_lock, flags);
		if (p->time_in_state)
			cputime = atomic_read(&p->time_in_state[i]);
		spin_unlock_irqrestore(&task_time_in_state_lock, flags);

		seq_printf(m, "%d %lu\n", cpufreq_states[i],
			(unsigned long)cputime_to_clock_t(cputime));
	}
	spin_unlock(&cpufreq_stats_lock);

	return 0;
}

int proc_concurrent_active_time_show(struct seq_file *m,
	struct pid_namespace *ns, struct pid *pid, struct task_struct *p)
{
	int i;
	cputime_t cputime;
	unsigned long flags;

	if (!cpufreq_stats_initialized || !p->concurrent_active_time)
		return 0;

	spin_lock(&cpufreq_stats_lock);
	for (i = 0; i < num_possible_cpus(); ++i) {
		cputime = 0;
		spin_lock_irqsave(&task_concurrent_active_time_lock, flags);
		if (p->concurrent_active_time)
			cputime = atomic_read(&p->concurrent_active_time[i]);
		spin_unlock_irqrestore(&task_concurrent_active_time_lock,
			flags);

		seq_printf(m, "%d %lu\n", i,
			(unsigned long)cputime_to_clock_t(cputime));
	}
	spin_unlock(&cpufreq_stats_lock);

	return 0;
}

int proc_concurrent_policy_time_show(struct seq_file *m,
	struct pid_namespace *ns, struct pid *pid, struct task_struct *p)
{
	struct cpufreq_policy *policy;
	struct cpufreq_policy *last_policy = NULL;
	int cpu, cnt = 0;
	cputime_t cputime;
	unsigned long flags;

	if (!cpufreq_stats_initialized || !p->concurrent_policy_time)
		return 0;

	spin_lock(&cpufreq_stats_lock);
	for (cpu = 0; cpu < num_possible_cpus(); ++cpu) {

		policy = cpufreq_cpu_get(cpu);
		if (policy != last_policy) {
			cnt = 0;
			last_policy = policy;
			seq_printf(m, "policy%i\n", cpu);
		}
		cpufreq_cpu_put(policy);
		cnt++;

		cputime = 0;
		spin_lock_irqsave(&task_concurrent_policy_time_lock, flags);
		if (p->concurrent_policy_time)
			cputime = atomic_read(&p->concurrent_policy_time[cpu]);
		spin_unlock_irqrestore(&task_concurrent_policy_time_lock,
			flags);

		seq_printf(m, "%d %lu\n", cnt,
			(unsigned long)cputime_to_clock_t(cputime));
	}
	spin_unlock(&cpufreq_stats_lock);

	return 0;
}

static ssize_t show_total_trans(struct cpufreq_policy *policy, char *buf)
{
	return sprintf(buf, "%d\n", policy->stats->total_trans);
}

static ssize_t show_time_in_state(struct cpufreq_policy *policy, char *buf)
{
	struct cpufreq_stats *stats = policy->stats;
	ssize_t len = 0;
	int i;

	cpufreq_stats_update(stats);
	for (i = 0; i < stats->max_state; i++) {
		len += sprintf(buf + len, "%u %llu\n", stats->freq_table[i],
			(unsigned long long)
			jiffies_64_to_clock_t(stats->time_in_state[i]));
	}
	return len;
}

/* Called without cpufreq_stats_lock held */
void acct_update_power(struct task_struct *task, cputime_t cputime)
{
	struct cpufreq_stats *stats;
	struct cpufreq_policy *policy;
	struct uid_entry *uid_entry;
	unsigned int cpu_num;
	unsigned int state;
	unsigned int active_cpu_cnt = 0;
	unsigned int policy_cpu_cnt = 0;
	unsigned int policy_first_cpu;
	unsigned int index;
	unsigned long flags;
	int cpu = 0;
	uid_t uid = from_kuid_munged(current_user_ns(), task_uid(task));

	if (!task)
		return;

	cpu_num = task_cpu(task);
	policy = cpufreq_cpu_get(cpu_num);
	if (!policy)
		return;

	stats = policy->stats;
	if (!stats) {
		cpufreq_cpu_put(policy);
		return;
	}

	state = stats->prev_states + atomic_read(&policy->stats->curr_state);

	/* This function is called from a different context
	 * Interruptions in between reads/assignements are ok
	 */
	if (cpufreq_stats_initialized &&
		!(task->flags & PF_EXITING) &&
		state < READ_ONCE(task->max_state)) {
		spin_lock_irqsave(&task_time_in_state_lock, flags);
		if (task->time_in_state)
			atomic64_add(cputime, &task->time_in_state[state]);
		spin_unlock_irqrestore(&task_time_in_state_lock, flags);
	}

	spin_lock_irqsave(&uid_lock, flags);
	uid_entry = find_or_register_uid(uid);
	if (uid_entry && state < uid_entry->max_state)
		uid_entry->time_in_state[state] += cputime;
	spin_unlock_irqrestore(&uid_lock, flags);

	if (uid_cpupower_enable) {
		rcu_read_lock();
		uid_entry = find_uid_entry_rcu(uid);

		for_each_possible_cpu(cpu)
			if (!idle_cpu(cpu))
				++active_cpu_cnt;

		index = active_cpu_cnt - 1;
		spin_lock_irqsave(&task_concurrent_active_time_lock, flags);
		if (cpufreq_stats_initialized && !(task->flags & PF_EXITING) &&
			task->concurrent_active_time)
			atomic64_add(cputime,
				     &task->concurrent_active_time[index]);
		spin_unlock_irqrestore(&task_concurrent_active_time_lock, flags);

		if (uid_entry) {
			atomic64_add(cputime,
				     &uid_entry->concurrent_active_time[index]);
		}

		for_each_cpu(cpu, policy->related_cpus)
			if (!idle_cpu(cpu))
				++policy_cpu_cnt;

		policy_first_cpu = cpumask_first(policy->related_cpus);

		index = policy_first_cpu + policy_cpu_cnt - 1;
		spin_lock_irqsave(&task_concurrent_policy_time_lock, flags);
		if (cpufreq_stats_initialized && !(task->flags & PF_EXITING) &&
			task->concurrent_policy_time)
			atomic64_add(cputime,
				&task->concurrent_policy_time[index]);
		spin_unlock_irqrestore(&task_concurrent_policy_time_lock, flags);

		if (uid_entry) {
			atomic64_add(cputime,
				     &uid_entry->concurrent_policy_time[index]);
		}
		rcu_read_unlock();
	}

	cpufreq_cpu_put(policy);

}
EXPORT_SYMBOL_GPL(acct_update_power);

static ssize_t show_all_time_in_state(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	ssize_t len = 0;
	unsigned int i, cpu, freq;
	struct cpufreq_policy *policy;
	struct cpufreq_stats *stats;

	len += scnprintf(buf + len, PAGE_SIZE - len, "freq\t\t");
	for_each_possible_cpu(cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			continue;
		stats = policy->stats;
		len += scnprintf(buf + len, PAGE_SIZE - len, "cpu%u\t\t", cpu);
		cpufreq_stats_update(stats);
		cpufreq_cpu_put(policy);
	}

	if (!cpufreq_stats_initialized)
		goto out;
	for (i = 0; i < cpufreq_max_state; i++) {
		freq = cpufreq_states[i];
		len += scnprintf(buf + len, PAGE_SIZE - len, "\n%u\t\t", freq);
		for_each_possible_cpu(cpu) {
			policy = cpufreq_cpu_get(cpu);
			if (!policy)
				continue;
			stats = policy->stats;
			if (i >= stats->prev_states &&
				i < stats->prev_states + stats->max_state) {
				len += scnprintf(buf + len, PAGE_SIZE - len,
					"%lu\t\t", (unsigned long)
					cputime64_to_clock_t(
						stats->time_in_state[i -
							stats->prev_states]));
			} else {
				len += scnprintf(buf + len, PAGE_SIZE - len,
						"N/A\t\t");
			}
			cpufreq_cpu_put(policy);
		}
	}

out:
	len += scnprintf(buf + len, PAGE_SIZE - len, "\n");
	return len;
}

cpufreq_freq_attr_ro(total_trans);
cpufreq_freq_attr_ro(time_in_state);

static struct attribute *default_attrs[] = {
	&total_trans.attr,
	&time_in_state.attr,
	NULL
};
static struct attribute_group stats_attr_group = {
	.attrs = default_attrs,
	.name = "stats"
};

static struct kobj_attribute _attr_all_time_in_state = __ATTR(all_time_in_state,
		0444, show_all_time_in_state, NULL);


static int freq_table_get_index(struct cpufreq_stats *stats, unsigned int freq)
{
	int index;
	for (index = 0; index < stats->max_state; index++)
		if (stats->freq_table[index] == freq)
			return index;
	return -1;
}

static void __cpufreq_stats_free_table(struct cpufreq_policy *policy)
{
	struct cpufreq_stats *stats = policy->stats;

	if (!stats)
		return;

	pr_debug("%s: Free stats table\n", __func__);

	sysfs_remove_group(&policy->kobj, &stats_attr_group);
	kfree(stats->time_in_state);
	kfree(stats);
	policy->stats = NULL;
	/* cpufreq_last_max_state is always incrementing, not changed here */
}

static void cpufreq_stats_free_table(unsigned int cpu)
{
	struct cpufreq_policy *policy;

	policy = cpufreq_cpu_get(cpu);
	if (!policy)
		return;

	if (cpufreq_frequency_get_table(policy->cpu))
		__cpufreq_stats_free_table(policy);

	cpufreq_cpu_put(policy);
}


static int cpufreq_stats_create_all_table(void)
{
	struct cpufreq_policy *last_policy = NULL;
	struct cpufreq_policy *policy;
	struct cpufreq_stats *stats;
	int cpu, i;

	cpufreq_states = kcalloc(cpufreq_max_state, sizeof(unsigned int),
		GFP_KERNEL);
	if (cpufreq_states == NULL)
		return -ENOMEM;

	for_each_possible_cpu(cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			continue;
		stats = policy->stats;
		if (policy != last_policy) {
			for (i = 0; i < stats->max_state; ++i)
				cpufreq_states[stats->prev_states + i]
					= stats->freq_table[i];
			last_policy = policy;
		}
		cpufreq_cpu_put(policy);
	}
	return 0;
}

static int __cpufreq_stats_create_table(struct cpufreq_policy *policy,
	struct cpufreq_frequency_table *table, int count)
{
	unsigned int i, ret = 0;
	struct cpufreq_stats *stats;
	unsigned int alloc_size;
	struct cpufreq_frequency_table *pos;

	if (policy->stats)
		return -EBUSY;

	stats = kzalloc(sizeof(*stats), GFP_KERNEL);
	if (stats == NULL)
		return -ENOMEM;

	ret = sysfs_create_group(&policy->kobj, &stats_attr_group);
	if (ret)
		pr_warn("Cannot create stats attr group\n");

	alloc_size = count * sizeof(u64) + count * sizeof(unsigned int);

	stats->time_in_state = kzalloc(alloc_size, GFP_KERNEL);
	if (!stats->time_in_state) {
		ret = -ENOMEM;
		goto error_alloc;
	}
	stats->freq_table = (unsigned int *)(stats->time_in_state + count);

	i = 0;
	cpufreq_for_each_valid_entry(pos, table)
		if (freq_table_get_index(stats, pos->frequency) == -1)
			stats->freq_table[i++] = pos->frequency;

	cpufreq_last_max_state = cpufreq_max_state;
	stats->prev_states = cpufreq_last_max_state;
	stats->max_state = count;
	cpufreq_max_state += count;

	spin_lock(&cpufreq_stats_lock);
	stats->last_time = get_jiffies_64();
	atomic_set(&stats->curr_state,
		freq_table_get_index(stats, policy->cur));
	spin_unlock(&cpufreq_stats_lock);
	policy->stats = stats;
	return 0;
error_alloc:
	sysfs_remove_group(&policy->kobj, &stats_attr_group);
	kfree(stats);
	policy->stats = NULL;
	return ret;
}

static void cpufreq_stats_create_table(struct cpufreq_policy *policy)
{
	struct cpufreq_frequency_table *table, *pos;
	int count = 0;

	table = cpufreq_frequency_get_table(policy->cpu);
	if (likely(table)) {
		cpufreq_for_each_valid_entry(pos, table)
			count++;

		__cpufreq_stats_create_table(policy, table, count);
	}
}

static void uid_entry_reclaim(struct rcu_head *rcu)
{
	struct uid_entry *uid_entry = container_of(rcu, struct uid_entry, rcu);

	kfree(uid_entry->concurrent_active_time);
	kfree(uid_entry);
}

void cpufreq_task_stats_remove_uids(uid_t uid_start, uid_t uid_end)
{
	struct uid_entry *uid_entry;
	struct hlist_node *tmp;
	unsigned long flags;

	spin_lock_irqsave(&uid_lock, flags);

	for (; uid_start <= uid_end; uid_start++) {
		hash_for_each_possible_safe(uid_hash_table, uid_entry, tmp,
			hash, uid_start) {
			if (uid_start == uid_entry->uid) {
				hash_del_rcu(&uid_entry->hash);
				call_rcu(&uid_entry->rcu, uid_entry_reclaim);
			}
		}
	}

	spin_unlock_irqrestore(&uid_lock, flags);
}

static int cpufreq_stat_notifier_policy(struct notifier_block *nb,
	unsigned long val, void *data)
{
	int ret = 0, count = 0;
	struct cpufreq_policy *policy = data;
	struct cpufreq_frequency_table *table, *pos;

	table = cpufreq_frequency_get_table(policy->cpu);
	if (!table)
		return 0;

	cpufreq_for_each_valid_entry(pos, table)
		count++;

	if (val == CPUFREQ_CREATE_POLICY)
		ret = __cpufreq_stats_create_table(policy, table, count);
	else if (val == CPUFREQ_REMOVE_POLICY)
		__cpufreq_stats_free_table(policy);

	return ret;
}

static int cpufreq_stat_notifier_trans(struct notifier_block *nb,
	unsigned long val, void *data)
{
	struct cpufreq_freqs *freq = data;
	struct cpufreq_stats *stat;
	struct cpufreq_policy *policy;

	if (val != CPUFREQ_POSTCHANGE)
		return 0;

	policy = cpufreq_cpu_get(freq->cpu);
	if (!policy)
		return 0;

	stat = policy->stats;
	if (!stat) {
		cpufreq_cpu_put(policy);
		return 0;
	}

	cpufreq_stats_update(policy->stats);
	spin_lock(&cpufreq_stats_lock);
	atomic_set(&stat->curr_state, freq_table_get_index(stat, freq->new));
	stat->total_trans++;
	spin_unlock(&cpufreq_stats_lock);
	cpufreq_cpu_put(policy);
	return 0;
}


static int process_notifier(struct notifier_block *self,
	unsigned long cmd, void *v)
{
	struct task_struct *task = v;
	unsigned long flags;
	void *temp_time_in_state, *temp_concurrent_active_time,
		*temp_concurrent_policy_time;

	if (!task)
		return NOTIFY_OK;

	spin_lock_irqsave(&task_time_in_state_lock, flags);
	temp_time_in_state = task->time_in_state;
	task->time_in_state = NULL;
	spin_unlock_irqrestore(&task_time_in_state_lock, flags);

	spin_lock_irqsave(&task_concurrent_active_time_lock, flags);
	temp_concurrent_active_time = task->concurrent_active_time;
	task->concurrent_active_time = NULL;
	spin_unlock_irqrestore(&task_concurrent_active_time_lock, flags);

	spin_lock_irqsave(&task_concurrent_policy_time_lock, flags);
	temp_concurrent_policy_time = task->concurrent_policy_time;
	task->concurrent_policy_time = NULL;
	spin_unlock_irqrestore(&task_concurrent_policy_time_lock, flags);

	kfree(temp_time_in_state);
	kfree(temp_concurrent_active_time);
	kfree(temp_concurrent_policy_time);

	return NOTIFY_OK;
}

void cpufreq_task_stats_free(struct task_struct *p)
{
	kfree(p->time_in_state);
	kfree(p->concurrent_active_time);
	kfree(p->concurrent_policy_time);
}

static const struct seq_operations uid_time_in_state_seq_ops = {
	.start = uid_seq_start,
	.next = uid_seq_next,
	.stop = uid_seq_stop,
	.show = uid_time_in_state_seq_show,
};

static int uid_time_in_state_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &uid_time_in_state_seq_ops);
}

int single_uid_time_in_state_open(struct inode *inode, struct file *file)
{
	return single_open(file, single_uid_time_in_state_show,
			&(inode->i_uid));
}

static const struct file_operations uid_time_in_state_fops = {
	.open		= uid_time_in_state_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_release,
};

static const struct seq_operations time_in_state_seq_ops = {
	.start = uid_seq_start,
	.next = uid_seq_next,
	.stop = uid_seq_stop,
	.show = time_in_state_seq_show,
};

int time_in_state_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &time_in_state_seq_ops);
}

const struct file_operations time_in_state_fops = {
	.open		= time_in_state_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_release,
};

static const struct seq_operations concurrent_active_time_seq_ops = {
	.start = uid_seq_start,
	.next = uid_seq_next,
	.stop = uid_seq_stop,
	.show = concurrent_active_time_seq_show,
};

static int concurrent_active_time_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &concurrent_active_time_seq_ops);
}

static const struct file_operations concurrent_active_time_fops = {
	.open		= concurrent_active_time_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_release,
};

static const struct seq_operations concurrent_policy_time_seq_ops = {
	.start = uid_seq_start,
	.next = uid_seq_next,
	.stop = uid_seq_stop,
	.show = concurrent_policy_time_seq_show,
};

static int concurrent_policy_time_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &concurrent_policy_time_seq_ops);
}

static const struct file_operations concurrent_policy_time_fops = {
	.open		= concurrent_policy_time_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_release,
};

static int uid_cpupower_enable_open(struct inode *inode, struct file *file)
{
	return single_open(file, uid_cpupower_enable_show, PDE_DATA(inode));
}

static const struct file_operations uid_cpupower_enable_fops = {
	.open		= uid_cpupower_enable_open,
	.read		= seq_read,
	.release	= single_release,
	.write		= uid_cpupower_enable_write,
};

static struct notifier_block notifier_policy_block = {
	.notifier_call = cpufreq_stat_notifier_policy
};

static struct notifier_block notifier_trans_block = {
	.notifier_call = cpufreq_stat_notifier_trans
};

static struct notifier_block process_notifier_block = {
	.notifier_call	= process_notifier,
};

static int __init cpufreq_stats_init(void)
{
	int ret;
	unsigned int cpu;
	struct cpufreq_policy *policy;
	struct cpufreq_policy *last_policy = NULL;

	spin_lock_init(&cpufreq_stats_lock);
	ret = cpufreq_register_notifier(&notifier_policy_block,
				CPUFREQ_POLICY_NOTIFIER);
	if (ret)
		return ret;

	get_online_cpus();
	for_each_online_cpu(cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			continue;
		if (policy != last_policy) {
			cpufreq_stats_create_table(policy);
			last_policy = policy;
		}
		cpufreq_cpu_put(policy);
	}
	put_online_cpus();

	/* XXX TODO task support for time_in_state doesn't update freq
	 * info for tasks already initialized, so tasks initialized early
	 * (before cpufreq_stat_init is done) do not get time_in_state data
	 * and CPUFREQ_TRANSITION_NOTIFIER does not update freq info for
	 * tasks already created
	 */
	ret = cpufreq_register_notifier(&notifier_trans_block,
				CPUFREQ_TRANSITION_NOTIFIER);
	if (ret) {
		cpufreq_unregister_notifier(&notifier_policy_block,
				CPUFREQ_POLICY_NOTIFIER);
		get_online_cpus();
		for_each_online_cpu(cpu)
			cpufreq_stats_free_table(cpu);
		put_online_cpus();
		return ret;
	}
	ret = sysfs_create_file(cpufreq_global_kobject,
			&_attr_all_time_in_state.attr);
	if (ret)
		pr_warn("Cannot create sysfs file for cpufreq stats\n");

	proc_create_data("uid_time_in_state", 0444, NULL,
		&uid_time_in_state_fops, NULL);

	profile_event_register(PROFILE_TASK_EXIT, &process_notifier_block);

	ret = cpufreq_stats_create_all_table();
	if (ret)
		pr_warn("Cannot create cpufreq all freqs table\n");

	uid_cpupower = proc_mkdir("uid_cpupower", NULL);
	if (!uid_cpupower) {
		pr_warn("%s: failed to create uid_cputime proc entry\n",
			__func__);
	} else {
		proc_create_data("enable", 0666, uid_cpupower,
			&uid_cpupower_enable_fops, NULL);

		proc_create_data("time_in_state", 0444, uid_cpupower,
			&time_in_state_fops, NULL);

		proc_create_data("concurrent_active_time", 0444, uid_cpupower,
			&concurrent_active_time_fops, NULL);

		proc_create_data("concurrent_policy_time", 0444, uid_cpupower,
			&concurrent_policy_time_fops, NULL);

		uid_cpupower_enable = 1;
	}

	cpufreq_stats_initialized = true;
	return 0;
}
static void __exit cpufreq_stats_exit(void)
{
	unsigned int cpu;

	cpufreq_unregister_notifier(&notifier_policy_block,
			CPUFREQ_POLICY_NOTIFIER);
	cpufreq_unregister_notifier(&notifier_trans_block,
			CPUFREQ_TRANSITION_NOTIFIER);
	for_each_online_cpu(cpu)
		cpufreq_stats_free_table(cpu);
}

MODULE_AUTHOR("Zou Nan hai <nanhai.zou@intel.com>");
MODULE_DESCRIPTION("Export cpufreq stats via sysfs");
MODULE_LICENSE("GPL");

module_init(cpufreq_stats_init);
module_exit(cpufreq_stats_exit);
