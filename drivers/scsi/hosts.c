/*
 *  hosts.c Copyright (C) 1992 Drew Eckhardt
 *          Copyright (C) 1993, 1994, 1995 Eric Youngdale
 *          Copyright (C) 2002-2003 Christoph Hellwig
 *
 *  mid to lowlevel SCSI driver interface
 *      Initial versions: Drew Eckhardt
 *      Subsequent revisions: Eric Youngdale
 *
 *  <drew@colorado.edu>
 *
 *  Jiffies wrap fixes (host->resetting), 3 Dec 1998 Andrea Arcangeli
 *  Added QLOGIC QLA1280 SCSI controller kernel host support. 
 *     August 4, 1999 Fred Lewis, Intel DuPont
 *
 *  Updated to reflect the new initialization scheme for the higher 
 *  level of scsi drivers (sd/sr/st)
 *  September 17, 2000 Torben Mathiasen <tmm@image.dk>
 *
 *  Restructured scsi_host lists and associated functions.
 *  September 04, 2002 Mike Anderson (andmike@us.ibm.com)
 */

#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/init.h>
#include <linux/completion.h>
#include <linux/transport_class.h>
#include <linux/platform_device.h>

#include <scsi/scsi_device.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_transport.h>

#include "scsi_priv.h"
#include "scsi_logging.h"


static int scsi_host_next_hn;		/* host_no for next new host */


static void scsi_host_cls_release(struct class_device *class_dev)
{
	put_device(&class_to_shost(class_dev)->shost_gendev);
}

static struct class shost_class = {
	.name		= "scsi_host",
	.release	= scsi_host_cls_release,
};

/**
 *	scsi_host_set_state - Take the given host through the host
 *		state model.
 *	@shost:	scsi host to change the state of.
 *	@state:	state to change to.
 *
 *	Returns zero if unsuccessful or an error if the requested
 *	transition is illegal.
 **/
int scsi_host_set_state(struct Scsi_Host *shost, enum scsi_host_state state)
{
	enum scsi_host_state oldstate = shost->shost_state;

	if (state == oldstate)
		return 0;

	switch (state) {
	case SHOST_CREATED:
		/* There are no legal states that come back to
		 * created.  This is the manually initialised start
		 * state */
		goto illegal;

	case SHOST_RUNNING:
		switch (oldstate) {
		case SHOST_CREATED:
		case SHOST_RECOVERY:
			break;
		default:
			goto illegal;
		}
		break;

	case SHOST_RECOVERY:
		switch (oldstate) {
		case SHOST_RUNNING:
			break;
		default:
			goto illegal;
		}
		break;

	case SHOST_CANCEL:
		switch (oldstate) {
		case SHOST_CREATED:
		case SHOST_RUNNING:
		case SHOST_CANCEL_RECOVERY:
			break;
		default:
			goto illegal;
		}
		break;

	case SHOST_DEL:
		switch (oldstate) {
		case SHOST_CANCEL:
		case SHOST_DEL_RECOVERY:
			break;
		default:
			goto illegal;
		}
		break;

	case SHOST_CANCEL_RECOVERY:
		switch (oldstate) {
		case SHOST_CANCEL:
		case SHOST_RECOVERY:
			break;
		default:
			goto illegal;
		}
		break;

	case SHOST_DEL_RECOVERY:
		switch (oldstate) {
		case SHOST_CANCEL_RECOVERY:
			break;
		default:
			goto illegal;
		}
		break;
	}
	shost->shost_state = state;
	return 0;

 illegal:
	SCSI_LOG_ERROR_RECOVERY(1,
				shost_printk(KERN_ERR, shost,
					     "Illegal host state transition"
					     "%s->%s\n",
					     scsi_host_state_name(oldstate),
					     scsi_host_state_name(state)));
	return -EINVAL;
}
EXPORT_SYMBOL(scsi_host_set_state);

/**
 * scsi_remove_host - remove a scsi host
 * @shost:	a pointer to a scsi host to remove
 **/
void scsi_remove_host(struct Scsi_Host *shost)
{
	unsigned long flags;
	mutex_lock(&shost->scan_mutex);
	spin_lock_irqsave(shost->host_lock, flags);
	if (scsi_host_set_state(shost, SHOST_CANCEL))
		if (scsi_host_set_state(shost, SHOST_CANCEL_RECOVERY)) {
			spin_unlock_irqrestore(shost->host_lock, flags);
			mutex_unlock(&shost->scan_mutex);
			return;
		}
	spin_unlock_irqrestore(shost->host_lock, flags);
	mutex_unlock(&shost->scan_mutex);
	scsi_forget_host(shost);
	scsi_proc_host_rm(shost);

	spin_lock_irqsave(shost->host_lock, flags);
	if (scsi_host_set_state(shost, SHOST_DEL))
		BUG_ON(scsi_host_set_state(shost, SHOST_DEL_RECOVERY));
	spin_unlock_irqrestore(shost->host_lock, flags);

	transport_unregister_device(&shost->shost_gendev);
	class_device_unregister(&shost->shost_classdev);
	device_del(&shost->shost_gendev);
	scsi_proc_hostdir_rm(shost->hostt);
}
EXPORT_SYMBOL(scsi_remove_host);

