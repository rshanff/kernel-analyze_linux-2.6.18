/*
 * linux/kernel/irq/manage.c
 *
 * Copyright (C) 1992, 1998-2006 Linus Torvalds, Ingo Molnar
 * Copyright (C) 2005-2006 Thomas Gleixner
 *
 * This file contains driver APIs to the irq subsystem.
 */

#include <linux/irq.h>
#include <linux/module.h>
#include <linux/random.h>
#include <linux/interrupt.h>

#include "internals.h"

#ifdef CONFIG_SMP

/**
 *	synchronize_irq - wait for pending IRQ handlers (on other CPUs)
 *	@irq: interrupt number to wait for
 *
 *	This function waits for any pending IRQ handlers for this interrupt
 *	to complete before returning. If you use this function while
 *	holding a resource the IRQ handler may need you will deadlock.
 *
 *	This function may be called - with care - from IRQ context.
 */
void synchronize_irq(unsigned int irq)
{
	struct irq_desc *desc = irq_desc + irq;

	if (irq >= NR_IRQS)
		return;

	while (desc->status & IRQ_INPROGRESS)
		cpu_relax();
}
EXPORT_SYMBOL(synchronize_irq);

#endif

/**
 *	disable_irq_nosync - disable an irq without waiting
 *	@irq: Interrupt to disable
 *
 *	Disable the selected interrupt line.  Disables and Enables are
 *	nested.
 *	Unlike disable_irq(), this function does not ensure existing
 *	instances of the IRQ handler have completed before returning.
 *
 *	This function may be called from IRQ context.
 */
void disable_irq_nosync(unsigned int irq)
{
	struct irq_desc *desc = irq_desc + irq;
	unsigned long flags;

	if (irq >= NR_IRQS)
		return;

	spin_lock_irqsave(&desc->lock, flags);
	if (!desc->depth++) {
		desc->status |= IRQ_DISABLED;
		desc->chip->disable(irq);
	}
	spin_unlock_irqrestore(&desc->lock, flags);
}
EXPORT_SYMBOL(disable_irq_nosync);

/**
 *	disable_irq - disable an irq and wait for completion
 *	@irq: Interrupt to disable
 *
 *	Disable the selected interrupt line.  Enables and Disables are
 *	nested.
 *	This function waits for any pending IRQ handlers for this interrupt
 *	to complete before returning. If you use this function while
 *	holding a resource the IRQ handler may need you will deadlock.
 *
 *	This function may be called - with care - from IRQ context.
 */
void disable_irq(unsigned int irq)
{
	struct irq_desc *desc = irq_desc + irq;

	if (irq >= NR_IRQS)
		return;

	disable_irq_nosync(irq);
	if (desc->action)
		synchronize_irq(irq);
}
EXPORT_SYMBOL(disable_irq);

/**
 *	enable_irq - enable handling of an irq
 *	@irq: Interrupt to enable
 *
 *	Undoes the effect of one call to disable_irq().  If this
 *	matches the last disable, processing of interrupts on this
 *	IRQ line is re-enabled.
 *
 *	This function may be called from IRQ context.
 */
void enable_irq(unsigned int irq)
{
	struct irq_desc *desc = irq_desc + irq;
	unsigned long flags;

	if (irq >= NR_IRQS)
		return;

	spin_lock_irqsave(&desc->lock, flags);
	switch (desc->depth) {
	case 0:
		printk(KERN_WARNING "Unbalanced enable for IRQ %d\n", irq);
		WARN_ON(1);
		break;
	case 1: {
		unsigned int status = desc->status & ~IRQ_DISABLED;

		/* Prevent probing on this irq: */
		desc->status = status | IRQ_NOPROBE;
		check_irq_resend(desc, irq);
		/* fall-through */
	}
	default:
		desc->depth--;
	}
	spin_unlock_irqrestore(&desc->lock, flags);
}
EXPORT_SYMBOL(enable_irq);

