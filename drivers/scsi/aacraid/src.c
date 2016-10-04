/*
 *	Adaptec AAC series RAID controller driver
 *
 * based on the old aacraid driver that is..
 * Adaptec aacraid device driver for Linux.
 *
 * Copyright (c) 2000-2010 Adaptec, Inc. (aacraid@adaptec.com)
 * Copyright (c) 2010-2015 PMC-Sierra, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * Module Name:
 *  src.c
 *
 * Abstract: Hardware Device Interface for PMC SRC based controllers
 *
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/types.h>
//#include <linux/sched.h>
#include <linux/pci.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/version.h>	/* Needed for the following */
#if (LINUX_VERSION_CODE > KERNEL_VERSION(2,4,2))
#include <linux/completion.h>
#endif
#include <linux/time.h>
#include <linux/interrupt.h>
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,4,23))
#if (!defined(IRQ_NONE))
  typedef void irqreturn_t;
# define IRQ_HANDLED
# define IRQ_NONE
#endif
#endif
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,4,25))
#include <asm/semaphore.h>
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0))
#include "scsi.h"
#include "hosts.h"
#else
#include <scsi/scsi_host.h>
#endif

#include "aacraid.h"
#if (!defined(CONFIG_COMMUNITY_KERNEL))
#include "fwdebug.h"
#endif

#if (defined(AAC_DEBUG_INSTRUMENT_IOCTL_SENDFIB))
#if (AAC_DEBUG_INSTRUMENT_IOCTL_SENDFIB & 0x70)
static char * aac_debug_timestamp(void)
{
	unsigned long seconds = get_seconds();
	static char buffer[80];
	sprintf(buffer, "%02u:%02u:%02u: ",
	  (int)((seconds / 3600) % 24),
	  (int)((seconds / 60) % 60),
	  (int)(seconds % 60));
	return buffer;
}
# define AAC_DEBUG_PREAMBLE	"%s"
# define AAC_DEBUG_POSTAMBLE	,aac_debug_timestamp()
# define AAC_DEBUG_PREAMBLE_SIZE 10
#else
# define AAC_DEBUG_PREAMBLE	
# define AAC_DEBUG_POSTAMBLE
# define AAC_DEBUG_PREAMBLE_SIZE 0
#endif
#endif

static int aac_src_get_sync_status(struct aac_dev *dev);

#if defined(__ESX5__)
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,19))
irqreturn_t aac_src_intr_message_poll(int irq, void *dev_id, struct pt_regs *regs)
#elif (defined(HAS_NEW_IRQ_HANDLER_T))
irqreturn_t aac_src_intr_message_poll(void *dev_id)
#else
irqreturn_t aac_src_intr_message_poll(int irq, void *dev_id)
#endif
{
	struct aac_msix_ctx *ctx;
	struct aac_dev *dev;
	int vector_no;

	ctx = (struct aac_msix_ctx *)dev_id;
	dev = ctx->dev;

	for (vector_no = 0; vector_no < dev->max_msix; vector_no++)
		aac_src_intr_message(vector_no, &(dev->aac_msix[vector_no]));
}
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,19))
irqreturn_t aac_src_intr_message(int irq, void *dev_id, struct pt_regs *regs)
#elif (defined(HAS_NEW_IRQ_HANDLER_T))
irqreturn_t aac_src_intr_message(void *dev_id)
#else
irqreturn_t aac_src_intr_message(int irq, void *dev_id)
#endif
{
	struct aac_msix_ctx *ctx;
	struct aac_dev *dev; 
	unsigned long bellbits, bellbits_shifted;
	int vector_no;
	int isFastResponse, mode;
	u32 index, handle;

	ctx = (struct aac_msix_ctx *)dev_id;
	dev = ctx->dev;
	vector_no = ctx->vector_no;

	if (dev->msi_enabled) {
		mode = AAC_INT_MODE_MSI;
		if (vector_no == 0) {
			bellbits = src_readl(dev, MUnit.ODR_MSI);
			if (bellbits & 0x40000)
				mode |= AAC_INT_MODE_AIF;
			if (bellbits & 0x1000)
				mode |= AAC_INT_MODE_SYNC;
		}
	} else {
		mode = AAC_INT_MODE_INTX;
		bellbits = src_readl(dev, MUnit.ODR_R);
		if (bellbits & PmDoorBellResponseSent) {
			bellbits = PmDoorBellResponseSent;
			src_writel(dev, MUnit.ODR_C, bellbits);
			src_readl(dev, MUnit.ODR_C);
		} else {
			bellbits_shifted = (bellbits >> SRC_ODR_SHIFT);
			src_writel(dev, MUnit.ODR_C, bellbits);
			src_readl(dev, MUnit.ODR_C);

			if (bellbits_shifted & DoorBellAifPending)
				mode |= AAC_INT_MODE_AIF;
			else if (bellbits_shifted & OUTBOUNDDOORBELL_0)
				 mode |= AAC_INT_MODE_SYNC;
		}
	}

	if (mode & AAC_INT_MODE_SYNC) {
		unsigned long sflags;
		struct list_head *entry;
		int send_it = 0;
		extern int aac_sync_mode;

		if (!aac_sync_mode && !dev->msi_enabled) {
			src_writel(dev, MUnit.ODR_C, bellbits);
			src_readl(dev, MUnit.ODR_C);
		}

		if (dev->sync_fib) {	
			if (dev->sync_fib->callback)
				dev->sync_fib->callback(dev->sync_fib->callback_data, 
					dev->sync_fib);
			spin_lock_irqsave(&dev->sync_fib->event_lock, sflags);
			if (dev->sync_fib->flags & FIB_CONTEXT_FLAG_WAIT) {
				dev->management_fib_count--;
				up(&dev->sync_fib->event_wait);
			}
			spin_unlock_irqrestore(&dev->sync_fib->event_lock, sflags);
			spin_lock_irqsave(&dev->sync_lock, sflags);
			if (!list_empty(&dev->sync_fib_list)) { 
				entry = dev->sync_fib_list.next;
				dev->sync_fib = list_entry(entry, struct fib, fiblink);
				list_del(entry); 
				send_it = 1;
			} else {
				dev->sync_fib = NULL;
			}
			spin_unlock_irqrestore(&dev->sync_lock, sflags);
			if (send_it) {
				aac_adapter_sync_cmd(dev, SEND_SYNCHRONOUS_FIB, 
					(u32)dev->sync_fib->hw_fib_pa, 0, 0, 0, 0, 0, 
					NULL, NULL, NULL, NULL, NULL);
			}
		}
		if (!dev->msi_enabled)
			mode = 0;

	}
	if (mode & AAC_INT_MODE_AIF) {
		/* handle AIF */
		if (dev->sa_firmware) {
			u32 events = src_readl(dev, MUnit.SCR0);
			aac_intr_normal(dev, events, 1, 0, NULL);
			writel(events, &dev->IndexRegs->Mailbox[0]);
			src_writel(dev, MUnit.IDR, 1 << 23);
		} else {
			aac_intr_normal(dev, 0, 2, 0, NULL);
		}
		if (dev->msi_enabled)
			aac_src_access_devreg(dev, AAC_CLEAR_AIF_BIT);
		else	
			mode = 0;
	}
	if (mode) {
		index = dev->host_rrq_idx[vector_no];

		for (;;) {
			isFastResponse = 0;
			/* remove toggle bit (31) */
			handle = le32_to_cpu((dev->host_rrq[index]) 
				& 0x7fffffff);
			/* check fast response bits (30, 1) */
			if (handle & 0x40000000) 
				isFastResponse = 1;
			handle &= 0x0000ffff;
			if (handle == 0) 
				break;
			handle >>= 2;
			DBG_SET_STATE((&dev->fibs[handle]), DBG_STATE_PRE_INT);
			atomic_dec(&dev->rrq_outstanding[vector_no]);
			dev->host_rrq[index++] = 0;
			aac_intr_normal(dev, handle, 0, isFastResponse, NULL);
			if (index == (vector_no + 1) * dev->vector_cap)
				index = vector_no * dev->vector_cap;
			dev->host_rrq_idx[vector_no] = index;
			DBG_SET_STATE((&dev->fibs[handle]), DBG_STATE_POST_INT);
		}
		mode = 0;
	}
	return IRQ_HANDLED;
}