/**
 * scsi_add_host - add a scsi host
 * @shost:	scsi host pointer to add
 * @dev:	a struct device of type scsi class
 *
 * Return value: 
 * 	0 on success / != 0 for error
 **/
/**ltl
功能:把分配成功并做初始化的主机适配器添加到系统中，主要是添加到sys系统中
参数:shost	->主机适配器
	dev	->此主机适配器所指的父设置(通常是一个pci设备)
返回值:0	->添加成功
*/
int scsi_add_host(struct Scsi_Host *shost, struct device *dev)
{
	struct scsi_host_template *sht = shost->hostt;
	int error = -EINVAL;

	printk(KERN_INFO "scsi%d : %s\n", shost->host_no,
			sht->info ? sht->info(shost) : sht->name);

	if (!shost->can_queue) {
		printk(KERN_ERR "%s: can_queue = 0 no longer supported\n",
				sht->name);
		goto out;
	}
	//设置此主机适配器的父设备(PCI设备)
	if (!shost->shost_gendev.parent)
		shost->shost_gendev.parent = dev ? dev : &platform_bus;

	//添加到sys子系统中。
	error = device_add(&shost->shost_gendev);
	if (error)
		goto out;
	//设置此主机适配器的状态:运行
	scsi_host_set_state(shost, SHOST_RUNNING);
	get_device(shost->shost_gendev.parent);

	error = class_device_add(&shost->shost_classdev);
	if (error)
		goto out_del_gendev;

	get_device(&shost->shost_gendev);

	if (shost->transportt->host_size &&
	    (shost->shost_data = kmalloc(shost->transportt->host_size,
					 GFP_KERNEL)) == NULL)
		goto out_del_classdev;

	//如果设置了此域，表明利用主机适配器的工作队列来处理scsi_cmnd命令。很多的驱动没有用到这个域。
	if (shost->transportt->create_work_queue) {
		snprintf(shost->work_q_name, KOBJ_NAME_LEN, "scsi_wq_%d",
			shost->host_no);
		shost->work_q = create_singlethread_workqueue(
					shost->work_q_name);
		if (!shost->work_q)
			goto out_free_shost_data;
	}
	//在/sys中创建与host相关的属性文件
	error = scsi_sysfs_add_host(shost);
	if (error)
		goto out_destroy_host;
	//在/proc/scsi文件下创建相应的文件，名字是主机控制号，如:/proc/scsi/usb-storage/6
	scsi_proc_host_add(shost);
	return error;

 out_destroy_host:
	if (shost->work_q)
		destroy_workqueue(shost->work_q);
 out_free_shost_data:
	kfree(shost->shost_data);
 out_del_classdev:
	class_device_del(&shost->shost_classdev);
 out_del_gendev:
	device_del(&shost->shost_gendev);
 out:
	return error;
}
EXPORT_SYMBOL(scsi_add_host);

static void scsi_host_dev_release(struct device *dev)
{
	struct Scsi_Host *shost = dev_to_shost(dev);
	struct device *parent = dev->parent;

	if (shost->ehandler)
		kthread_stop(shost->ehandler);
	if (shost->work_q)
		destroy_workqueue(shost->work_q);

	scsi_destroy_command_freelist(shost);
	kfree(shost->shost_data);

	if (parent)
		put_device(parent);
	kfree(shost);
}

/**
 * scsi_host_alloc - register a scsi host adapter instance.
 * @sht:	pointer to scsi host template
 * @privsize:	extra bytes to allocate for driver
 *
 * Note:
 * 	Allocate a new Scsi_Host and perform basic initialization.
 * 	The host is not published to the scsi midlayer until scsi_add_host
 * 	is called.
 *
 * Return value:
 * 	Pointer to a new Scsi_Host
 **/
/**ltl
功能:分配SCSI主机适配器的对象
参数:sht	->主机适配器模板，这个模板有与驱动相关的操作函数
	privsize	->Scsi_Host主机适配器的私有数据的大小，一般是与底层驱动相关的数据结构的大小
返回值:SCSI主机适配器对象
*/
struct Scsi_Host *scsi_host_alloc(struct scsi_host_template *sht, int privsize)
{
	struct Scsi_Host *shost;
	gfp_t gfp_mask = GFP_KERNEL;
	int rval;

