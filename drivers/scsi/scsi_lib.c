/*
 *  scsi_lib.c Copyright (C) 1999 Eric Youngdale
 *
 *  SCSI queueing library.
 *      Initial versions: Eric Youngdale (eric@andante.org).
 *                        Based upon conversations with large numbers
 *                        of people at Linux Expo.
 */

#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/completion.h>
#include <linux/kernel.h>
#include <linux/mempool.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/hardirq.h>

#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_dbg.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_driver.h>
#include <scsi/scsi_eh.h>
#include <scsi/scsi_host.h>

#include "scsi_priv.h"
#include "scsi_logging.h"


#define SG_MEMPOOL_NR		ARRAY_SIZE(scsi_sg_pools)
#define SG_MEMPOOL_SIZE		32

struct scsi_host_sg_pool {
	size_t		size;
	char		*name; 
	kmem_cache_t	*slab;
	mempool_t	*pool;
};

#if (SCSI_MAX_PHYS_SEGMENTS < 32)
#error SCSI_MAX_PHYS_SEGMENTS is too small
#endif

#define SP(x) { x, "sgpool-" #x } 
static struct scsi_host_sg_pool scsi_sg_pools[] = {
	SP(8),
	SP(16),
	SP(32),
#if (SCSI_MAX_PHYS_SEGMENTS > 32)
	SP(64),
#if (SCSI_MAX_PHYS_SEGMENTS > 64)
	SP(128),
#if (SCSI_MAX_PHYS_SEGMENTS > 128)
	SP(256),
#if (SCSI_MAX_PHYS_SEGMENTS > 256)
#error SCSI_MAX_PHYS_SEGMENTS is too large
#endif
#endif
#endif
#endif
}; 	
#undef SP

static void scsi_run_queue(struct request_queue *q);

/*
 * Function:	scsi_unprep_request()
 *
 * Purpose:	Remove all preparation done for a request, including its
 *		associated scsi_cmnd, so that it can be requeued.
 *
 * Arguments:	req	- request to unprepare
 *
 * Lock status:	Assumed that no locks are held upon entry.
 *
 * Returns:	Nothing.
 */
static void scsi_unprep_request(struct request *req)
{
	struct scsi_cmnd *cmd = req->special;

	req->flags &= ~REQ_DONTPREP;
	req->special = NULL;

	scsi_put_command(cmd);
}

/*
 * Function:    scsi_queue_insert()
 *
 * Purpose:     Insert a command in the midlevel queue.
 *
 * Arguments:   cmd    - command that we are adding to queue.
 *              reason - why we are inserting command to queue.
 *
 * Lock status: Assumed that lock is not held upon entry.
 *
 * Returns:     Nothing.
 *
 * Notes:       We do this for one of two cases.  Either the host is busy
 *              and it cannot accept any more commands for the time being,
 *              or the device returned QUEUE_FULL and can accept no more
 *              commands.
 * Notes:       This could be called either from an interrupt context or a
 *              normal process context.
 */
/**ltl
功能:把命令插回到派发队列中
参数:cmd	->scsi_cmnd对象
	reason	->导致重新插入到派发队列的原因
返回值:
*/
int scsi_queue_insert(struct scsi_cmnd *cmd, int reason)
{
	struct Scsi_Host *host = cmd->device->host;
	struct scsi_device *device = cmd->device;
	struct request_queue *q = device->request_queue;
	unsigned long flags;

	SCSI_LOG_MLQUEUE(1,
		 printk("Inserting command %p into mlqueue\n", cmd));

	/*
	 * Set the appropriate busy bit for the device/host.
	 *
	 * If the host/device isn't busy, assume that something actually
	 * completed, and that we should be able to queue a command now.
	 *
	 * Note that the prior mid-layer assumption that any host could
	 * always queue at least one command is now broken.  The mid-layer
	 * will implement a user specifiable stall (see
	 * scsi_host.max_host_blocked and scsi_device.max_device_blocked)
	 * if a command is requeued with no other commands outstanding
	 * either for the device or for the host.
	 */
	 //重置host_blocked和device_blocked
	if (reason == SCSI_MLQUEUE_HOST_BUSY)
		host->host_blocked = host->max_host_blocked;
	else if (reason == SCSI_MLQUEUE_DEVICE_BUSY)
		device->device_blocked = device->max_device_blocked;

	/*
	 * Decrement the counters, since these commands are no longer
	 * active on the host/device.
	 */
	//
	scsi_device_unbusy(device);

	/*
	 * Requeue this command.  It will go before all other commands
	 * that are already in the queue.
	 *
	 * NOTE: there is magic here about the way the queue is plugged if
	 * we have no outstanding commands.
	 * 
	 * Although we *don't* plug the queue, we call the request
	 * function.  The SCSI request function detects the blocked condition
	 * and plugs the queue appropriately.
         */
	spin_lock_irqsave(q->queue_lock, flags);
	blk_requeue_request(q, cmd->request);//把命令插入到派发队列中
	spin_unlock_irqrestore(q->queue_lock, flags);

	scsi_run_queue(q);//执行"泄流"操作

	return 0;
}

/**
 * scsi_execute - insert request and wait for the result
 * @sdev:	scsi device
 * @cmd:	scsi command
 * @data_direction: data direction
 * @buffer:	data buffer
 * @bufflen:	len of buffer
 * @sense:	optional sense buffer
 * @timeout:	request timeout in seconds
 * @retries:	number of times to retry request
 * @flags:	or into request flags;
 *
 * returns the req->errors value which is the the scsi_cmnd result
 * field.
 **/
/**ltl
功能:执行scsi命令
参数:sdev			->SCSI逻辑设备对象
	cmd			->SCSI命令数组
	data_direction	->命令的方向:DMA_TO_DEVICE/DMA_FROM_DEVICE
	buffer	->操作时，数据存放的buffer
	bufferlen	->buffer的长度
	sense		->感测数据的存放地址
	timeout	->请求的超时时长
	retries	->请求重复次数
	flags		->请求对象的属性标志
返回值:
*/
int scsi_execute(struct scsi_device *sdev, const unsigned char *cmd,
		 int data_direction, void *buffer, unsigned bufflen,
		 unsigned char *sense, int timeout, int retries, int flags)
{
	struct request *req;
	int write = (data_direction == DMA_TO_DEVICE);
	int ret = DRIVER_ERROR << 24;
	//获取一个reuqest对象
	req = blk_get_request(sdev->request_queue, write, __GFP_WAIT);

	//映射request与buffer
	if (bufflen &&	blk_rq_map_kern(sdev->request_queue, req,
					buffer, bufflen, __GFP_WAIT))
		goto out;

	req->cmd_len = COMMAND_SIZE(cmd[0]);
	memcpy(req->cmd, cmd, req->cmd_len);
	req->sense = sense;//设置感测数据的地址
	req->sense_len = 0;
	req->retries = retries;
	req->timeout = timeout;
	req->flags |= flags | REQ_BLOCK_PC | REQ_SPECIAL | REQ_QUIET;

	/*
	 * head injection *required* here otherwise quiesce won't work
	 */
	 //把请求传递给块IO层
	blk_execute_rq(req->q, NULL, req, 1);
	//errors值在scsi_io_completion中赋值。
	ret = req->errors;
 out:
	blk_put_request(req);

	return ret;
}
EXPORT_SYMBOL(scsi_execute);


int scsi_execute_req(struct scsi_device *sdev, const unsigned char *cmd,
		     int data_direction, void *buffer, unsigned bufflen,
		     struct scsi_sense_hdr *sshdr, int timeout, int retries)
{
	char *sense = NULL;
	int result;
	
	if (sshdr) {
		sense = kzalloc(SCSI_SENSE_BUFFERSIZE, GFP_NOIO);
		if (!sense)
			return DRIVER_ERROR << 24;
	}
	result = scsi_execute(sdev, cmd, data_direction, buffer, bufflen,
			      sense, timeout, retries, 0);
	if (sshdr)
		scsi_normalize_sense(sense, SCSI_SENSE_BUFFERSIZE, sshdr);

	kfree(sense);
	return result;
}
EXPORT_SYMBOL(scsi_execute_req);

struct scsi_io_context {
	void *data;
	void (*done)(void *data, char *sense, int result, int resid);
	char sense[SCSI_SENSE_BUFFERSIZE];
};

static kmem_cache_t *scsi_io_context_cache;

static void scsi_end_async(struct request *req, int uptodate)
{
	struct scsi_io_context *sioc = req->end_io_data;

	if (sioc->done)
		sioc->done(sioc->data, sioc->sense, req->errors, req->data_len);

	kmem_cache_free(scsi_io_context_cache, sioc);
	__blk_put_request(req->q, req);
}

static int scsi_merge_bio(struct request *rq, struct bio *bio)
{
	struct request_queue *q = rq->q;

	bio->bi_flags &= ~(1 << BIO_SEG_VALID);
	if (rq_data_dir(rq) == WRITE)
		bio->bi_rw |= (1 << BIO_RW);
	blk_queue_bounce(q, &bio);

	if (!rq->bio)
		blk_rq_bio_prep(q, rq, bio);
	else if (!q->back_merge_fn(q, rq, bio))
		return -EINVAL;
	else {
		rq->biotail->bi_next = bio;
		rq->biotail = bio;
		rq->hard_nr_sectors += bio_sectors(bio);
		rq->nr_sectors = rq->hard_nr_sectors;
	}

	return 0;
}

static int scsi_bi_endio(struct bio *bio, unsigned int bytes_done, int error)
{
	if (bio->bi_size)
		return 1;

	bio_put(bio);
	return 0;
}

/**
 * scsi_req_map_sg - map a scatterlist into a request
 * @rq:		request to fill
 * @sg:		scatterlist
 * @nsegs:	number of elements
 * @bufflen:	len of buffer
 * @gfp:	memory allocation flags
 *
 * scsi_req_map_sg maps a scatterlist into a request so that the
 * request can be sent to the block layer. We do not trust the scatterlist
 * sent to use, as some ULDs use that struct to only organize the pages.
 */