/**
 *	aac_src_disable_interrupt	-	Disable interrupts
 *	@dev: Adapter
 */

static void aac_src_disable_interrupt(struct aac_dev *dev)
{
	src_writel(dev, MUnit.OIMR, dev->OIMR = 0xffffffff);
}

/**
 *	aac_src_enable_interrupt_message	-	Enable interrupts
 *	@dev: Adapter
 */

static void aac_src_enable_interrupt_message(struct aac_dev *dev)
{
	aac_src_access_devreg(dev, AAC_ENABLE_INTERRUPT);
}

/**
 *	src_sync_cmd	-	send a command and wait
 *	@dev: Adapter
 *	@command: Command to execute
 *	@p1: first parameter
 *	@ret: adapter status
 *
 *	This routine will send a synchronous command to the adapter and wait 
 *	for its	completion.
 */

static int src_sync_cmd(struct aac_dev *dev, u32 command,
	u32 p1, u32 p2, u32 p3, u32 p4, u32 p5, u32 p6,
	u32 *status, u32 * r1, u32 * r2, u32 * r3, u32 * r4)
{
	unsigned long start;
	unsigned long delay;
	int ok;
#if (defined(AAC_DEBUG_INSTRUMENT_FIB) || defined(AAC_DEBUG_INSTRUMENT_MSIX))
	printk(KERN_INFO "src_sync_cmd(%p,0x%lx,0x%lx,0x%lx,0x%lx,0x%lx,"
	  "0x%lx,0x%lx,%p,%p,%p,%p,%p)\n",
	  dev, command, p1, p2, p3, p4, p5, p6, status, r1, r2, r3, r4);
#endif

	/*
	 *	Write the command into Mailbox 0
	 */
	writel(command, &dev->IndexRegs->Mailbox[0]);
	/*
	 *	Write the parameters into Mailboxes 1 - 6
	 */
	writel(p1, &dev->IndexRegs->Mailbox[1]);
	writel(p2, &dev->IndexRegs->Mailbox[2]);
	writel(p3, &dev->IndexRegs->Mailbox[3]);
	writel(p4, &dev->IndexRegs->Mailbox[4]);
#if (defined(AAC_LM_SENSOR) && !defined(CONFIG_COMMUNITY_KERNEL))
	writel(p5, &dev->IndexRegs->Mailbox[5]);
	writel(p6, &dev->IndexRegs->Mailbox[6]);
#endif
#if (defined(AAC_DEBUG_INSTRUMENT_FIB) || defined(AAC_DEBUG_INSTRUMENT_MSIX))
	printk(KERN_INFO "OutMaiboxes=%p="
	  "{0x%lx,0x%lx,0x%lx,0x%lx,0x%lx,0x%lx,0x%lx}\n",
	  &dev->IndexRegs->Mailbox[0],
	  readl(&dev->IndexRegs->Mailbox[0]),
	  readl(&dev->IndexRegs->Mailbox[1]),
	  readl(&dev->IndexRegs->Mailbox[2]),
	  readl(&dev->IndexRegs->Mailbox[3]),
	  readl(&dev->IndexRegs->Mailbox[4]),
	  readl(&dev->IndexRegs->Mailbox[5]),
	  readl(&dev->IndexRegs->Mailbox[6]));
#endif
	/*
	 *	Clear the synch command doorbell to start on a clean slate.
	 */
	if (!dev->msi_enabled)
		src_writel(dev, MUnit.ODR_C, OUTBOUNDDOORBELL_0 << SRC_ODR_SHIFT);
	/*
	 *	Disable doorbell interrupts
	 */
	src_writel(dev, MUnit.OIMR, dev->OIMR = 0xffffffff);
	/*
	 *	Force the completion of the mask register write before issuing
	 *	the interrupt.
	 */
	src_readl(dev, MUnit.OIMR);
	/*
	 *	Signal that there is a new synch command
	 */
	src_writel(dev, MUnit.IDR, INBOUNDDOORBELL_0 << SRC_IDR_SHIFT);

	if (!dev->sync_mode || command != SEND_SYNCHRONOUS_FIB) {
		ok = 0;
		start = jiffies;

		if(command == IOP_RESET_ALWAYS)
		{
			/*
			 *	Wait up to 10 sec
			 */
			delay = 10*HZ;
		}
		else
		{
			/*
			 *	Wait up to 5 minutes
			 */
			delay = 300*HZ;
		}
		while (time_before(jiffies,start+delay )) {
			udelay(5);	/* Delay 5 microseconds to let Mon960 get info. */
			/*
			 *	Mon960 will set doorbell0 bit when it has completed the command.
			 */
			if (aac_src_get_sync_status(dev) & OUTBOUNDDOORBELL_0) {
				/*
				 *	Clear the doorbell.
				 */
				if (dev->msi_enabled)
					aac_src_access_devreg(dev, AAC_CLEAR_SYNC_BIT);
				else
					src_writel(dev, MUnit.ODR_C, OUTBOUNDDOORBELL_0 << SRC_ODR_SHIFT);
				ok = 1;
				break;
			}
			/*
			 *	Yield the processor in case we are slow 
			 */
			msleep(1);
		}
		if (unlikely(ok != 1)) {
			/*
			 *	Restore interrupt mask even though we timed out
			 */
			aac_adapter_enable_int(dev);
			return -ETIMEDOUT;
		}
		/*
		 *	Pull the synch status from Mailbox 0.
		 */
		if (status)
			*status = readl(&dev->IndexRegs->Mailbox[0]);
		if (r1)
			*r1 = readl(&dev->IndexRegs->Mailbox[1]);
		if (r2)
			*r2 = readl(&dev->IndexRegs->Mailbox[2]);
		if (r3)
			*r3 = readl(&dev->IndexRegs->Mailbox[3]);
		if (r4)
			*r4 = readl(&dev->IndexRegs->Mailbox[4]);

		if (command == GET_COMM_PREFERRED_SETTINGS)
			dev->max_msix = readl(&dev->IndexRegs->Mailbox[5]) & 0xFFFF;
#if (defined(AAC_DEBUG_INSTRUMENT_FIB) || defined(AAC_DEBUG_INSTRUMENT_MSIX))
		printk(KERN_INFO "InMaiboxes={0x%lx,0x%lx,0x%lx,0x%lx,0x%lx}\n",
			   readl(&dev->IndexRegs->Mailbox[0]),
			   readl(&dev->IndexRegs->Mailbox[1]),
			   readl(&dev->IndexRegs->Mailbox[2]),
			   readl(&dev->IndexRegs->Mailbox[3]),
			   readl(&dev->IndexRegs->Mailbox[4]));
#endif

		/*
		 *	Clear the synch command doorbell.
		 */
		if (!dev->msi_enabled)
			src_writel(dev, MUnit.ODR_C, OUTBOUNDDOORBELL_0 << SRC_ODR_SHIFT);
	}

	/*
	 *	Restore interrupt mask
	 */
	aac_adapter_enable_int(dev);
	return 0;

}

