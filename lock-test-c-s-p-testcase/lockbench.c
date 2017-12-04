#include <linux/init.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/percpu.h>
#include <linux/kallsyms.h>
#include <linux/delay.h>
#include <asm/delay.h>

#include "lockbench_trace.h"

struct thread_data {
	struct task_struct *task;
	int out_lock;
};

static int threads_num = 62;
module_param(threads_num, int, 0);

static int c_time = 50;
module_param(c_time, int, 0);

static int s_tests = 0;
module_param(s_tests, int, S_IRUGO|S_IWUSR);

unsigned int test_threads = 0;
unsigned int p_to_s = 1;
//the ciruclation time of p_to_s is (max_p_to_s-1)
unsigned int max_p_to_s = 32;
//the circulation time of s_tests is max_s_tests
unsigned int max_s_tests = 5;

static int test_done = 0;
module_param(test_done, int, S_IRUGO|S_IWUSR);

//s_time：serial process time
//p_time: parallel process time
unsigned long s_time = 0;
unsigned long p_time =0;
unsigned long spin_time = 0;

//core number change flag
bool b_coreNumChange = false;
//parallel process time change flag
bool b_pChange = false;
//monitor end flag
bool b_monitorEnd = false;

static struct spinlock test_spinlock;

static atomic_t threads_come;
static atomic_t threads_left;
static DECLARE_COMPLETION(threads_done);

static int snap_over;
static int snap_exit;
static DECLARE_COMPLETION(snap_start);
static DECLARE_COMPLETION(snap_done);

DEFINE_PER_CPU(struct thread_data, thread_datas);

struct task_struct *(*kthread_create_on_cpu_ptr)(int (*threadfn)(void *data),
		void *data, unsigned int cpu,
		const char *namefmt);
int (*sched_setscheduler_nocheck_ptr)(struct task_struct *p, int policy,
		const struct sched_param *param);

static int thread_fn(void *arg)
{
	struct thread_data *td = (struct thread_data *)arg;
	//unsigned long i = 0;
	//unsigned long outlock_num;

	td->out_lock = 1;
	if (atomic_dec_and_test(&threads_come)) {
		/* sync to snap to begin */
		snap_over = 0;
		complete(&snap_start);
	}

	while (1) {
		td->out_lock = 0;
		//atomic_dec(&out_lock);
		spin_lock(&test_spinlock);

		ndelay(s_time);

		spin_unlock(&test_spinlock);
		td->out_lock = 1;
		//outlock_num = atomic_inc_return(&out_lock);

		ndelay(p_time);

		if (b_coreNumChange)
			break;
	}

	if (atomic_dec_and_test(&threads_left)) {
		/* sync to snap to over */
		snap_over = 1;
		wait_for_completion(&snap_done);
		reinit_completion(&snap_done);

		complete(&threads_done);
	}
	do_exit(0);
	return 0;
}

static unsigned long all_num = 0;
static unsigned long all_times = 0;

static int snap(void *unused)
{
	int i;
	unsigned int num;
	
	
	wait_for_completion(&snap_start);
	printk("start snap");
	while (1) {
		/* all threads begin */
		if(b_coreNumChange){
			if((s_tests==max_s_tests-1)&&
			(p_to_s==max_p_to_s)&&
			(test_threads==threads_num)){
				printk("stop snap");
				break;
			}
			wait_for_completion(&snap_start);
			reinit_completion(&snap_start);
			printk("change thread num");
		}
		
		//unsigned long snap_wait_time = spin_time * 5 / 1000;
		
		while (!READ_ONCE(snap_over)) {
			if(b_pChange){
				all_num = all_times = 0;
				b_pChange = false;
				//snap_wait_time = spin_time * 5 / 1000;
				mdelay(10);
			}
			
			num = 0;
			for (i = 0; i < test_threads; i++)
				if (per_cpu(thread_datas, i).out_lock)
					num++;
			all_num += num;
			all_times++;
			
			
			mdelay(10);
		}

		/* tell master to go on */
		complete(&snap_done);
		if (READ_ONCE(snap_exit))
			break;
	}
	do_exit(0);
	return 0;
}


static int s_tests_v[] = {20, 10, 5, 2, 1};