static int scsi_req_map_sg(struct request *rq, struct scatterlist *sgl,
			   int nsegs, unsigned bufflen, gfp_t gfp)
{
	struct request_queue *q = rq->q;
	int nr_pages = (bufflen + sgl[0].offset + PAGE_SIZE - 1) >> PAGE_SHIFT;
	unsigned int data_len = 0, len, bytes, off;
	struct page *page;
	struct bio *bio = NULL;
	int i, err, nr_vecs = 0;

	for (i = 0; i < nsegs; i++) {
		page = sgl[i].page;
		off = sgl[i].offset;
		len = sgl[i].length;
		data_len += len;

		while (len > 0) {
			bytes = min_t(unsigned int, len, PAGE_SIZE - off);

			if (!bio) {
				nr_vecs = min_t(int, BIO_MAX_PAGES, nr_pages);
				nr_pages -= nr_vecs;

				bio = bio_alloc(gfp, nr_vecs);
				if (!bio) {
					err = -ENOMEM;
					goto free_bios;
				}
				bio->bi_end_io = scsi_bi_endio;
			}

			if (bio_add_pc_page(q, bio, page, bytes, off) !=
			    bytes) {
				bio_put(bio);
				err = -EINVAL;
				goto free_bios;
			}

			if (bio->bi_vcnt >= nr_vecs) {
				err = scsi_merge_bio(rq, bio);
				if (err) {
					bio_endio(bio, bio->bi_size, 0);
					goto free_bios;
				}
				bio = NULL;
			}

			page++;
			len -= bytes;
			off = 0;
		}
	}

	rq->buffer = rq->data = NULL;
	rq->data_len = data_len;
	return 0;

free_bios:
	while ((bio = rq->bio) != NULL) {
		rq->bio = bio->bi_next;
		/*
		 * call endio instead of bio_put incase it was bounced
		 */
		bio_endio(bio, bio->bi_size, 0);
	}

	return err;
}

/**
 * scsi_execute_async - insert request
 * @sdev:	scsi device
 * @cmd:	scsi command
 * @cmd_len:	length of scsi cdb
 * @data_direction: data direction
 * @buffer:	data buffer (this can be a kernel buffer or scatterlist)
 * @bufflen:	len of buffer
 * @use_sg:	if buffer is a scatterlist this is the number of elements
 * @timeout:	request timeout in seconds
 * @retries:	number of times to retry request
 * @flags:	or into request flags
 **/
/**ltl
功能:异步执行一条scsi命令
参数:
返回值:
*/
int scsi_execute_async(struct scsi_device *sdev, const unsigned char *cmd,
		       int cmd_len, int data_direction, void *buffer, unsigned bufflen,
		       int use_sg, int timeout, int retries, void *privdata,
		       void (*done)(void *, char *, int, int), gfp_t gfp)
{
	struct request *req;
	struct scsi_io_context *sioc;
	int err = 0;
	int write = (data_direction == DMA_TO_DEVICE);

	sioc = kmem_cache_alloc(scsi_io_context_cache, gfp);
	if (!sioc)
		return DRIVER_ERROR << 24;
	memset(sioc, 0, sizeof(*sioc));

	req = blk_get_request(sdev->request_queue, write, gfp);
	if (!req)
		goto free_sense;
	req->flags |= REQ_BLOCK_PC | REQ_QUIET;

	if (use_sg)
		err = scsi_req_map_sg(req, buffer, use_sg, bufflen, gfp);
	else if (bufflen)
		err = blk_rq_map_kern(req->q, req, buffer, bufflen, gfp);

	if (err)
		goto free_req;

	req->cmd_len = cmd_len;
	memcpy(req->cmd, cmd, req->cmd_len);
	req->sense = sioc->sense;
	req->sense_len = 0;
	req->timeout = timeout;
	req->retries = retries;
	req->end_io_data = sioc;

	sioc->data = privdata;
	sioc->done = done;
	//把request提交请求队列，没有等到命令执行结束就返回。
	blk_execute_rq_nowait(req->q, NULL, req, 1, scsi_end_async);
	return 0;

free_req:
	blk_put_request(req);
free_sense:
	kfree(sioc);
	return DRIVER_ERROR << 24;
}
EXPORT_SYMBOL_GPL(scsi_execute_async);

/*
 * Function:    scsi_init_cmd_errh()
 *
 * Purpose:     Initialize cmd fields related to error handling.
 *
 * Arguments:   cmd	- command that is ready to be queued.
 *
 * Notes:       This function has the job of initializing a number of
 *              fields related to error handling.   Typically this will
 *              be called once for each command, as required.
 */
static void scsi_init_cmd_errh(struct scsi_cmnd *cmd)
{
	cmd->serial_number = 0;
	memset(cmd->sense_buffer, 0, sizeof cmd->sense_buffer);
	if (cmd->cmd_len == 0)
		cmd->cmd_len = COMMAND_SIZE(cmd->cmnd[0]);
}
/**ltl
功能:唤醒错误处理线程、标记设备状态
参数:sdev	->scsi设备对象
说明:当scsi设备处理完一个命令后，调用此函数。
*/
void scsi_device_unbusy(struct scsi_device *sdev)
{
	struct Scsi_Host *shost = sdev->host;
	unsigned long flags;

	spin_lock_irqsave(shost->host_lock, flags);
	shost->host_busy--;
	if (unlikely(scsi_host_in_recovery(shost) &&
		     (shost->host_failed || shost->host_eh_scheduled)))
		scsi_eh_wakeup(shost);//触发错误处理线程
	spin_unlock(shost->host_lock);
	spin_lock(sdev->request_queue->queue_lock);
	sdev->device_busy--;
	spin_unlock_irqrestore(sdev->request_queue->queue_lock, flags);
}

/*
 * Called for single_lun devices on IO completion. Clear starget_sdev_user,
 * and call blk_run_queue for all the scsi_devices on the target -
 * including current_sdev first.
 *
 * Called with *no* scsi locks held.
 */
/**ltl
功能:目标节点在一个时间内只能允许一个SCSI设备执行IO请求，则执行这个设备的IO请求。
参数:current_sdev	->SCSI设备对象
*/
static void scsi_single_lun_run(struct scsi_device *current_sdev)
{
	struct Scsi_Host *shost = current_sdev->host;
	struct scsi_device *sdev, *tmp;
	struct scsi_target *starget = scsi_target(current_sdev);
	unsigned long flags;

	spin_lock_irqsave(shost->host_lock, flags);
	starget->starget_sdev_user = NULL;//重新清空目标节点的设备域
	spin_unlock_irqrestore(shost->host_lock, flags);

	/*
	 * Call blk_run_queue for all LUNs on the target, starting with
	 * current_sdev. We race with others (to set starget_sdev_user),
	 * but in most cases, we will be first. Ideally, each LU on the
	 * target would get some limited time or requests on the target.
	 */
	/*前面把starget_sdev_user置NULL,后面又要判定starget_sdev_user是否为NULL的原因:
	  执行当前设备的请求队列,在执行请求队列时，会执行scsi_request_fn函数，在scsi_request_fn里，
 	  会对starget_sdev_user设置，因此要重新判定starget_sdev_user字段
	*/
	blk_run_queue(current_sdev->request_queue);

	spin_lock_irqsave(shost->host_lock, flags);
	if (starget->starget_sdev_user)
		goto out;
	//遍历目标节点中的逻辑设备，并去执行各个设备的请求队列
	list_for_each_entry_safe(sdev, tmp, &starget->devices,
			same_target_siblings) {
		if (sdev == current_sdev)
			continue;
		if (scsi_device_get(sdev))
			continue;

		spin_unlock_irqrestore(shost->host_lock, flags);
		blk_run_queue(sdev->request_queue);
		spin_lock_irqsave(shost->host_lock, flags);
	
		scsi_device_put(sdev);
	}
 out:
	spin_unlock_irqrestore(shost->host_lock, flags);
}

/*
 * Function:	scsi_run_queue()
 *
 * Purpose:	Select a proper request queue to serve next
 *
 * Arguments:	q	- last request's queue
 *
 * Returns:     Nothing
 *
 * Notes:	The previous command was completely finished, start
 *		a new one if possible.
 */
/**ltl
功能:1.如果目标节点是一个"同一时间只能处理一个SCSI设备的IO请求"，则先处理这个lun的请求。
	2.执行处理"饥饿"列表中的SCSI设备的请求队列
参数:
*/
static void scsi_run_queue(struct request_queue *q)
{
	struct scsi_device *sdev = q->queuedata;
	struct Scsi_Host *shost = sdev->host;
	unsigned long flags;
	//目标节点是一个"同一时间只能处理一个SCSI设备的IO请求"，则逐一执行各个逻辑设备的请求队列
	if (sdev->single_lun)
		scsi_single_lun_run(sdev);

	spin_lock_irqsave(shost->host_lock, flags);
	/*
	1.主机适配器的"饥饿"链表不为空；2.主机适配器的请求个数已经达到最大值max_host_blocked；3.主机适配器不处于忙状态
	*/
	while (!list_empty(&shost->starved_list) &&
	       !shost->host_blocked && !shost->host_self_blocked &&
		!((shost->can_queue > 0) &&
		  (shost->host_busy >= shost->can_queue))) {
		/*
		 * As long as shost is accepting commands and we have
		 * starved queues, call blk_run_queue. scsi_request_fn
		 * drops the queue_lock and can add us back to the
		 * starved_list.
		 *
		 * host_lock protects the starved_list and starved_entry.
		 * scsi_request_fn must get the host_lock before checking
		 * or modifying starved_list or starved_entry.
		 */
		 //从"饥饿"列表中获取一个元素
		sdev = list_entry(shost->starved_list.next,
					  struct scsi_device, starved_entry);
		list_del_init(&sdev->starved_entry);//从列表中删除
		spin_unlock_irqrestore(shost->host_lock, flags);
		//执行处在"饥饿"列表中的设备
		blk_run_queue(sdev->request_queue);

		spin_lock_irqsave(shost->host_lock, flags);
		//这个一般不会发生!!!!
		if (unlikely(!list_empty(&sdev->starved_entry)))
			/*
			 * sdev lost a race, and was put back on the
			 * starved list. This is unlikely but without this
			 * in theory we could loop forever.
			 */
			break;
	}
	spin_unlock_irqrestore(shost->host_lock, flags);
	//执行请求队列
	blk_run_queue(q);
}

