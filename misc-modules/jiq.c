/*
 * jiq.c -- the just-in-queue module
 *
 * Copyright (C) 2001 Alessandro Rubini and Jonathan Corbet
 * Copyright (C) 2001 O'Reilly & Associates
 *
 * The source code in this file can be freely used, adapted,
 * and redistributed in source or binary form, so long as an
 * acknowledgment appears in derived source files.  The citation
 * should list that the code comes from the book "Linux Device
 * Drivers" by Alessandro Rubini and Jonathan Corbet, published
 * by O'Reilly & Associates.   No warranty is attached;
 * we cannot take responsibility for errors or fitness for use.
 *
 * $Id: jiq.c,v 1.7 2004/09/26 07:02:43 gregkh Exp $
 */
 
#include <linux/string.h>
#include <linux/seq_file.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/time.h>
#include <linux/timer.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/fs.h>     /* everything... */
#include <linux/proc_fs.h>
#include <linux/errno.h>  /* error codes */
#include <linux/workqueue.h>
#include <linux/preempt.h>
#include <linux/interrupt.h> /* tasklets */

MODULE_AUTHOR("Dan Nakahara");
MODULE_LICENSE("Dual BSD/GPL");

/*
 * The delay for the delayed workqueue timer file.
 */
static long delay = 1;
module_param(delay, long, 0);


/*
 * This module is a silly one: it only embeds short code fragments
 * that show how enqueued tasks `feel' the environment
 */
#define LIMIT	(PAGE_SIZE-128)	/* don't print any more after this size */

/*
 * Print information about the current environment. This is called from
 * within the task queues. If the limit is reched, awake the reading
 * process.
 */
static DECLARE_WAIT_QUEUE_HEAD (jiq_wait);


static struct work_struct jiq_work;


/*
 * Keep track of info we need between task queue runs.
 */
static struct clientdata {
    struct seq_file *sq_file;
	unsigned long jiffies;
	long delay;
} jiq_data;

#define SCHEDULER_QUEUE ((task_queue *) 1)


static void jiq_print_tasklet(unsigned long);
static DECLARE_TASKLET(jiq_tasklet, jiq_print_tasklet, (unsigned long)&jiq_data);


/*
 * Do the printing; return non-zero if the task should be rescheduled.
 */
static int jiq_print(void *ptr)
{
	struct clientdata *data = ptr;
	struct seq_file *file = data->sq_file;
    int size = file->size;
	unsigned long j = jiffies;

	if (size > LIMIT) { 
		wake_up_interruptible(&jiq_wait);
		return 0;
	}

	if (size == 0)
		seq_printf(file, "    time  delta preempt   pid cpu command\n");

  	/* intr_count is only exported since 1.3.5, but 1.99.4 is needed anyways */
	seq_printf(file, "%9li  %4li     %3i %5i %3i %s\n",
			j, j - data->jiffies,
			preempt_count(), current->pid, smp_processor_id(),
			current->comm);

	data->jiffies = j;
	return 1;
}


/*
 * Call jiq_print from a work queue
 */
static void jiq_print_wq(void *ptr)
{
	struct clientdata *data = (struct clientdata *) ptr;
    
	if (! jiq_print (ptr))
		return;
    
	if (data->delay)
		schedule_delayed_work(&jiq_work, data->delay);
	else
		schedule_work(&jiq_work);
}


static int jiqwq_show(struct seq_file *file, void *v)
{
	DEFINE_WAIT(wait);
	
	jiq_data.jiffies = jiffies;      /* initial time */
	jiq_data.delay = 0;
    jiq_data.sq_file = file;
    
	prepare_to_wait(&jiq_wait, &wait, TASK_INTERRUPTIBLE);
	schedule_work(&jiq_work);
	schedule();
	finish_wait(&jiq_wait, &wait);

	return 0;
}