	//在DMA区域分配私有数据空间(0-16M)
	if (sht->unchecked_isa_dma && privsize)
		gfp_mask |= __GFP_DMA;

	//分配Scsi_Host对象和私有数据的空间，并初始化为0
	shost = kzalloc(sizeof(struct Scsi_Host) + privsize, gfp_mask);
	if (!shost)
		return NULL;

	spin_lock_init(&shost->default_lock);
	scsi_assign_lock(shost, &shost->default_lock);
	shost->shost_state = SHOST_CREATED;//设置SCSI主机适配器的状态:创建
	INIT_LIST_HEAD(&shost->__devices);
	INIT_LIST_HEAD(&shost->__targets);
	//命令处理队列。当底层驱动处理命令出错时，就把这个命令插入到eh_cmd_q列表中，然后唤醒错误处理线程，去处理这些命令
	INIT_LIST_HEAD(&shost->eh_cmd_q);
	//"饥饿"队列，当主机适配器中的命令总数小于can_queue，则要把SCSI设备添加到"饥饿"队列中。当超过某个值后，再从这个队列中取出SCSI设备对象，去处理相应的命令
	INIT_LIST_HEAD(&shost->starved_list);
	//主机适配器的等待队列。这个等待队列作用:当scsi设备处于错误处理时，就不能再去接收正常的命令，因此要把当前的进程添加到等待队列中。
	init_waitqueue_head(&shost->host_wait);

	mutex_init(&shost->scan_mutex);
	//设置主机适配器的状态
	shost->host_no = scsi_host_next_hn++; /* XXX(hch): still racy */
	shost->dma_channel = 0xff;

	/* These three are default values which can be overridden */
	shost->max_channel = 0;
	shost->max_id = 8;//目标设备的最大值
	shost->max_lun = 8;//每个目标设备中包括多少的逻辑号。

	/* Give each shost a default transportt */
	shost->transportt = &blank_transport_template;

	/*
	 * All drivers right now should be able to handle 12 byte
	 * commands.  Every so often there are requests for 16 byte
	 * commands, but individual low-level drivers need to certify that
	 * they actually do something sensible with such commands.
	 */
	shost->max_cmd_len = 12;//最大命令长度(12字节)
	shost->hostt = sht;	//设置此主机适配器的模板
	shost->this_id = sht->this_id;
	shost->can_queue = sht->can_queue;//此主机适配器在同一时刻可以接收的命令数
	shost->sg_tablesize = sht->sg_tablesize;//支持主机适配器的scatterlist大小
	shost->cmd_per_lun = sht->cmd_per_lun;//
	shost->unchecked_isa_dma = sht->unchecked_isa_dma;//是否在DMA区域分配空间
	shost->use_clustering = sht->use_clustering;//可以合并内存连续IO请求
	shost->ordered_tag = sht->ordered_tag;
	//如果主机适配器中的命令数小于这个值，则暂时阻塞此SCSI设备。
	if (sht->max_host_blocked)
		shost->max_host_blocked = sht->max_host_blocked;
	else
		shost->max_host_blocked = SCSI_DEFAULT_HOST_BLOCKED;/*=7*/

	/*
	 * If the driver imposes no hard sector transfer limit, start at
	 * machine infinity initially.
	 */
	if (sht->max_sectors)
		shost->max_sectors = sht->max_sectors;//单个SCSI命令可以访问磁盘扇区的最大值
	else
		shost->max_sectors = SCSI_DEFAULT_MAX_SECTORS;

	/*
	 * assume a 4GB boundary, if not set
	 */
	if (sht->dma_boundary)
		shost->dma_boundary = sht->dma_boundary;
	else
		shost->dma_boundary = 0xffffffff;

	//用于预先分配scsi_cmnd对象，当要构造一个scsi命令时，先从内存分配，如果分配失败，则从free_list列表中取出一个scsi_cmnd对象。
	rval = scsi_setup_command_freelist(shost);
	if (rval)
		goto fail_kfree;

	//初始化主机适配器结构中的总线设备驱动模型的域
	device_initialize(&shost->shost_gendev);
	snprintf(shost->shost_gendev.bus_id, BUS_ID_SIZE, "host%d",
		shost->host_no);
	shost->shost_gendev.release = scsi_host_dev_release;