/**
 *	set_irq_wake - control irq power management wakeup
 *	@irq:	interrupt to control
 *	@on:	enable/disable power management wakeup
 *
 *	Enable/disable power management wakeup mode, which is
 *	disabled by default.  Enables and disables must match,
 *	just as they match for non-wakeup mode support.
 *
 *	Wakeup mode lets this IRQ wake the system from sleep
 *	states like "suspend to RAM".
 */
int set_irq_wake(unsigned int irq, unsigned int on)
{
	struct irq_desc *desc = irq_desc + irq;
	unsigned long flags;
	int ret = -ENXIO;
	int (*set_wake)(unsigned, unsigned) = desc->chip->set_wake;

	/* wakeup-capable irqs can be shared between drivers that
	 * don't need to have the same sleep mode behaviors.
	 */
	spin_lock_irqsave(&desc->lock, flags);
	if (on) {
		if (desc->wake_depth++ == 0)
			desc->status |= IRQ_WAKEUP;
		else
			set_wake = NULL;
	} else {
		if (desc->wake_depth == 0) {
			printk(KERN_WARNING "Unbalanced IRQ %d "
					"wake disable\n", irq);
			WARN_ON(1);
		} else if (--desc->wake_depth == 0)
			desc->status &= ~IRQ_WAKEUP;
		else
			set_wake = NULL;
	}
	if (set_wake)
		ret = desc->chip->set_wake(irq, on);
	spin_unlock_irqrestore(&desc->lock, flags);
	return ret;
}
EXPORT_SYMBOL(set_irq_wake);

/*
 * Internal function that tells the architecture code whether a
 * particular irq has been exclusively allocated or is available
 * for driver use.
 */
int can_request_irq(unsigned int irq, unsigned long irqflags)
{
	struct irqaction *action;

	if (irq >= NR_IRQS || irq_desc[irq].status & IRQ_NOREQUEST)
		return 0;

	action = irq_desc[irq].action;
	if (action)
		if (irqflags & action->flags & IRQF_SHARED)
			action = NULL;

	return !action;
}

void compat_irq_chip_set_default_handler(struct irq_desc *desc)
{
	/*
	 * If the architecture still has not overriden
	 * the flow handler then zap the default. This
	 * should catch incorrect flow-type setting.
	 */
	if (desc->handle_irq == &handle_bad_irq)
		desc->handle_irq = NULL;
}

/*
 * Internal function to register an irqaction - typically used to
 * allocate special interrupts that are part of the architecture.
 */
/**ltl
 * 功能: 将中断例程描述符插入到全局变量irq_desc的对应列表中。
 * 参数: irq	->中断号
 *		new	->中断例程对象
 * 返回值:
 * 说明:
 */