/**
 *	aac_src_interrupt_adapter	-	interrupt adapter
 *	@dev: Adapter
 *
 *	Send an interrupt to the i960 and breakpoint it.
 */

static void aac_src_interrupt_adapter(struct aac_dev *dev)
{
	src_sync_cmd(dev, BREAKPOINT_REQUEST, 0, 0, 0, 0, 0, 0, NULL, NULL, NULL, NULL, NULL);
}

/**
 *	aac_src_notify_adapter		-	send an event to the adapter
 *	@dev: Adapter
 *	@event: Event to send
 *
 *	Notify the i960 that something it probably cares about has
 *	happened.
 */

static void aac_src_notify_adapter(struct aac_dev *dev, u32 event)
{
	switch (event) {

	case AdapNormCmdQue:
		src_writel(dev, MUnit.ODR_C, INBOUNDDOORBELL_1 << SRC_ODR_SHIFT);
		break;
	case HostNormRespNotFull:
		src_writel(dev, MUnit.ODR_C, INBOUNDDOORBELL_4 << SRC_ODR_SHIFT);
		break;
	case AdapNormRespQue:
		src_writel(dev, MUnit.ODR_C, INBOUNDDOORBELL_2 << SRC_ODR_SHIFT);
		break;
	case HostNormCmdNotFull:
		src_writel(dev, MUnit.ODR_C, INBOUNDDOORBELL_3 << SRC_ODR_SHIFT);
		break;
	case HostShutdown:
//		src_sync_cmd(dev, HOST_CRASHING, 0, 0, 0, 0, 0, 0,
//		  NULL, NULL, NULL, NULL, NULL);
		break;
	case FastIo:
		src_writel(dev, MUnit.ODR_C, INBOUNDDOORBELL_6 << SRC_ODR_SHIFT);
		break;
	case AdapPrintfDone:
		src_writel(dev, MUnit.ODR_C, INBOUNDDOORBELL_5 << SRC_ODR_SHIFT);
		break;
	default:
		BUG();
		break;
	}
}

/**
 *	aac_src_start_adapter		-	activate adapter
 *	@dev:	Adapter
 *
 *	Start up processing on an i960 based AAC adapter
 */