	class_device_initialize(&shost->shost_classdev);
	shost->shost_classdev.dev = &shost->shost_gendev;
	shost->shost_classdev.class = &shost_class;
	snprintf(shost->shost_classdev.class_id, BUS_ID_SIZE, "host%d",
		  shost->host_no);
	//创建命令出错处理线程，当设备处理scsi命令出错(从感测数据可获知)后，就把这个出错的命令提交给这个线程处理
	shost->ehandler = kthread_run(scsi_error_handler, shost,
			"scsi_eh_%d", shost->host_no);
	if (IS_ERR(shost->ehandler)) {
		rval = PTR_ERR(shost->ehandler);
		goto fail_destroy_freelist;
	}
	//在/proc/scsi下创建一个目录项
	scsi_proc_hostdir_add(shost->hostt);
	return shost;

 fail_destroy_freelist:
	scsi_destroy_command_freelist(shost);
 fail_kfree:
	kfree(shost);
	return NULL;
}
EXPORT_SYMBOL(scsi_host_alloc);

struct Scsi_Host *scsi_register(struct scsi_host_template *sht, int privsize)
{
	struct Scsi_Host *shost = scsi_host_alloc(sht, privsize);

	if (!sht->detect) {
		printk(KERN_WARNING "scsi_register() called on new-style "
				    "template for driver %s\n", sht->name);
		dump_stack();
	}

	if (shost)
		list_add_tail(&shost->sht_legacy_list, &sht->legacy_hosts);
	return shost;
}
EXPORT_SYMBOL(scsi_register);

void scsi_unregister(struct Scsi_Host *shost)
{
	list_del(&shost->sht_legacy_list);
	scsi_host_put(shost);
}
EXPORT_SYMBOL(scsi_unregister);

/**
 * scsi_host_lookup - get a reference to a Scsi_Host by host no
 *
 * @hostnum:	host number to locate
 *
 * Return value:
 *	A pointer to located Scsi_Host or NULL.
 **/
struct Scsi_Host *scsi_host_lookup(unsigned short hostnum)
{
	struct class *class = &shost_class;
	struct class_device *cdev;
	struct Scsi_Host *shost = ERR_PTR(-ENXIO), *p;

	down_read(&class->subsys.rwsem);
	list_for_each_entry(cdev, &class->children, node) {
		p = class_to_shost(cdev);
		if (p->host_no == hostnum) {
			shost = scsi_host_get(p);
			break;
		}
	}
	up_read(&class->subsys.rwsem);

	return shost;
}
EXPORT_SYMBOL(scsi_host_lookup);

/**
 * scsi_host_get - inc a Scsi_Host ref count
 * @shost:	Pointer to Scsi_Host to inc.
 **/
struct Scsi_Host *scsi_host_get(struct Scsi_Host *shost)
{
	if ((shost->shost_state == SHOST_DEL) ||
		!get_device(&shost->shost_gendev))
		return NULL;
	return shost;
}
EXPORT_SYMBOL(scsi_host_get);

/**
 * scsi_host_put - dec a Scsi_Host ref count
 * @shost:	Pointer to Scsi_Host to dec.
 **/
void scsi_host_put(struct Scsi_Host *shost)
{
	put_device(&shost->shost_gendev);
}
EXPORT_SYMBOL(scsi_host_put);

int scsi_init_hosts(void)
{
	return class_register(&shost_class);
}

void scsi_exit_hosts(void)
{
	class_unregister(&shost_class);
}

int scsi_is_host_device(const struct device *dev)
{
	return dev->release == scsi_host_dev_release;
}
EXPORT_SYMBOL(scsi_is_host_device);

/**
 * scsi_queue_work - Queue work to the Scsi_Host workqueue.
 * @shost:	Pointer to Scsi_Host.
 * @work:	Work to queue for execution.
 *
 * Return value:
 * 	0 on success / != 0 for error
 **/
int scsi_queue_work(struct Scsi_Host *shost, struct work_struct *work)
{
	if (unlikely(!shost->work_q)) {
		printk(KERN_ERR
			"ERROR: Scsi host '%s' attempted to queue scsi-work, "
			"when no workqueue created.\n", shost->hostt->name);
		dump_stack();

		return -EINVAL;
	}

	return queue_work(shost->work_q, work);
}
EXPORT_SYMBOL_GPL(scsi_queue_work);

/**
 * scsi_flush_work - Flush a Scsi_Host's workqueue.
 * @shost:	Pointer to Scsi_Host.
 **/
void scsi_flush_work(struct Scsi_Host *shost)
{
	if (!shost->work_q) {
		printk(KERN_ERR
			"ERROR: Scsi host '%s' attempted to flush scsi-work, "
			"when no workqueue created.\n", shost->hostt->name);
		dump_stack();
		return;
	}

	flush_workqueue(shost->work_q);
}
EXPORT_SYMBOL_GPL(scsi_flush_work);
