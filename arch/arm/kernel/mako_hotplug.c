/*
 * Copyright (c) 2013, Francisco Franco <franciscofranco.1990@gmail.com>. 
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
 * Simple no bullshit hot[un]plug driver for SMP
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/cpu.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/earlysuspend.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/hotplug.h>
 
#include <mach/cpufreq.h>

#define DEFAULT_FIRST_LEVEL 60
#define DEFAULT_SUSPEND_FREQ 702000
#define DEFAULT_CORES_ON_TOUCH 2
#define HIGH_LOAD_COUNTER 20
#define TIMER HZ

/*
 * 1000ms = 1 second
 */
#define MIN_TIME_CPU_ONLINE_MS 1000

struct cpu_stats
{
    unsigned int default_first_level;
    unsigned int suspend_frequency;
    unsigned int cores_on_touch;

    unsigned int counter[2];
	unsigned long timestamp[2];
};

static struct cpu_stats stats;
static struct workqueue_struct *wq;
static struct delayed_work decide_hotplug;

#if 0
static void scale_interactive_tunables(unsigned int above_hispeed_delay,
    unsigned int timer_rate, 
    unsigned int min_sample_time)
{
    scale_above_hispeed_delay(above_hispeed_delay);
    scale_timer_rate(timer_rate);
    scale_min_sample_time(min_sample_time);
}
#endif

static inline void calc_cpu_hotplug(unsigned int counter0,
									unsigned int counter1)
{
	bool online_cpu2 = counter0 >= 10;
	bool online_cpu3 = counter1 >= 10;

	if (online_cpu2)
	{
		if (cpu_is_offline(2))
		{
			cpu_up(2);
			stats.timestamp[0] = ktime_to_ms(ktime_get());		
		}
	}
	else if (cpu_online(2))
	{
		/*
		 * Let's not unplug this cpu unless its been online for longer than 1
		 * second to avoid consecutive ups and downs if the load is varying
		 * closer to the threshold point.
		 */
		if (ktime_to_ms(ktime_get()) + MIN_TIME_CPU_ONLINE_MS 
				> stats.timestamp[0])
			cpu_down(2);
	} 
	
	if (online_cpu3)
	{
		if (cpu_is_offline(3))
		{
			cpu_up(3);
			stats.timestamp[1] = ktime_to_ms(ktime_get());		
		}
	}
	else if (cpu_online(3))
	{
		if (ktime_to_ms(ktime_get()) + MIN_TIME_CPU_ONLINE_MS 
				> stats.timestamp[1])
			cpu_down(3);
	}
}

static void __cpuinit decide_hotplug_func(struct work_struct *work)
{
    int cpu;
    int cpu_boost;

	if (interactive_selected)
	{
		if (is_touching && num_online_cpus() < stats.cores_on_touch)
		{
			for (cpu_boost = 2; cpu_boost < stats.cores_on_touch; cpu_boost++)
            	if (cpu_is_offline(cpu_boost)) 
            	    cpu_up(cpu_boost);
		}
	}

    for_each_online_cpu(cpu) 
    {
        if (report_load_at_max_freq(cpu) >= stats.default_first_level)
        {
            if (likely(stats.counter[cpu] < HIGH_LOAD_COUNTER))    
                stats.counter[cpu] += 2;
        }

        else
        {
            if (stats.counter[cpu] > 0)
                stats.counter[cpu]--;
        }

		if (cpu)
			break;
    }

	calc_cpu_hotplug(stats.counter[0], stats.counter[1]);
	
    queue_delayed_work(wq, &decide_hotplug, msecs_to_jiffies(TIMER));
}

static void __cpuinit mako_hotplug_early_suspend(struct early_suspend *handler)
{	 
    int cpu;

    /* cancel the hotplug work when the screen is off and flush the WQ */
    cancel_delayed_work_sync(&decide_hotplug);
    flush_workqueue(wq);

    pr_info("Early Suspend stopping Hotplug work...\n");
    
    for_each_online_cpu(cpu) 
    {
        if (cpu) 
            cpu_down(cpu);
    }

    
    /* cap max frequency to 702MHz by default */
    msm_cpufreq_set_freq_limits(0, MSM_CPUFREQ_NO_LIMIT, 
            stats.suspend_frequency);
    pr_info("Cpulimit: Early suspend - limit cpu%d max frequency to: %dMHz\n",
            0, stats.suspend_frequency/1000);
}

static void __cpuinit mako_hotplug_late_resume(struct early_suspend *handler)
{  
    int cpu;

    /* online all cores when the screen goes online */
    for_each_possible_cpu(cpu) 
    {
        if (cpu) 
            cpu_up(cpu);
    }

    stats.counter[0] = 0;
    stats.counter[1] = 0;
    
    /* restore max frequency */
    msm_cpufreq_set_freq_limits(0, MSM_CPUFREQ_NO_LIMIT, MSM_CPUFREQ_NO_LIMIT);
    pr_info("Cpulimit: Late resume - restore cpu%d max frequency.\n", 0);
    
    pr_info("Late Resume starting Hotplug work...\n");
    queue_delayed_work(wq, &decide_hotplug, HZ);
}

static struct early_suspend mako_hotplug_suspend =
{
    .level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1,
	.suspend = mako_hotplug_early_suspend,
	.resume = mako_hotplug_late_resume,
};

/* sysfs functions for external driver */
void update_first_level(unsigned int level)
{
    stats.default_first_level = level;
}

void update_suspend_frequency(unsigned int freq)
{
    stats.suspend_frequency = freq;
}

void update_cores_on_touch(unsigned int num)
{
    stats.cores_on_touch = num;
}

unsigned int get_first_level()
{
    return stats.default_first_level;
}

unsigned int get_suspend_frequency()
{
    return stats.suspend_frequency;
}

unsigned int get_cores_on_touch()
{
    return stats.cores_on_touch;
}
/* end sysfs functions from external driver */

int __init mako_hotplug_init(void)
{
	pr_info("Mako Hotplug driver started.\n");
    
    /* init everything here */
    stats.default_first_level = DEFAULT_FIRST_LEVEL;
    stats.suspend_frequency = DEFAULT_SUSPEND_FREQ;
    stats.cores_on_touch = DEFAULT_CORES_ON_TOUCH;
    stats.counter[0] = 0;
    stats.counter[1] = 0;

    wq = alloc_workqueue("mako_hotplug_workqueue", 
                    WQ_UNBOUND | WQ_RESCUER | WQ_FREEZABLE, 1);
    
    if (!wq)
        return -ENOMEM;
    
    INIT_DELAYED_WORK(&decide_hotplug, decide_hotplug_func);
    queue_delayed_work(wq, &decide_hotplug, HZ*25);
    
    register_early_suspend(&mako_hotplug_suspend);
    
    return 0;
}
late_initcall(mako_hotplug_init);