/*
 * Function:	scsi_requeue_command()
 *
 * Purpose:	Handle post-processing of completed commands.
 *
 * Arguments:	q	- queue to operate on
 *		cmd	- command that may need to be requeued.
 *
 * Returns:	Nothing
 *
 * Notes:	After command completion, there may be blocks left
 *		over which weren't finished by the previous command
 *		this can be for a number of reasons - the main one is
 *		I/O errors in the middle of the request, in which case
 *		we need to request the blocks that come after the bad
 *		sector.
 * Notes:	Upon return, cmd is a stale pointer.
 */
/**ltl
功能:把request重新放派发队列中，并运行
参数:q	->
	cmd	->
说明:此函数只有request中数据没有全部传输完成才使用
*/
static void scsi_requeue_command(struct request_queue *q, struct scsi_cmnd *cmd)
{
	struct request *req = cmd->request;
	unsigned long flags;

	scsi_unprep_request(req);
	spin_lock_irqsave(q->queue_lock, flags);
	blk_requeue_request(q, req);//插入到派发队列中
	spin_unlock_irqrestore(q->queue_lock, flags);
	//运行请求队列
	scsi_run_queue(q);
}
/**ltl
功能:处理等待队列中下一个reqeust
参数:
说明:在一个request处理完成后，调用此函数让SCSI子系统处理一下个命令
*/
void scsi_next_command(struct scsi_cmnd *cmd)
{
	struct scsi_device *sdev = cmd->device;
	struct request_queue *q = sdev->request_queue;

	/* need to hold a reference on the device before we let go of the cmd */
	get_device(&sdev->sdev_gendev);

	scsi_put_command(cmd);//释放cmd对象
	scsi_run_queue(q);//运行请求队列

	/* ok to remove device now */
	put_device(&sdev->sdev_gendev);
}
/**ltl
功能:遍历主机适配器下的各个设备，开始执行各个设备的请求处理函数
参数:
说明:在错误处理线程对一个出错的scsi_cmnd处理完后，调用此接口重新运行所有scsi设备。
*/
void scsi_run_host_queues(struct Scsi_Host *shost)
{
	struct scsi_device *sdev;

	shost_for_each_device(sdev, shost)
		scsi_run_queue(sdev->request_queue);
}

/*
 * Function:    scsi_end_request()
 *
 * Purpose:     Post-processing of completed commands (usually invoked at end
 *		of upper level post-processing and scsi_io_completion).
 *
 * Arguments:   cmd	 - command that is complete.
 *              uptodate - 1 if I/O indicates success, <= 0 for I/O error.
 *              bytes    - number of bytes of completed I/O
 *		requeue  - indicates whether we should requeue leftovers.
 *
 * Lock status: Assumed that lock is not held upon entry.
 *
 * Returns:     cmd if requeue required, NULL otherwise.
 *
 * Notes:       This is called for block device requests in order to
 *              mark some number of sectors as complete.
 * 
 *		We are guaranteeing that the request queue will be goosed
 *		at some point during this call.
 * Notes:	If cmd was requeued, upon return it will be a stale pointer.
 */
static struct scsi_cmnd *scsi_end_request(struct scsi_cmnd *cmd, int uptodate,
					  int bytes, int requeue)
{
	request_queue_t *q = cmd->device->request_queue;
	struct request *req = cmd->request;
	unsigned long flags;

	/*
	 * If there are blocks left over at the end, set up the command
	 * to queue the remainder of them.
	 */
	if (end_that_request_chunk(req, uptodate, bytes)) 
	{//说明request中的数据没有全部传输完成
		//剩下的数据大小
		int leftover = (req->hard_nr_sectors << 9);
		//如果是SCSI命令的话，要全部重新传输。即大小要重新设置成命令数据大小
		if (blk_pc_request(req))
			leftover = req->data_len;

		/* kill remainder if no retrys */
		if (!uptodate && blk_noretry_request(req))
			end_that_request_chunk(req, 0, leftover);
		else {
			//把request对象重放入到请求队列中
			if (requeue) {
				/*
				 * Bleah.  Leftovers again.  Stick the
				 * leftovers in the front of the
				 * queue, and goose the queue again.
				 */
				scsi_requeue_command(q, cmd);
				cmd = NULL;
			}
			return cmd;
		}
	}
	//执行这里，说明reqeust的数据一次性传输完成。
	add_disk_randomness(req->rq_disk);

	spin_lock_irqsave(q->queue_lock, flags);
	if (blk_rq_tagged(req))
		blk_queue_end_tag(q, req);
	//通知上层，request数据传输完成----释放request内存。
	end_that_request_last(req, uptodate);
	spin_unlock_irqrestore(q->queue_lock, flags);

	/*
	 * This will goose the queue request function at the end, so we don't
	 * need to worry about launching another command.
	 */
	 //处理下一个请求request
	scsi_next_command(cmd);
	return NULL;
}
//分配与cmd对应的聚散列表
static struct scatterlist *scsi_alloc_sgtable(struct scsi_cmnd *cmd, gfp_t gfp_mask)
{
	struct scsi_host_sg_pool *sgp;
	struct scatterlist *sgl;

	BUG_ON(!cmd->use_sg);

	switch (cmd->use_sg) {
	case 1 ... 8:
		cmd->sglist_len = 0;
		break;
	case 9 ... 16:
		cmd->sglist_len = 1;
		break;
	case 17 ... 32:
		cmd->sglist_len = 2;
		break;
#if (SCSI_MAX_PHYS_SEGMENTS > 32)
	case 33 ... 64:
		cmd->sglist_len = 3;
		break;
#if (SCSI_MAX_PHYS_SEGMENTS > 64)
	case 65 ... 128:
		cmd->sglist_len = 4;
		break;
#if (SCSI_MAX_PHYS_SEGMENTS  > 128)
	case 129 ... 256:
		cmd->sglist_len = 5;
		break;
#endif
#endif
#endif
	default:
		return NULL;
	}

	sgp = scsi_sg_pools + cmd->sglist_len;
	//从内存池中分配得聚散列表对象
	sgl = mempool_alloc(sgp->pool, gfp_mask);
	return sgl;
}

static void scsi_free_sgtable(struct scatterlist *sgl, int index)
{
	struct scsi_host_sg_pool *sgp;

	BUG_ON(index >= SG_MEMPOOL_NR);

	sgp = scsi_sg_pools + index;
	mempool_free(sgl, sgp->pool);
}

/*
 * Function:    scsi_release_buffers()
 *
 * Purpose:     Completion processing for block device I/O requests.
 *
 * Arguments:   cmd	- command that we are bailing.
 *
 * Lock status: Assumed that no lock is held upon entry.
 *
 * Returns:     Nothing
 *
 * Notes:       In the event that an upper level driver rejects a
 *		command, we must release resources allocated during
 *		the __init_io() function.  Primarily this would involve
 *		the scatter-gather table, and potentially any bounce
 *		buffers.
 */
static void scsi_release_buffers(struct scsi_cmnd *cmd)
{
	if (cmd->use_sg)
		scsi_free_sgtable(cmd->request_buffer, cmd->sglist_len);

	/*
	 * Zero these out.  They now point to freed memory, and it is
	 * dangerous to hang onto the pointers.
	 */
	cmd->request_buffer = NULL;
	cmd->request_bufflen = 0;
}

/*
 * Function:    scsi_io_completion()
 *
 * Purpose:     Completion processing for block device I/O requests.
 *
 * Arguments:   cmd   - command that is finished.
 *
 * Lock status: Assumed that no lock is held upon entry.
 *
 * Returns:     Nothing
 *
 * Notes:       This function is matched in terms of capabilities to
 *              the function that created the scatter-gather list.
 *              In other words, if there are no bounce buffers
 *              (the normal case for most drivers), we don't need
 *              the logic to deal with cleaning up afterwards.
 *
 *		We must do one of several things here:
 *
 *		a) Call scsi_end_request.  This will finish off the
 *		   specified number of sectors.  If we are done, the
 *		   command block will be released, and the queue
 *		   function will be goosed.  If we are not done, then
 *		   scsi_end_request will directly goose the queue.
 *
 *		b) We can just use scsi_requeue_command() here.  This would
 *		   be used if we just wanted to retry, for example.
 */