static void aac_src_start_adapter(struct aac_dev *dev)
{
	union aac_init *init;
	int i;

	/* reset host_rrq_idx first */
	for (i = 0; i < dev->max_msix; i++) {
		dev->host_rrq_idx[i] = i * dev->vector_cap;
		atomic_set(&dev->rrq_outstanding[i], 0);
	}
	atomic_set(&dev->msix_counter, 0);
	dev->fibs_pushed_no = 0;

	init = dev->init;
	if (dev->comm_interface == AAC_COMM_MESSAGE_TYPE3) {
		init->r8.HostElapsedSeconds = cpu_to_le32(get_seconds());
		src_sync_cmd(dev, INIT_STRUCT_BASE_ADDRESS, (u32)(ulong)dev->init_pa,
			(u32)((ulong)(((dev->init_pa) >> 16) >> 16)),
			sizeof(struct _r8) + (AAC_MAX_HRRQ - 1) * sizeof(struct _rrq),
			0, 0, 0, NULL, NULL, NULL, NULL, NULL);
	} else {
		init->r7.HostElapsedSeconds = cpu_to_le32(get_seconds());
		// We can only use a 32 bit address here
		src_sync_cmd(dev, INIT_STRUCT_BASE_ADDRESS, (u32)(ulong)dev->init_pa,
			0, 0, 0, 0, 0, NULL, NULL, NULL, NULL, NULL);
	}

    	adbg_init(dev,KERN_INFO,"INIT_STRUCT_BASE_ADDRESS=0x%lx\n",
		  (unsigned long)dev->init_pa);

#if (defined(__VMKERNEL_MODULE__) || defined(__VMKLNX30__) || defined(__VMKLNX__))
	/*
	 * On some cards, without a wait here after the INIT_STRUCT_BASE_ADDRESS
	 * command, the ContainerCommand in AacHba_ProbeContainers() fails to
	 * report the container.
	 * The wait time was determined by trial-and-error.
	 */
	 mdelay(500);
#endif
}

/**
 *	aac_src_check_health
 *	@dev: device to check if healthy
 *
 *	Will attempt to determine if the specified adapter is alive and
 *	capable of handling requests, returning 0 if alive.
 */
static int aac_src_check_health(struct aac_dev *dev)
{
	u32 status = src_readl(dev, MUnit.OMR);

	/*
	 *	Check to see if the board failed any self tests.
	 */
	if (unlikely(status & SELF_TEST_FAILED))
		return -1;

	/*
	 *	Check to see if the board panic'd.
	 */
	if (unlikely(status & KERNEL_PANIC))
		return (status >> 16) & 0xFF;
	
	/*
	 *	Wait for the adapter to be up and running.
	 */
	if (unlikely(!(status & KERNEL_UP_AND_RUNNING)))
		return -3;
	/*
	 *	Everything is OK
	 */
	return 0;
}

static u32 aac_get_vector(struct aac_dev *dev)
{
        if(num_online_cpus() == dev->msix_vector_tbl[smp_processor_id()])
                return atomic_inc_return(&dev->msix_counter)%dev->max_msix;
        return dev->msix_vector_tbl[smp_processor_id()];
}

/**
 *	aac_src_deliver_message
 *	@fib: fib to issue
 *
 *	Will send a fib, returning 0 if successful.
 */
