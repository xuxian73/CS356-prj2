#include "sched.h"

#include <linux/slab.h>

#define WRR_DEBUG
/* Never need impl */
#ifdef CONFIG_SMP
static int
select_task_rq_wrr(struct task_struct *p, int sd_flag, int flags) {return 0;}

static void rq_online_wrr(struct rq *rq) {}

static void rq_offline_wrr(struct rq *rq) {}

static void pre_schedule_wrr(struct rq *rq, struct task_struct *prev) {}

static void post_schedule_wrr(struct rq *rq) {}

static void task_woken_wrr(struct rq *rq, struct task_struct *p) {}

static void switched_from_wrr(struct rq *rq, struct task_struct *p) {}
#endif

/* Init wrr rq */
void
init_wrr_rq(struct wrr_rq *wrr_rq, struct rq *rq) {
	INIT_LIST_HEAD(&wrr_rq->queue);
	wrr_rq->wrr_nr_running = 0;
	wrr_rq->total_weight = 0;
}

/* update current task runtime, skip task that are not in wrr class*/
/* Required */
static void
update_curr_wrr(struct rq*rq) {
	struct task_struct *curr = rq->curr;
	u64 delta_exec;

	if (curr->sched_class != &wrr_sched_class)
		return;
	
	delta_exec = rq->clock_task - curr->se.exec_start;
	if (unlikely((s64)delta_exec < 0))
		delta_exec = 0;
	
	schedstat_set(curr->se.statistics.exec_max,
		      max(curr->se.statistics.exec_max, delta_exec));

	curr->se.sum_exec_runtime += delta_exec;
	account_group_exec_runtime(curr, delta_exec);

	curr->se.exec_start = rq->clock_task;
	cpuacct_charge(curr, delta_exec);
}
static void 
switched_to_wrr(struct rq *rq, struct task_struct *p)
{
	#ifdef WRR_DEBUG
	printk("switched_to_wrr\n");
	#endif
	if (p->on_rq && rq->curr != p)
		if (rq == task_rq(p))
			resched_task(rq->curr);
}

static void 
enqueue_wrr_entity(struct rq *rq, struct sched_wrr_entity *wrr_se, bool head)
{
	struct list_head* q = &(rq->wrr.queue);
	if (head)
		list_add(&wrr_se->run_list, q);
	else
		list_add_tail(&wrr_se->run_list, q);

	rq->wrr.wrr_nr_running++;
}

static void
enqueue_task_wrr(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_wrr_entity *wrr_se = &p->wrr;

	if (flags & ENQUEUE_WAKEUP)
		wrr_se->timeout = 0;

	enqueue_wrr_entity(rq, wrr_se, flags & ENQUEUE_HEAD);

	inc_nr_running(rq);
}

static void
dequeue_wrr_entity(struct rq* rq, struct sched_wrr_entity *wrr_se, int flags)
{
	list_del_init(&wrr_se->run_list);
	rq->wrr.wrr_nr_running--;
}
static void 
dequeue_task_wrr(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_wrr_entity *wrr_se = &p->wrr;
	update_curr_wrr(rq);
	dequeue_wrr_entity(rq, wrr_se, flags);
	dec_nr_running(rq);
}

/* put the first entity to the tail of run queue */
static void
requeue_task_wrr(struct rq* rq, struct task_struct* p, int head)
{
	struct sched_wrr_entity* wrr_se = &p->wrr;
	struct list_head *queue = &(rq->wrr.queue);
	// #ifdef WRR_DEBUG
	// printk("requeue task: %d\n", p->pid);
	// #endif
	
	if (head)
		list_move(&wrr_se->run_list, queue);
	else
		list_move_tail(&wrr_se->run_list, queue);
}
static void 
yield_task_wrr(struct rq *rq)
{
	requeue_task_wrr(rq, rq->curr, 0);
}

/* preempt the current task with a newly woken task if needed */
static void 
check_preempt_curr_wrr(struct rq *rq, struct task_struct *p, int flags)
{

}

static struct task_struct *
pick_next_task_wrr(struct rq *rq)
{
	struct sched_wrr_entity* head;
	struct task_struct* task;

	if (!rq->wrr.wrr_nr_running)
		return NULL;

	head = list_first_entry(&rq->wrr.queue, struct sched_wrr_entity, run_list);
	task = container_of(head, struct task_struct, wrr);

	if (!task) return NULL;
	task->se.exec_start = rq->clock_task;

	return task;
}

static void 
put_prev_task_wrr(struct rq *rq, struct task_struct *p)
{
	update_curr_wrr(rq);
	p->se.exec_start = 0;
}

static void 
set_curr_task_wrr(struct rq *rq)
{
	struct task_struct* p = rq->curr;
	#ifdef WRR_DEBUG
	printk("set_curr_task_wrr(WRR)\n");
	#endif
	p->se.exec_start =  rq->clock_task;
}