void scsi_io_completion(struct scsi_cmnd *cmd, unsigned int good_bytes)
{
	//超时的值在scsi_eh_flush_done_q函数中赋值
	int result = cmd->result;
	int this_count = cmd->request_bufflen;
	request_queue_t *q = cmd->device->request_queue;
	struct request *req = cmd->request;
	int clear_errors = 1;
	struct scsi_sense_hdr sshdr;
	int sense_valid = 0;
	int sense_deferred = 0;

	scsi_release_buffers(cmd);
	//传输出错
	if (result) {
		sense_valid = scsi_command_normalize_sense(cmd, &sshdr);
		if (sense_valid)
			sense_deferred = scsi_sense_is_deferred(&sshdr);
	}
	//scsi命令
	if (blk_pc_request(req)) { /* SG_IO ioctl from block level */
		req->errors = result;
		if (result) {//保存感测数据
			clear_errors = 0;
			if (sense_valid && req->sense) {
				/*
				 * SG_IO wants current and deferred errors
				 */
				int len = 8 + cmd->sense_buffer[7];

				if (len > SCSI_SENSE_BUFFERSIZE)
					len = SCSI_SENSE_BUFFERSIZE;
				memcpy(req->sense, cmd->sense_buffer,  len);
				req->sense_len = len;
			}
		} else
			req->data_len = cmd->resid;
	}

	/*
	 * Next deal with any sectors which we were able to correctly
	 * handle.
	 */
	SCSI_LOG_HLCOMPLETE(1, printk("%ld sectors total, "
				      "%d bytes done.\n",
				      req->nr_sectors, good_bytes));
	SCSI_LOG_HLCOMPLETE(1, printk("use_sg is %d\n", cmd->use_sg));
	//对来自于FS的请求，error域无用
	if (clear_errors)
		req->errors = 0;

	/* A number of bytes were successfully read.  If there
	 * are leftovers and there is some kind of error
	 * (result != 0), retry the rest.
	 */
	if (scsi_end_request(cmd, 1, good_bytes, result == 0) == NULL)
		return;

	/* good_bytes = 0, or (inclusive) there were leftovers and
	 * result = 0, so scsi_end_request couldn't retry.
	 */
	if (sense_valid && !sense_deferred) {
		switch (sshdr.sense_key) {
		case UNIT_ATTENTION:
			if (cmd->device->removable) {
				/* Detected disc change.  Set a bit
				 * and quietly refuse further access.
				 */
				cmd->device->changed = 1;
				scsi_end_request(cmd, 0, this_count, 1);
				return;
			} else {
				/* Must have been a power glitch, or a
				 * bus reset.  Could not have been a
				 * media change, so we just retry the
				 * request and see what happens.
				 */
				scsi_requeue_command(q, cmd);
				return;
			}
			break;
		case ILLEGAL_REQUEST:
			/* If we had an ILLEGAL REQUEST returned, then
			 * we may have performed an unsupported
			 * command.  The only thing this should be
			 * would be a ten byte read where only a six
			 * byte read was supported.  Also, on a system
			 * where READ CAPACITY failed, we may have
			 * read past the end of the disk.
			 */
			if ((cmd->device->use_10_for_rw &&
			    sshdr.asc == 0x20 && sshdr.ascq == 0x00) &&
			    (cmd->cmnd[0] == READ_10 ||
			     cmd->cmnd[0] == WRITE_10)) {
				cmd->device->use_10_for_rw = 0;
				/* This will cause a retry with a
				 * 6-byte command.
				 */
				scsi_requeue_command(q, cmd);
				return;
			} else {
				scsi_end_request(cmd, 0, this_count, 1);
				return;
			}
			break;
		case NOT_READY:
			/* If the device is in the process of becoming
			 * ready, or has a temporary blockage, retry.
			 */
			if (sshdr.asc == 0x04) {
				switch (sshdr.ascq) {
				case 0x01: /* becoming ready */
				case 0x04: /* format in progress */
				case 0x05: /* rebuild in progress */
				case 0x06: /* recalculation in progress */
				case 0x07: /* operation in progress */
				case 0x08: /* Long write in progress */
				case 0x09: /* self test in progress */
					scsi_requeue_command(q, cmd);
					return;
				default:
					break;
				}
			}
			if (!(req->flags & REQ_QUIET)) {
				scmd_printk(KERN_INFO, cmd,
					    "Device not ready: ");
				scsi_print_sense_hdr("", &sshdr);
			}
			scsi_end_request(cmd, 0, this_count, 1);
			return;
		case VOLUME_OVERFLOW:
			if (!(req->flags & REQ_QUIET)) {
				scmd_printk(KERN_INFO, cmd,
					    "Volume overflow, CDB: ");
				__scsi_print_command(cmd->cmnd);
				scsi_print_sense("", cmd);
			}
			/* See SSC3rXX or current. */
			scsi_end_request(cmd, 0, this_count, 1);
			return;
		default:
			break;
		}
	}
	if (host_byte(result) == DID_RESET) {
		/* Third party bus reset or reset for error recovery
		 * reasons.  Just retry the request and see what
		 * happens.
		 */
		scsi_requeue_command(q, cmd);
		return;
	}
	if (result) {
		if (!(req->flags & REQ_QUIET)) {
			scmd_printk(KERN_INFO, cmd,
				    "SCSI error: return code = 0x%08x\n",
				    result);
			if (driver_byte(result) & DRIVER_SENSE)
				scsi_print_sense("", cmd);
		}
	}
	scsi_end_request(cmd, 0, this_count, !result);
}
EXPORT_SYMBOL(scsi_io_completion);

/*
 * Function:    scsi_init_io()
 *
 * Purpose:     SCSI I/O initialize function.
 *
 * Arguments:   cmd   - Command descriptor we wish to initialize
 *
 * Returns:     0 on success
 *		BLKPREP_DEFER if the failure is retryable
 *		BLKPREP_KILL if the failure is fatal
 */
/**ltl
 * 功能:初始化scsi_cmnd各个域；1.如果请求是来自于SCSI层，且请求数据长度为0(TEST_UNIT_READY)，则直接赋值。
 *					2.如果请求来自于用户进程或者是SCSI层，且请求数据长度大于0，为这个请求准备聚散列表(scatterlist)
 * 参数:cmnd	->
 * 返回值:0	->表示成功
 * 说明: 对req->buffer要重新设置。若request是内部命令，则指向req->data; 若reqeust是fs命令，则直接置空。
 */
static int scsi_init_io(struct scsi_cmnd *cmd)
{
	struct request     *req = cmd->request;
	struct scatterlist *sgpnt;
	int		   count;

	/*
	 * if this is a rq->data based REQ_BLOCK_PC, setup for a non-sg xfer
	 */
	//表示命令来自于scsi中间层，并且req->bio的域为空，即没有数据要传输。只有TEST_UNIT_READY命令
	if ((req->flags & REQ_BLOCK_PC) && !req->bio) 
	{
		cmd->request_bufflen = req->data_len;
		cmd->request_buffer = req->data;
		req->buffer = req->data;
		cmd->use_sg = 0;
		return 0;
	}

	/*
	 * we used to not use scatter-gather for single segment request,
	 * but now we do (it makes highmem I/O easier to support without
	 * kmapping pages)
	 */
	 //聚散列表的段数
	cmd->use_sg = req->nr_phys_segments;

	/*
	 * if sg table allocation fails, requeue request later.
	 */
	 //分配聚散列表
	sgpnt = scsi_alloc_sgtable(cmd, GFP_ATOMIC);
	if (unlikely(!sgpnt)) {
		scsi_unprep_request(req);
		return BLKPREP_DEFER;
	}

	cmd->request_buffer = (char *) sgpnt;
	cmd->request_bufflen = req->nr_sectors << 9;
	if (blk_pc_request(req))//发送的是scsi命令，重设置长度
		cmd->request_bufflen = req->data_len;
	req->buffer = NULL; /* 因为从磁盘读取通过DMA完成，因此不需要此域，将其置NULL */

	/* 
	 * Next, walk the list, and fill in the addresses and sizes of
	 * each segment.
	 */
	//映射聚散列表与request的数据区
	count = blk_rq_map_sg(req->q, req, cmd->request_buffer);

	/*
	 * mapped well, send it off
	 */
	 //表示映射成功的段数小于等于分配时指定的段数
	if (likely(count <= cmd->use_sg)) {
		cmd->use_sg = count;
		return 0;
	}

	printk(KERN_ERR "Incorrect number of segments after building list\n");
	printk(KERN_ERR "counted %d, received %d\n", count, cmd->use_sg);
	printk(KERN_ERR "req nr_sec %lu, cur_nr_sec %u\n", req->nr_sectors,
			req->current_nr_sectors);
	/* release the command and kill it */

	scsi_release_buffers(cmd);
	scsi_put_command(cmd);
	return BLKPREP_KILL;
}

/**ltl
功能:执行磁盘的同步命令SYNCHRONIZE_CACHE
参数:q	->请求队列
	disk	->通用磁盘描述符
	error_sector->
*/
static int scsi_issue_flush_fn(request_queue_t *q, struct gendisk *disk,
			       sector_t *error_sector)
{
	struct scsi_device *sdev = q->queuedata;
	struct scsi_driver *drv;

	//如果设置不处理在执行状态，则退出
	if (sdev->sdev_state != SDEV_RUNNING)
		return -ENXIO;

	drv = *(struct scsi_driver **) disk->private_data;
	if (drv->issue_flush)//执行scsi_deiver->issue_flush去刷新磁盘
		return drv->issue_flush(&sdev->sdev_gendev, error_sector);

	return -EOPNOTSUPP;
}
/**ltl
功能:自于SCSI中间层的命令完成处理函数。
*/
static void scsi_blk_pc_done(struct scsi_cmnd *cmd)
{
	BUG_ON(!blk_pc_request(cmd->request));
	/*
	 * This will complete the whole command with uptodate=1 so
	 * as far as the block layer is concerned the command completed
	 * successfully. Since this is a REQ_BLOCK_PC command the
	 * caller should check the request's errors value
	 */
	scsi_io_completion(cmd, cmd->request_bufflen);
}