static int aac_src_deliver_message(struct fib * fib)
{
	struct aac_dev *dev = fib->dev;
	struct aac_queue *q = &dev->queues->queue[AdapNormCmdQueue];
	u32 fibsize;
	u64 address;
	struct aac_fib_xporthdr *pFibX;
	int native_hba;
	u_int16_t vector_no;

#if !defined(writeq)
	unsigned long flags;
#endif

	atomic_inc(&q->numpending); 

	native_hba = (fib->flags & FIB_CONTEXT_FLAG_NATIVE_HBA) ? 1 : 0;

	if (!native_hba) {
#if (defined(AAC_DEBUG_INSTRUMENT_IOCTL_SENDFIB))
#if (AAC_DEBUG_INSTRUMENT_IOCTL_SENDFIB & 0x70)
	if (nblank(fwprintf(x))
#if ((AAC_DEBUG_INSTRUMENT_IOCTL_SENDFIB & 0x30) == 0x10)
	 && !(fib->hw_fib_va->header.XferState &
	  cpu_to_le32(NoResponseExpected | Async))
#else
	 && dev->aif_thread
#endif
	) {
		unsigned long DebugFlags = dev->FwDebugFlags;
		dev->FwDebugFlags |= FW_DEBUG_FLAGS_NO_HEADERS_B;
#if (AAC_DEBUG_INSTRUMENT_IOCTL_SENDFIB & 0x30)
		fwprintf((dev, HBA_FLAGS_DBG_FW_PRINT_B, AAC_DEBUG_PREAMBLE
		  "send Q=0x%X I=0x%X %u+%u+%u\n" AAC_DEBUG_POSTAMBLE,
		  (int)(fib - dev->fibs), 
	   	  fib->hw_fib_va->header.Handle,
		  le16_to_cpu(fib->hw_fib_va->header.Command),
		  le32_to_cpu(((struct aac_query_mount *)
		    fib->hw_fib_va->data)->command),
		  le32_to_cpu(((struct aac_query_mount *)
		    fib->hw_fib_va->data)->type)));
#endif
#if (AAC_DEBUG_INSTRUMENT_IOCTL_SENDFIB & 0x40)
		{
			u8 * p = (u8 *)fib->hw_fib_va;
			unsigned len = le16_to_cpu(fib->hw_fib_va->header.Size);
			char buffer[80-AAC_DEBUG_PREAMBLE_SIZE];
			char * cp = buffer;

			strcpy(cp, "FIB=");
			cp += 4;
			while (len > 0) {
				if (cp >= &buffer[sizeof(buffer)-4]) {
					fwprintf((dev,
					  HBA_FLAGS_DBG_FW_PRINT_B,
					  AAC_DEBUG_PREAMBLE "%s\n"
					  AAC_DEBUG_POSTAMBLE,
					  buffer));
					strcpy(cp = buffer, "    ");
					cp += 4;
				}
				sprintf (cp, "%02x ", *(p++));
				cp += strlen(cp);
				--len;
			}
			if (cp > &buffer[4]) {
				fwprintf((dev,
				  HBA_FLAGS_DBG_FW_PRINT_B,
				  AAC_DEBUG_PREAMBLE "%s\n"
				  AAC_DEBUG_POSTAMBLE, buffer));
			}
		}
#endif
		dev->FwDebugFlags = DebugFlags;
	}
#endif
#endif
	}

	if (dev->msi_enabled && dev->max_msix > 1 &&
		(native_hba || fib->hw_fib_va->header.Command != AifRequest)) {

		if(dev->comm_interface == AAC_COMM_MESSAGE_TYPE3 && dev->sa_firmware)
                        vector_no = aac_get_vector(dev);
		else
                        vector_no = fib->vector_no;
		if (native_hba) {
			if (fib->flags & FIB_CONTEXT_FLAG_NATIVE_HBA_TMF) {
			 ((struct aac_hba_tm_req *)fib->hw_fib_va)->reply_qid
				= vector_no;
			 ((struct aac_hba_tm_req *)fib->hw_fib_va)->request_id
				+= (vector_no << 16);
			} else {	
			 ((struct aac_hba_cmd_req *)fib->hw_fib_va)->reply_qid
				= vector_no;
			 ((struct aac_hba_cmd_req *)fib->hw_fib_va)->request_id
				+= (vector_no << 16);
			}
		} else {
			fib->hw_fib_va->header.Handle += (vector_no << 16);
		}
	} else {
		vector_no = 0;
	}

	atomic_inc(&dev->rrq_outstanding[vector_no]);
	DBG_OVERFLOW_CHK(dev, vector_no);
	
        if (native_hba) {
		address = fib->hw_fib_pa;
		fibsize = (fib->hbacmd_size + 127) / 128 - 1;
		if (fibsize > 31)
			fibsize = 31;

#if defined(writeq)
		src_writeq(dev, MUnit.IQN_L, (u64)address + (u64)fibsize);
#else
		spin_lock_irqsave(&fib->dev->iq_lock, flags);
		src_writel(dev, MUnit.IQN_H, 
			((u32)(((address) >> 16) >> 16)) & 0xffffffff);
		src_writel(dev, MUnit.IQN_L, 
			(u32)(address & 0xffffffff) + fibsize);
		spin_unlock_irqrestore(&fib->dev->iq_lock, flags);
#endif

	} else {
		if (dev->comm_interface == AAC_COMM_MESSAGE_TYPE2 ||
			dev->comm_interface == AAC_COMM_MESSAGE_TYPE3) {			
			/* Calculate the amount to the fibsize bits */
			fibsize = (le16_to_cpu(fib->hw_fib_va->header.Size)
				+ 127) / 128 - 1;
			/* New FIB header, 32-bit */
			address = fib->hw_fib_pa;
			fib->hw_fib_va->header.StructType = FIB_MAGIC2;
			fib->hw_fib_va->header.SenderFibAddress = 
				cpu_to_le32((u32)address);
			fib->hw_fib_va->header.u.TimeStamp = 0; 
			BUG_ON(((u32)(((address) >> 16) >> 16)) != 0L);
		} else {
			/* Calculate the amount to the fibsize bits */
			fibsize = (sizeof(struct aac_fib_xporthdr) + 
				le16_to_cpu(fib->hw_fib_va->header.Size)
				+ 127) / 128 - 1; 
			/* Fill XPORT header */ 
			pFibX = (struct aac_fib_xporthdr *)
				((unsigned char *)fib->hw_fib_va - 
				sizeof(struct aac_fib_xporthdr));
			pFibX->Handle = fib->hw_fib_va->header.Handle;
			pFibX->HostAddress = 
				cpu_to_le64((u64)fib->hw_fib_pa);
			pFibX->Size = cpu_to_le32(
				le16_to_cpu(fib->hw_fib_va->header.Size));
			address = fib->hw_fib_pa - 
				(u64)sizeof(struct aac_fib_xporthdr);
		}
		if (fibsize > 31) 
			fibsize = 31;

#if defined(writeq)
		src_writeq(dev, MUnit.IQ_L, (u64)address + (u64)fibsize);
#else
		spin_lock_irqsave(&fib->dev->iq_lock, flags);
		src_writel(dev, MUnit.IQ_H, 
			((u32)(((address) >> 16) >> 16)) & 0xffffffff);
		src_writel(dev, MUnit.IQ_L, 
			(u32)(address & 0xffffffff) + fibsize);
		spin_unlock_irqrestore(&fib->dev->iq_lock, flags);
#endif
	}	

	DBG_SET_STATE(fib, DBG_STATE_IO_SUBMITTED);

	return 0;
}

/**
 *	aac_src_ioremap
 *	@size: mapping resize request
 *
 */
static int aac_src_ioremap(struct aac_dev * dev, u32 size)
{
	if (!size) {
		iounmap(dev->regs.src.bar0);
		dev->base = dev->regs.src.bar0 = NULL;
		return 0;
	}
	dev->regs.src.bar1 = 
        ioremap(pci_resource_start(dev->pdev, 2), AAC_MIN_SRC_BAR1_SIZE);
	dev->base = NULL;
	if (dev->regs.src.bar1 == NULL)
		return -1;
	dev->base = dev->regs.src.bar0 = ioremap(dev->scsi_host_ptr->base, size);
	if (dev->base == NULL) {
		iounmap(dev->regs.src.bar1);
		dev->regs.src.bar1 = NULL;
		return -1;
	}
	dev->IndexRegs = &((struct src_registers __iomem *)dev->base)->u.tupelo.IndexRegs;
	return 0;
}

/**
 *  aac_srcv_ioremap
 *	@size: mapping resize request
 *
 */
static int aac_srcv_ioremap(struct aac_dev * dev, u32 size)
{
	if (!size) {
		iounmap(dev->regs.src.bar0);
		dev->base = dev->regs.src.bar0 = NULL;
		return 0;
	}
	dev->regs.src.bar1 = 
        ioremap(pci_resource_start(dev->pdev, 2), AAC_MIN_SRCV_BAR1_SIZE);
	dev->base = NULL;
	if (dev->regs.src.bar1 == NULL)
		return -1;
	dev->base = dev->regs.src.bar0 = ioremap(dev->scsi_host_ptr->base, size);
	if (dev->base == NULL) {
		iounmap(dev->regs.src.bar1);
		dev->regs.src.bar1 = NULL;
		return -1;
	}
	dev->IndexRegs = &((struct src_registers __iomem *)dev->base)->u.denali.IndexRegs;
	return 0;
}