int setup_irq(unsigned int irq, struct irqaction *new)
{
	struct irq_desc *desc = irq_desc + irq; /* irq_desc全局变量在Handle.c里定义 */
	struct irqaction *old, **p;
	unsigned long flags;
	int shared = 0;
	
	/* 中断号合法性检查 */
	if (irq >= NR_IRQS)
		return -EINVAL;
	
	/* 表示此中断号描述符还没有初始化过 */
	if (desc->chip == &no_irq_chip)
		return -ENOSYS;
	/*
	 * Some drivers like serial.c use request_irq() heavily,
	 * so we have to be careful not to interfere with a
	 * running system.
	 */
	if (new->flags & IRQF_SAMPLE_RANDOM) {
		/*
		 * This function might sleep, we want to call it first,
		 * outside of the atomic block.
		 * Yes, this might clear the entropy pool if the wrong
		 * driver is attempted to be loaded, without actually
		 * installing a new handler, but is this really a problem,
		 * only the sysadmin is able to do this.
		 */
		rand_initialize_irq(irq);
	}

	/*
	 * The following block of code has to be executed atomically
	 */
	spin_lock_irqsave(&desc->lock, flags);
	p = &desc->action; /* 中断服务例程头指针 */
	old = *p;
	if (old) {
		/*
		 * Can't share interrupts unless both agree to and are
		 * the same type (level, edge, polarity). So both flag
		 * fields must have IRQF_SHARED set and the bits which
		 * set the trigger type must match.
		 */
		if (!((old->flags & new->flags) & IRQF_SHARED) || /* 非共享中断号 */
		    ((old->flags ^ new->flags) & IRQF_TRIGGER_MASK)) /* 中断触发方式一致 */
			goto mismatch;

#if defined(CONFIG_IRQ_PER_CPU)
		/* All handlers must agree on per-cpuness */
		if ((old->flags & IRQF_PERCPU) !=
		    (new->flags & IRQF_PERCPU))
			goto mismatch;
#endif

		/* add new interrupt at end of irq queue */
		/* 遍历到最后一个中断例程描述符 */
		do {
			p = &old->next;
			old = *p;
		} while (old);
		shared = 1; /* 此信号为共享信号 */
	}
	/* 把中断服务例程描述符描插入到链表尾部 */
	*p = new;
#if defined(CONFIG_IRQ_PER_CPU)
	if (new->flags & IRQF_PERCPU)
		desc->status |= IRQ_PER_CPU;
#endif
	if (!shared) { /* 共享链表中为NULL或者非共享的中断 */
		irq_chip_set_defaults(desc->chip);

		/* Setup the type (level, edge polarity) if configured: */
		if (new->flags & IRQF_TRIGGER_MASK) {
			if (desc->chip && desc->chip->set_type)
				desc->chip->set_type(irq,
						new->flags & IRQF_TRIGGER_MASK);
			else
				/*
				 * IRQF_TRIGGER_* but the PIC does not support
				 * multiple flow-types?
				 */
				printk(KERN_WARNING "No IRQF_TRIGGER set_type "
				       "function for IRQ %d (%s)\n", irq,
				       desc->chip ? desc->chip->name :
				       "unknown");
		} else
			compat_irq_chip_set_default_handler(desc);

		desc->status &= ~(IRQ_AUTODETECT | IRQ_WAITING |
				  IRQ_INPROGRESS);

		if (!(desc->status & IRQ_NOAUTOEN)) {
			desc->depth = 0;
			desc->status &= ~IRQ_DISABLED;
			/* 对8259A控制器，调用startup_8259A_irq()接口
 			 * 对IOAPIC控制器，调用startup_edge_ioapic_vector/startup_level_ioapic_vector接口
 			 * 对MSI/MSIX中断方式方式而言，调用startup_msi_irq_w_maskbit/
			 */
			if (desc->chip->startup)
				desc->chip->startup(irq);
			else
				desc->chip->enable(irq);
		} else
			/* Undo nested disables: */
			desc->depth = 1;
	}
	spin_unlock_irqrestore(&desc->lock, flags);

	new->irq = irq;	/* 设置中断号 */
	register_irq_proc(irq);	/* 创建proc文件 */
	new->dir = NULL;
	register_handler_proc(irq, new);

	return 0;

mismatch:
	spin_unlock_irqrestore(&desc->lock, flags);
	if (!(new->flags & IRQF_PROBE_SHARED)) {
		printk(KERN_ERR "IRQ handler type mismatch for IRQ %d\n", irq);
		dump_stack();
	}
	return -EBUSY;
}

/**
 *	free_irq - free an interrupt
 *	@irq: Interrupt line to free
 *	@dev_id: Device identity to free
 *
 *	Remove an interrupt handler. The handler is removed and if the
 *	interrupt line is no longer in use by any driver it is disabled.
 *	On a shared IRQ the caller must ensure the interrupt is disabled
 *	on the card it drives before calling this function. The function
 *	does not return until any executing interrupts for this IRQ
 *	have completed.
 *
 *	This function must not be called from interrupt context.
 */
/**ltl
 * 功能: 释放中断
 * 参数:
 * 返回值:
 * 说明:
 */
