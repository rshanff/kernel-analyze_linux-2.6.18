/*
 *  Block device elevator/IO-scheduler.
 *
 *  Copyright (C) 2000 Andrea Arcangeli <andrea@suse.de> SuSE
 *
 * 30042000 Jens Axboe <axboe@suse.de> :
 *
 * Split the elevator a bit so that it is possible to choose a different
 * one or even write a new "plug in". There are three pieces:
 * - elevator_fn, inserts a new request in the queue list
 * - elevator_merge_fn, decides whether a new buffer can be merged with
 *   an existing request
 * - elevator_dequeue_fn, called when a request is taken off the active list
 *
 * 20082000 Dave Jones <davej@suse.de> :
 * Removed tests for max-bomb-segments, which was breaking elvtune
 *  when run without -bN
 *
 * Jens:
 * - Rework again to work with bio instead of buffer_heads
 * - loose bi_dev comparisons, partition handling is right now
 * - completely modularize elevator setup and teardown
 *
 */
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/compiler.h>
#include <linux/delay.h>
#include <linux/blktrace_api.h>

#include <asm/uaccess.h>

static DEFINE_SPINLOCK(elv_list_lock);
static LIST_HEAD(elv_list);

/*
 * can we safely merge with this request?
 */
/**ltl
 * 功能:从request和bio自身属性判定是否可以合并。
 */
inline int elv_rq_merge_ok(struct request *rq, struct bio *bio)
{
	/* 1.request对象是否设置了不可合并标志 */
	if (!rq_mergeable(rq))
		return 0;

	/*
	 * different data direction or already started, don't merge
	 */
	 /* 2.bio与request的请求方向是否一至 */
	if (bio_data_dir(bio) != rq_data_dir(rq))
		return 0;

	/*
	 * same device and no special stuff set, merge is ok
	 */
	 /*  3.1.对同一个磁盘的请求;
	     3.2.request是来自应用进程(在init_request_from_bio中设置rq->waiting=NULL);
	     3.3.request还没有派发到底层驱动(special还没有关联scsi_cmnd对象)。*/
	if (rq->rq_disk == bio->bi_bdev->bd_disk &&
	    !rq->waiting && !rq->special)
		return 1;

	return 0;
}
EXPORT_SYMBOL(elv_rq_merge_ok);
/**ltl
 * 功能: 判定bio是否可能并入到rq请求
 * 参数: __rq	-> 欲被合入的request对象
 *		bio	-> 欲合入的bio对象
 * 返回值:
 * 说明:
 */
static inline int elv_try_merge(struct request *__rq, struct bio *bio)
{
	int ret = ELEVATOR_NO_MERGE;

	/*
	 * we can merge and sequence is ok, check if it's possible
	 */
	if (elv_rq_merge_ok(__rq, bio)) {/* bio符合合并的条件下 */
		/* request的最后一个扇区刚好是bio的起始扇区,则把bio合到request中的bio链表尾 */
		if (__rq->sector + __rq->nr_sectors == bio->bi_sector)
			ret = ELEVATOR_BACK_MERGE;
		/* request的起始扇区刚好是bio的最后扇区，则把bio合到request的bio链表头 */
		else if (__rq->sector - bio_sectors(bio) == bio->bi_sector)
			ret = ELEVATOR_FRONT_MERGE;
	}

	return ret;
}
/**ltl
 * 功能:根据调度算法的名字在全局列表elv_list中查找调度算法对象
 */
static struct elevator_type *elevator_find(const char *name)
{
	struct elevator_type *e = NULL;
	struct list_head *entry;
	/* 各个调度算法初始化时，都会创建一个elevator_type对象并添加到全局列表elv_list中。*/
	list_for_each(entry, &elv_list) {
		struct elevator_type *__e;

		__e = list_entry(entry, struct elevator_type, list);

		if (!strcmp(__e->elevator_name, name)) {
			e = __e;
			break;
		}
	}

	return e;
}

static void elevator_put(struct elevator_type *e)
{
	module_put(e->elevator_owner);
}
/**ltl
 * 功能: 根据名字获取相应的调度算法对象
 * 参数: name	->调度算法名
 * 返回值:
 * 说明:
 */
static struct elevator_type *elevator_get(const char *name)
{
	struct elevator_type *e;

	spin_lock_irq(&elv_list_lock);

	e = elevator_find(name);
	if (e && !try_module_get(e->elevator_owner))
		e = NULL;

	spin_unlock_irq(&elv_list_lock);

