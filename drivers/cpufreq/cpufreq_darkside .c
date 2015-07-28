/*
* drivers/cpufreq/cpufreq_darkside.c
*
* Copyright (C) 2010 Google, Inc.
*
* This software is licensed under the terms of the GNU General Public
* License version 2, as published by the Free Software Foundation, and
* may be copied, distributed, and modified under those terms.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* Author: Erasmux
*
* Based on the interactive governor By Mike Chan (mike@android.com)
* which was adaptated to 2.6.29 kernel by Nadlabak (pavel@doshaska.net)
*
* requires to add
* EXPORT_SYMBOL_GPL(nr_running);
* at the end of kernel/sched.c
*
*/
#include <linux/module.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/cpufreq.h>
#include <linux/sched.h>
#include <linux/tick.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/moduleparam.h>
#include <asm/cputime.h>
#include <linux/earlysuspend.h>
static void (*pm_idle_old)(void);
static atomic_t active_count = ATOMIC_INIT(0);
struct darkside_info_s {
struct cpufreq_policy *cur_policy;
struct timer_list timer;
u64 time_in_idle;
u64 idle_exit_time;
u64 freq_change_time;
u64 freq_change_time_in_idle;
int cur_cpu_load;
unsigned int force_ramp_up;
unsigned int enable;
int max_speed;
int min_speed;
};
static DEFINE_PER_CPU(struct darkside_info_s, darkside_info);
/* Workqueues handle frequency scaling */
static struct workqueue_struct *up_wq;
static struct workqueue_struct *down_wq;
static struct work_struct freq_scale_work;
static cpumask_t work_cpumask;
static unsigned int suspended;
enum {
DARKSIDE_DEBUG_JUMPS=1,
DARKSIDE_DEBUG_LOAD=2
};
/*
* Combination of the above debug flags.
*/
static unsigned long debug_mask;
/*
* The minimum amount of time to spend at a frequency before we can ramp up.
*/
#define DEFAULT_UP_RATE_US 10000;
static unsigned long up_rate_us;
/*
* The minimum amount of time to spend at a frequency before we can ramp down.
*/
#define DEFAULT_DOWN_RATE_US 20000;
static unsigned long down_rate_us;
/*
* When ramping up frequency with no idle cycles jump to at least this frequency.
* Zero disables. Set a very high value to jump to policy max freqeuncy.
*/
#define DEFAULT_UP_MIN_FREQ 1900000
static unsigned int up_min_freq;
/*
* When sleep_max_freq>0 the frequency when suspended will be capped
* by this frequency. Also will wake up at max frequency of policy
* to minimize wakeup issues.
* Set sleep_max_freq=0 to disable this behavior.
*/
#define DEFAULT_SLEEP_MAX_FREQ 368640
static unsigned int sleep_max_freq;
/*
* The frequency to set when waking up from sleep.
* When sleep_max_freq=0 this will have no effect.
*/
#define DEFAULT_SLEEP_WAKEUP_FREQ 768000
static unsigned int sleep_wakeup_freq;
#define UP_THRESHOLD_FREQ 1800000
static unsigned int threshold_freq;
/*
* When awake_min_freq>0 the frequency when not suspended will not
* go below this frequency.
* Set awake_min_freq=0 to disable this behavior.
*/
#define DEFAULT_AWAKE_MIN_FREQ 122000
static unsigned int awake_min_freq;
static unsigned int suspendfreq = 400000;
/*
* Sampling rate, I highly recommend to leave it at 2.
*/
#define DEFAULT_SAMPLE_RATE_JIFFIES 2
static unsigned int sample_rate_jiffies;
/*
* Minimum Freqeuncy delta when ramping up.
* zero disables and causes to always jump straight to max frequency.
*/
#define DEFAULT_RAMP_UP_STEP 614400
static unsigned int ramp_up_step;
/*
* Miminum Freqeuncy delta when ramping down.
* zero disables and will calculate ramp down according to load heuristic.
*/
#define DEFAULT_RAMP_DOWN_STEP 384000
static unsigned int ramp_down_step;
/*
* CPU freq will be increased if measured load > max_cpu_load;
*/
#define DEFAULT_MAX_CPU_LOAD 45
static unsigned long max_cpu_load;
#define DEFAULT_X_CPU_LOAD 70
static unsigned long x_cpu_load;
/*
* CPU freq will be decreased if measured load < min_cpu_load;
*/
#define DEFAULT_MIN_CPU_LOAD 25
static unsigned long min_cpu_load;
#define RAPID_MIN_CPU_LOAD 5
static unsigned long rapid_min_cpu_load;
static int cpufreq_governor_darkside(struct cpufreq_policy *policy,
unsigned int event);
#ifndef CONFIG_CPU_FREQ_DEFAULT_GOV_DARKSIDE
static
#endif
struct cpufreq_governor cpufreq_gov_darkside = {
.name = "darkside",
.governor = cpufreq_governor_darkside,
.max_transition_latency = 9000000,
.owner = THIS_MODULE,
};
static void darkside_update_min_max(struct darkside_info_s *this_darkside, struct cpufreq_policy *policy, int suspend) {
if (suspend) {
this_darkside->min_speed = policy->min;
this_darkside->max_speed = sleep_max_freq;
// this_darkside->max_speed = // sleep_max_freq; but make sure it obeys the policy min/max
// policy->max > sleep_max_freq ? (sleep_max_freq > policy->min ? sleep_max_freq : policy->min) : policy->max;
} else {
this_darkside->min_speed = // awake_min_freq; but make sure it obeys the policy min/max
policy->min < awake_min_freq ? (awake_min_freq < policy->max ? awake_min_freq : policy->max) : policy->min;
this_darkside->max_speed = policy->max;
}
}
inline static unsigned int validate_freq(struct darkside_info_s *this_darkside, int freq) {
if (freq > this_darkside->max_speed)
return this_darkside->max_speed;
if (freq < this_darkside->min_speed)
return this_darkside->min_speed;
return freq;
}
static void reset_timer(unsigned long cpu, struct darkside_info_s *this_darkside) {
this_darkside->time_in_idle = get_cpu_idle_time_us(cpu, &this_darkside->idle_exit_time);
mod_timer(&this_darkside->timer, jiffies + sample_rate_jiffies);
}
static void cpufreq_darkside_timer(unsigned long data)
{
u64 delta_idle;
u64 delta_time;
int cpu_load;
u64 update_time;
u64 now_idle;
unsigned long new_rate;
struct darkside_info_s *this_darkside = &per_cpu(darkside_info, data);
struct cpufreq_policy *policy = this_darkside->cur_policy;
now_idle = get_cpu_idle_time_us(data, &update_time);
if (this_darkside->idle_exit_time == 0 || update_time == this_darkside->idle_exit_time)
return;
delta_idle = cputime64_sub(now_idle, this_darkside->time_in_idle);
delta_time = cputime64_sub(update_time, this_darkside->idle_exit_time);
//printk(KERN_INFO "darksideT: t=%llu i=%llu\n",cputime64_sub(update_time,this_darkside->idle_exit_time),delta_idle);
// If timer ran less than 1ms after short-term sample started, retry.
if (delta_time < 1000) {
if (!timer_pending(&this_darkside->timer))
reset_timer(data,this_darkside);
return;
}
if (delta_idle > delta_time)
cpu_load = 0;
else
cpu_load = 100 * (unsigned int)(delta_time - delta_idle) / (unsigned int)delta_time;
if (debug_mask & DARKSIDE_DEBUG_LOAD)
printk(KERN_INFO "darksideT @ %d: load %d (delta_time %llu)\n",policy->cur,cpu_load,delta_time);
this_darkside->cur_cpu_load = cpu_load;
// Scale up if load is above max or if there where no idle cycles since coming out of idle,
// or when we are above our max speed for a very long time (should only happend if entering sleep
// at high loads)
if ((cpu_load > max_cpu_load || delta_idle == 0) &&
!(policy->cur > this_darkside->max_speed &&
cputime64_sub(update_time, this_darkside->freq_change_time) > 100*down_rate_us)) {
if (policy->cur > this_darkside->max_speed) {
reset_timer(data,this_darkside);
}
if (policy->cur == policy->max)
return;
if (nr_running() < 1)
return;
new_rate = up_rate_us;
// minimize going above 1.8Ghz
if (policy->cur > up_min_freq) new_rate = 75000;
if (cputime64_sub(update_time, this_darkside->freq_change_time) < new_rate)
return;
this_darkside->force_ramp_up = 1;
cpumask_set_cpu(data, &work_cpumask);
queue_work(up_wq, &freq_scale_work);
return;
}
/*
* There is a window where if the cpu utlization can go from low to high
* between the timer expiring, delta_idle will be > 0 and the cpu will
* be 100% busy, preventing idle from running, and this timer from
* firing. So setup another timer to fire to check cpu utlization.
* Do not setup the timer if there is no scheduled work or if at max speed.
*/
if (policy->cur < this_darkside->max_speed && !timer_pending(&this_darkside->timer) && nr_running() > 0)
reset_timer(data,this_darkside);
if (policy->cur == policy->min)
return;
/*
* Do not scale down unless we have been at this frequency for the
* minimum sample time.
*/
if (cputime64_sub(update_time, this_darkside->freq_change_time) < down_rate_us)
return;
cpumask_set_cpu(data, &work_cpumask);
queue_work(down_wq, &freq_scale_work);
}
static void cpufreq_idle(void)
{
struct darkside_info_s *this_darkside = &per_cpu(darkside_info, smp_processor_id());
struct cpufreq_policy *policy = this_darkside->cur_policy;
if (!this_darkside->enable) {
pm_idle_old();
return;
}
if (policy->cur == this_darkside->min_speed && timer_pending(&this_darkside->timer))
del_timer(&this_darkside->timer);
pm_idle_old();
if (!timer_pending(&this_darkside->timer))
reset_timer(smp_processor_id(), this_darkside);
}
/* We use the same work function to sale up and down */
static void cpufreq_darkside_freq_change_time_work(struct work_struct *work)
{
unsigned int cpu;
int new_freq, old_freq;
unsigned int force_ramp_up;
int cpu_load;
struct darkside_info_s *this_darkside;
struct cpufreq_policy *policy;
unsigned int relation = CPUFREQ_RELATION_L;
cpumask_t tmp_mask = work_cpumask;
for_each_cpu(cpu, tmp_mask) {
this_darkside = &per_cpu(darkside_info, cpu);
policy = this_darkside->cur_policy;
cpu_load = this_darkside->cur_cpu_load;
force_ramp_up = this_darkside->force_ramp_up && nr_running() > 1;
this_darkside->force_ramp_up = 0;
if (force_ramp_up || cpu_load > max_cpu_load) {
if (!suspended) {
if (force_ramp_up && up_min_freq && policy->cur < up_min_freq) {
// imoseyon - ramp up faster
new_freq = up_min_freq;
relation = CPUFREQ_RELATION_L;
} else if (ramp_up_step) {
new_freq = policy->cur + ramp_up_step;
relation = CPUFREQ_RELATION_H;
} else {
new_freq = this_darkside->max_speed;
relation = CPUFREQ_RELATION_H;
}
// try to minimize going above 1.8Ghz
if ((new_freq > threshold_freq) && (cpu_load < 95)) {
new_freq = threshold_freq;
relation = CPUFREQ_RELATION_H;
}
} else {
new_freq = policy->cur + 150000;
if (new_freq > suspendfreq) new_freq = suspendfreq;
relation = CPUFREQ_RELATION_H;
}
} else if (cpu_load < min_cpu_load) {
if (cpu_load < rapid_min_cpu_load) {
new_freq = awake_min_freq;
} else if (ramp_down_step) {
new_freq = policy->cur - ramp_down_step;
} else {
cpu_load += 100 - max_cpu_load; // dummy load.
new_freq = policy->cur * cpu_load / 100;
}
relation = CPUFREQ_RELATION_L;
}
else new_freq = policy->cur;
old_freq = policy->cur;
new_freq = validate_freq(this_darkside,new_freq);
if (new_freq != policy->cur) {
if (debug_mask & DARKSIDE_DEBUG_JUMPS)
printk(KERN_INFO "SmartassQ: jumping from %d to %d\n",policy->cur,new_freq);
__cpufreq_driver_target(policy, new_freq, relation);
this_darkside->freq_change_time_in_idle =
get_cpu_idle_time_us(cpu,&this_darkside->freq_change_time);
if (relation == CPUFREQ_RELATION_L && old_freq == policy->cur) {
// step down one more time
new_freq = new_freq - 100000;
__cpufreq_driver_target(policy, new_freq, relation);
this_darkside->freq_change_time_in_idle =
get_cpu_idle_time_us(cpu,&this_darkside->freq_change_time);
}
if (relation == CPUFREQ_RELATION_H && old_freq == policy->cur) {
// step up one more time
new_freq = new_freq + 100000;
__cpufreq_driver_target(policy, new_freq, relation);
this_darkside->freq_change_time_in_idle =
get_cpu_idle_time_us(cpu,&this_darkside->freq_change_time);
}
}
cpumask_clear_cpu(cpu, &work_cpumask);
}
}
static ssize_t show_debug_mask(struct cpufreq_policy *policy, char *buf)
{
return sprintf(buf, "%lu\n", debug_mask);
}
static ssize_t store_debug_mask(struct cpufreq_policy *policy, const char *buf, size_t count)
{
ssize_t res;
unsigned long input;
res = strict_strtoul(buf, 0, &input);
if (res >= 0)
debug_mask = input;
return res;
}
static struct freq_attr debug_mask_attr = __ATTR(debug_mask, 0644,
show_debug_mask, store_debug_mask);
static ssize_t show_up_rate_us(struct cpufreq_policy *policy, char *buf)
{
return sprintf(buf, "%lu\n", up_rate_us);
}
static ssize_t store_up_rate_us(struct cpufreq_policy *policy, const char *buf, size_t count)
{
ssize_t res;
unsigned long input;
res = strict_strtoul(buf, 0, &input);
if (res >= 0 && input >= 0 && input <= 100000000)
up_rate_us = input;
return res;
}
static struct freq_attr up_rate_us_attr = __ATTR(up_rate_us, 0644,
show_up_rate_us, store_up_rate_us);
static ssize_t show_down_rate_us(struct cpufreq_policy *policy, char *buf)
{
return sprintf(buf, "%lu\n", down_rate_us);
}
static ssize_t store_down_rate_us(struct cpufreq_policy *policy, const char *buf, size_t count)
{
ssize_t res;
unsigned long input;
res = strict_strtoul(buf, 0, &input);
if (res >= 0 && input >= 0 && input <= 100000000)
down_rate_us = input;
return res;
}
static struct freq_attr down_rate_us_attr = __ATTR(down_rate_us, 0644,
show_down_rate_us, store_down_rate_us);
static ssize_t show_up_min_freq(struct cpufreq_policy *policy, char *buf)
{
return sprintf(buf, "%u\n", up_min_freq);
}
static ssize_t store_up_min_freq(struct cpufreq_policy *policy, const char *buf, size_t count)
{
ssize_t res;
unsigned long input;
res = strict_strtoul(buf, 0, &input);
if (res >= 0 && input >= 0)
up_min_freq = input;
return res;
}
static struct freq_attr up_min_freq_attr = __ATTR(up_min_freq, 0644,
show_up_min_freq, store_up_min_freq);
static ssize_t show_sleep_max_freq(struct cpufreq_policy *policy, char *buf)
{
return sprintf(buf, "%u\n", sleep_max_freq);
}
static ssize_t store_sleep_max_freq(struct cpufreq_policy *policy, const char *buf, size_t count)
{
ssize_t res;
unsigned long input;
res = strict_strtoul(buf, 0, &input);
if (res >= 0 && input >= 0)
sleep_max_freq = input;
return res;
}
static struct freq_attr sleep_max_freq_attr = __ATTR(sleep_max_freq, 0644,
show_sleep_max_freq, store_sleep_max_freq);
static ssize_t show_sleep_wakeup_freq(struct cpufreq_policy *policy, char *buf)
{
return sprintf(buf, "%u\n", sleep_wakeup_freq);
}
static ssize_t store_sleep_wakeup_freq(struct cpufreq_policy *policy, const char *buf, size_t count)
{
ssize_t res;
unsigned long input;
res = strict_strtoul(buf, 0, &input);
if (res >= 0 && input >= 0)
sleep_wakeup_freq = input;
return res;
}
static struct freq_attr sleep_wakeup_freq_attr = __ATTR(sleep_wakeup_freq, 0644,
show_sleep_wakeup_freq, store_sleep_wakeup_freq);
static ssize_t show_awake_min_freq(struct cpufreq_policy *policy, char *buf)
{
return sprintf(buf, "%u\n", awake_min_freq);
}
static ssize_t store_awake_min_freq(struct cpufreq_policy *policy, const char *buf, size_t count)
{
ssize_t res;
unsigned long input;
res = strict_strtoul(buf, 0, &input);
if (res >= 0 && input >= 0)
awake_min_freq = input;
return res;
}
static struct freq_attr awake_min_freq_attr = __ATTR(awake_min_freq, 0644,
show_awake_min_freq, store_awake_min_freq);
static ssize_t show_sample_rate_jiffies(struct cpufreq_policy *policy, char *buf)
{
return sprintf(buf, "%u\n", sample_rate_jiffies);
}
static ssize_t store_sample_rate_jiffies(struct cpufreq_policy *policy, const char *buf, size_t count)
{
ssize_t res;
unsigned long input;
res = strict_strtoul(buf, 0, &input);
if (res >= 0 && input > 0 && input <= 1000)
sample_rate_jiffies = input;
return res;
}
static struct freq_attr sample_rate_jiffies_attr = __ATTR(sample_rate_jiffies, 0644,
show_sample_rate_jiffies, store_sample_rate_jiffies);
static ssize_t show_ramp_up_step(struct cpufreq_policy *policy, char *buf)
{
return sprintf(buf, "%u\n", ramp_up_step);
}
static ssize_t store_ramp_up_step(struct cpufreq_policy *policy, const char *buf, size_t count)
{
ssize_t res;
unsigned long input;
res = strict_strtoul(buf, 0, &input);
if (res >= 0 && input >= 0)
ramp_up_step = input;
return res;
}
static struct freq_attr ramp_up_step_attr = __ATTR(ramp_up_step, 0644,
show_ramp_up_step, store_ramp_up_step);
static ssize_t show_ramp_down_step(struct cpufreq_policy *policy, char *buf)
{
return sprintf(buf, "%u\n", ramp_down_step);
}
static ssize_t store_ramp_down_step(struct cpufreq_policy *policy, const char *buf, size_t count)
{
ssize_t res;
unsigned long input;
res = strict_strtoul(buf, 0, &input);
if (res >= 0 && input >= 0)
ramp_down_step = input;
return res;
}
static struct freq_attr ramp_down_step_attr = __ATTR(ramp_down_step, 0644,
show_ramp_down_step, store_ramp_down_step);
static ssize_t show_max_cpu_load(struct cpufreq_policy *policy, char *buf)
{
return sprintf(buf, "%lu\n", max_cpu_load);
}
static ssize_t store_max_cpu_load(struct cpufreq_policy *policy, const char *buf, size_t count)
{
ssize_t res;
unsigned long input;
res = strict_strtoul(buf, 0, &input);
if (res >= 0 && input > 0 && input <= 100)
max_cpu_load = input;
return res;
}
static struct freq_attr max_cpu_load_attr = __ATTR(max_cpu_load, 0644,
show_max_cpu_load, store_max_cpu_load);
static ssize_t show_min_cpu_load(struct cpufreq_policy *policy, char *buf)
{
return sprintf(buf, "%lu\n", min_cpu_load);
}
static ssize_t store_min_cpu_load(struct cpufreq_policy *policy, const char *buf, size_t count)
{
ssize_t res;
unsigned long input;
res = strict_strtoul(buf, 0, &input);
if (res >= 0 && input > 0 && input < 100)
min_cpu_load = input;
return res;
}
static struct freq_attr min_cpu_load_attr = __ATTR(min_cpu_load, 0644,
show_min_cpu_load, store_min_cpu_load);
static struct attribute * darkside_attributes[] = {
&debug_mask_attr.attr,
&up_rate_us_attr.attr,
&down_rate_us_attr.attr,
&up_min_freq_attr.attr,
&sleep_max_freq_attr.attr,
&sleep_wakeup_freq_attr.attr,
&awake_min_freq_attr.attr,
&sample_rate_jiffies_attr.attr,
&ramp_up_step_attr.attr,
&ramp_down_step_attr.attr,
&max_cpu_load_attr.attr,
&min_cpu_load_attr.attr,
NULL,
};
static struct attribute_group darkside_attr_group = {
.attrs = darkside_attributes,
.name = "darkside",
};
static void darkside_suspend(int cpu, int suspend)
{
struct darkside_info_s *this_darkside = &per_cpu(darkside_info, smp_processor_id());
struct cpufreq_policy *policy = this_darkside->cur_policy;
unsigned int new_freq;
if (!this_darkside->enable || sleep_max_freq==0) // disable behavior for sleep_max_freq==0
return;
darkside_update_min_max(this_darkside,policy,suspend);
if (!suspend) { // resume at max speed:
suspended=0;
new_freq = validate_freq(this_darkside,sleep_wakeup_freq);
if (debug_mask & DARKSIDE_DEBUG_JUMPS)
printk(KERN_INFO "SmartassS: awaking at %d\n",new_freq);
__cpufreq_driver_target(policy, new_freq,
CPUFREQ_RELATION_L);
if (policy->cur < this_darkside->max_speed && !timer_pending(&this_darkside->timer))
reset_timer(smp_processor_id(),this_darkside);
pr_info("[imoseyon] darkside awake at %d\n", policy->cur);
} else {
// to avoid wakeup issues with quick sleep/wakeup don't change actual frequency when entering sleep
// to allow some time to settle down.
// we reset the timer, if eventually, even at full load the timer will lower the freqeuncy.
reset_timer(smp_processor_id(),this_darkside);
this_darkside->freq_change_time_in_idle =
get_cpu_idle_time_us(cpu,&this_darkside->freq_change_time);
if (debug_mask & DARKSIDE_DEBUG_JUMPS)
printk(KERN_INFO "SmartassS: suspending at %d\n",policy->cur);
__cpufreq_driver_target(policy, suspendfreq, CPUFREQ_RELATION_H);
pr_info("[imoseyon] darkside suspending with %d\n", policy->cur);
suspended=1;
}
}
static void darkside_early_suspend(struct early_suspend *handler) {
int i;
for_each_online_cpu(i)
darkside_suspend(i,1);
}
static void darkside_late_resume(struct early_suspend *handler) {
int i;
for_each_online_cpu(i)
darkside_suspend(i,0);
}
static struct early_suspend darkside_power_suspend = {
.suspend = darkside_early_suspend,
.resume = darkside_late_resume,
.level = EARLY_SUSPEND_LEVEL_DISABLE_FB + 1,
};
static int cpufreq_governor_darkside(struct cpufreq_policy *new_policy,
unsigned int event)
{
unsigned int cpu = new_policy->cpu;
int rc;
struct darkside_info_s *this_darkside = &per_cpu(darkside_info, cpu);
switch (event) {
case CPUFREQ_GOV_START:
if ((!cpu_online(cpu)) || (!new_policy->cur))
return -EINVAL;
/*
* Do not register the idle hook and create sysfs
* entries if we have already done so.
*/
if (atomic_inc_return(&active_count) <= 1) {
rc = sysfs_create_group(&new_policy->kobj, &darkside_attr_group);
if (rc)
return rc;
pm_idle_old = pm_idle;
pm_idle = cpufreq_idle;
}
this_darkside->cur_policy = new_policy;
this_darkside->enable = 1;
// imoseyon - should only register for suspend when governor active
register_early_suspend(&darkside_power_suspend);
pr_info("[imoseyon] darkside active\n");
// notice no break here!
case CPUFREQ_GOV_LIMITS:
darkside_update_min_max(this_darkside,new_policy,suspended);
if (this_darkside->cur_policy->cur != this_darkside->max_speed) {
if (debug_mask & DARKSIDE_DEBUG_JUMPS)
printk(KERN_INFO "SmartassI: initializing to %d\n",this_darkside->max_speed);
__cpufreq_driver_target(new_policy, this_darkside->max_speed, CPUFREQ_RELATION_H);
}
break;
case CPUFREQ_GOV_STOP:
del_timer(&this_darkside->timer);
this_darkside->enable = 0;
if (atomic_dec_return(&active_count) > 1)
return 0;
sysfs_remove_group(&new_policy->kobj,
&darkside_attr_group);
pm_idle = pm_idle_old;
// unregister when governor exits
unregister_early_suspend(&darkside_power_suspend);
pr_info("[imoseyon] darkside inactive\n");
break;
}
return 0;
}
static int __init cpufreq_darkside_init(void)
{
unsigned int i;
struct darkside_info_s *this_darkside;
debug_mask = 0;
up_rate_us = DEFAULT_UP_RATE_US;
down_rate_us = DEFAULT_DOWN_RATE_US;
up_min_freq = DEFAULT_UP_MIN_FREQ;
sleep_max_freq = DEFAULT_SLEEP_MAX_FREQ;
sleep_wakeup_freq = DEFAULT_SLEEP_WAKEUP_FREQ;
threshold_freq = UP_THRESHOLD_FREQ;
awake_min_freq = DEFAULT_AWAKE_MIN_FREQ;
sample_rate_jiffies = DEFAULT_SAMPLE_RATE_JIFFIES;
ramp_up_step = DEFAULT_RAMP_UP_STEP;
ramp_down_step = DEFAULT_RAMP_DOWN_STEP;
max_cpu_load = DEFAULT_MAX_CPU_LOAD;
x_cpu_load = DEFAULT_X_CPU_LOAD;
min_cpu_load = DEFAULT_MIN_CPU_LOAD;
rapid_min_cpu_load = RAPID_MIN_CPU_LOAD;
suspended = 0;
/* Initalize per-cpu data: */
for_each_possible_cpu(i) {
this_darkside = &per_cpu(darkside_info, i);
this_darkside->enable = 0;
this_darkside->cur_policy = 0;
this_darkside->force_ramp_up = 0;
this_darkside->max_speed = DEFAULT_SLEEP_WAKEUP_FREQ;
this_darkside->min_speed = DEFAULT_AWAKE_MIN_FREQ;
this_darkside->time_in_idle = 0;
this_darkside->idle_exit_time = 0;
this_darkside->freq_change_time = 0;
this_darkside->freq_change_time_in_idle = 0;
this_darkside->cur_cpu_load = 0;
// intialize timer:
init_timer_deferrable(&this_darkside->timer);
this_darkside->timer.function = cpufreq_darkside_timer;
this_darkside->timer.data = i;
}
/* Scale up is high priority */
up_wq = alloc_workqueue("kdarkside_up", WQ_HIGHPRI, 1);
down_wq = alloc_workqueue("kdarkside_down", 0, 1);
if (!up_wq || !down_wq)
return -ENOMEM;
INIT_WORK(&freq_scale_work, cpufreq_darkside_freq_change_time_work);
pr_info("[imoseyon] darkside enter\n");
return cpufreq_register_governor(&cpufreq_gov_darkside);
}
#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_DARKSIDE
pure_initcall(cpufreq_darkside_init);
#else
module_init(cpufreq_darkside_init);
#endif
static void __exit cpufreq_darkside_exit(void)
{
pr_info("[imoseyon] darkside exit\n");
cpufreq_unregister_governor(&cpufreq_gov_darkside);
destroy_workqueue(up_wq);
destroy_workqueue(down_wq);
}
module_exit(cpufreq_darkside_exit);
MODULE_AUTHOR ("Erasmux/imoseyon");
MODULE_DESCRIPTION ("'cpufreq_darkside' - A smart cpufreq governor optimized for the hero!");
MODULE_LICENSE ("GPL");