static int aac_src_restart_adapter(struct aac_dev *dev, int bled)
{
	u32 var, reset_mask;

	if (bled >= 0) {
		if (bled)
			printk(KERN_ERR "%s%d: adapter kernel panic'd %x.\n",
				dev->name, dev->id, bled);

		dev->a_ops.adapter_enable_int = aac_src_disable_interrupt;

		bled = aac_adapter_sync_cmd(dev, IOP_RESET_ALWAYS,
			0, 0, 0, 0, 0, 0, &var, &reset_mask, NULL, NULL, NULL);
			if ((bled || var != 0x00000001) && !dev->doorbell_mask)
				bled = -EINVAL;
			else if (dev->doorbell_mask) {
				reset_mask = dev->doorbell_mask;
				bled = 0;
				var = 0x00000001;
			}
		
			if ((dev->pdev->device == PMC_DEVICE_S7 ||
	     		    dev->pdev->device == PMC_DEVICE_S8 ||
	     		    dev->pdev->device == PMC_DEVICE_S9) && dev->msi_enabled) {
				aac_src_access_devreg(dev, AAC_ENABLE_INTX);
				dev->msi_enabled = 0;
				msleep(5000); /* Delay 5 seconds */
			}
		if (!bled && (dev->supplement_adapter_info.SupportedOptions2 &
			AAC_OPTION_DOORBELL_RESET)) {
			src_writel(dev, MUnit.IDR, reset_mask);
			ssleep(45);
		}
		else
		{

			src_writel(dev, MUnit.IDR, 0x100);
			ssleep(45);

		}
	}

	if (src_readl(dev, MUnit.OMR) & KERNEL_PANIC)
#if (defined(AAC_DEBUG_INSTRUMENT_RESET))
	{
		if (var == 10)
			printk(KERN_INFO "IOP_RESET disabled\n");
#endif
		return -ENODEV;
#if (defined(AAC_DEBUG_INSTRUMENT_RESET))
	}
#endif
	if (startup_timeout < 300)
		startup_timeout = 300;
	return 0;
}

/**
 *	aac_src_select_comm	-	Select communications method
 *	@dev: Adapter
 *	@comm: communications method
 */
int aac_src_select_comm(struct aac_dev *dev, int comm)
{
	switch (comm) {
	case AAC_COMM_MESSAGE:
		dev->a_ops.adapter_intr = aac_src_intr_message;
#if defined(__ESX5__)
		dev->a_ops.adapter_intr_poll = aac_src_intr_message_poll;
#endif
		dev->a_ops.adapter_deliver = aac_src_deliver_message;
		break;
	default:
		return 1;
	}
	return 0;
}

/**
 *  aac_src_init	-	initialize an Cardinal Frey Bar card
 *  @dev: device to configure
 *
 */

int aac_src_init(struct aac_dev * dev)
{
	unsigned long start;
	unsigned long status;
	int restart = 0;
	int instance = dev->id;
	const char * name = dev->name;

#if ((LINUX_VERSION_CODE < KERNEL_VERSION(2,6,19)) && !defined(HAS_RESET_DEVICES))
#	define reset_devices aac_reset_devices
#endif

	dev->a_ops.adapter_ioremap = aac_src_ioremap;
	dev->a_ops.adapter_comm = aac_src_select_comm;

	dev->base_size = AAC_MIN_SRC_BAR0_SIZE;
	if (aac_adapter_ioremap(dev, dev->base_size)) {
		printk(KERN_WARNING "%s: unable to map adapter.\n", name);
		goto error_iounmap;
	}

	/* Failure to reset here is an option ... */
	dev->a_ops.adapter_sync_cmd = src_sync_cmd;
	dev->a_ops.adapter_enable_int = aac_src_disable_interrupt;
	if ((aac_reset_devices || reset_devices) &&
		!aac_src_restart_adapter(dev, 0))
		++restart;
	/*
	 *	Check to see if the board panic'd while booting.
	 */
	status = src_readl(dev, MUnit.OMR);
	if (status & KERNEL_PANIC) {
		if (aac_src_restart_adapter(dev, aac_src_check_health(dev)))
			goto error_iounmap;
		++restart;
	}
	/*
	 *	Check to see if the board failed any self tests.
	 */
	status = src_readl(dev, MUnit.OMR);
	if (status & SELF_TEST_FAILED) {
		printk(KERN_ERR "%s%d: adapter self-test failed.\n", dev->name, instance);
		goto error_iounmap;
	}
	/*
	 *	Check to see if the monitor panic'd while booting.
	 */
	if (status & MONITOR_PANIC) {
		printk(KERN_ERR "%s%d: adapter monitor panic.\n", dev->name, instance);
		goto error_iounmap;
	}
	start = jiffies;
	/*
	 *	Wait for the adapter to be up and running. Wait up to 3 minutes
	 */
	while (!((status = src_readl(dev, MUnit.OMR)) & KERNEL_UP_AND_RUNNING))
	{
		if ((restart &&
		  (status & (KERNEL_PANIC|SELF_TEST_FAILED|MONITOR_PANIC))) ||
		  time_after(jiffies, start+HZ*startup_timeout)) {
			printk(KERN_ERR "%s%d: adapter kernel failed to start, init status = %lx.\n", 
					dev->name, instance, status);
			goto error_iounmap;
		}
		if (!restart &&
		  ((status & (KERNEL_PANIC|SELF_TEST_FAILED|MONITOR_PANIC)) ||
		  time_after(jiffies, start + HZ *
		  ((startup_timeout > 60)
		    ? (startup_timeout - 60)
		    : (startup_timeout / 2))))) {
			if (likely(!aac_src_restart_adapter(dev, aac_src_check_health(dev))))
				start = jiffies;
			++restart;
		}
		msleep(1);
	}
	if (restart && aac_commit)
		aac_commit = 1;
	/*
	 *	Fill in the common function dispatch table.
	 */
	dev->a_ops.adapter_interrupt = aac_src_interrupt_adapter;
	dev->a_ops.adapter_disable_int = aac_src_disable_interrupt;
	dev->a_ops.adapter_enable_int = aac_src_disable_interrupt;
	dev->a_ops.adapter_notify = aac_src_notify_adapter;
	dev->a_ops.adapter_sync_cmd = src_sync_cmd;
	dev->a_ops.adapter_check_health = aac_src_check_health;
	dev->a_ops.adapter_restart = aac_src_restart_adapter;
	dev->a_ops.adapter_start = aac_src_start_adapter;

	/*
	 *	First clear out all interrupts.  Then enable the one's that we
	 *	can handle.
	 */
	aac_adapter_comm(dev, AAC_COMM_MESSAGE);
	aac_adapter_disable_int(dev);
	src_writel(dev, MUnit.ODR_C, 0xffffffff);
	aac_adapter_enable_int(dev);

	if (aac_init_adapter(dev) == NULL)
		goto error_iounmap;
	if (dev->comm_interface != AAC_COMM_MESSAGE_TYPE1)
		goto error_iounmap;
#if ((LINUX_VERSION_CODE > KERNEL_VERSION(2,6,8)) || defined(PCI_HAS_ENABLE_MSI))
#if ((!defined(CONFIG_XEN) || LINUX_VERSION_CODE > KERNEL_VERSION(2,6,19)) || \
     defined(__VMKERNEL_MODULE__) || defined(__VMKLNX30__) || defined(__VMKLNX__))
	dev->msi = !pci_enable_msi(dev->pdev);