	return e;
}
/**ltl
 * 功能:初始化IO调度器
 * 参数:q	->请求队列
 *	eq	->IO调度器对象
 * 返回值:
 */
static void *elevator_init_queue(request_queue_t *q, struct elevator_queue *eq)
{
	return eq->ops->elevator_init_fn(q, eq);
}
/**ltl
 * 功能:关联IO调度器与请求队列
 * 参数:q	->请求队列
 *	eq	->IO调度器对象
 * 返回值:
 */
static void elevator_attach(request_queue_t *q, struct elevator_queue *eq,
			   void *data)
{
	q->elevator = eq;
	eq->elevator_data = data;
}

static char chosen_elevator[16];
/* 解析cmdline中的elevator=字段 */
static int __init elevator_setup(char *str)
{
	/*
	 * Be backwards-compatible with previous kernels, so users
	 * won't get the wrong elevator.
	 */
	if (!strcmp(str, "as"))
		strcpy(chosen_elevator, "anticipatory");
	else
		strncpy(chosen_elevator, str, sizeof(chosen_elevator) - 1);
	return 1;
}

__setup("elevator=", elevator_setup);

static struct kobj_type elv_ktype;
/**ltl
 * 功能: 根据调度算法获取调度队列对象
 * 参数:
 * 返回值:
 * 说明:
 */
static elevator_t *elevator_alloc(struct elevator_type *e)
{
	elevator_t *eq = kmalloc(sizeof(elevator_t), GFP_KERNEL);
	if (eq) {
		memset(eq, 0, sizeof(*eq));
		eq->ops = &e->ops;
		eq->elevator_type = e;
		kobject_init(&eq->kobj);
		snprintf(eq->kobj.name, KOBJ_NAME_LEN, "%s", "iosched");
		eq->kobj.ktype = &elv_ktype;
		mutex_init(&eq->sysfs_lock);
	} else {
		elevator_put(e);
	}
	return eq;
}

static void elevator_release(struct kobject *kobj)
{
	elevator_t *e = container_of(kobj, elevator_t, kobj);
	elevator_put(e->elevator_type);
	kfree(e);
}
/**ltl
 * 功能:把名字为name的IO调度器与请求队列q关联
 * 参数:q	->请求队列
 *	name	->IO调度器名字
 * 返回值: 0	->关联成功
 *	 <0	->关联失败
 */
int elevator_init(request_queue_t *q, char *name)
{
	struct elevator_type *e = NULL;
	struct elevator_queue *eq;
	int ret = 0;
	void *data;
	/* 初始化派发队列头 */
	INIT_LIST_HEAD(&q->queue_head);
	q->last_merge = NULL;
	q->end_sector = 0;
	q->boundary_rq = NULL;

	/* 根据name获取IO调度器对象 */
	if (name && !(e = elevator_get(name)))
		return -EINVAL;

	/* 在cmdline设置elevator=cfg，则根据这个名字获取IO调度器名字 */
	if (!e && *chosen_elevator && !(e = elevator_get(chosen_elevator)))
		printk("I/O scheduler %s not found\n", chosen_elevator);

	/* 获取在config文件中指定的IO调度器名字 */
	if (!e && !(e = elevator_get(CONFIG_DEFAULT_IOSCHED))) {
		printk("Default I/O scheduler not found, using no-op\n");
		e = elevator_get("noop");
	}

	/* 分配一个调度队列 */
	eq = elevator_alloc(e);
	if (!eq)
		return -ENOMEM;

	/* 调用具体的IO调度算法的初始化接口分配IO调度器的私有数据 */
	data = elevator_init_queue(q, eq);
	if (!data) {
		kobject_put(&eq->kobj);
		return -ENOMEM;
	}
	/* 请求队列与调度器关联 */
	elevator_attach(q, eq, data);
	return ret;
}

void elevator_exit(elevator_t *e)
{
	mutex_lock(&e->sysfs_lock);
	if (e->ops->elevator_exit_fn)
		e->ops->elevator_exit_fn(e);
	e->ops = NULL;
	mutex_unlock(&e->sysfs_lock);

	kobject_put(&e->kobj);
}

/*
 * Insert rq into dispatch queue of q.  Queue lock must be held on
 * entry.  If sort != 0, rq is sort-inserted; otherwise, rq will be
 * appended to the dispatch queue.  To be used by specific elevators.
 */
/**ltl
 * 功能:把rq插入到请求队列q中。
 * 参数:q	->请求队列
 *	rq	->请求对象
 */