static int jiqwqdelayed_show(struct seq_file *file, void *v)
{
	DEFINE_WAIT(wait);
	
	jiq_data.jiffies = jiffies;      /* initial time */
	jiq_data.delay = delay;
    jiq_data.sq_file = file;
    
	prepare_to_wait(&jiq_wait, &wait, TASK_INTERRUPTIBLE);
	schedule_delayed_work(&jiq_work, delay);
	schedule();
	finish_wait(&jiq_wait, &wait);

	return 0;
}


/*
 * Call jiq_print from a tasklet
 */
static void jiq_print_tasklet(unsigned long ptr)
{
	if (jiq_print ((void *) ptr))
		tasklet_schedule (&jiq_tasklet);
}


static int jiqtasklet_show(struct seq_file *file, void *v)
{
	jiq_data.jiffies = jiffies;      /* initial time */
    jiq_data.sq_file = file;

	tasklet_schedule(&jiq_tasklet);
	interruptible_sleep_on(&jiq_wait);    /* sleep till completion */

	return 0;
}


/*
 * This one, instead, tests out the timers.
 */
static struct timer_list jiq_timer;

static void jiq_timedout(unsigned long ptr)
{
	jiq_print((void *)ptr);            /* print a line */
	wake_up_interruptible(&jiq_wait);  /* awake the process */
}


static int jiqruntimer_show(struct seq_file *file, void *v)
{

	jiq_data.jiffies = jiffies;
    jiq_data.sq_file = file;

	init_timer(&jiq_timer);              /* init the timer structure */
	jiq_timer.function = jiq_timedout;
	jiq_timer.data = (unsigned long)&jiq_data;
	jiq_timer.expires = jiffies + HZ; /* one second */

	jiq_print(&jiq_data);   /* print and go to sleep */
	add_timer(&jiq_timer);
	interruptible_sleep_on(&jiq_wait);  /* RACE */
	del_timer_sync(&jiq_timer);  /* in case a signal woke us up */
    
	return 0;
}


#define BUILD_JIQ_PROC_OPEN(type) \
    static int type##_proc_open(struct inode *inode, struct file *file) \
    {   \
        return single_open(file, type##_show, NULL);    \
    }

/*
 * Now to implement the /proc file we need only make an open
 * method which sets up the sequence operators.
 */
BUILD_JIQ_PROC_OPEN(jiqwq)
BUILD_JIQ_PROC_OPEN(jiqwqdelayed)
BUILD_JIQ_PROC_OPEN(jiqruntimer)
BUILD_JIQ_PROC_OPEN(jiqtasklet)


#define BUILD_JIQ_PROC_OPS(type) \
    static struct file_operations type##_proc_ops = {\
        .owner   = THIS_MODULE,         \
        .open    = type##_proc_open,    \
        .read    = seq_read,            \
        .llseek  = seq_lseek,           \
        .release = single_release,      \
    };


/*
 * Create a set of file operations for our proc file.
 */
BUILD_JIQ_PROC_OPS(jiqwq)
BUILD_JIQ_PROC_OPS(jiqwqdelayed)
BUILD_JIQ_PROC_OPS(jiqruntimer)
BUILD_JIQ_PROC_OPS(jiqtasklet)

/*
 * the init/clean material
 */
static int jiq_init(void)
{

	/* this line is in jiq_init() */
	INIT_WORK(&jiq_work, jiq_print_wq, &jiq_data);

	proc_create("jiqwq", 0, NULL, &jiqwq_proc_ops);
	proc_create("jiqwqdelay", 0, NULL, &jiqwqdelayed_proc_ops);
	proc_create("jitimer", 0, NULL, &jiqruntimer_proc_ops);
	proc_create("jiqtasklet", 0, NULL, &jiqtasklet_proc_ops);

	return 0; /* succeed */
}

static void jiq_cleanup(void)
{
	remove_proc_entry("jiqwq", NULL);
	remove_proc_entry("jiqwqdelay", NULL);
	remove_proc_entry("jitimer", NULL);
	remove_proc_entry("jiqtasklet", NULL);
}


module_init(jiq_init);
module_exit(jiq_cleanup);