static void scsi_setup_blk_pc_cmnd(struct scsi_cmnd *cmd)
{
	struct request *req = cmd->request;

	BUG_ON(sizeof(req->cmd) > sizeof(cmd->cmnd));
	memcpy(cmd->cmnd, req->cmd, sizeof(cmd->cmnd));//拷贝SCSI命令
	cmd->cmd_len = req->cmd_len;//SCSI命令的长度
	//设备操作方向
	if (!req->data_len)
		cmd->sc_data_direction = DMA_NONE;
	else if (rq_data_dir(req) == WRITE)
		cmd->sc_data_direction = DMA_TO_DEVICE;
	else
		cmd->sc_data_direction = DMA_FROM_DEVICE;
	
	cmd->transfersize = req->data_len;//要传递的长度
	cmd->allowed = req->retries;//重复次数
	cmd->timeout_per_command = req->timeout;	//命令超时时长
	cmd->done = scsi_blk_pc_done;//设置命令完成处理函数，这个与来自用户进程的命令的处理函数不一样
}
/**ltl
功能:根据req构造scsi_cmnd对象
参数:q	->请求队列
	req	->请求对象
返回值:
	BLKPREP_OK		->构造命令成功
	BLKPREP_DEFER	->暂时不能继续处理，需要将命令重新排入队列中。
	BLKPREP_KILL	->表示出错，该请求没办法继续处理。
*/
static int scsi_prep_fn(struct request_queue *q, struct request *req)
{
	struct scsi_device *sdev = q->queuedata;
	struct scsi_cmnd *cmd;
	int specials_only = 0;

	/*
	 * Just check to see if the device is online.  If it isn't, we
	 * refuse to process any commands.  The device must be brought
	 * online before trying any recovery commands
	 */
	 //SCSI设备是否存在
	if (unlikely(!scsi_device_online(sdev))) {
		sdev_printk(KERN_ERR, sdev,
			    "rejecting I/O to offline device\n");
		goto kill;
	}
	//设备的状态不是在"运行"阶段
	if (unlikely(sdev->sdev_state != SDEV_RUNNING)) {
		/* OK, we're not in a running state don't prep
		 * user commands */
		if (sdev->sdev_state == SDEV_DEL) {
			/* Device is fully deleted, no commands
			 * at all allowed down */
			sdev_printk(KERN_ERR, sdev,
				    "rejecting I/O to dead device\n");
			goto kill;
		}
		/* OK, we only allow special commands (i.e. not
		 * user initiated ones */
		specials_only = sdev->sdev_state;
	}

	/*
	 * Find the actual device driver associated with this command.
	 * The SPECIAL requests are things like character device or
	 * ioctls, which did not originate from ll_rw_blk.  Note that
	 * the special field is also used to indicate the cmd for
	 * the remainder of a partially fulfilled request that can 
	 * come up when there is a medium error.  We have to treat
	 * these two cases differently.  We differentiate by looking
	 * at request->cmd, as this tells us the real story.
	 */
	if (req->flags & REQ_SPECIAL && req->special) {
		cmd = req->special;
	} 
	else if (req->flags & (REQ_CMD | REQ_BLOCK_PC)) 
	{//表示命令来自用户进程或者来自SCSI中间层

		if(unlikely(specials_only) && !(req->flags & REQ_SPECIAL)) 
		{
			if(specials_only == SDEV_QUIESCE || specials_only == SDEV_BLOCK)
				goto defer;
			
			sdev_printk(KERN_ERR, sdev,
				    "rejecting I/O to device being removed\n");
			goto kill;
		}
			
			
		/*
		 * Now try and find a command block that we can use.
		 */
		 //请求的special域为空，则要去申请一个scsi_cmnd对象
		if (!req->special) {
			cmd = scsi_get_command(sdev, GFP_ATOMIC);
			if (unlikely(!cmd))
				goto defer;
		} else
			cmd = req->special;
		
		/* pull a tag out of the request if we have one */
		cmd->tag = req->tag;
	} 
	else 
	{
		blk_dump_rq_flags(req, "SCSI bad req");
		goto kill;
	}
	
	/* note the overloading of req->special.  When the tag
	 * is active it always means cmd.  If the tag goes
	 * back for re-queueing, it may be reset */
	req->special = cmd;
	cmd->request = req;
	
	/*
	 * FIXME: drop the lock here because the functions below
	 * expect to be called without the queue lock held.  Also,
	 * previously, we dequeued the request before dropping the
	 * lock.  We hope REQ_STARTED prevents anything untoward from
	 * happening now.
	 */
	 //如果请求是来自于用户进程或者SCSI中间件
	if (req->flags & (REQ_CMD | REQ_BLOCK_PC)) 
	{
		int ret;

		/*
		 * This will do a couple of things:
		 *  1) Fill in the actual SCSI command.
		 *  2) Fill in any other upper-level specific fields
		 * (timeout).
		 *
		 * If this returns 0, it means that the request failed
		 * (reading past end of disk, reading offline device,
		 * etc).   This won't actually talk to the device, but
		 * some kinds of consistency checking may cause the	
		 * request to be rejected immediately.
		 */

		/* 
		 * This sets up the scatter-gather table (allocating if
		 * required).
		 */
		//初始化命令各个域
		ret = scsi_init_io(cmd);
		switch(ret) {
			/* For BLKPREP_KILL/DEFER the cmd was released */
		case BLKPREP_KILL:
			goto kill;
		case BLKPREP_DEFER:
			goto defer;
		}
		
		/*
		 * Initialize the actual SCSI command for this request.
		 */
		 
		if (req->flags & REQ_BLOCK_PC) 
		{//命令来自于SCSI中间层
			scsi_setup_blk_pc_cmnd(cmd);
		} 
		else if (req->rq_disk) 
		{//这个命令来自于用户进程
			struct scsi_driver *drv;

			drv = *(struct scsi_driver **)req->rq_disk->private_data;
			if (unlikely(!drv->init_command(cmd))) 
			{//调用高层驱动中init_command处理函数，如sd驱动的sd_init_command
				scsi_release_buffers(cmd);
				scsi_put_command(cmd);
				goto kill;
			}
		}
	}

	/*
	 * The request is now prepped, no need to come back here
	 */
	req->flags |= REQ_DONTPREP;//表示请求的cmnd已经准备就绪
	return BLKPREP_OK;

 defer:
	/* If we defer, the elv_next_request() returns NULL, but the
	 * queue must be restarted, so we plug here if no returning
	 * command will automatically do that. */
	if (sdev->device_busy == 0)
		blk_plug_device(q);
	return BLKPREP_DEFER;
 kill:
	req->errors = DID_NO_CONNECT << 16;
	return BLKPREP_KILL;
}

/*
 * scsi_dev_queue_ready: if we can send requests to sdev, return 1 else
 * return 0.
 *
 * Called with the queue_lock held.
 */
/**ltl
功能:判定SCSI设备是否准备就绪
参数:q	->请求队列
	sdev	->SCSI设备对象
返回值:0	->设备没有准备去处理请求
	1	->设备已经准备就绪去处理请求
*/
static inline int scsi_dev_queue_ready(struct request_queue *q,
				  struct scsi_device *sdev)
{
	/*SCSI设备正在处理的命令已经超过了最大值，即此SCSI设备处于繁忙状态*/
	if (sdev->device_busy >= sdev->queue_depth)
		return 0;
	//SCSI子系统没有向SCSI设备提交命令，而此时设备能处理的命令数不为0，则把这个记数器减1，同时让块IO子系统开始"畜流"
	if (sdev->device_busy == 0 && sdev->device_blocked) {
		/*
		 * unblock after device_blocked iterates to zero
		 */
		if (--sdev->device_blocked == 0) {
			SCSI_LOG_MLQUEUE(3,
				   sdev_printk(KERN_INFO, sdev,
				   "unblocking device at zero depth\n"));
		} else {//在派发队列中还没有达到max_device_blocked，则让块IO子系统开始"畜流"
		//注:每当"畜流"超时时，就会开始"泄流"，每次"泄流"，都会把device_blocked减1，因此当进程没有发送请求时，不会无限的处理"泄流"状态。
			blk_plug_device(q);
			return 0;
		}
	}
	//，这个情况几乎不会出现。。。。在派发队列中的request个数没有达到max_device_blocked
	if (sdev->device_blocked)
		return 0;

	return 1;
}

/*
 * scsi_host_queue_ready: if we can send requests to shost, return 1 else
 * return 0. We must end up running the queue again whenever 0 is
 * returned, else IO can hang.
 *
 * Called with host_lock held.
 */
/**ltl
功能:判定是可否可以把request派发到主机适配器
参数:q	->请求队列
	shost	->SCSI主机适配器
	sdev	->SCSI设备
返回值:
	0	->表示不能向主机适配器提交请求
	1	->可以提交请求。
*/
static inline int scsi_host_queue_ready(struct request_queue *q,
				   struct Scsi_Host *shost,
				   struct scsi_device *sdev)
{
	//主机适配器的状态处于扫描状态
	if (scsi_host_in_recovery(shost))
		return 0;
	//如果没有请求提交给主机适配器，而且在派发队列中的request数量没有超过max_host_blocked.
	//这个与scsi_dev_queue_ready接口一致。
	if (shost->host_busy == 0 && shost->host_blocked) {
		/*
		 * unblock after host_blocked iterates to zero
		 */
		if (--shost->host_blocked == 0) {
			SCSI_LOG_MLQUEUE(3,
				printk("scsi%d unblocking host at zero depth\n",
					shost->host_no));
		} else {//让块IO子系统处于"畜流"状态
			blk_plug_device(q);
			return 0;
		}
	}
	//如果主机适配器的命令数大于0且主机适配器处理忙的状态(主机适配器接收太多的request或者说主机适配器正在处理的请求比较多--主机控制器"饱"了)
	//或者在派发队列中请求的数量还没有达到max_host_blocked，则把这个SCSI设备添加到"饥饿"队列中---主机控制器"饿"了!
	if ((shost->can_queue > 0 && shost->host_busy >= shost->can_queue) ||
	    shost->host_blocked || shost->host_self_blocked) {
		if (list_empty(&sdev->starved_entry))
			list_add_tail(&sdev->starved_entry, &shost->starved_list);
		return 0;
	}

	/* We're OK to process the command, so we can't be starved */
	//如果设备已经在"饥饿"队列中，则除之
	if (!list_empty(&sdev->starved_entry))
		list_del_init(&sdev->starved_entry);

	return 1;
}

/*
 * Kill a request for a dead device
 */