void elv_dispatch_sort(request_queue_t *q, struct request *rq)
{
	sector_t boundary;
	struct list_head *entry;

	if (q->last_merge == rq)
		q->last_merge = NULL;
	q->nr_sorted--;

	boundary = q->end_sector;

	list_for_each_prev(entry, &q->queue_head) {/* 遍历请求队列 */
		struct request *pos = list_entry_rq(entry);
		/* 如果pos已经执行或者是一个屏障IO的话，则直接在后面插入 */
		if (pos->flags & (REQ_SOFTBARRIER|REQ_HARDBARRIER|REQ_STARTED))
			break;
		if (rq->sector >= boundary) {
			if (pos->sector < boundary)
				continue;
		} else {
			if (pos->sector >= boundary)
				break;
		}
		if (rq->sector >= pos->sector)
			break;
	}
	/* 把rq插入到entry节点之后 */
	list_add(&rq->queuelist, entry);
}
/**ltl
 * 功能: 根据系统的IO调度算法，找出在请求队列中可以合入bio的request对象和要合入到request对象的列表位置
 * 参数:q	->请求队列
 *	req	->[out]可以合并的request,如果不能合并，值为空
 *	bio	->当前请求的bio对象
 */
int elv_merge(request_queue_t *q, struct request **req, struct bio *bio)
{
	elevator_t *e = q->elevator;
	int ret;

	/* 上一次合并的请求request地址。*/
	if (q->last_merge) {
		ret = elv_try_merge(q->last_merge, bio);/* 获取bio合入到请求last_merge的列表位置 */
		if (ret != ELEVATOR_NO_MERGE) {
			*req = q->last_merge;
			return ret;
		}
	}
	/* 根据系统IO调度算法，找出合入bio的request对象和合入列表的位置 */
	if (e->ops->elevator_merge_fn)
		return e->ops->elevator_merge_fn(q, req, bio);
	
	/* 说明这个bio不能合入到现在的request,而是要重新申请一个新的request。*/
	return ELEVATOR_NO_MERGE;
}

/**ltl
 * 功能:由于有一个bio插入到rq中，因此要去处理与IO调度器相关的私有数据
 * 参数:q	->请求队列
 *	rq	->请求
 */
void elv_merged_request(request_queue_t *q, struct request *rq)
{
	elevator_t *e = q->elevator;

	if (e->ops->elevator_merged_fn)
		e->ops->elevator_merged_fn(q, rq);

	q->last_merge = rq;
}

/**ltl
 * 功能:由于有一个bio插入到rq中，现在要试着去合并rq和next两个请求
 * 参数:q	->请求队列
 *	rq	->要合并的request
 *	next	->要被合并的request
 */
void elv_merge_requests(request_queue_t *q, struct request *rq,
			     struct request *next)
{
	elevator_t *e = q->elevator;

	if (e->ops->elevator_merge_req_fn)
		e->ops->elevator_merge_req_fn(q, rq, next);
	q->nr_sorted--; 

	q->last_merge = rq;
}
/**ltl
 * 功能:把request重新插入到派发队列(插入队尾)中
 * 参数:q	->请求队列
 * 	rq	->请求对象
 */
void elv_requeue_request(request_queue_t *q, struct request *rq)
{
	elevator_t *e = q->elevator;

	/*
	 * it already went through dequeue, we need to decrement the
	 * in_flight count again
	 */
	if (blk_account_rq(rq)) {
		q->in_flight--;
		if (blk_sorted_rq(rq) && e->ops->elevator_deactivate_req_fn)
			e->ops->elevator_deactivate_req_fn(q, rq);
	}
	/* 去除REQ_STARTED标志 */
	rq->flags &= ~REQ_STARTED;
	/* 把request插入到派发队列中。*/
	elv_insert(q, rq, ELEVATOR_INSERT_REQUEUE);
}
/**ltl
 * 功能:抽干IO调度器的请求，即把IO调度器中的所有请求都转移到派发队列中。
 * 参数:q	->请求队列
 */
static void elv_drain_elevator(request_queue_t *q)
{
	static int printed;
	/* 调用IO调度器中的派发处理函数,抽干IO调度器的请求 */
	while (q->elevator->ops->elevator_dispatch_fn(q, 1))
		;
	if (q->nr_sorted == 0)/* 在IO调度器中没有请求，则返回 */
		return;
	if (printed++ < 10) {
		printk(KERN_ERR "%s: forced dispatching is broken "
		       "(nr_sorted=%u), please report this\n",
		       q->elevator->elevator_type->elevator_name, q->nr_sorted);
	}
}
/**ltl
 * 功能:把request插入到请求队列
 * 参数:q->请求队列
 *	rq->请求队列
 *	where->位置
 */