#endif
#endif
	dev->aac_msix[0].vector_no = 0;
	dev->aac_msix[0].dev = dev;

	if (request_irq(dev->pdev->irq, dev->a_ops.adapter_intr,
		IRQF_SHARED|IRQF_DISABLED, "aacraid", &(dev->aac_msix[0])) < 0) {
#if ((LINUX_VERSION_CODE > KERNEL_VERSION(2,6,8)) || defined(PCI_HAS_DISABLE_MSI))
		if (dev->msi)
			pci_disable_msi(dev->pdev);
#endif
		printk(KERN_ERR "%s%d: Interrupt unavailable.\n",
			name, instance);
		goto error_iounmap;
	}
	dev->dbg_base = pci_resource_start(dev->pdev, 2);
	dev->dbg_base_mapped = dev->regs.src.bar1;
	dev->dbg_size = AAC_MIN_SRC_BAR1_SIZE;
	dev->a_ops.adapter_enable_int = aac_src_enable_interrupt_message;


	aac_adapter_enable_int(dev);

	if (!dev->sync_mode) {
		/*
		 *	Tell the adapter that all is configured, and it can
		 * start accepting requests
		 */
		aac_src_start_adapter(dev);
	}
	return 0;

error_iounmap:

	return -1;
}

/**
 *  aac_srcv_init	-	initialize an SRCv card
 *  @dev: device to configure
 *
 */

int aac_srcv_init(struct aac_dev * dev)
{
	unsigned long start;
	unsigned long status;
	int restart = 0;
	int instance = dev->id;
	int waitCount;
	const char * name = dev->name;
#if ((LINUX_VERSION_CODE < KERNEL_VERSION(2,6,19)) && !defined(HAS_RESET_DEVICES))
#	define reset_devices aac_reset_devices
#endif
	dev->a_ops.adapter_ioremap = aac_srcv_ioremap;
	dev->a_ops.adapter_comm = aac_src_select_comm;

	dev->base_size = AAC_MIN_SRCV_BAR0_SIZE;
	if (aac_adapter_ioremap(dev, dev->base_size)) {
		printk(KERN_WARNING "%s: unable to map adapter.\n", name);
		goto error_iounmap;
	}

	/* Failure to reset here is an option ... */
	dev->a_ops.adapter_sync_cmd = src_sync_cmd;
	dev->a_ops.adapter_enable_int = aac_src_disable_interrupt;
	if ((aac_reset_devices || reset_devices) &&
		!aac_src_restart_adapter(dev, 0))
		++restart;
	/*
	 *	Check to see if flash update is running.
	 *	Wait for the adapter to be up and running. Wait up to 5 minutes
	 */
	status = src_readl(dev, MUnit.OMR);
	if (status & FLASH_UPD_PENDING) {
		start = jiffies;
		do {
			status = src_readl(dev, MUnit.OMR);
			if (time_after(jiffies, start+HZ*FWUPD_TIMEOUT)) {
				printk(KERN_ERR "%s%d: adapter flash update failed.\n", dev->name, instance);
				goto error_iounmap;
			}
		} while (!(status & FLASH_UPD_SUCCESS) && !(status & FLASH_UPD_FAILED));
		/* Delay 10 seconds. Because right now FW is doing a soft reset, do not
		 * read scratch pad register at this time
		 */
		waitCount = 10 * 10000;
		while (waitCount) {
			udelay(100);	/* delay 100 microseconds */
			waitCount--;
		}
	}
			
	/*
	 *	Check to see if the board panic'd while booting.
	 */
	status = src_readl(dev, MUnit.OMR);
	if (status & KERNEL_PANIC) {
		if (aac_src_restart_adapter(dev, aac_src_check_health(dev)))
			goto error_iounmap;
		++restart;
	}
	/*
	 *	Check to see if the board failed any self tests.
	 */
	status = src_readl(dev, MUnit.OMR);
	if (status & SELF_TEST_FAILED) {
		printk(KERN_ERR "%s%d: adapter self-test failed.\n", dev->name, instance);
		goto error_iounmap;
	}
	/*
	 *	Check to see if the monitor panic'd while booting.
	 */
	if (status & MONITOR_PANIC) {
		printk(KERN_ERR "%s%d: adapter monitor panic.\n", dev->name, instance);
		goto error_iounmap;
	}
	start = jiffies;
	/*
	 *	Wait for the adapter to be up and running. Wait up to 3 minutes
	 */
	while (!((status = src_readl(dev, MUnit.OMR)) & KERNEL_UP_AND_RUNNING) ||
	       status == 0xffffffff)
	{
		if ((restart &&
		  (status & (KERNEL_PANIC|SELF_TEST_FAILED|MONITOR_PANIC))) ||
		  time_after(jiffies, start+HZ*startup_timeout)) {
			printk(KERN_ERR "%s%d: adapter kernel failed to start, init status = %lx.\n", 
					dev->name, instance, status);
			goto error_iounmap;
		}
		if (!restart &&
		  ((status & (KERNEL_PANIC|SELF_TEST_FAILED|MONITOR_PANIC)) ||
		  time_after(jiffies, start + HZ *
		  ((startup_timeout > 60)
		    ? (startup_timeout - 60)
		    : (startup_timeout / 2))))) {
			if (likely(!aac_src_restart_adapter(dev, aac_src_check_health(dev))))
				start = jiffies;
			++restart;
		}
		msleep(1);
	}
	if (restart && aac_commit)
		aac_commit = 1;
	/*
	 *	Fill in the common function dispatch table.
	 */
	dev->a_ops.adapter_interrupt = aac_src_interrupt_adapter;
	dev->a_ops.adapter_disable_int = aac_src_disable_interrupt;
	dev->a_ops.adapter_enable_int = aac_src_disable_interrupt;
	dev->a_ops.adapter_notify = aac_src_notify_adapter;
	dev->a_ops.adapter_sync_cmd = src_sync_cmd;
	dev->a_ops.adapter_check_health = aac_src_check_health;
	dev->a_ops.adapter_restart = aac_src_restart_adapter;
	dev->a_ops.adapter_start = aac_src_start_adapter;

	/*
	 *	First clear out all interrupts.  Then enable the one's that we
	 *	can handle.
	 */
	aac_adapter_comm(dev, AAC_COMM_MESSAGE);
	aac_adapter_disable_int(dev);
	src_writel(dev, MUnit.ODR_C, 0xffffffff);
	aac_adapter_enable_int(dev);

	if (aac_init_adapter(dev) == NULL)
		goto error_iounmap;
	if (dev->comm_interface != AAC_COMM_MESSAGE_TYPE2 &&
		dev->comm_interface != AAC_COMM_MESSAGE_TYPE3)
		goto error_iounmap;

	if (dev->msi_enabled)
		aac_src_access_devreg(dev, AAC_ENABLE_MSIX);

	if(aac_acquire_irq(dev))
		goto error_iounmap;

	dev->dbg_base = pci_resource_start(dev->pdev, 2);
	dev->dbg_base_mapped = dev->regs.src.bar1;
	dev->dbg_size = AAC_MIN_SRCV_BAR1_SIZE;
	dev->a_ops.adapter_enable_int = aac_src_enable_interrupt_message;

	aac_adapter_enable_int(dev);
	
	if (!dev->sync_mode) {
		/*
	 	 *	Tell the adapter that all is configured, and it can
	 	 * start accepting requests
	 	 */
		aac_src_start_adapter(dev);
	}
	return 0;

error_iounmap:

	return -1;
}