/**ltl
功能:释放request对象
参数:req	->要释放的request
	q	->请求队列
*/
static void scsi_kill_request(struct request *req, request_queue_t *q)
{
	struct scsi_cmnd *cmd = req->special;
	struct scsi_device *sdev = cmd->device;
	struct Scsi_Host *shost = sdev->host;

	blkdev_dequeue_request(req);//把request从派发队列中删除

	if (unlikely(cmd == NULL)) {
		printk(KERN_CRIT "impossible request in %s.\n",
				 __FUNCTION__);
		BUG();
	}

	scsi_init_cmd_errh(cmd);
	cmd->result = DID_NO_CONNECT << 16;
	atomic_inc(&cmd->device->iorequest_cnt);

	/*
	 * SCSI request completion path will do scsi_device_unbusy(),
	 * bump busy counts.  To bump the counters, we need to dance
	 * with the locks as normal issue path does.
	 */
	//由于在__scsi_done()函数会去调用scsi_device_unbusy()函数，在这个函数里对device_busy--操作，因此在这里要先++
	sdev->device_busy++;
	spin_unlock(sdev->request_queue->queue_lock);
	spin_lock(shost->host_lock);
	shost->host_busy++;//这个字段++的原因device_busy的一样
	spin_unlock(shost->host_lock);
	spin_lock(sdev->request_queue->queue_lock);

	__scsi_done(cmd);//调用done结果cmnd
}
/**ltl
功能:块设备的软件中断处理函数
参数:rq	->已经处理完毕的请求对象
*/
static void scsi_softirq_done(struct request *rq)
{
	struct scsi_cmnd *cmd = rq->completion_data;
	unsigned long wait_for = (cmd->allowed + 1) * cmd->timeout_per_command;
	int disposition;

	INIT_LIST_HEAD(&cmd->eh_entry);

	disposition = scsi_decide_disposition(cmd);
	if (disposition != SUCCESS &&
	    time_before(cmd->jiffies_at_alloc + wait_for, jiffies)) {
		sdev_printk(KERN_ERR, cmd->device,
			    "timing out command, waited %lus\n",
			    wait_for/HZ);
		disposition = SUCCESS;
	}
			
	scsi_log_completion(cmd, disposition);

	switch (disposition) {
		case SUCCESS://命令处理成功
			scsi_finish_command(cmd);
			break;
		case NEEDS_RETRY://处理失败的命令要重试
			scsi_retry_command(cmd);
			break;
		case ADD_TO_MLQUEUE://要重新加入到派发队列中。
			scsi_queue_insert(cmd, SCSI_MLQUEUE_DEVICE_BUSY);
			break;
		default:// 其它的各种错误，这个不是出现超时
			if (!scsi_eh_scmd_add(cmd, 0))//把命令插入到错误处理链表，并唤醒错误处理线程去处理这个发过错误的命令
				scsi_finish_command(cmd);//结束这个错误命令
	}
}

/*
 * Function:    scsi_request_fn()
 *
 * Purpose:     Main strategy routine for SCSI.
 *
 * Arguments:   q       - Pointer to actual queue.
 *
 * Returns:     Nothing
 *
 * Lock status: IO request lock assumed to be held when called.
 */
/**ltl
功能:scsi设备的请求处理函数
参数:	q	->请求队列
*/
static void scsi_request_fn(struct request_queue *q)
{
	struct scsi_device *sdev = q->queuedata;
	struct Scsi_Host *shost;
	struct scsi_cmnd *cmd;
	struct request *req;

	if (!sdev) {//这个请求队列不与设备相关联，则清空派发队列中的所有请求
		printk("scsi: killing requests for dead queue\n");
		while ((req = elv_next_request(q)) != NULL)
			scsi_kill_request(req, q);
		return;
	}

	//此SCSI设备不存在父设备
	if(!get_device(&sdev->sdev_gendev))
		/* We must be tearing the block queue down already */
		return;

	/*
	 * To start with, we keep looping until the queue is empty, or until
	 * the host is no longer able to accept any more requests.
	 */
	shost = sdev->host;
	while (!blk_queue_plugged(q)) {//请求队列不处于"畜流"状态
		int rtn;
		/*
		 * get next queueable request.  We do this early to make sure
		 * that the request is fully prepared even if we cannot 
		 * accept it.
		 */
		req = elv_next_request(q);//从派发队列中获取请求request
		/*没有获取到一个请求，或者设备还没有ready,则退出*/
		if (!req || !scsi_dev_queue_ready(q, sdev))
			break;
		//设备不存在，即设备的状态为SDEV_OFFLINE
		if (unlikely(!scsi_device_online(sdev))) {
			sdev_printk(KERN_ERR, sdev,
				    "rejecting I/O to offline device\n");
			scsi_kill_request(req, q);
			continue;
		}


		/*
		 * Remove the request from the request list.
		 */
		if (!(blk_queue_tagged(q) && !blk_queue_start_tag(q, req)))
			blkdev_dequeue_request(req);//把请求req从派发队列中删除
			
		sdev->device_busy++;//已经派发到底层驱动的命令数

		spin_unlock(q->queue_lock);
		cmd = req->special;//request有special指向scsi_cmnd对象, 对special域赋值是在scsi_prep_fn函数完成
		if (unlikely(cmd == NULL)) {
			printk(KERN_CRIT "impossible request in %s.\n"
					 "please mail a stack trace to "
					 "linux-scsi@vger.kernel.org",
					 __FUNCTION__);
			BUG();
		}
		spin_lock(shost->host_lock);
		/*
		*判定主机适配器是否已经准备就绪,当出现主机控制执行命令失败后，
		*在scsi_eh_scmd_add函数中把scsi_host的状态设置成SHOST_RECOVERY,因此在处理错误时，等待队列不运行。
		*/
		if (!scsi_host_queue_ready(q, shost, sdev))
			goto not_ready;
		//表示这个目标节点在一个时间点上只能处理一个SCSI逻辑设备上的IO请求
		if (sdev->single_lun) {
			if (scsi_target(sdev)->starget_sdev_user &&
			    scsi_target(sdev)->starget_sdev_user != sdev)
				goto not_ready;
			scsi_target(sdev)->starget_sdev_user = sdev;//记录设备指针
		}
		shost->host_busy++;//往主机适配器发送的命令数

		/*
		 * XXX(hch): This is rather suboptimal, scsi_dispatch_cmd will
		 *		take the lock again.
		 */
		spin_unlock_irq(shost->host_lock);

		/*
		 * Finally, initialize any error handling parameters, and set up
		 * the timers for timeouts.
		 */
		scsi_init_cmd_errh(cmd);

		/*
		 * Dispatch the command to the low-level driver.
		 */
		 //分发请求命令到底层驱动
		rtn = scsi_dispatch_cmd(cmd);
		spin_lock_irq(q->queue_lock);
		if(rtn) {
			/* we're refusing the command; because of
			 * the way locks get dropped, we need to 
			 * check here if plugging is required */
			if(sdev->device_busy == 0)
				blk_plug_device(q);

			break;
		}
	}

	goto out;

 not_ready:
	spin_unlock_irq(shost->host_lock);

	/*
	 * lock q, handle tag, requeue req, and decrement device_busy. We
	 * must return with queue_lock held.
	 *
	 * Decrementing device_busy without checking it is OK, as all such
	 * cases (host limits or settings) should run the queue at some
	 * later time.
	 */
	spin_lock_irq(q->queue_lock);
	blk_requeue_request(q, req);
	sdev->device_busy--;
	if(sdev->device_busy == 0)
		blk_plug_device(q);
 out:
	/* must be careful here...if we trigger the ->remove() function
	 * we cannot be holding the q lock */
	spin_unlock_irq(q->queue_lock);
	put_device(&sdev->sdev_gendev);
	spin_lock_irq(q->queue_lock);
}

u64 scsi_calculate_bounce_limit(struct Scsi_Host *shost)
{
	struct device *host_dev;
	u64 bounce_limit = 0xffffffff;

	if (shost->unchecked_isa_dma)
		return BLK_BOUNCE_ISA;
	/*
	 * Platforms with virtual-DMA translation
	 * hardware have no practical limit.
	 */
	if (!PCI_DMA_BUS_IS_PHYS)
		return BLK_BOUNCE_ANY;

	host_dev = scsi_get_device(shost);
	if (host_dev && host_dev->dma_mask)
		bounce_limit = *host_dev->dma_mask;

	return bounce_limit;
}
EXPORT_SYMBOL(scsi_calculate_bounce_limit);

/**ltl
功能:为SCSI设备分配请求队列
参数:sdev	->SCSI设备
返回值:申请到的请求队列对象
*/
struct request_queue *scsi_alloc_queue(struct scsi_device *sdev)
{
	struct Scsi_Host *shost = sdev->host;
	struct request_queue *q;
	//调用块IO层的请求队列分配函数，scsi_request_fn是这个请求队列的策略处理函数。
	q = blk_init_queue(scsi_request_fn, NULL);
	if (!q)
		return NULL;
	//为请求队列指定预先处理函数
	blk_queue_prep_rq(q, scsi_prep_fn);
	//设定硬件段数(request中请求数据的最大段数)
	blk_queue_max_hw_segments(q, shost->sg_tablesize/*=128*/);
	//设定物理段大小
	blk_queue_max_phys_segments(q, SCSI_MAX_PHYS_SEGMENTS/*=128*/);
	//请求队列的最大扇区数,对ata_piix驱动，这个值为200,对虚拟机驱动，这个值为8192
	blk_queue_max_sectors(q, shost->max_sectors);
	blk_queue_bounce_limit(q, scsi_calculate_bounce_limit(shost));
	blk_queue_segment_boundary(q, shost->dma_boundary);
	//刷新的处理函数
	blk_queue_issue_flush_fn(q, scsi_issue_flush_fn);
	//BLOCK_SOFTIRQ软中断的处理函数
	blk_queue_softirq_done(q, scsi_softirq_done);

	if (!shost->use_clustering)
		clear_bit(QUEUE_FLAG_CLUSTER, &q->queue_flags);
	return q;
}

void scsi_free_queue(struct request_queue *q)
{
	blk_cleanup_queue(q);
}

/*
 * Function:    scsi_block_requests()
 *
 * Purpose:     Utility function used by low-level drivers to prevent further
 *		commands from being queued to the device.
 *
 * Arguments:   shost       - Host in question
 *
 * Returns:     Nothing
 *
 * Lock status: No locks are assumed held.
 *
 * Notes:       There is no timer nor any other means by which the requests
 *		get unblocked other than the low-level driver calling
 *		scsi_unblock_requests().
 */
void scsi_block_requests(struct Scsi_Host *shost)
{
	shost->host_self_blocked = 1;
}
EXPORT_SYMBOL(scsi_block_requests);

/*
 * Function:    scsi_unblock_requests()
 *
 * Purpose:     Utility function used by low-level drivers to allow further
 *		commands from being queued to the device.
 *
 * Arguments:   shost       - Host in question
 *
 * Returns:     Nothing
 *
 * Lock status: No locks are assumed held.
 *
 * Notes:       There is no timer nor any other means by which the requests
 *		get unblocked other than the low-level driver calling
 *		scsi_unblock_requests().
 *
 *		This is done as an API function so that changes to the
 *		internals of the scsi mid-layer won't require wholesale
 *		changes to drivers that use this feature.
 */