void elv_insert(request_queue_t *q, struct request *rq, int where)
{
	struct list_head *pos;
	unsigned ordseq;
	int unplug_it = 1;

	blk_add_trace_rq(q, rq, BLK_TA_INSERT);

	rq->q = q;

	switch (where) {
	case ELEVATOR_INSERT_FRONT:/* 前插，一般用于scsi命令插入 */
		rq->flags |= REQ_SOFTBARRIER;

		list_add(&rq->queuelist, &q->queue_head);  /* 在派发队列头部插入 */
		break;
	/*对屏障IO的处理，抽干IO调度器中的请求，把屏障IO直插到派发队列中*/
	case ELEVATOR_INSERT_BACK:
		rq->flags |= REQ_SOFTBARRIER;
		elv_drain_elevator(q);/* 将IO调度队列的所有请求抽干,即把调度队列中的所有请求都转到派发队列中 */
		list_add_tail(&rq->queuelist, &q->queue_head); /* 在队列尾部插入 */
		/*
		 * We kick the queue here for the following reasons.
		 * - The elevator might have returned NULL previously
		 *   to delay requests and returned them now.  As the
		 *   queue wasn't empty before this request, ll_rw_blk
		 *   won't run the queue on return, resulting in hang.
		 * - Usually, back inserted requests won't be merged
		 *   with anything.  There's no point in delaying queue
		 *   processing.
		 */
		blk_remove_plug(q);/* 删除"畜流"定时器 */
		q->request_fn(q);  /* 调用底层驱动策略处理函数 */
		break;
	/*对普通IO的处理，添加到IO调度器中*/
	case ELEVATOR_INSERT_SORT:
		BUG_ON(!blk_fs_request(rq));
		rq->flags |= REQ_SORTED;
		q->nr_sorted++;/* 在把request插入到IO调度器时，+1,在把request移到派发队列或者合并两个request时，-1 */
		if (q->last_merge == NULL && rq_mergeable(rq))
			q->last_merge = rq;
		/*
		 * Some ioscheds (cfq) run q->request_fn directly, so
		 * rq cannot be accessed after calling
		 * elevator_add_req_fn.
		 */
		/* 将请求添加到IO调度器中的队列中，注:在request插入IO调度器之前，没有在往派发队列插入。*/
		q->elevator->ops->elevator_add_req_fn(q, rq);
		break;

	case ELEVATOR_INSERT_REQUEUE:/* 把request重新插入到派发队列中 */
		/*
		 * If ordered flush isn't in progress, we do front
		 * insertion; otherwise, requests should be requeued
		 * in ordseq order.
		 */
		rq->flags |= REQ_SOFTBARRIER;

		if (q->ordseq == 0) {/* 非屏障IO */
			list_add(&rq->queuelist, &q->queue_head);
			break;
		}

		ordseq = blk_ordered_req_seq(rq);

		list_for_each(pos, &q->queue_head) {
			struct request *pos_rq = list_entry_rq(pos);
			if (ordseq <= blk_ordered_req_seq(pos_rq))
				break;
		}

		list_add_tail(&rq->queuelist, pos);
		/*
		 * most requeues happen because of a busy condition, don't
		 * force unplug of the queue for that case.
		 */
		unplug_it = 0;
		break;

	default:
		printk(KERN_ERR "%s: bad insertion point %d\n",
		       __FUNCTION__, where);
		BUG();
	}
	if (unplug_it && blk_queue_plugged(q)) {
		int nrq = q->rq.count[READ] + q->rq.count[WRITE] /* count:表示从系统中申请到的request的个数 */
			- q->in_flight;/* in_flight:表示已经移到派发队列的个数 */

		if (nrq >= q->unplug_thresh/*=4*/)/* 当在IO调度器中的请求达到unplug_thresh(4)值时，就开始"泄流"，而不是等到"泄流"定时器的到来。*/
			__generic_unplug_device(q);/* 执行"泄流" */
	}
}

/**ltl
 * 功能:将request插入到请求队列中，注，一般都是以where=ELEVATOR_INSERT_SORT插入的调度器中
 * 参数:q	->请求队列
 *	rq	->请求
 *	where	->插入队列的位置标志
 *	plug	->是否"畜流"
 */