static int monitor(void *unused)
{
	struct thread_data *td;
	int ret = 0, i, have_one_thread = 0;
	struct sched_param param = {.sched_priority = 1};
	
	printk("monitor test start\n");
	
	while(s_tests < max_s_tests)  {
		test_threads = 0;
		while(test_threads < threads_num){
			p_to_s=1;
			s_time = c_time * s_tests_v[s_tests];
			p_time = s_time * 1000 / p_to_s;
			spin_time = s_time+p_time;

			b_pChange = false;
			b_coreNumChange = false;
			reinit_completion(&threads_done);
				
			spin_lock_init(&test_spinlock);
			test_threads++;
			atomic_set(&threads_left, test_threads);
			atomic_set(&threads_come, test_threads);

			for (i = 0; i < test_threads; i++) {
				td = &per_cpu(thread_datas, i);
				td->task = kthread_create_on_node(thread_fn, td, cpu_to_node(i),
						"lockbench-%d", i);
				if (IS_ERR(td->task)) {
					ret = 1;
					atomic_set(&threads_left, i);
					atomic_set(&threads_come, i);
					goto error_out;
				}
				kthread_bind(td->task, i);

				ret = sched_setscheduler(td->task, SCHED_FIFO, &param);
				if (ret) {
					ret = 1;
					atomic_set(&threads_left, i + 1);
					atomic_set(&threads_come, i + 1);
					goto error_out;
				}
			}

			for (i = 0; i < test_threads; i++) {
				td = &per_cpu(thread_datas, i);
				wake_up_process(td->task);
			}
			
			/* from 1 to max*/
			while(p_to_s<max_p_to_s) {
				//test_threads = 0;
				p_time = s_time * 1000 / p_to_s;
				spin_time = p_time + s_time;
				//wait time unit is us, wait 100 circulation time of spin
				//unsigned long monitor_wait_time = spin_time * 100/1000;
				
				b_pChange = true;
				
				mdelay(200);
				
				/* print this test result */
				printk("lockbench: %d %d %d %ld %ld %ld\n",
				test_threads,
				s_tests, p_to_s,
				all_num, all_times, all_num * 10/all_times);
				
				p_to_s++;
			}

			/* wait for work done */
			b_coreNumChange = true;
			wait_for_completion(&threads_done);
			
			//printk("threads_num: %d, threads_num: %d", test_threads, threads_num);
		}
		/* from 0.05 to 1 */
		s_tests++;
	}
	
	

error_out:
	if (ret) {
		printk("lockbench: test failed\n");
		for (i = 0; i < test_threads; i++) {
			td = &per_cpu(thread_datas, i);
			if (IS_ERR(td->task))
				break;
			have_one_thread = 1;
			wake_up_process(td->task);
		}
		if (have_one_thread) {
			/* let snap task exit */
			snap_exit = 1;

			wait_for_completion(&threads_done);
			reinit_completion(&threads_done);
		} else {
			snap_over = snap_exit = 1;
			complete(&snap_start);
			wait_for_completion(&snap_done);
		}
		test_done = -1;
	} else
		test_done = 1;

	module_put(THIS_MODULE);
	printk("monitor test end\n");
	do_exit(0);
	return 0;
}

static __init int lockbench_init(void)
{
static struct task_struct *monitor_task;
static struct task_struct *snap_task;
static int monitor_cpu;
static int snap_cpu;

	int ret;
	struct sched_param param = {.sched_priority = 2};

	if (threads_num > num_online_cpus() - 2)
		return -EINVAL;
	monitor_cpu = threads_num;
	snap_cpu = threads_num + 1;

	monitor_task = kthread_create_on_node(monitor, NULL, cpu_to_node(monitor_cpu),
		"monitor-%d", monitor_cpu);
	if (IS_ERR(monitor_task))
		return PTR_ERR(monitor_task);
	
	//printk("create snap");
	snap_task = kthread_create_on_node(snap, NULL, cpu_to_node(snap_cpu),
		"snap-%d", snap_cpu);
	if (IS_ERR(snap_task))
		return PTR_ERR(snap_task);

	kthread_bind(monitor_task, monitor_cpu);
	kthread_bind(snap_task, snap_cpu);

	ret = sched_setscheduler(monitor_task, SCHED_FIFO, &param);
	if (ret)
		return ret;

	ret = sched_setscheduler(snap_task, SCHED_FIFO, &param);
	if (ret)
		return ret;

	__module_get(THIS_MODULE);
	wake_up_process(monitor_task);
	//printk("wake up snap");
	mdelay(50);
	wake_up_process(snap_task);
	return 0;
}

static __exit void lockbench_exit(void)
{
}

module_init(lockbench_init);
module_exit(lockbench_exit);
MODULE_LICENSE("GPL");