void scsi_unblock_requests(struct Scsi_Host *shost)
{
	shost->host_self_blocked = 0;
	scsi_run_host_queues(shost);
}
EXPORT_SYMBOL(scsi_unblock_requests);

int __init scsi_init_queue(void)
{
	int i;
	//创建高速缓存:这个高速缓存的特点是每一块的大小一样，并且经常被使用。第个对象的大小由size指定
	scsi_io_context_cache = kmem_cache_create("scsi_io_context",
					sizeof(struct scsi_io_context),
					0, 0, NULL, NULL);
	if (!scsi_io_context_cache) {
		printk(KERN_ERR "SCSI: can't init scsi io context cache\n");
		return -ENOMEM;
	}

	for (i = 0; i < SG_MEMPOOL_NR; i++) {
		struct scsi_host_sg_pool *sgp = scsi_sg_pools + i;
		int size = sgp->size * sizeof(struct scatterlist);
		//分配高速缓存，每一块内存大小为size
		sgp->slab = kmem_cache_create(sgp->name, size, 0,
				SLAB_HWCACHE_ALIGN, NULL, NULL);
		if (!sgp->slab) {
			printk(KERN_ERR "SCSI: can't init sg slab %s\n",
					sgp->name);
		}
		//分配内存池，这个池中至少要有SG_MEMPOOL_SIZE块空闲的内存对象
		//在分配scetterlist时会去分配(scsi_alloc_sgtable)
		sgp->pool = mempool_create_slab_pool(SG_MEMPOOL_SIZE,
						     sgp->slab);
		if (!sgp->pool) {
			printk(KERN_ERR "SCSI: can't init sg mempool %s\n",
					sgp->name);
		}
	}

	return 0;
}

void scsi_exit_queue(void)
{
	int i;

	kmem_cache_destroy(scsi_io_context_cache);

	for (i = 0; i < SG_MEMPOOL_NR; i++) {
		struct scsi_host_sg_pool *sgp = scsi_sg_pools + i;
		mempool_destroy(sgp->pool);
		kmem_cache_destroy(sgp->slab);
	}
}

/**
 *	scsi_mode_select - issue a mode select
 *	@sdev:	SCSI device to be queried
 *	@pf:	Page format bit (1 == standard, 0 == vendor specific)
 *	@sp:	Save page bit (0 == don't save, 1 == save)
 *	@modepage: mode page being requested
 *	@buffer: request buffer (may not be smaller than eight bytes)
 *	@len:	length of request buffer.
 *	@timeout: command timeout
 *	@retries: number of retries before failing
 *	@data: returns a structure abstracting the mode header data
 *	@sense: place to put sense data (or NULL if no sense to be collected).
 *		must be SCSI_SENSE_BUFFERSIZE big.
 *
 *	Returns zero if successful; negative error number or scsi
 *	status on error
 *
 */
int
scsi_mode_select(struct scsi_device *sdev, int pf, int sp, int modepage,
		 unsigned char *buffer, int len, int timeout, int retries,
		 struct scsi_mode_data *data, struct scsi_sense_hdr *sshdr)
{
	unsigned char cmd[10];
	unsigned char *real_buffer;
	int ret;

	memset(cmd, 0, sizeof(cmd));
	cmd[1] = (pf ? 0x10 : 0) | (sp ? 0x01 : 0);

	if (sdev->use_10_for_ms) {
		if (len > 65535)
			return -EINVAL;
		real_buffer = kmalloc(8 + len, GFP_KERNEL);
		if (!real_buffer)
			return -ENOMEM;
		memcpy(real_buffer + 8, buffer, len);
		len += 8;
		real_buffer[0] = 0;
		real_buffer[1] = 0;
		real_buffer[2] = data->medium_type;
		real_buffer[3] = data->device_specific;
		real_buffer[4] = data->longlba ? 0x01 : 0;
		real_buffer[5] = 0;
		real_buffer[6] = data->block_descriptor_length >> 8;
		real_buffer[7] = data->block_descriptor_length;

		cmd[0] = MODE_SELECT_10;
		cmd[7] = len >> 8;
		cmd[8] = len;
	} else {
		if (len > 255 || data->block_descriptor_length > 255 ||
		    data->longlba)
			return -EINVAL;

		real_buffer = kmalloc(4 + len, GFP_KERNEL);
		if (!real_buffer)
			return -ENOMEM;
		memcpy(real_buffer + 4, buffer, len);
		len += 4;
		real_buffer[0] = 0;
		real_buffer[1] = data->medium_type;
		real_buffer[2] = data->device_specific;
		real_buffer[3] = data->block_descriptor_length;
		

		cmd[0] = MODE_SELECT;
		cmd[4] = len;
	}

	ret = scsi_execute_req(sdev, cmd, DMA_TO_DEVICE, real_buffer, len,
			       sshdr, timeout, retries);
	kfree(real_buffer);
	return ret;
}
EXPORT_SYMBOL_GPL(scsi_mode_select);

/**
 *	scsi_mode_sense - issue a mode sense, falling back from 10 to 
 *		six bytes if necessary.
 *	@sdev:	SCSI device to be queried
 *	@dbd:	set if mode sense will allow block descriptors to be returned
 *	@modepage: mode page being requested
 *	@buffer: request buffer (may not be smaller than eight bytes)
 *	@len:	length of request buffer.
 *	@timeout: command timeout
 *	@retries: number of retries before failing
 *	@data: returns a structure abstracting the mode header data
 *	@sense: place to put sense data (or NULL if no sense to be collected).
 *		must be SCSI_SENSE_BUFFERSIZE big.
 *
 *	Returns zero if unsuccessful, or the header offset (either 4
 *	or 8 depending on whether a six or ten byte command was
 *	issued) if successful.
 **/
/*获取mode sense*/
int scsi_mode_sense(struct scsi_device *sdev, int dbd, int modepage,
		  unsigned char *buffer, int len, int timeout, int retries,
		  struct scsi_mode_data *data, struct scsi_sense_hdr *sshdr)
{
	unsigned char cmd[12];
	int use_10_for_ms;
	int header_length;
	int result;
	struct scsi_sense_hdr my_sshdr;

	memset(data, 0, sizeof(*data));
	memset(&cmd[0], 0, 12);
	cmd[1] = dbd & 0x18;	/* allows DBD and LLBA bits */
	cmd[2] = modepage;

	/* caller might not be interested in sense, but we need it */
	if (!sshdr)
		sshdr = &my_sshdr;

 retry:
 	//configure in ata_scsi_sdev_config function
	use_10_for_ms = sdev->use_10_for_ms;

	if (use_10_for_ms) {
		if (len < 8)
			len = 8;

		cmd[0] = MODE_SENSE_10;
		cmd[8] = len;
		header_length = 8;
	} else {
		if (len < 4)
			len = 4;

		cmd[0] = MODE_SENSE;
		cmd[4] = len;
		header_length = 4;
	}

	memset(buffer, 0, len);

	result = scsi_execute_req(sdev, cmd, DMA_FROM_DEVICE, buffer, len,
				  sshdr, timeout, retries);

	/* This code looks awful: what it's doing is making sure an
	 * ILLEGAL REQUEST sense return identifies the actual command
	 * byte as the problem.  MODE_SENSE commands can return
	 * ILLEGAL REQUEST if the code page isn't supported */

	if (use_10_for_ms && !scsi_status_is_good(result) &&
	    (driver_byte(result) & DRIVER_SENSE)) {
		if (scsi_sense_valid(sshdr)) {
			if ((sshdr->sense_key == ILLEGAL_REQUEST) &&
			    (sshdr->asc == 0x20) && (sshdr->ascq == 0)) {
				/* 
				 * Invalid command operation code
				 */
				sdev->use_10_for_ms = 0;
				goto retry;
			}
		}
	}

	if(scsi_status_is_good(result)) {
		if (unlikely(buffer[0] == 0x86 && buffer[1] == 0x0b &&
			     (modepage == 6 || modepage == 8))) 
		{
			/* Initio breakage? */
			header_length = 0;
			data->length = 13;
			data->medium_type = 0;
			data->device_specific = 0;
			data->longlba = 0;
			data->block_descriptor_length = 0;
		}
		else if(use_10_for_ms) 
		{//mode sense 10
			data->length = buffer[0]*256 + buffer[1] + 2;
			data->medium_type = buffer[2];
			data->device_specific = buffer[3];
			data->longlba = buffer[4] & 0x01;
			data->block_descriptor_length = buffer[6]*256 + buffer[7];
		} 
		else 
		{//mode sense 6
			data->length = buffer[0] + 1;
			data->medium_type = buffer[1];
			data->device_specific = buffer[2];
			data->block_descriptor_length = buffer[3];
		}
		data->header_length = header_length;
	}

	return result;
}
EXPORT_SYMBOL(scsi_mode_sense);

/**ltl
功能:检查设备是否就绪
参数:
说明:
*/
int scsi_test_unit_ready(struct scsi_device *sdev, int timeout, int retries)
{
	char cmd[] = {
		TEST_UNIT_READY, 0, 0, 0, 0, 0,
	};
	struct scsi_sense_hdr sshdr;
	int result;
	
	result = scsi_execute_req(sdev, cmd, DMA_NONE, NULL, 0, &sshdr,
				  timeout, retries);

	if ((driver_byte(result) & DRIVER_SENSE) && sdev->removable) {

		if ((scsi_sense_valid(&sshdr)) &&
		    ((sshdr.sense_key == UNIT_ATTENTION) ||
		     (sshdr.sense_key == NOT_READY))) {
			sdev->changed = 1;
			result = 0;
		}
	}
	return result;
}
EXPORT_SYMBOL(scsi_test_unit_ready);

/**
 *	scsi_device_set_state - Take the given device through the device
 *		state model.
 *	@sdev:	scsi device to change the state of.
 *	@state:	state to change to.
 *
 *	Returns zero if unsuccessful or an error if the requested 
 *	transition is illegal.
 **/