void __elv_add_request(request_queue_t *q, struct request *rq, int where,
		       int plug)
{
	
	if (q->ordcolor)/* 设置屏障请求的反转标志 */
		rq->flags |= REQ_ORDERED_COLOR;

	/* request设置的请求屏障标识 */
	if (rq->flags & (REQ_SOFTBARRIER | REQ_HARDBARRIER)) {
		/*
		 * toggle ordered color
		 */
		if (blk_barrier_rq(rq))/* 屏障请求，就反转ordcolor值 */
			q->ordcolor ^= 1;

		/*
		 * barriers implicitly indicate back insertion
		 */
		 /* 由于设置了屏障请求标志，就不能对这个request进行合并和排序，只是把这个request插入到派发队列尾部。而不经过IO调度器 */
		if (where == ELEVATOR_INSERT_SORT)
			where = ELEVATOR_INSERT_BACK;

		/*
		 * this request is scheduling boundary, update
		 * end_sector
		 */
		if (blk_fs_request(rq)) {
			q->end_sector = rq_end_sector(rq); /* 上一次请求的最后扇区编号 */
			q->boundary_rq = rq;				/* 上一次请求的request */
		}
	} 
	/* 因为在get_request中分配一个request对象时，flags设置了REQ_ELVPRIV，所以where一般都是ELEVATOR_INSERT_SORT 
	   注:没有设置REQ_ELVPRIV标志，request对象没有绑定IO调度器的私有数据，因而也就没有"把request插入到IO调度器"这一说。*/
	else if (!(rq->flags & REQ_ELVPRIV) && where == ELEVATOR_INSERT_SORT)
		where = ELEVATOR_INSERT_BACK;

	if (plug)/* 表示要"畜流" */
		blk_plug_device(q);

	elv_insert(q, rq, where);
}

void elv_add_request(request_queue_t *q, struct request *rq, int where,
		     int plug)
{
	unsigned long flags;

	spin_lock_irqsave(q->queue_lock, flags);
	__elv_add_request(q, rq, where, plug);
	spin_unlock_irqrestore(q->queue_lock, flags);
}
/*ltl
 * 功能:从派发队列中获取request
 * 参数:q->派发队列指针
 * 返回值:NULL	->获取request失败
 *	 !NULL->获取到request
 */
static inline struct request *__elv_next_request(request_queue_t *q)
{
	struct request *rq;

	while (1) {
		while (!list_empty(&q->queue_head)) {
			rq = list_entry_rq(q->queue_head.next);
			if (blk_do_ordered(q, &rq))/* 处理屏障请求 */
				return rq;
		}
		/* 如果派发队列为空的话，则就要将request请求从IO调度队列转移请求到派发队列中。 */
		if (!q->elevator->ops->elevator_dispatch_fn(q, 0))
			return NULL;
	}
}

/**ltl
 * 功能:从派发队列中获取request，如果派发队列是空的，则把IO调度器的请求转移到派发队列中。
 * 参数:q	->请求队列
 * 返回值:NULL	->获取request失败
 *	 !NULL->获取到request
 */
struct request *elv_next_request(request_queue_t *q)
{
	struct request *rq;
	int ret;

	while ((rq = __elv_next_request(q)) != NULL) {/* 从派发队列中获取request */
		if (!(rq->flags & REQ_STARTED)) {/* 没有设置"REQ_STARTED"标志 */
			elevator_t *e = q->elevator;

			/*
			 * This is the first time the device driver
			 * sees this request (possibly after
			 * requeueing).  Notify IO scheduler.
			 */
			if (blk_sorted_rq(rq) &&
			    e->ops->elevator_activate_req_fn)/* 对Deadline算法无用，对CFQ有用 */
				e->ops->elevator_activate_req_fn(q, rq);

			/*
			 * just mark as started even if we don't start
			 * it, a request that has been delayed should
			 * not be passed by new incoming requests
			 */
			rq->flags |= REQ_STARTED;/* 设置REQ_STARTED标志 */
			blk_add_trace_rq(q, rq, BLK_TA_ISSUE);
		}

		if (!q->boundary_rq || q->boundary_rq == rq) {/* Q:记录请求的最后一个扇区 */
			q->end_sector = rq_end_sector(rq);
			q->boundary_rq = NULL;
		}

		/* 表示这个request不要做在转发至硬件前的处理 */
		if ((rq->flags & REQ_DONTPREP) || !q->prep_rq_fn)
			break;
		/* 在提交给底层驱动时，先构造rq的命令。对磁盘驱动来说调用scsi_prep_fn */
		ret = q->prep_rq_fn(q, rq);
		if (ret == BLKPREP_OK) {/* 构造命令成功 */
			break;
		} else if (ret == BLKPREP_DEFER) {/* 暂时不能处理继续处理，需要将命令重新排入队列。*/
			/*
			 * the request may have been (partially) prepped.
			 * we need to keep this request in the front to
			 * avoid resource deadlock.  REQ_STARTED will
			 * prevent other fs requests from passing this one.
			 */
			rq = NULL;
			break;
		} else if (ret == BLKPREP_KILL) {/* 构造失败，没办法继续处理此命令 */
			int nr_bytes = rq->hard_nr_sectors << 9;

			if (!nr_bytes)
				nr_bytes = rq->data_len;
		
			blkdev_dequeue_request(rq);/* 从等待队列中删除 */
			rq->flags |= REQ_QUIET;/* 标志request退出标志 */
			end_that_request_chunk(rq, 0, nr_bytes);/* 调用bio->done()结束当前的bio */
			end_that_request_last(rq, 0);/* 结束request */
		} else {
			printk(KERN_ERR "%s: bad return=%d\n", __FUNCTION__,
								ret);
			break;
		}
	}