void free_irq(unsigned int irq, void *dev_id)
{
	struct irq_desc *desc;
	struct irqaction **p;
	unsigned long flags;

	WARN_ON(in_interrupt());
	if (irq >= NR_IRQS)
		return;

	desc = irq_desc + irq;
	spin_lock_irqsave(&desc->lock, flags);
	p = &desc->action;
	for (;;) {
		struct irqaction *action = *p;

		if (action) {
			struct irqaction **pp = p;

			p = &action->next;
			if (action->dev_id != dev_id)
				continue;

			/* Found it - now remove it from the list of entries */
			*pp = action->next;

			/* Currently used only by UML, might disappear one day.*/
#ifdef CONFIG_IRQ_RELEASE_METHOD
			if (desc->chip->release)
				desc->chip->release(irq, dev_id);
#endif

			if (!desc->action) {
				desc->status |= IRQ_DISABLED;
				if (desc->chip->shutdown)
					desc->chip->shutdown(irq);
				else
					desc->chip->disable(irq);
			}
			spin_unlock_irqrestore(&desc->lock, flags);
			unregister_handler_proc(irq, action);

			/* Make sure it's not being used on another CPU */
			synchronize_irq(irq);
			kfree(action);
			return;
		}
		printk(KERN_ERR "Trying to free already-free IRQ %d\n", irq);
		spin_unlock_irqrestore(&desc->lock, flags);
		return;
	}
}
EXPORT_SYMBOL(free_irq);

/**
 *	request_irq - allocate an interrupt line
 *	@irq: Interrupt line to allocate
 *	@handler: Function to be called when the IRQ occurs
 *	@irqflags: Interrupt type flags
 *	@devname: An ascii name for the claiming device
 *	@dev_id: A cookie passed back to the handler function
 *
 *	This call allocates interrupt resources and enables the
 *	interrupt line and IRQ handling. From the point this
 *	call is made your handler function may be invoked. Since
 *	your handler function must clear any interrupt the board
 *	raises, you must take care both to initialise your hardware
 *	and to set up the interrupt handler in the right order.
 *
 *	Dev_id must be globally unique. Normally the address of the
 *	device data structure is used as the cookie. Since the handler
 *	receives this value it makes sense to use it.
 *
 *	If your interrupt is shared you must pass a non NULL dev_id
 *	as this is required when freeing the interrupt.
 *
 *	Flags:
 *
 *	IRQF_SHARED		Interrupt is shared
 *	IRQF_DISABLED	Disable local interrupts while processing
 *	IRQF_SAMPLE_RANDOM	The interrupt can be used for entropy
 *
 */
/**ltl
功能:注册中断服务例程
参数:irq	->中断号(0~224)，要排除0x80
	handler	->中断服务例程
	irqflags	->中断标志
	devname	->中断名字
	dev_id	->中断设备(用区分共享同一中断号irq的对象)
返回值:
*/
int request_irq(unsigned int irq,
		irqreturn_t (*handler)(int, void *, struct pt_regs *),
		unsigned long irqflags, const char *devname, void *dev_id)
{
	struct irqaction *action;
	int retval;

#ifdef CONFIG_LOCKDEP
	/*
	 * Lockdep wants atomic interrupt handlers:
	 */
	irqflags |= SA_INTERRUPT;
#endif
	/*
	 * Sanity-check: shared interrupts must pass in a real dev-ID,
	 * otherwise we'll have trouble later trying to figure out
	 * which interrupt is which (messes up the interrupt freeing
	 * logic etc).
	 */
	if ((irqflags & IRQF_SHARED) && !dev_id)//标志了共享，而 dev_id却是空的
		return -EINVAL;
	if (irq >= NR_IRQS)//中断号超出了224
		return -EINVAL;
	if (irq_desc[irq].status & IRQ_NOREQUEST)
		return -EINVAL;
	if (!handler)
		return -EINVAL;
	//申请一个中断服务例程描述符
	action = kmalloc(sizeof(struct irqaction), GFP_ATOMIC);
	if (!action)
		return -ENOMEM;

	action->handler = handler; /* 中断例程 */
	action->flags = irqflags;  /* 中断标志 */
	cpus_clear(action->mask);
	action->name = devname;
	action->next = NULL;
	action->dev_id = dev_id;   /* 此中断对应的设备号 */

	select_smp_affinity(irq);
	/* 将中断服务例程描述符插入到全局变量irq_desc */
	retval = setup_irq(irq, action);
	if (retval)
		kfree(action);

	return retval;
}
EXPORT_SYMBOL(request_irq);