static unsigned int 
get_rr_interval_wrr(struct rq *rq, struct task_struct *task)
{
	struct task_group* group = task->sched_task_group;
	char path[64];

	#ifdef WRR_DEBUG
	printk("get_rr_interval_wrr(WRR)\n");
	#endif

	if (task->policy == SCHED_WRR) {
		if (!autogroup_path(group, path, 64))
		{
			if (!group->css.cgroup) {
				path[0] = '\0';
				return 0;
			}
			cgroup_path(group->css.cgroup, path, 64);
		}
		#ifdef WRR_DEBUG
		printk("GOURP_PATH: %s\n", path);
		#endif
		if (path[1] == 'b')
			return WRR_BG_TIMESLICE;
		else {
			printk("return %d\n", WRR_FG_TIMESLICE);
			return WRR_FG_TIMESLICE;
		}
	}
	else return 0;	
}

static void 
task_tick_wrr(struct rq *rq, struct task_struct *p, int queued)
{
	struct sched_wrr_entity *wrr_se = &p->wrr;
	struct task_group *g = p->sched_task_group;
	char path[64];
	unsigned int time_slice;
	update_curr_wrr(rq);
	
	if (!autogroup_path(g, path, 64))
	{
		if (!g->css.cgroup) {
			path[0] = '\0';
			return;
		}
		cgroup_path(g->css.cgroup, path, 64);
	}
	
	#ifdef WRR_DEBUG
	printk("task_tick_wrr: task_group %s\n", path);
	printk("cpu: %d task_tick: %d, time_slice: %d, sched_class: %d\n",
		cpu_of(rq),
		p->pid,
		p->wrr.time_slice,
		(int)p->sched_class);
	time_slice = get_rr_interval_wrr(rq, p);
	#endif
	if (--p->wrr.time_slice)
		return;

	if (path[1] == 'b') {
		p->wrr.time_slice = WRR_BG_TIMESLICE;
		printk("FG_TIMESLICE: %d\n", WRR_FG_TIMESLICE);
		printk("BG_TIMESLICE: %d\n", WRR_BG_TIMESLICE);
	}
	else {
		p->wrr.time_slice = WRR_FG_TIMESLICE;
		printk("FG_TIMESLICE: %d\n", WRR_FG_TIMESLICE);
		printk("BG_TIMESLICE: %d\n", WRR_BG_TIMESLICE);
	}
	if (wrr_se->run_list.prev != wrr_se->run_list.next) {
		requeue_task_wrr(rq, p, 0);
		resched_task(p);
	}	
}

static void
prio_changed_wrr(struct rq *rq, struct task_struct *p, int oldprio)
{
	
}


const struct sched_class wrr_sched_class = {
	.next			= &fair_sched_class,		/* Required */
	.enqueue_task		= enqueue_task_wrr,		/* Required */
	.dequeue_task		= dequeue_task_wrr,		/* Required */
	.yield_task		= yield_task_wrr,			/* Required */

	.check_preempt_curr	= check_preempt_curr_wrr,/* Required */

	.pick_next_task		= pick_next_task_wrr,	/* Required */
	.put_prev_task		= put_prev_task_wrr,	/* Required */

#ifdef CONFIG_SMP
	.select_task_rq		= select_task_rq_wrr,		/* Never need impl */

	.set_cpus_allowed       = set_cpus_allowed_wrr,	
	.rq_online              = rq_online_wrr,		/* Never need impl */
	.rq_offline             = rq_offline_wrr,		/* Never need impl */
	.pre_schedule		= pre_schedule_wrr,			/* Never need impl */
	.post_schedule		= post_schedule_wrr,		/* Never need impl */
	.task_woken		= task_woken_wrr,				/* Never need impl */
	.switched_from		= switched_from_wrr,		/* Never need impl */
#endif

	.set_curr_task          = set_curr_task_wrr,/* Required */
	.task_tick		= task_tick_wrr,			/* Required */
	.get_rr_interval	= get_rr_interval_wrr,
	.prio_changed		= prio_changed_wrr,		/* Required */
	.switched_to		= switched_to_wrr,		/* Required */
};

// #ifdef CONFIG_SCHED_DEBUG
// extern void print_rt_rq(struct seq_file *m, int cpu, struct rt_rq *rt_rq);

// void print_rt_stats(struct seq_file *m, int cpu)
// {
// 	rt_rq_iter_t iter;
// 	struct rt_rq *rt_rq;

// 	rcu_read_lock();
// 	for_each_rt_rq(rt_rq, iter, cpu_rq(cpu))
// 		print_rt_rq(m, cpu, rt_rq);
// 	rcu_read_unlock();
// }