	return rq;
}

/**ltl
 * 功能:把rq从派发队列中删除
 * 参数:q	->请求队列
 *	rq	->要删除的请求对象
 */
void elv_dequeue_request(request_queue_t *q, struct request *rq)
{
	BUG_ON(list_empty(&rq->queuelist));/* 如果rq是一孤立的rq，则挂起系统 */

	list_del_init(&rq->queuelist);/* 从派发队列中删除请求 */

	/*
	 * the time frame between a request being removed from the lists
	 * and to it is freed is accounted as io that is in progress at
	 * the driver side.
	 */
	if (blk_account_rq(rq))
		q->in_flight++;/* 已经提交到底层设备(派发队列中)，但未完成处理的请求数。*/
}
/**ltl
 * 功能:判定IO调度器和派发队列是否为空
 * 参数:
 */
int elv_queue_empty(request_queue_t *q)
{
	elevator_t *e = q->elevator;

	if (!list_empty(&q->queue_head))
		return 0;

	if (e->ops->elevator_queue_empty_fn)
		return e->ops->elevator_queue_empty_fn(q);

	return 1;
}
/**ltl
 * 功能:在IO调度器中，获取与rq在请求扇区最近的下一个request
 */
struct request *elv_latter_request(request_queue_t *q, struct request *rq)
{
	elevator_t *e = q->elevator;

	if (e->ops->elevator_latter_req_fn)
		return e->ops->elevator_latter_req_fn(q, rq);
	return NULL;
}
/**ltl
 * 功能:在IO调度器中，获取与rq在请求扇区最近的前一个request
 */
struct request *elv_former_request(request_queue_t *q, struct request *rq)
{
	elevator_t *e = q->elevator;

	if (e->ops->elevator_former_req_fn)
		return e->ops->elevator_former_req_fn(q, rq);
	return NULL;
}
/**ltl
 * 功能:分配每个request对象与IO调度器相关的私有数据成员elevator_private
 */
int elv_set_request(request_queue_t *q, struct request *rq, struct bio *bio,
		    gfp_t gfp_mask)
{
	elevator_t *e = q->elevator;

	if (e->ops->elevator_set_req_fn)
		return e->ops->elevator_set_req_fn(q, rq, bio, gfp_mask);

	rq->elevator_private = NULL;
	return 0;
}
/**ltl
 * 功能:释放每个request对象与IO调度器相关的私有数据成员elevator_private
 */
void elv_put_request(request_queue_t *q, struct request *rq)
{
	elevator_t *e = q->elevator;

	if (e->ops->elevator_put_req_fn)
		e->ops->elevator_put_req_fn(q, rq);
}

int elv_may_queue(request_queue_t *q, int rw, struct bio *bio)
{
	elevator_t *e = q->elevator;

	if (e->ops->elevator_may_queue_fn)
		return e->ops->elevator_may_queue_fn(q, rw, bio);

	return ELV_MQUEUE_MAY;
}
/**ltl
 * 功能:请求处理完成
 * 参数:
 */