void aac_src_access_devreg(struct aac_dev *dev, int mode) {
	u_int32_t val;

	switch (mode) {
	case AAC_ENABLE_INTERRUPT:
		src_writel(dev, MUnit.OIMR, dev->OIMR = (dev->msi_enabled ? AAC_INT_ENABLE_TYPE1_MSIX :
									    AAC_INT_ENABLE_TYPE1_INTX));
		break;

	case AAC_DISABLE_INTERRUPT:
		src_writel(dev, MUnit.OIMR, dev->OIMR = AAC_INT_DISABLE_ALL);
		break;
		
	case AAC_ENABLE_MSIX:
		/* set bit 6 */
		val = src_readl(dev, MUnit.IDR);
		val |= 0x40;
		src_writel(dev,  MUnit.IDR, val);
		src_readl(dev, MUnit.IDR);
		/* unmask int. */
		val = PMC_ALL_INTERRUPT_BITS;
		src_writel(dev, MUnit.IOAR, val);
		val = src_readl(dev, MUnit.OIMR);
		src_writel(dev, MUnit.OIMR,
				val & (~(PMC_GLOBAL_INT_BIT2 | PMC_GLOBAL_INT_BIT0)));
		break;

	case AAC_DISABLE_MSIX:
		/* reset bit 6 */
		val = src_readl(dev, MUnit.IDR);
		val &= ~0x40;
		src_writel(dev, MUnit.IDR, val);
		src_readl(dev, MUnit.IDR);
		break;

	case AAC_CLEAR_AIF_BIT:
		/* set bit 5 */
		val = src_readl(dev, MUnit.IDR);
		val |= 0x20;
		src_writel(dev, MUnit.IDR, val);
		src_readl(dev, MUnit.IDR);
		break;

	case AAC_CLEAR_SYNC_BIT:
		/* set bit 4 */
		val = src_readl(dev, MUnit.IDR);
		val |= 0x10;
		src_writel(dev, MUnit.IDR, val);
		src_readl(dev, MUnit.IDR);
		break;

	case AAC_ENABLE_INTX:
		/* set bit 7 */
		val = src_readl(dev, MUnit.IDR);
		val |= 0x80;
		src_writel(dev, MUnit.IDR, val);
		src_readl(dev, MUnit.IDR);
		/* unmask int. */
		val = PMC_ALL_INTERRUPT_BITS;
		src_writel(dev, MUnit.IOAR, val);
		src_readl(dev, MUnit.IOAR);
		val = src_readl(dev, MUnit.OIMR);
		src_writel(dev, MUnit.OIMR,
				val & (~(PMC_GLOBAL_INT_BIT2)));
		break;

	default:
		break;
	}
}

static int aac_src_get_sync_status(struct aac_dev *dev) {

	int val;

	if (dev->msi_enabled)
		val = src_readl(dev, MUnit.ODR_MSI) & 0x1000 ? 1 : 0;
	else
		val = src_readl(dev, MUnit.ODR_R) >> SRC_ODR_SHIFT;

	return val;
}