int
scsi_device_set_state(struct scsi_device *sdev, enum scsi_device_state state)
{
	enum scsi_device_state oldstate = sdev->sdev_state;

	if (state == oldstate)
		return 0;

	switch (state) {
	case SDEV_CREATED:
		/* There are no legal states that come back to
		 * created.  This is the manually initialised start
		 * state */
		goto illegal;
			
	case SDEV_RUNNING:
		switch (oldstate) {
		case SDEV_CREATED:
		case SDEV_OFFLINE:
		case SDEV_QUIESCE:
		case SDEV_BLOCK:
			break;
		default:
			goto illegal;
		}
		break;

	case SDEV_QUIESCE:
		switch (oldstate) {
		case SDEV_RUNNING:
		case SDEV_OFFLINE:
			break;
		default:
			goto illegal;
		}
		break;

	case SDEV_OFFLINE:
		switch (oldstate) {
		case SDEV_CREATED:
		case SDEV_RUNNING:
		case SDEV_QUIESCE:
		case SDEV_BLOCK:
			break;
		default:
			goto illegal;
		}
		break;

	case SDEV_BLOCK:
		switch (oldstate) {
		case SDEV_CREATED:
		case SDEV_RUNNING:
			break;
		default:
			goto illegal;
		}
		break;

	case SDEV_CANCEL:
		switch (oldstate) {
		case SDEV_CREATED:
		case SDEV_RUNNING:
		case SDEV_QUIESCE:
		case SDEV_OFFLINE:
		case SDEV_BLOCK:
			break;
		default:
			goto illegal;
		}
		break;

	case SDEV_DEL:
		switch (oldstate) {
		case SDEV_CREATED:
		case SDEV_RUNNING:
		case SDEV_OFFLINE:
		case SDEV_CANCEL:
			break;
		default:
			goto illegal;
		}
		break;

	}
	sdev->sdev_state = state;
	return 0;

 illegal:
	SCSI_LOG_ERROR_RECOVERY(1, 
				sdev_printk(KERN_ERR, sdev,
					    "Illegal state transition %s->%s\n",
					    scsi_device_state_name(oldstate),
					    scsi_device_state_name(state))
				);
	return -EINVAL;
}
EXPORT_SYMBOL(scsi_device_set_state);

/**
 *	scsi_device_quiesce - Block user issued commands.
 *	@sdev:	scsi device to quiesce.
 *
 *	This works by trying to transition to the SDEV_QUIESCE state
 *	(which must be a legal transition).  When the device is in this
 *	state, only special requests will be accepted, all others will
 *	be deferred.  Since special requests may also be requeued requests,
 *	a successful return doesn't guarantee the device will be 
 *	totally quiescent.
 *
 *	Must be called with user context, may sleep.
 *
 *	Returns zero if unsuccessful or an error if not.
 **/
int
scsi_device_quiesce(struct scsi_device *sdev)
{
	int err = scsi_device_set_state(sdev, SDEV_QUIESCE);
	if (err)
		return err;

	scsi_run_queue(sdev->request_queue);
	while (sdev->device_busy) {
		msleep_interruptible(200);
		scsi_run_queue(sdev->request_queue);
	}
	return 0;
}
EXPORT_SYMBOL(scsi_device_quiesce);

/**
 *	scsi_device_resume - Restart user issued commands to a quiesced device.
 *	@sdev:	scsi device to resume.
 *
 *	Moves the device from quiesced back to running and restarts the
 *	queues.
 *
 *	Must be called with user context, may sleep.
 **/
void
scsi_device_resume(struct scsi_device *sdev)
{
	if(scsi_device_set_state(sdev, SDEV_RUNNING))
		return;
	scsi_run_queue(sdev->request_queue);
}
EXPORT_SYMBOL(scsi_device_resume);

static void
device_quiesce_fn(struct scsi_device *sdev, void *data)
{
	scsi_device_quiesce(sdev);
}

void
scsi_target_quiesce(struct scsi_target *starget)
{
	starget_for_each_device(starget, NULL, device_quiesce_fn);
}
EXPORT_SYMBOL(scsi_target_quiesce);

static void
device_resume_fn(struct scsi_device *sdev, void *data)
{
	scsi_device_resume(sdev);
}

void
scsi_target_resume(struct scsi_target *starget)
{
	starget_for_each_device(starget, NULL, device_resume_fn);
}
EXPORT_SYMBOL(scsi_target_resume);

/**
 * scsi_internal_device_block - internal function to put a device
 *				temporarily into the SDEV_BLOCK state
 * @sdev:	device to block
 *
 * Block request made by scsi lld's to temporarily stop all
 * scsi commands on the specified device.  Called from interrupt
 * or normal process context.
 *
 * Returns zero if successful or error if not
 *
 * Notes:       
 *	This routine transitions the device to the SDEV_BLOCK state
 *	(which must be a legal transition).  When the device is in this
 *	state, all commands are deferred until the scsi lld reenables
 *	the device with scsi_device_unblock or device_block_tmo fires.
 *	This routine assumes the host_lock is held on entry.
 **/
int
scsi_internal_device_block(struct scsi_device *sdev)
{
	request_queue_t *q = sdev->request_queue;
	unsigned long flags;
	int err = 0;

	err = scsi_device_set_state(sdev, SDEV_BLOCK);
	if (err)
		return err;

	/* 
	 * The device has transitioned to SDEV_BLOCK.  Stop the
	 * block layer from calling the midlayer with this device's
	 * request queue. 
	 */
	spin_lock_irqsave(q->queue_lock, flags);
	blk_stop_queue(q);
	spin_unlock_irqrestore(q->queue_lock, flags);

	return 0;
}
EXPORT_SYMBOL_GPL(scsi_internal_device_block);
 
/**
 * scsi_internal_device_unblock - resume a device after a block request
 * @sdev:	device to resume
 *
 * Called by scsi lld's or the midlayer to restart the device queue
 * for the previously suspended scsi device.  Called from interrupt or
 * normal process context.
 *
 * Returns zero if successful or error if not.
 *
 * Notes:       
 *	This routine transitions the device to the SDEV_RUNNING state
 *	(which must be a legal transition) allowing the midlayer to
 *	goose the queue for this device.  This routine assumes the 
 *	host_lock is held upon entry.
 **/
int
scsi_internal_device_unblock(struct scsi_device *sdev)
{
	request_queue_t *q = sdev->request_queue; 
	int err;
	unsigned long flags;
	
	/* 
	 * Try to transition the scsi device to SDEV_RUNNING
	 * and goose the device queue if successful.  
	 */
	err = scsi_device_set_state(sdev, SDEV_RUNNING);
	if (err)
		return err;

	spin_lock_irqsave(q->queue_lock, flags);
	blk_start_queue(q);
	spin_unlock_irqrestore(q->queue_lock, flags);

	return 0;
}
EXPORT_SYMBOL_GPL(scsi_internal_device_unblock);

static void
device_block(struct scsi_device *sdev, void *data)
{
	scsi_internal_device_block(sdev);
}

static int
target_block(struct device *dev, void *data)
{
	if (scsi_is_target_device(dev))
		starget_for_each_device(to_scsi_target(dev), NULL,
					device_block);
	return 0;
}

void
scsi_target_block(struct device *dev)
{
	if (scsi_is_target_device(dev))
		starget_for_each_device(to_scsi_target(dev), NULL,
					device_block);
	else
		device_for_each_child(dev, NULL, target_block);
}
EXPORT_SYMBOL_GPL(scsi_target_block);

static void
device_unblock(struct scsi_device *sdev, void *data)
{
	scsi_internal_device_unblock(sdev);
}

static int
target_unblock(struct device *dev, void *data)
{
	if (scsi_is_target_device(dev))
		starget_for_each_device(to_scsi_target(dev), NULL,
					device_unblock);
	return 0;
}

void
scsi_target_unblock(struct device *dev)
{
	if (scsi_is_target_device(dev))
		starget_for_each_device(to_scsi_target(dev), NULL,
					device_unblock);
	else
		device_for_each_child(dev, NULL, target_unblock);
}
EXPORT_SYMBOL_GPL(scsi_target_unblock);

/**
 * scsi_kmap_atomic_sg - find and atomically map an sg-elemnt
 * @sg:		scatter-gather list
 * @sg_count:	number of segments in sg
 * @offset:	offset in bytes into sg, on return offset into the mapped area
 * @len:	bytes to map, on return number of bytes mapped
 *
 * Returns virtual address of the start of the mapped page
 */
void *scsi_kmap_atomic_sg(struct scatterlist *sg, int sg_count,
			  size_t *offset, size_t *len)
{
	int i;
	size_t sg_len = 0, len_complete = 0;
	struct page *page;

	for (i = 0; i < sg_count; i++) {
		len_complete = sg_len; /* Complete sg-entries */
		sg_len += sg[i].length;
		if (sg_len > *offset)
			break;
	}

	if (unlikely(i == sg_count)) {
		printk(KERN_ERR "%s: Bytes in sg: %zu, requested offset %zu, "
			"elements %d\n",
		       __FUNCTION__, sg_len, *offset, sg_count);
		WARN_ON(1);
		return NULL;
	}

	/* Offset starting from the beginning of first page in this sg-entry */
	*offset = *offset - len_complete + sg[i].offset;

	/* Assumption: contiguous pages can be accessed as "page + i" */
	page = nth_page(sg[i].page, (*offset >> PAGE_SHIFT));
	*offset &= ~PAGE_MASK;

	/* Bytes in this sg-entry from *offset to the end of the page */
	sg_len = PAGE_SIZE - *offset;
	if (*len > sg_len)
		*len = sg_len;

	return kmap_atomic(page, KM_BIO_SRC_IRQ);
}
EXPORT_SYMBOL(scsi_kmap_atomic_sg);

/**
 * scsi_kunmap_atomic_sg - atomically unmap a virtual address, previously
 *			   mapped with scsi_kmap_atomic_sg
 * @virt:	virtual address to be unmapped
 */
void scsi_kunmap_atomic_sg(void *virt)
{
	kunmap_atomic(virt, KM_BIO_SRC_IRQ);
}
EXPORT_SYMBOL(scsi_kunmap_atomic_sg);