void elv_completed_request(request_queue_t *q, struct request *rq)
{
	elevator_t *e = q->elevator;

	/*
	 * request is released from the driver, io must be done
	 */
	if (blk_account_rq(rq)) {/* 表示rq是一个文件系统的请求 */
		q->in_flight--;
		/* 对于排序请求，调用IO调度算法中的completed方法 */
		if (blk_sorted_rq(rq) && e->ops->elevator_completed_req_fn)
			e->ops->elevator_completed_req_fn(q, rq);/* 只对CFQ算法有用 */
	}

	/*
	 * Check if the queue is waiting for fs requests to be
	 * drained for flush sequence.
	 */
	if (unlikely(q->ordseq)) {/* "抽干"所有的请求，并修改请求队列排序队列的状态 */
		struct request *first_rq = list_entry_rq(q->queue_head.next);
		if (q->in_flight == 0 &&
		    blk_ordered_cur_seq(q) == QUEUE_ORDSEQ_DRAIN &&
		    blk_ordered_req_seq(first_rq) > QUEUE_ORDSEQ_DRAIN) {
			blk_ordered_complete_seq(q, QUEUE_ORDSEQ_DRAIN, 0);
			q->request_fn(q);/* 请求队列"泄流" */
		}
	}
}

#define to_elv(atr) container_of((atr), struct elv_fs_entry, attr)

static ssize_t
elv_attr_show(struct kobject *kobj, struct attribute *attr, char *page)
{
	elevator_t *e = container_of(kobj, elevator_t, kobj);
	struct elv_fs_entry *entry = to_elv(attr);
	ssize_t error;

	if (!entry->show)
		return -EIO;

	mutex_lock(&e->sysfs_lock);
	error = e->ops ? entry->show(e, page) : -ENOENT;
	mutex_unlock(&e->sysfs_lock);
	return error;
}

static ssize_t
elv_attr_store(struct kobject *kobj, struct attribute *attr,
	       const char *page, size_t length)
{
	elevator_t *e = container_of(kobj, elevator_t, kobj);
	struct elv_fs_entry *entry = to_elv(attr);
	ssize_t error;

	if (!entry->store)
		return -EIO;

	mutex_lock(&e->sysfs_lock);
	error = e->ops ? entry->store(e, page, length) : -ENOENT;
	mutex_unlock(&e->sysfs_lock);
	return error;
}

static struct sysfs_ops elv_sysfs_ops = {
	.show	= elv_attr_show,
	.store	= elv_attr_store,
};

static struct kobj_type elv_ktype = {
	.sysfs_ops	= &elv_sysfs_ops,
	.release	= elevator_release,
};

int elv_register_queue(struct request_queue *q)
{
	elevator_t *e = q->elevator;
	int error;

	e->kobj.parent = &q->kobj;

	error = kobject_add(&e->kobj);
	if (!error) {
		struct elv_fs_entry *attr = e->elevator_type->elevator_attrs;
		if (attr) {
			while (attr->attr.name) {
				if (sysfs_create_file(&e->kobj, &attr->attr))
					break;
				attr++;
			}
		}
		kobject_uevent(&e->kobj, KOBJ_ADD);
	}
	return error;
}

static void __elv_unregister_queue(elevator_t *e)
{
	kobject_uevent(&e->kobj, KOBJ_REMOVE);
	kobject_del(&e->kobj);
}

void elv_unregister_queue(struct request_queue *q)
{
	if (q)
		__elv_unregister_queue(q->elevator);
}

/**ltl
 * 功能:IO调度器注册接口
 * 参数:IO调度器对象
 */
int elv_register(struct elevator_type *e)
{
	spin_lock_irq(&elv_list_lock);
	BUG_ON(elevator_find(e->elevator_name));
	list_add_tail(&e->list, &elv_list);/* 插入到列表中 */
	spin_unlock_irq(&elv_list_lock);

	printk(KERN_INFO "io scheduler %s registered", e->elevator_name);
	if (!strcmp(e->elevator_name, chosen_elevator) ||
			(!*chosen_elevator &&
			 !strcmp(e->elevator_name, CONFIG_DEFAULT_IOSCHED)))
				printk(" (default)");
	printk("\n");
	return 0;
}
EXPORT_SYMBOL_GPL(elv_register);

void elv_unregister(struct elevator_type *e)
{
	struct task_struct *g, *p;

	/*
	 * Iterate every thread in the process to remove the io contexts.
	 */
	if (e->ops.trim) {
		read_lock(&tasklist_lock);
		do_each_thread(g, p) {
			task_lock(p);
			if (p->io_context)
				e->ops.trim(p->io_context);
			task_unlock(p);
		} while_each_thread(g, p);
		read_unlock(&tasklist_lock);
	}

	spin_lock_irq(&elv_list_lock);
	list_del_init(&e->list);
	spin_unlock_irq(&elv_list_lock);
}
EXPORT_SYMBOL_GPL(elv_unregister);

/*
 * switch to new_e io scheduler. be careful not to introduce deadlocks -
 * we don't free the old io scheduler, before we have allocated what we
 * need for the new one. this way we have a chance of going back to the old
 * one, if the new one fails init for some reason.
 */
/**ltl
功能:在/sys中更换IO调度算法，要用到
	eg:/sys/devices/pci0000:00/0000:00:1f.2/host4/target4:0:0/4:0:0:0/block/sda/queue/scheduler
*/
static int elevator_switch(request_queue_t *q, struct elevator_type *new_e)
{
	elevator_t *old_elevator, *e;
	void *data;

	/*
	 * Allocate new elevator
	 */
	e = elevator_alloc(new_e);
	if (!e)
		return 0;

	data = elevator_init_queue(q, e);
	if (!data) {
		kobject_put(&e->kobj);
		return 0;
	}

	/*
	 * Turn on BYPASS and drain all requests w/ elevator private data
	 */
	spin_lock_irq(q->queue_lock);

	set_bit(QUEUE_FLAG_ELVSWITCH, &q->queue_flags);

	elv_drain_elevator(q);

	while (q->rq.elvpriv) {
		blk_remove_plug(q);
		q->request_fn(q);
		spin_unlock_irq(q->queue_lock);
		msleep(10);
		spin_lock_irq(q->queue_lock);
		elv_drain_elevator(q);
	}

	/*
	 * Remember old elevator.
	 */
	old_elevator = q->elevator;

	/*
	 * attach and start new elevator
	 */
	elevator_attach(q, e, data);

	spin_unlock_irq(q->queue_lock);

	__elv_unregister_queue(old_elevator);

	if (elv_register_queue(q))
		goto fail_register;

	/*
	 * finally exit old elevator and turn off BYPASS.
	 */
	elevator_exit(old_elevator);
	clear_bit(QUEUE_FLAG_ELVSWITCH, &q->queue_flags);
	return 1;

fail_register:
	/*
	 * switch failed, exit the new io scheduler and reattach the old
	 * one again (along with re-adding the sysfs dir)
	 */
	elevator_exit(e);
	q->elevator = old_elevator;
	elv_register_queue(q);
	clear_bit(QUEUE_FLAG_ELVSWITCH, &q->queue_flags);
	return 0;
}

ssize_t elv_iosched_store(request_queue_t *q, const char *name, size_t count)
{
	char elevator_name[ELV_NAME_MAX];
	size_t len;
	struct elevator_type *e;

	elevator_name[sizeof(elevator_name) - 1] = '\0';
	strncpy(elevator_name, name, sizeof(elevator_name) - 1);
	len = strlen(elevator_name);

	if (len && elevator_name[len - 1] == '\n')
		elevator_name[len - 1] = '\0';

	e = elevator_get(elevator_name);
	if (!e) {
		printk(KERN_ERR "elevator: type %s not found\n", elevator_name);
		return -EINVAL;
	}

	if (!strcmp(elevator_name, q->elevator->elevator_type->elevator_name)) {
		elevator_put(e);
		return count;
	}

	if (!elevator_switch(q, e))
		printk(KERN_ERR "elevator: switch to %s failed\n",elevator_name);
	return count;
}

ssize_t elv_iosched_show(request_queue_t *q, char *name)
{
	elevator_t *e = q->elevator;
	struct elevator_type *elv = e->elevator_type;
	struct list_head *entry;
	int len = 0;

	spin_lock_irq(q->queue_lock);
	list_for_each(entry, &elv_list) {
		struct elevator_type *__e;

		__e = list_entry(entry, struct elevator_type, list);
		if (!strcmp(elv->elevator_name, __e->elevator_name))
			len += sprintf(name+len, "[%s] ", elv->elevator_name);
		else
			len += sprintf(name+len, "%s ", __e->elevator_name);
	}
	spin_unlock_irq(q->queue_lock);

	len += sprintf(len+name, "\n");
	return len;
}

EXPORT_SYMBOL(elv_dispatch_sort);
EXPORT_SYMBOL(elv_add_request);
EXPORT_SYMBOL(__elv_add_request);
EXPORT_SYMBOL(elv_next_request);
EXPORT_SYMBOL(elv_dequeue_request);
EXPORT_SYMBOL(elv_queue_empty);
EXPORT_SYMBOL(elevator_exit);
EXPORT_SYMBOL(elevator_init);
