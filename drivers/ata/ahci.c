/*
 *  ahci.c - AHCI SATA support
 *
 *  Maintained by:  Tejun Heo <tj@kernel.org>
 *    		    Please ALWAYS copy linux-ide@vger.kernel.org
 *		    on emails.
 *
 *  Copyright 2004-2005 Red Hat, Inc.
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *
 * libata documentation is available via 'make {ps|pdf}docs',
 * as Documentation/DocBook/libata.*
 *
 * AHCI hardware documentation:
 * http://www.intel.com/technology/serialata/pdf/rev1_0.pdf
 * http://www.intel.com/technology/serialata/pdf/rev1_1.pdf
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/device.h>
#include <linux/dmi.h>
#include <linux/gfp.h>
#if defined(CONFIG_SYNO_LSP_ALPINE)
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#endif /* CONFIG_SYNO_LSP_ALPINE */
#include <scsi/scsi_host.h>
#include <scsi/scsi_cmnd.h>
#ifdef CONFIG_SYNO_ATA_AHCI_LED_SGPIO
#include <scsi/scsi.h>
#include <scsi/scsi_eh.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_tcq.h>
#include <scsi/scsi_transport.h>
#endif /* CONFIG_SYNO_ATA_AHCI_LED_SGPIO */
#include <linux/libata.h>
#include "ahci.h"
#if defined(CONFIG_SYNO_LSP_ALPINE) && defined(CONFIG_ARCH_ALPINE)
#include "al_hal_iofic.h"
#include "al_hal_iofic_regs.h"
#endif /* CONFIG_SYNO_LSP_ALPINE && CONFIG_ARCH_ALPINE */

#define DRV_NAME	"ahci"
#define DRV_VERSION	"3.0"

#ifdef CONFIG_SYNO_AHCI_SWITCH
extern char g_ahci_switch;
#endif /* CONFIG_SYNO_AHCI_SWITCH */

enum {
	AHCI_PCI_BAR_STA2X11	= 0,
	AHCI_PCI_BAR_ENMOTUS	= 2,
	AHCI_PCI_BAR_STANDARD	= 5,
};

enum board_ids {
	/* board IDs by feature in alphabetical order */
	board_ahci,
	board_ahci_ign_iferr,
	board_ahci_noncq,
	board_ahci_nosntf,
	board_ahci_yes_fbs,
#ifdef CONFIG_SYNO_SATA_AHCI_FBS_NONCQ
	board_ahci_yes_fbs_no_ncq,
#endif /* CONFIG_SYNO_SATA_AHCI_FBS_NONCQ */

	/* board IDs for specific chipsets in alphabetical order */
	board_ahci_mcp65,
	board_ahci_mcp77,
	board_ahci_mcp89,
	board_ahci_mv,
	board_ahci_sb600,
	board_ahci_sb700,	/* for SB700 and SB800 */
	board_ahci_vt8251,
#if defined(CONFIG_SYNO_LSP_ALPINE)
	board_ahci_alpine,
#endif /* CONFIG_SYNO_LSP_ALPINE */

	/* aliases */
	board_ahci_mcp_linux	= board_ahci_mcp65,
	board_ahci_mcp67	= board_ahci_mcp65,
	board_ahci_mcp73	= board_ahci_mcp65,
	board_ahci_mcp79	= board_ahci_mcp77,
};

static int ahci_init_one(struct pci_dev *pdev, const struct pci_device_id *ent);
static int ahci_vt8251_hardreset(struct ata_link *link, unsigned int *class,
				 unsigned long deadline);
static int ahci_p5wdh_hardreset(struct ata_link *link, unsigned int *class,
				unsigned long deadline);
#ifdef CONFIG_PM
static int ahci_pci_device_suspend(struct pci_dev *pdev, pm_message_t mesg);
static int ahci_pci_device_resume(struct pci_dev *pdev);
#endif

static struct scsi_host_template ahci_sht = {
	AHCI_SHT("ahci"),
};

#ifdef CONFIG_SYNO_SATA_PM_DEVICE_GPIO
static struct ata_port_operations ahci_pmp_ops = {
	.inherits		= &ahci_ops,

	.qc_defer		= sata_syno_ahci_defer_cmd,
};
#endif /* CONFIG_SYNO_SATA_PM_DEVICE_GPIO */

static struct ata_port_operations ahci_vt8251_ops = {
	.inherits		= &ahci_ops,
	.hardreset		= ahci_vt8251_hardreset,
};

static struct ata_port_operations ahci_p5wdh_ops = {
	.inherits		= &ahci_ops,
	.hardreset		= ahci_p5wdh_hardreset,
};

#if defined(CONFIG_SYNO_LSP_ALPINE)
ssize_t al_ahci_transmit_led_message(struct ata_port *ap, u32 state,
					    ssize_t size);

static struct ata_port_operations ahci_al_ops = {
	.inherits		= &ahci_ops,
	.transmit_led_message   = al_ahci_transmit_led_message,
};
#endif /* CONFIG_SYNO_LSP_ALPINE */

static const struct ata_port_info ahci_port_info[] = {
	/* by features */
	[board_ahci] = {
		.flags		= AHCI_FLAG_COMMON,
		.pio_mask	= ATA_PIO4,
		.udma_mask	= ATA_UDMA6,
		.port_ops	= &ahci_ops,
	},
	[board_ahci_ign_iferr] = {
		AHCI_HFLAGS	(AHCI_HFLAG_IGN_IRQ_IF_ERR),
		.flags		= AHCI_FLAG_COMMON,
		.pio_mask	= ATA_PIO4,
		.udma_mask	= ATA_UDMA6,
		.port_ops	= &ahci_ops,
	},
	[board_ahci_noncq] = {
		AHCI_HFLAGS	(AHCI_HFLAG_NO_NCQ),
		.flags		= AHCI_FLAG_COMMON,
		.pio_mask	= ATA_PIO4,
		.udma_mask	= ATA_UDMA6,
		.port_ops	= &ahci_ops,
	},
	[board_ahci_nosntf] = {
		AHCI_HFLAGS	(AHCI_HFLAG_NO_SNTF),
		.flags		= AHCI_FLAG_COMMON,
		.pio_mask	= ATA_PIO4,
		.udma_mask	= ATA_UDMA6,
#ifdef CONFIG_SYNO_SATA_PM_DEVICE_GPIO
		.port_ops	= &ahci_pmp_ops,
#else
		.port_ops	= &ahci_ops,
#endif /* CONFIG_SYNO_SATA_PM_DEVICE_GPIO */
	},
	[board_ahci_yes_fbs] = {
		AHCI_HFLAGS	(AHCI_HFLAG_YES_FBS),
		.flags		= AHCI_FLAG_COMMON,
		.pio_mask	= ATA_PIO4,
		.udma_mask	= ATA_UDMA6,
		.port_ops	= &ahci_ops,
	},
#ifdef CONFIG_SYNO_SATA_AHCI_FBS_NONCQ
	[board_ahci_yes_fbs_no_ncq] =
	{
		AHCI_HFLAGS	(AHCI_HFLAG_YES_FBS | AHCI_HFLAG_NO_NCQ),
		.flags		= AHCI_FLAG_COMMON,
		.pio_mask	= ATA_PIO4,
		.udma_mask	= ATA_UDMA6,
		.port_ops	= &ahci_ops,
	},
#endif /* CONFIG_SYNO_SATA_AHCI_FBS_NONCQ */
	/* by chipsets */
	[board_ahci_mcp65] = {
		AHCI_HFLAGS	(AHCI_HFLAG_NO_FPDMA_AA | AHCI_HFLAG_NO_PMP |
				 AHCI_HFLAG_YES_NCQ),
		.flags		= AHCI_FLAG_COMMON | ATA_FLAG_NO_DIPM,
		.pio_mask	= ATA_PIO4,
		.udma_mask	= ATA_UDMA6,
		.port_ops	= &ahci_ops,
	},
	[board_ahci_mcp77] = {
		AHCI_HFLAGS	(AHCI_HFLAG_NO_FPDMA_AA | AHCI_HFLAG_NO_PMP),
		.flags		= AHCI_FLAG_COMMON,
		.pio_mask	= ATA_PIO4,
		.udma_mask	= ATA_UDMA6,
		.port_ops	= &ahci_ops,
	},
	[board_ahci_mcp89] = {
		AHCI_HFLAGS	(AHCI_HFLAG_NO_FPDMA_AA),
		.flags		= AHCI_FLAG_COMMON,
		.pio_mask	= ATA_PIO4,
		.udma_mask	= ATA_UDMA6,
		.port_ops	= &ahci_ops,
	},
	[board_ahci_mv] = {
		AHCI_HFLAGS	(AHCI_HFLAG_NO_NCQ | AHCI_HFLAG_NO_MSI |
				 AHCI_HFLAG_MV_PATA | AHCI_HFLAG_NO_PMP),
		.flags		= ATA_FLAG_SATA | ATA_FLAG_PIO_DMA,
		.pio_mask	= ATA_PIO4,
		.udma_mask	= ATA_UDMA6,
		.port_ops	= &ahci_ops,
	},
	[board_ahci_sb600] = {
		AHCI_HFLAGS	(AHCI_HFLAG_IGN_SERR_INTERNAL |
				 AHCI_HFLAG_NO_MSI | AHCI_HFLAG_SECT255 |
				 AHCI_HFLAG_32BIT_ONLY),
		.flags		= AHCI_FLAG_COMMON,
		.pio_mask	= ATA_PIO4,
		.udma_mask	= ATA_UDMA6,
		.port_ops	= &ahci_pmp_retry_srst_ops,
	},
	[board_ahci_sb700] = {	/* for SB700 and SB800 */
		AHCI_HFLAGS	(AHCI_HFLAG_IGN_SERR_INTERNAL),
		.flags		= AHCI_FLAG_COMMON,
		.pio_mask	= ATA_PIO4,
		.udma_mask	= ATA_UDMA6,
		.port_ops	= &ahci_pmp_retry_srst_ops,
	},
	[board_ahci_vt8251] = {
		AHCI_HFLAGS	(AHCI_HFLAG_NO_NCQ | AHCI_HFLAG_NO_PMP),
		.flags		= AHCI_FLAG_COMMON,
		.pio_mask	= ATA_PIO4,
		.udma_mask	= ATA_UDMA6,
		.port_ops	= &ahci_vt8251_ops,
	},
#if defined(CONFIG_SYNO_LSP_ALPINE)
	[board_ahci_alpine] = {
		AHCI_HFLAGS	(AHCI_HFLAG_NO_PMP | AHCI_HFLAG_MSIX),
		.flags		= AHCI_FLAG_COMMON,
		.pio_mask	= ATA_PIO4,
		.udma_mask	= ATA_UDMA6,
		.port_ops	= &ahci_al_ops,
	},
#endif /* CONFIG_SYNO_LSP_ALPINE */
};

static const struct pci_device_id ahci_pci_tbl[] = {
	/* Intel */
	{ PCI_VDEVICE(INTEL, 0x2652), board_ahci }, /* ICH6 */
	{ PCI_VDEVICE(INTEL, 0x2653), board_ahci }, /* ICH6M */
	{ PCI_VDEVICE(INTEL, 0x27c1), board_ahci }, /* ICH7 */
	{ PCI_VDEVICE(INTEL, 0x27c5), board_ahci }, /* ICH7M */
	{ PCI_VDEVICE(INTEL, 0x27c3), board_ahci }, /* ICH7R */
	{ PCI_VDEVICE(AL, 0x5288), board_ahci_ign_iferr }, /* ULi M5288 */
	{ PCI_VDEVICE(INTEL, 0x2681), board_ahci }, /* ESB2 */
	{ PCI_VDEVICE(INTEL, 0x2682), board_ahci }, /* ESB2 */
	{ PCI_VDEVICE(INTEL, 0x2683), board_ahci }, /* ESB2 */
	{ PCI_VDEVICE(INTEL, 0x27c6), board_ahci }, /* ICH7-M DH */
	{ PCI_VDEVICE(INTEL, 0x2821), board_ahci }, /* ICH8 */
	{ PCI_VDEVICE(INTEL, 0x2822), board_ahci_nosntf }, /* ICH8 */
	{ PCI_VDEVICE(INTEL, 0x2824), board_ahci }, /* ICH8 */
	{ PCI_VDEVICE(INTEL, 0x2829), board_ahci }, /* ICH8M */
	{ PCI_VDEVICE(INTEL, 0x282a), board_ahci }, /* ICH8M */
	{ PCI_VDEVICE(INTEL, 0x2922), board_ahci }, /* ICH9 */
	{ PCI_VDEVICE(INTEL, 0x2923), board_ahci }, /* ICH9 */
	{ PCI_VDEVICE(INTEL, 0x2924), board_ahci }, /* ICH9 */
	{ PCI_VDEVICE(INTEL, 0x2925), board_ahci }, /* ICH9 */
	{ PCI_VDEVICE(INTEL, 0x2927), board_ahci }, /* ICH9 */
	{ PCI_VDEVICE(INTEL, 0x2929), board_ahci }, /* ICH9M */
	{ PCI_VDEVICE(INTEL, 0x292a), board_ahci }, /* ICH9M */
	{ PCI_VDEVICE(INTEL, 0x292b), board_ahci }, /* ICH9M */
	{ PCI_VDEVICE(INTEL, 0x292c), board_ahci }, /* ICH9M */
	{ PCI_VDEVICE(INTEL, 0x292f), board_ahci }, /* ICH9M */
	{ PCI_VDEVICE(INTEL, 0x294d), board_ahci }, /* ICH9 */
	{ PCI_VDEVICE(INTEL, 0x294e), board_ahci }, /* ICH9M */
	{ PCI_VDEVICE(INTEL, 0x502a), board_ahci }, /* Tolapai */
	{ PCI_VDEVICE(INTEL, 0x502b), board_ahci }, /* Tolapai */
	{ PCI_VDEVICE(INTEL, 0x3a05), board_ahci }, /* ICH10 */
	{ PCI_VDEVICE(INTEL, 0x3a22), board_ahci }, /* ICH10 */
	{ PCI_VDEVICE(INTEL, 0x3a25), board_ahci }, /* ICH10 */
	{ PCI_VDEVICE(INTEL, 0x3b22), board_ahci }, /* PCH AHCI */
	{ PCI_VDEVICE(INTEL, 0x3b23), board_ahci }, /* PCH AHCI */
	{ PCI_VDEVICE(INTEL, 0x3b24), board_ahci }, /* PCH RAID */
	{ PCI_VDEVICE(INTEL, 0x3b25), board_ahci }, /* PCH RAID */
	{ PCI_VDEVICE(INTEL, 0x3b29), board_ahci }, /* PCH AHCI */
	{ PCI_VDEVICE(INTEL, 0x3b2b), board_ahci }, /* PCH RAID */
	{ PCI_VDEVICE(INTEL, 0x3b2c), board_ahci }, /* PCH RAID */
	{ PCI_VDEVICE(INTEL, 0x3b2f), board_ahci }, /* PCH AHCI */
	{ PCI_VDEVICE(INTEL, 0x1c02), board_ahci }, /* CPT AHCI */
	{ PCI_VDEVICE(INTEL, 0x1c03), board_ahci }, /* CPT AHCI */
	{ PCI_VDEVICE(INTEL, 0x1c04), board_ahci }, /* CPT RAID */
	{ PCI_VDEVICE(INTEL, 0x1c05), board_ahci }, /* CPT RAID */
	{ PCI_VDEVICE(INTEL, 0x1c06), board_ahci }, /* CPT RAID */
	{ PCI_VDEVICE(INTEL, 0x1c07), board_ahci }, /* CPT RAID */
	{ PCI_VDEVICE(INTEL, 0x1d02), board_ahci }, /* PBG AHCI */
	{ PCI_VDEVICE(INTEL, 0x1d04), board_ahci }, /* PBG RAID */
	{ PCI_VDEVICE(INTEL, 0x1d06), board_ahci }, /* PBG RAID */
	{ PCI_VDEVICE(INTEL, 0x2826), board_ahci }, /* PBG RAID */
	{ PCI_VDEVICE(INTEL, 0x2323), board_ahci }, /* DH89xxCC AHCI */
	{ PCI_VDEVICE(INTEL, 0x1e02), board_ahci }, /* Panther Point AHCI */
	{ PCI_VDEVICE(INTEL, 0x1e03), board_ahci }, /* Panther Point AHCI */
	{ PCI_VDEVICE(INTEL, 0x1e04), board_ahci }, /* Panther Point RAID */
	{ PCI_VDEVICE(INTEL, 0x1e05), board_ahci }, /* Panther Point RAID */
	{ PCI_VDEVICE(INTEL, 0x1e06), board_ahci }, /* Panther Point RAID */
	{ PCI_VDEVICE(INTEL, 0x1e07), board_ahci }, /* Panther Point RAID */
	{ PCI_VDEVICE(INTEL, 0x1e0e), board_ahci }, /* Panther Point RAID */
	{ PCI_VDEVICE(INTEL, 0x8c02), board_ahci }, /* Lynx Point AHCI */
	{ PCI_VDEVICE(INTEL, 0x8c03), board_ahci }, /* Lynx Point AHCI */
	{ PCI_VDEVICE(INTEL, 0x8c04), board_ahci }, /* Lynx Point RAID */
	{ PCI_VDEVICE(INTEL, 0x8c05), board_ahci }, /* Lynx Point RAID */
	{ PCI_VDEVICE(INTEL, 0x8c06), board_ahci }, /* Lynx Point RAID */
	{ PCI_VDEVICE(INTEL, 0x8c07), board_ahci }, /* Lynx Point RAID */
	{ PCI_VDEVICE(INTEL, 0x8c0e), board_ahci }, /* Lynx Point RAID */
	{ PCI_VDEVICE(INTEL, 0x8c0f), board_ahci }, /* Lynx Point RAID */
	{ PCI_VDEVICE(INTEL, 0x9c02), board_ahci }, /* Lynx Point-LP AHCI */
	{ PCI_VDEVICE(INTEL, 0x9c03), board_ahci }, /* Lynx Point-LP AHCI */
	{ PCI_VDEVICE(INTEL, 0x9c04), board_ahci }, /* Lynx Point-LP RAID */
	{ PCI_VDEVICE(INTEL, 0x9c05), board_ahci }, /* Lynx Point-LP RAID */
	{ PCI_VDEVICE(INTEL, 0x9c06), board_ahci }, /* Lynx Point-LP RAID */
	{ PCI_VDEVICE(INTEL, 0x9c07), board_ahci }, /* Lynx Point-LP RAID */
	{ PCI_VDEVICE(INTEL, 0x9c0e), board_ahci }, /* Lynx Point-LP RAID */
	{ PCI_VDEVICE(INTEL, 0x9c0f), board_ahci }, /* Lynx Point-LP RAID */
	{ PCI_VDEVICE(INTEL, 0x1f22), board_ahci }, /* Avoton AHCI */
	{ PCI_VDEVICE(INTEL, 0x1f23), board_ahci }, /* Avoton AHCI */
	{ PCI_VDEVICE(INTEL, 0x1f24), board_ahci }, /* Avoton RAID */
	{ PCI_VDEVICE(INTEL, 0x1f25), board_ahci }, /* Avoton RAID */
	{ PCI_VDEVICE(INTEL, 0x1f26), board_ahci }, /* Avoton RAID */
	{ PCI_VDEVICE(INTEL, 0x1f27), board_ahci }, /* Avoton RAID */
	{ PCI_VDEVICE(INTEL, 0x1f2e), board_ahci }, /* Avoton RAID */
	{ PCI_VDEVICE(INTEL, 0x1f2f), board_ahci }, /* Avoton RAID */
	{ PCI_VDEVICE(INTEL, 0x1f32), board_ahci }, /* Avoton AHCI */
	{ PCI_VDEVICE(INTEL, 0x1f33), board_ahci }, /* Avoton AHCI */
	{ PCI_VDEVICE(INTEL, 0x1f34), board_ahci }, /* Avoton RAID */
	{ PCI_VDEVICE(INTEL, 0x1f35), board_ahci }, /* Avoton RAID */
	{ PCI_VDEVICE(INTEL, 0x1f36), board_ahci }, /* Avoton RAID */
	{ PCI_VDEVICE(INTEL, 0x1f37), board_ahci }, /* Avoton RAID */
	{ PCI_VDEVICE(INTEL, 0x1f3e), board_ahci }, /* Avoton RAID */
	{ PCI_VDEVICE(INTEL, 0x1f3f), board_ahci }, /* Avoton RAID */
	{ PCI_VDEVICE(INTEL, 0x2823), board_ahci }, /* Wellsburg RAID */
	{ PCI_VDEVICE(INTEL, 0x2827), board_ahci }, /* Wellsburg RAID */
	{ PCI_VDEVICE(INTEL, 0x8d02), board_ahci }, /* Wellsburg AHCI */
	{ PCI_VDEVICE(INTEL, 0x8d04), board_ahci }, /* Wellsburg RAID */
	{ PCI_VDEVICE(INTEL, 0x8d06), board_ahci }, /* Wellsburg RAID */
	{ PCI_VDEVICE(INTEL, 0x8d0e), board_ahci }, /* Wellsburg RAID */
	{ PCI_VDEVICE(INTEL, 0x8d62), board_ahci }, /* Wellsburg AHCI */
	{ PCI_VDEVICE(INTEL, 0x8d64), board_ahci }, /* Wellsburg RAID */
	{ PCI_VDEVICE(INTEL, 0x8d66), board_ahci }, /* Wellsburg RAID */
	{ PCI_VDEVICE(INTEL, 0x8d6e), board_ahci }, /* Wellsburg RAID */
	{ PCI_VDEVICE(INTEL, 0x23a3), board_ahci }, /* Coleto Creek AHCI */
	{ PCI_VDEVICE(INTEL, 0x9c83), board_ahci }, /* Wildcat Point-LP AHCI */
	{ PCI_VDEVICE(INTEL, 0x9c85), board_ahci }, /* Wildcat Point-LP RAID */
	{ PCI_VDEVICE(INTEL, 0x9c87), board_ahci }, /* Wildcat Point-LP RAID */
	{ PCI_VDEVICE(INTEL, 0x9c8f), board_ahci }, /* Wildcat Point-LP RAID */

	/* JMicron 360/1/3/5/6, match class to avoid IDE function */
	{ PCI_VENDOR_ID_JMICRON, PCI_ANY_ID, PCI_ANY_ID, PCI_ANY_ID,
	  PCI_CLASS_STORAGE_SATA_AHCI, 0xffffff, board_ahci_ign_iferr },
	/* JMicron 362B and 362C have an AHCI function with IDE class code */
	{ PCI_VDEVICE(JMICRON, 0x2362), board_ahci_ign_iferr },
	{ PCI_VDEVICE(JMICRON, 0x236f), board_ahci_ign_iferr },

	/* ATI */
	{ PCI_VDEVICE(ATI, 0x4380), board_ahci_sb600 }, /* ATI SB600 */
	{ PCI_VDEVICE(ATI, 0x4390), board_ahci_sb700 }, /* ATI SB700/800 */
	{ PCI_VDEVICE(ATI, 0x4391), board_ahci_sb700 }, /* ATI SB700/800 */
	{ PCI_VDEVICE(ATI, 0x4392), board_ahci_sb700 }, /* ATI SB700/800 */
	{ PCI_VDEVICE(ATI, 0x4393), board_ahci_sb700 }, /* ATI SB700/800 */
	{ PCI_VDEVICE(ATI, 0x4394), board_ahci_sb700 }, /* ATI SB700/800 */
	{ PCI_VDEVICE(ATI, 0x4395), board_ahci_sb700 }, /* ATI SB700/800 */

#if defined(CONFIG_SYNO_LSP_ALPINE)
	/* Annapurna Labs */
	{ PCI_VDEVICE(ANNAPURNA_LABS, 0x0031), board_ahci_alpine }, /* 0031 */
#endif /* CONFIG_SYNO_LSP_ALPINE */

	/* AMD */
	{ PCI_VDEVICE(AMD, 0x7800), board_ahci }, /* AMD Hudson-2 */
	{ PCI_VDEVICE(AMD, 0x7900), board_ahci }, /* AMD CZ */
	/* AMD is using RAID class only for ahci controllers */
	{ PCI_VENDOR_ID_AMD, PCI_ANY_ID, PCI_ANY_ID, PCI_ANY_ID,
	  PCI_CLASS_STORAGE_RAID << 8, 0xffffff, board_ahci },

	/* VIA */
	{ PCI_VDEVICE(VIA, 0x3349), board_ahci_vt8251 }, /* VIA VT8251 */
	{ PCI_VDEVICE(VIA, 0x6287), board_ahci_vt8251 }, /* VIA VT8251 */

	/* NVIDIA */
	{ PCI_VDEVICE(NVIDIA, 0x044c), board_ahci_mcp65 },	/* MCP65 */
	{ PCI_VDEVICE(NVIDIA, 0x044d), board_ahci_mcp65 },	/* MCP65 */
	{ PCI_VDEVICE(NVIDIA, 0x044e), board_ahci_mcp65 },	/* MCP65 */
	{ PCI_VDEVICE(NVIDIA, 0x044f), board_ahci_mcp65 },	/* MCP65 */
	{ PCI_VDEVICE(NVIDIA, 0x045c), board_ahci_mcp65 },	/* MCP65 */
	{ PCI_VDEVICE(NVIDIA, 0x045d), board_ahci_mcp65 },	/* MCP65 */
	{ PCI_VDEVICE(NVIDIA, 0x045e), board_ahci_mcp65 },	/* MCP65 */
	{ PCI_VDEVICE(NVIDIA, 0x045f), board_ahci_mcp65 },	/* MCP65 */
	{ PCI_VDEVICE(NVIDIA, 0x0550), board_ahci_mcp67 },	/* MCP67 */
	{ PCI_VDEVICE(NVIDIA, 0x0551), board_ahci_mcp67 },	/* MCP67 */
	{ PCI_VDEVICE(NVIDIA, 0x0552), board_ahci_mcp67 },	/* MCP67 */
	{ PCI_VDEVICE(NVIDIA, 0x0553), board_ahci_mcp67 },	/* MCP67 */
	{ PCI_VDEVICE(NVIDIA, 0x0554), board_ahci_mcp67 },	/* MCP67 */
	{ PCI_VDEVICE(NVIDIA, 0x0555), board_ahci_mcp67 },	/* MCP67 */
	{ PCI_VDEVICE(NVIDIA, 0x0556), board_ahci_mcp67 },	/* MCP67 */
	{ PCI_VDEVICE(NVIDIA, 0x0557), board_ahci_mcp67 },	/* MCP67 */
	{ PCI_VDEVICE(NVIDIA, 0x0558), board_ahci_mcp67 },	/* MCP67 */
	{ PCI_VDEVICE(NVIDIA, 0x0559), board_ahci_mcp67 },	/* MCP67 */
	{ PCI_VDEVICE(NVIDIA, 0x055a), board_ahci_mcp67 },	/* MCP67 */
	{ PCI_VDEVICE(NVIDIA, 0x055b), board_ahci_mcp67 },	/* MCP67 */
	{ PCI_VDEVICE(NVIDIA, 0x0580), board_ahci_mcp_linux },	/* Linux ID */
	{ PCI_VDEVICE(NVIDIA, 0x0581), board_ahci_mcp_linux },	/* Linux ID */
	{ PCI_VDEVICE(NVIDIA, 0x0582), board_ahci_mcp_linux },	/* Linux ID */
	{ PCI_VDEVICE(NVIDIA, 0x0583), board_ahci_mcp_linux },	/* Linux ID */
	{ PCI_VDEVICE(NVIDIA, 0x0584), board_ahci_mcp_linux },	/* Linux ID */
	{ PCI_VDEVICE(NVIDIA, 0x0585), board_ahci_mcp_linux },	/* Linux ID */
	{ PCI_VDEVICE(NVIDIA, 0x0586), board_ahci_mcp_linux },	/* Linux ID */
	{ PCI_VDEVICE(NVIDIA, 0x0587), board_ahci_mcp_linux },	/* Linux ID */
	{ PCI_VDEVICE(NVIDIA, 0x0588), board_ahci_mcp_linux },	/* Linux ID */
	{ PCI_VDEVICE(NVIDIA, 0x0589), board_ahci_mcp_linux },	/* Linux ID */
	{ PCI_VDEVICE(NVIDIA, 0x058a), board_ahci_mcp_linux },	/* Linux ID */
	{ PCI_VDEVICE(NVIDIA, 0x058b), board_ahci_mcp_linux },	/* Linux ID */
	{ PCI_VDEVICE(NVIDIA, 0x058c), board_ahci_mcp_linux },	/* Linux ID */
	{ PCI_VDEVICE(NVIDIA, 0x058d), board_ahci_mcp_linux },	/* Linux ID */
	{ PCI_VDEVICE(NVIDIA, 0x058e), board_ahci_mcp_linux },	/* Linux ID */
	{ PCI_VDEVICE(NVIDIA, 0x058f), board_ahci_mcp_linux },	/* Linux ID */
	{ PCI_VDEVICE(NVIDIA, 0x07f0), board_ahci_mcp73 },	/* MCP73 */
	{ PCI_VDEVICE(NVIDIA, 0x07f1), board_ahci_mcp73 },	/* MCP73 */
	{ PCI_VDEVICE(NVIDIA, 0x07f2), board_ahci_mcp73 },	/* MCP73 */
	{ PCI_VDEVICE(NVIDIA, 0x07f3), board_ahci_mcp73 },	/* MCP73 */
	{ PCI_VDEVICE(NVIDIA, 0x07f4), board_ahci_mcp73 },	/* MCP73 */
	{ PCI_VDEVICE(NVIDIA, 0x07f5), board_ahci_mcp73 },	/* MCP73 */
	{ PCI_VDEVICE(NVIDIA, 0x07f6), board_ahci_mcp73 },	/* MCP73 */
	{ PCI_VDEVICE(NVIDIA, 0x07f7), board_ahci_mcp73 },	/* MCP73 */
	{ PCI_VDEVICE(NVIDIA, 0x07f8), board_ahci_mcp73 },	/* MCP73 */
	{ PCI_VDEVICE(NVIDIA, 0x07f9), board_ahci_mcp73 },	/* MCP73 */
	{ PCI_VDEVICE(NVIDIA, 0x07fa), board_ahci_mcp73 },	/* MCP73 */
	{ PCI_VDEVICE(NVIDIA, 0x07fb), board_ahci_mcp73 },	/* MCP73 */
	{ PCI_VDEVICE(NVIDIA, 0x0ad0), board_ahci_mcp77 },	/* MCP77 */
	{ PCI_VDEVICE(NVIDIA, 0x0ad1), board_ahci_mcp77 },	/* MCP77 */
	{ PCI_VDEVICE(NVIDIA, 0x0ad2), board_ahci_mcp77 },	/* MCP77 */
	{ PCI_VDEVICE(NVIDIA, 0x0ad3), board_ahci_mcp77 },	/* MCP77 */
	{ PCI_VDEVICE(NVIDIA, 0x0ad4), board_ahci_mcp77 },	/* MCP77 */
	{ PCI_VDEVICE(NVIDIA, 0x0ad5), board_ahci_mcp77 },	/* MCP77 */
	{ PCI_VDEVICE(NVIDIA, 0x0ad6), board_ahci_mcp77 },	/* MCP77 */
	{ PCI_VDEVICE(NVIDIA, 0x0ad7), board_ahci_mcp77 },	/* MCP77 */
	{ PCI_VDEVICE(NVIDIA, 0x0ad8), board_ahci_mcp77 },	/* MCP77 */
	{ PCI_VDEVICE(NVIDIA, 0x0ad9), board_ahci_mcp77 },	/* MCP77 */
	{ PCI_VDEVICE(NVIDIA, 0x0ada), board_ahci_mcp77 },	/* MCP77 */
	{ PCI_VDEVICE(NVIDIA, 0x0adb), board_ahci_mcp77 },	/* MCP77 */
	{ PCI_VDEVICE(NVIDIA, 0x0ab4), board_ahci_mcp79 },	/* MCP79 */
	{ PCI_VDEVICE(NVIDIA, 0x0ab5), board_ahci_mcp79 },	/* MCP79 */
	{ PCI_VDEVICE(NVIDIA, 0x0ab6), board_ahci_mcp79 },	/* MCP79 */
	{ PCI_VDEVICE(NVIDIA, 0x0ab7), board_ahci_mcp79 },	/* MCP79 */
	{ PCI_VDEVICE(NVIDIA, 0x0ab8), board_ahci_mcp79 },	/* MCP79 */
	{ PCI_VDEVICE(NVIDIA, 0x0ab9), board_ahci_mcp79 },	/* MCP79 */
	{ PCI_VDEVICE(NVIDIA, 0x0aba), board_ahci_mcp79 },	/* MCP79 */
	{ PCI_VDEVICE(NVIDIA, 0x0abb), board_ahci_mcp79 },	/* MCP79 */
	{ PCI_VDEVICE(NVIDIA, 0x0abc), board_ahci_mcp79 },	/* MCP79 */
	{ PCI_VDEVICE(NVIDIA, 0x0abd), board_ahci_mcp79 },	/* MCP79 */
	{ PCI_VDEVICE(NVIDIA, 0x0abe), board_ahci_mcp79 },	/* MCP79 */
	{ PCI_VDEVICE(NVIDIA, 0x0abf), board_ahci_mcp79 },	/* MCP79 */
	{ PCI_VDEVICE(NVIDIA, 0x0d84), board_ahci_mcp89 },	/* MCP89 */
	{ PCI_VDEVICE(NVIDIA, 0x0d85), board_ahci_mcp89 },	/* MCP89 */
	{ PCI_VDEVICE(NVIDIA, 0x0d86), board_ahci_mcp89 },	/* MCP89 */
	{ PCI_VDEVICE(NVIDIA, 0x0d87), board_ahci_mcp89 },	/* MCP89 */
	{ PCI_VDEVICE(NVIDIA, 0x0d88), board_ahci_mcp89 },	/* MCP89 */
	{ PCI_VDEVICE(NVIDIA, 0x0d89), board_ahci_mcp89 },	/* MCP89 */
	{ PCI_VDEVICE(NVIDIA, 0x0d8a), board_ahci_mcp89 },	/* MCP89 */
	{ PCI_VDEVICE(NVIDIA, 0x0d8b), board_ahci_mcp89 },	/* MCP89 */
	{ PCI_VDEVICE(NVIDIA, 0x0d8c), board_ahci_mcp89 },	/* MCP89 */
	{ PCI_VDEVICE(NVIDIA, 0x0d8d), board_ahci_mcp89 },	/* MCP89 */
	{ PCI_VDEVICE(NVIDIA, 0x0d8e), board_ahci_mcp89 },	/* MCP89 */
	{ PCI_VDEVICE(NVIDIA, 0x0d8f), board_ahci_mcp89 },	/* MCP89 */

	/* SiS */
	{ PCI_VDEVICE(SI, 0x1184), board_ahci },		/* SiS 966 */
	{ PCI_VDEVICE(SI, 0x1185), board_ahci },		/* SiS 968 */
	{ PCI_VDEVICE(SI, 0x0186), board_ahci },		/* SiS 968 */

	/* ST Microelectronics */
	{ PCI_VDEVICE(STMICRO, 0xCC06), board_ahci },		/* ST ConneXt */

	/* Marvell */
	{ PCI_VDEVICE(MARVELL, 0x6145), board_ahci_mv },	/* 6145 */
	{ PCI_VDEVICE(MARVELL, 0x6121), board_ahci_mv },	/* 6121 */
	{ PCI_DEVICE(PCI_VENDOR_ID_MARVELL_EXT, 0x9123),
	  .class = PCI_CLASS_STORAGE_SATA_AHCI,
	  .class_mask = 0xffffff,
	  .driver_data = board_ahci_yes_fbs },			/* 88se9128 */
	{ PCI_DEVICE(PCI_VENDOR_ID_MARVELL_EXT, 0x9125),
	  .driver_data = board_ahci_yes_fbs },			/* 88se9125 */
	{ PCI_DEVICE_SUB(PCI_VENDOR_ID_MARVELL_EXT, 0x9178,
			 PCI_VENDOR_ID_MARVELL_EXT, 0x9170),
	  .driver_data = board_ahci_yes_fbs },			/* 88se9170 */
	{ PCI_DEVICE(PCI_VENDOR_ID_MARVELL_EXT, 0x917a),
	  .driver_data = board_ahci_yes_fbs },			/* 88se9172 */
	{ PCI_DEVICE(PCI_VENDOR_ID_MARVELL_EXT, 0x9172),
	  .driver_data = board_ahci_yes_fbs },			/* 88se9172 */
	{ PCI_DEVICE(PCI_VENDOR_ID_MARVELL_EXT, 0x9192),
	  .driver_data = board_ahci_yes_fbs },			/* 88se9172 on some Gigabyte */
	{ PCI_DEVICE(PCI_VENDOR_ID_MARVELL_EXT, 0x91a3),
	  .driver_data = board_ahci_yes_fbs },
	{ PCI_DEVICE(PCI_VENDOR_ID_MARVELL_EXT, 0x9230),
	  .driver_data = board_ahci_yes_fbs },
#if defined(CONFIG_SYNO_MV_9235_PORTING) || defined(CONFIG_SYNO_LSP_ALPINE)
	{ PCI_DEVICE(PCI_VENDOR_ID_MARVELL_EXT, 0x9235),
	  .driver_data = board_ahci_yes_fbs },			/* 88se9235 */
	{ PCI_DEVICE(PCI_VENDOR_ID_MARVELL_EXT, 0x9215),
	  .driver_data = board_ahci_yes_fbs },			/* 88se9215 */
#endif /* CONFIG_SYNO_MV_9235_PORTING || CONFIG_SYNO_LSP_ALPINE */
#ifdef CONFIG_SYNO_SATA_AHCI_FBS_NONCQ
	{ PCI_DEVICE(PCI_VENDOR_ID_MARVELL_EXT, 0x9170),
	  .driver_data = board_ahci_yes_fbs_no_ncq },   /* 88se9170 */
#endif /* CONFIG_SYNO_SATA_AHCI_FBS_NONCQ */

	/* Promise */
	{ PCI_VDEVICE(PROMISE, 0x3f20), board_ahci },	/* PDC42819 */

	/* Asmedia */
	{ PCI_VDEVICE(ASMEDIA, 0x0601), board_ahci },	/* ASM1060 */
	{ PCI_VDEVICE(ASMEDIA, 0x0602), board_ahci },	/* ASM1060 */
	{ PCI_VDEVICE(ASMEDIA, 0x0611), board_ahci },	/* ASM1061 */
	{ PCI_VDEVICE(ASMEDIA, 0x0612), board_ahci },	/* ASM1062 */

	/*
	 * Samsung SSDs found on some macbooks.  NCQ times out.
	 * https://bugzilla.kernel.org/show_bug.cgi?id=60731
	 */
	{ PCI_VDEVICE(SAMSUNG, 0x1600), board_ahci_noncq },

	/* Enmotus */
	{ PCI_DEVICE(0x1c44, 0x8000), board_ahci },

	/* Generic, PCI class code for AHCI */
	{ PCI_ANY_ID, PCI_ANY_ID, PCI_ANY_ID, PCI_ANY_ID,
	  PCI_CLASS_STORAGE_SATA_AHCI, 0xffffff, board_ahci },

	{ }	/* terminate list */
};

#ifdef CONFIG_SYNO_MV_9235_GPIO_CTRL

#define MV_GEN 3
#define MV_PORT 4
/* The register addrs are provided by Marvell and no description on spec */
const unsigned int mv_sata_gen[MV_GEN] = {0x8D, 0x8F, 0x91};
const unsigned mv_port_addr[MV_PORT] = {0x178, 0x1f8, 0x278, 0x2f8};
const unsigned mv_port_data[MV_PORT] = {0x17c, 0x1fc, 0x27c, 0x2fc};

/*
 * 9235 gpio mmio address, to control 9235 GPIO, please read register manual section 1.6
 */
enum {
	/* MV_9235_GPIO_DATA_OUT the actual address is 0x71020, the 0x07000000 is MBUS_ID for vendor specific register 1 */
	MV_9235_GPIO_DATA_OUT			= 0x07071020,
	MV_9235_GPIO_DATA_OUT_ENABLE		= 0x07071024,
	MV_9235_GPIO_ACTIVE			= 0x07071058,
	MV_9235_VENDOR_SPEC1_ADDR_OFFSET	= 0xA8,			/* To manipulate GPIO via Vendor specific register */
	MV_9235_VENDOR_SPEC1_DATA_OFFSET	= 0xAC,
};

/*
 *	Read value from 9235 gpio register
 */
u32 syno_mv_9235_gpio_reg_read(struct ata_host *host, const unsigned int gpioaddr)
{
	void __iomem *host_mmio = NULL;
	u32 value = 0;

	if(NULL == (host_mmio = ahci_host_base(host))) {
		goto END;
	}

	// write to 9235 gpio active register address to VENDER_SPEC_ADDR1
	writel(gpioaddr, host_mmio + MV_9235_VENDOR_SPEC1_ADDR_OFFSET);
	// read original value from vendor specific data1
	value = readl(host_mmio + MV_9235_VENDOR_SPEC1_DATA_OFFSET);
END:
	return value;
}

/*
 *	9235 GPIO register set
 */
void syno_mv_9235_gpio_reg_set(struct ata_host *host, const unsigned int gpioaddr, u32 value)
{
	void __iomem *host_mmio = NULL;
	u32 reg_val;

	if(NULL == (host_mmio = ahci_host_base(host))) {
		goto END;
	}

	// write to 9235 gpio active register address to VENDER_SPEC_ADDR1
	writel(gpioaddr, host_mmio + MV_9235_VENDOR_SPEC1_ADDR_OFFSET);
	// read original value from vendor specific data1
	reg_val = readl(host_mmio + MV_9235_VENDOR_SPEC1_DATA_OFFSET);
	// then write value to it
	writel(value, host_mmio + MV_9235_VENDOR_SPEC1_DATA_OFFSET);
END:
	return;
}

/*
 *	Get value from 9xxx vendor spec register2
 */
static u32 syno_mv_9xxx_reg_get(struct ata_host *host, const unsigned int reg_addr, unsigned int addr_offset, unsigned int data_offset)
{
	void __iomem *host_mmio = NULL;
	u32 value = 0;

	if(NULL == (host_mmio = ahci_host_base(host))) {
		goto END;
	}

	// write to 9xxx from register
	writel(reg_addr, host_mmio + addr_offset);
	// read original value from register
	value = readl(host_mmio + data_offset);
END:
	return value;
}

/*
 *	Set value to 9xxx vendor spec register2
 */
static void syno_mv_9xxx_reg_set(struct ata_host *host, const unsigned int reg_addr, u32 value, const unsigned int addr_offset, const unsigned int data_offset)
{
	void __iomem *host_mmio = NULL;

	if(NULL == (host_mmio = ahci_host_base(host))) {
		goto END;
	}

	// set 9xxx register for writting
	writel(reg_addr, host_mmio + addr_offset);
	// then write value to it
	writel(value, host_mmio + data_offset);
END:
	return;
}

/*
 *	9235 GPIO init
 */
void syno_mv_9235_gpio_active_init(struct ata_host *host)
{
	// gpio enable is low active
	syno_mv_9235_gpio_reg_set(host, MV_9235_GPIO_DATA_OUT_ENABLE, 0x0);
	syno_mv_9235_gpio_reg_set(host, MV_9235_GPIO_DATA_OUT, 0x0);
	// set the lower 4 GPIO as link/active to disk 1~4 and upper 4 GPIO as faulty to disk 1~4
	syno_mv_9235_gpio_reg_set(host, MV_9235_GPIO_ACTIVE, 0x00B6D8D1);
}

void syno_mv_9xxx_amp_adjust_by_port(struct ata_host *host, u32 val, unsigned int addr_offset, const unsigned int data_offset, const unsigned int reg_addr)
{
	u32 reg_val = 0;

	reg_val = syno_mv_9xxx_reg_get(host, 0x0E, addr_offset, data_offset);
	syno_mv_9xxx_reg_set(host, 0xE, reg_val & ~0x100, addr_offset, data_offset);
	reg_val = syno_mv_9xxx_reg_get(host, reg_addr, addr_offset, data_offset);
	reg_val &= ~0xFBE;
	reg_val |= val;
	syno_mv_9xxx_reg_set(host, reg_addr, reg_val, addr_offset, data_offset);
}

void syno_mv_9xxx_amp_adjust(struct ata_host *host)
{
	int port = 0;

	if (syno_is_hw_version(HW_RS2416p) || syno_is_hw_version(HW_RS2416rpp)) {
		for (port = 0; port < 4; port++) {
			// set G3_TX_EMPH_EN = 1, G3_TX_EMPH_AMP = 0xF, G3_TX_AMP = 0x1F
			syno_mv_9xxx_amp_adjust_by_port(host, 0xFBE, mv_port_addr[port], mv_port_data[port], mv_sata_gen[2]);
		}
	} else if (syno_is_hw_version(HW_DS416p)) {
		// set port1 & port2 G3_TX_EMPH_EN = 1, G3_TX_EMPH_AMP = 0xF, G3_TX_AMP = 0x1F
		port = 1;
		syno_mv_9xxx_amp_adjust_by_port(host, 0xFBE, mv_port_addr[port], mv_port_data[port], mv_sata_gen[2]);

		// ESATA port
		port = 2;
		syno_mv_9xxx_amp_adjust_by_port(host, 0xFBE, mv_port_addr[port], mv_port_data[port], mv_sata_gen[2]);
	} else if (syno_is_hw_version(HW_DS716p)) {
		// set port0  GX_TX_EMPH_EN = 1, GX_TX_EMPH_AMP = 0xF, GX_TX_AMP = 0x1F
		port = 0;
		syno_mv_9xxx_amp_adjust_by_port(host, 0xFBE, mv_port_addr[port], mv_port_data[port], mv_sata_gen[2]);
		syno_mv_9xxx_amp_adjust_by_port(host, 0xFBE, mv_port_addr[port], mv_port_data[port], mv_sata_gen[1]);
		syno_mv_9xxx_amp_adjust_by_port(host, 0xFBE, mv_port_addr[port], mv_port_data[port], mv_sata_gen[0]);
	} else if (syno_is_hw_version(HW_DS1616p)) {
		// set port0 port1 G2_TX_EMPH_EN = 1, G2_TX_EMPH_AMP = 0xC, G2_TX_AMP = 0x1F
		port = 0;
		syno_mv_9xxx_amp_adjust_by_port(host, 0xE3E, mv_port_addr[port], mv_port_data[port], mv_sata_gen[1]);
		port = 1;
		syno_mv_9xxx_amp_adjust_by_port(host, 0xE3E, mv_port_addr[port], mv_port_data[port], mv_sata_gen[1]);
	}
}

int syno_mv_9235_disk_led_get(const unsigned short hostnum)
{
	struct Scsi_Host *shost = scsi_host_lookup(hostnum);
	struct ata_port *ap = NULL;
	int ret = -1;
	u32 value;
	int led_idx;

	if (NULL == shost) {
		goto END;
	}

	if (NULL == (ap = ata_shost_to_port(shost))) {
		goto END;
	}

	// fault pins on last 4 pins
	led_idx = ap->print_id - ap->host->ports[0]->print_id + 4;

	value = syno_mv_9235_gpio_reg_read(ap->host, MV_9235_GPIO_DATA_OUT);

	if (value & (1 << led_idx)) {
		ret = 1;
	} else {
		ret = 0;
	}
END:
	if (NULL != shost) {
		scsi_host_put(shost);
	}
	return ret;
}
EXPORT_SYMBOL(syno_mv_9235_disk_led_get);

/*
 *	Write value to 9235 gpio
 */
int syno_mv_9235_disk_led_set(const unsigned short hostnum, int iValue)
{
	struct Scsi_Host *shost = scsi_host_lookup(hostnum);
	struct ata_port *ap = NULL;
	int ret = -EINVAL;
	u32 value;
	int led_idx;

	if(NULL == shost) {
		goto END;
	}

	if(NULL == (ap = ata_shost_to_port(shost))) {
		goto END;
	}

	led_idx = ap->print_id - ap->host->ports[0]->print_id + 4;
	value = syno_mv_9235_gpio_reg_read(ap->host, MV_9235_GPIO_DATA_OUT);
	if (1 == iValue) {
		value |= (1 << led_idx);
	} else {
		value &= ~(1 << led_idx);
	}
	syno_mv_9235_gpio_reg_set(ap->host, MV_9235_GPIO_DATA_OUT, value);
	ret = 0;
END:
	if (NULL != shost) {
		scsi_host_put(shost);
	}
	return ret;
}

EXPORT_SYMBOL(syno_mv_9235_disk_led_set);
#endif /* CONFIG_SYNO_MV_9235_GPIO_CTRL*/

static struct pci_driver ahci_pci_driver = {
	.name			= DRV_NAME,
	.id_table		= ahci_pci_tbl,
	.probe			= ahci_init_one,
	.remove			= ata_pci_remove_one,
#ifdef CONFIG_PM
	.suspend		= ahci_pci_device_suspend,
	.resume			= ahci_pci_device_resume,
#endif
};

#if defined(CONFIG_PATA_MARVELL) || defined(CONFIG_PATA_MARVELL_MODULE)
static int marvell_enable;
#else
static int marvell_enable = 1;
#endif
module_param(marvell_enable, int, 0644);
MODULE_PARM_DESC(marvell_enable, "Marvell SATA via AHCI (1 = enabled)");

static void ahci_pci_save_initial_config(struct pci_dev *pdev,
					 struct ahci_host_priv *hpriv)
{
	unsigned int force_port_map = 0;
	unsigned int mask_port_map = 0;

	if (pdev->vendor == PCI_VENDOR_ID_JMICRON && pdev->device == 0x2361) {
		dev_info(&pdev->dev, "JMB361 has only one port\n");
		force_port_map = 1;
	}

	/*
	 * Temporary Marvell 6145 hack: PATA port presence
	 * is asserted through the standard AHCI port
	 * presence register, as bit 4 (counting from 0)
	 */
	if (hpriv->flags & AHCI_HFLAG_MV_PATA) {
		if (pdev->device == 0x6121)
			mask_port_map = 0x3;
		else
			mask_port_map = 0xf;
		dev_info(&pdev->dev,
			  "Disabling your PATA port. Use the boot option 'ahci.marvell_enable=0' to avoid this.\n");
	}

	ahci_save_initial_config(&pdev->dev, hpriv, force_port_map,
				 mask_port_map);
}

static int ahci_pci_reset_controller(struct ata_host *host)
{
	struct pci_dev *pdev = to_pci_dev(host->dev);

	ahci_reset_controller(host);

	if (pdev->vendor == PCI_VENDOR_ID_INTEL) {
		struct ahci_host_priv *hpriv = host->private_data;
		u16 tmp16;

		/* configure PCS */
		pci_read_config_word(pdev, 0x92, &tmp16);
		if ((tmp16 & hpriv->port_map) != hpriv->port_map) {
			tmp16 |= hpriv->port_map;
			pci_write_config_word(pdev, 0x92, tmp16);
		}
	}

	return 0;
}

static void ahci_pci_init_controller(struct ata_host *host)
{
	struct ahci_host_priv *hpriv = host->private_data;
	struct pci_dev *pdev = to_pci_dev(host->dev);
	void __iomem *port_mmio;
	u32 tmp;
	int mv;

	if (hpriv->flags & AHCI_HFLAG_MV_PATA) {
		if (pdev->device == 0x6121)
			mv = 2;
		else
			mv = 4;
		port_mmio = __ahci_port_base(host, mv);

		writel(0, port_mmio + PORT_IRQ_MASK);

		/* clear port IRQ */
		tmp = readl(port_mmio + PORT_IRQ_STAT);
		VPRINTK("PORT_IRQ_STAT 0x%x\n", tmp);
		if (tmp)
			writel(tmp, port_mmio + PORT_IRQ_STAT);
	}

	ahci_init_controller(host);
}

static int ahci_vt8251_hardreset(struct ata_link *link, unsigned int *class,
				 unsigned long deadline)
{
	struct ata_port *ap = link->ap;
	bool online;
	int rc;

	DPRINTK("ENTER\n");

	ahci_stop_engine(ap);

	rc = sata_link_hardreset(link, sata_ehc_deb_timing(&link->eh_context),
				 deadline, &online, NULL);

	ahci_start_engine(ap);

	DPRINTK("EXIT, rc=%d, class=%u\n", rc, *class);

	/* vt8251 doesn't clear BSY on signature FIS reception,
	 * request follow-up softreset.
	 */
	return online ? -EAGAIN : rc;
}

static int ahci_p5wdh_hardreset(struct ata_link *link, unsigned int *class,
				unsigned long deadline)
{
	struct ata_port *ap = link->ap;
	struct ahci_port_priv *pp = ap->private_data;
	u8 *d2h_fis = pp->rx_fis + RX_FIS_D2H_REG;
	struct ata_taskfile tf;
	bool online;
	int rc;

	ahci_stop_engine(ap);

	/* clear D2H reception area to properly wait for D2H FIS */
	ata_tf_init(link->device, &tf);
	tf.command = 0x80;
	ata_tf_to_fis(&tf, 0, 0, d2h_fis);

	rc = sata_link_hardreset(link, sata_ehc_deb_timing(&link->eh_context),
				 deadline, &online, NULL);

	ahci_start_engine(ap);

	/* The pseudo configuration device on SIMG4726 attached to
	 * ASUS P5W-DH Deluxe doesn't send signature FIS after
	 * hardreset if no device is attached to the first downstream
	 * port && the pseudo device locks up on SRST w/ PMP==0.  To
	 * work around this, wait for !BSY only briefly.  If BSY isn't
	 * cleared, perform CLO and proceed to IDENTIFY (achieved by
	 * ATA_LFLAG_NO_SRST and ATA_LFLAG_ASSUME_ATA).
	 *
	 * Wait for two seconds.  Devices attached to downstream port
	 * which can't process the following IDENTIFY after this will
	 * have to be reset again.  For most cases, this should
	 * suffice while making probing snappish enough.
	 */
	if (online) {
		rc = ata_wait_after_reset(link, jiffies + 2 * HZ,
					  ahci_check_ready);
		if (rc)
			ahci_kick_engine(ap);
	}
	return rc;
}

#ifdef CONFIG_PM
static int ahci_pci_device_suspend(struct pci_dev *pdev, pm_message_t mesg)
{
	struct ata_host *host = dev_get_drvdata(&pdev->dev);
	struct ahci_host_priv *hpriv = host->private_data;
	void __iomem *mmio = hpriv->mmio;
	u32 ctl;

	if (mesg.event & PM_EVENT_SUSPEND &&
	    hpriv->flags & AHCI_HFLAG_NO_SUSPEND) {
		dev_err(&pdev->dev,
			"BIOS update required for suspend/resume\n");
		return -EIO;
	}

	if (mesg.event & PM_EVENT_SLEEP) {
		/* AHCI spec rev1.1 section 8.3.3:
		 * Software must disable interrupts prior to requesting a
		 * transition of the HBA to D3 state.
		 */
		ctl = readl(mmio + HOST_CTL);
		ctl &= ~HOST_IRQ_EN;
		writel(ctl, mmio + HOST_CTL);
		readl(mmio + HOST_CTL); /* flush */
	}

	return ata_pci_device_suspend(pdev, mesg);
}

static int ahci_pci_device_resume(struct pci_dev *pdev)
{
	struct ata_host *host = dev_get_drvdata(&pdev->dev);
	int rc;

	rc = ata_pci_device_do_resume(pdev);
	if (rc)
		return rc;

	if (pdev->dev.power.power_state.event == PM_EVENT_SUSPEND) {
		rc = ahci_pci_reset_controller(host);
		if (rc)
			return rc;

		ahci_pci_init_controller(host);
	}

	ata_host_resume(host);

	return 0;
}
#endif

static int ahci_configure_dma_masks(struct pci_dev *pdev, int using_dac)
{
	int rc;

	/*
	 * If the device fixup already set the dma_mask to some non-standard
	 * value, don't extend it here. This happens on STA2X11, for example.
	 */
	if (pdev->dma_mask && pdev->dma_mask < DMA_BIT_MASK(32))
		return 0;

	if (using_dac &&
	    !pci_set_dma_mask(pdev, DMA_BIT_MASK(64))) {
		rc = pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(64));
		if (rc) {
			rc = pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(32));
			if (rc) {
				dev_err(&pdev->dev,
					"64-bit DMA enable failed\n");
				return rc;
			}
		}
	} else {
		rc = pci_set_dma_mask(pdev, DMA_BIT_MASK(32));
		if (rc) {
			dev_err(&pdev->dev, "32-bit DMA enable failed\n");
			return rc;
		}
		rc = pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(32));
		if (rc) {
			dev_err(&pdev->dev,
				"32-bit consistent DMA enable failed\n");
			return rc;
		}
	}
	return 0;
}

static void ahci_pci_print_info(struct ata_host *host)
{
	struct pci_dev *pdev = to_pci_dev(host->dev);
	u16 cc;
	const char *scc_s;

	pci_read_config_word(pdev, 0x0a, &cc);
	if (cc == PCI_CLASS_STORAGE_IDE)
		scc_s = "IDE";
	else if (cc == PCI_CLASS_STORAGE_SATA)
		scc_s = "SATA";
	else if (cc == PCI_CLASS_STORAGE_RAID)
		scc_s = "RAID";
	else
		scc_s = "unknown";

	ahci_print_info(host, scc_s);
}

/* On ASUS P5W DH Deluxe, the second port of PCI device 00:1f.2 is
 * hardwired to on-board SIMG 4726.  The chipset is ICH8 and doesn't
 * support PMP and the 4726 either directly exports the device
 * attached to the first downstream port or acts as a hardware storage
 * controller and emulate a single ATA device (can be RAID 0/1 or some
 * other configuration).
 *
 * When there's no device attached to the first downstream port of the
 * 4726, "Config Disk" appears, which is a pseudo ATA device to
 * configure the 4726.  However, ATA emulation of the device is very
 * lame.  It doesn't send signature D2H Reg FIS after the initial
 * hardreset, pukes on SRST w/ PMP==0 and has bunch of other issues.
 *
 * The following function works around the problem by always using
 * hardreset on the port and not depending on receiving signature FIS
 * afterward.  If signature FIS isn't received soon, ATA class is
 * assumed without follow-up softreset.
 */
static void ahci_p5wdh_workaround(struct ata_host *host)
{
	static struct dmi_system_id sysids[] = {
		{
			.ident = "P5W DH Deluxe",
			.matches = {
				DMI_MATCH(DMI_SYS_VENDOR,
					  "ASUSTEK COMPUTER INC"),
				DMI_MATCH(DMI_PRODUCT_NAME, "P5W DH Deluxe"),
			},
		},
		{ }
	};
	struct pci_dev *pdev = to_pci_dev(host->dev);

	if (pdev->bus->number == 0 && pdev->devfn == PCI_DEVFN(0x1f, 2) &&
	    dmi_check_system(sysids)) {
		struct ata_port *ap = host->ports[1];

		dev_info(&pdev->dev,
			 "enabling ASUS P5W DH Deluxe on-board SIMG4726 workaround\n");

		ap->ops = &ahci_p5wdh_ops;
		ap->link.flags |= ATA_LFLAG_NO_SRST | ATA_LFLAG_ASSUME_ATA;
	}
}

/* only some SB600 ahci controllers can do 64bit DMA */
static bool ahci_sb600_enable_64bit(struct pci_dev *pdev)
{
	static const struct dmi_system_id sysids[] = {
		/*
		 * The oldest version known to be broken is 0901 and
		 * working is 1501 which was released on 2007-10-26.
		 * Enable 64bit DMA on 1501 and anything newer.
		 *
		 * Please read bko#9412 for more info.
		 */
		{
			.ident = "ASUS M2A-VM",
			.matches = {
				DMI_MATCH(DMI_BOARD_VENDOR,
					  "ASUSTeK Computer INC."),
				DMI_MATCH(DMI_BOARD_NAME, "M2A-VM"),
			},
			.driver_data = "20071026",	/* yyyymmdd */
		},
		/*
		 * All BIOS versions for the MSI K9A2 Platinum (MS-7376)
		 * support 64bit DMA.
		 *
		 * BIOS versions earlier than 1.5 had the Manufacturer DMI
		 * fields as "MICRO-STAR INTERANTIONAL CO.,LTD".
		 * This spelling mistake was fixed in BIOS version 1.5, so
		 * 1.5 and later have the Manufacturer as
		 * "MICRO-STAR INTERNATIONAL CO.,LTD".
		 * So try to match on DMI_BOARD_VENDOR of "MICRO-STAR INTER".
		 *
		 * BIOS versions earlier than 1.9 had a Board Product Name
		 * DMI field of "MS-7376". This was changed to be
		 * "K9A2 Platinum (MS-7376)" in version 1.9, but we can still
		 * match on DMI_BOARD_NAME of "MS-7376".
		 */
		{
			.ident = "MSI K9A2 Platinum",
			.matches = {
				DMI_MATCH(DMI_BOARD_VENDOR,
					  "MICRO-STAR INTER"),
				DMI_MATCH(DMI_BOARD_NAME, "MS-7376"),
			},
		},
		/*
		 * All BIOS versions for the MSI K9AGM2 (MS-7327) support
		 * 64bit DMA.
		 *
		 * This board also had the typo mentioned above in the
		 * Manufacturer DMI field (fixed in BIOS version 1.5), so
		 * match on DMI_BOARD_VENDOR of "MICRO-STAR INTER" again.
		 */
		{
			.ident = "MSI K9AGM2",
			.matches = {
				DMI_MATCH(DMI_BOARD_VENDOR,
					  "MICRO-STAR INTER"),
				DMI_MATCH(DMI_BOARD_NAME, "MS-7327"),
			},
		},
		/*
		 * All BIOS versions for the Asus M3A support 64bit DMA.
		 * (all release versions from 0301 to 1206 were tested)
		 */
		{
			.ident = "ASUS M3A",
			.matches = {
				DMI_MATCH(DMI_BOARD_VENDOR,
					  "ASUSTeK Computer INC."),
				DMI_MATCH(DMI_BOARD_NAME, "M3A"),
			},
		},
		{ }
	};
	const struct dmi_system_id *match;
	int year, month, date;
	char buf[9];

	match = dmi_first_match(sysids);
	if (pdev->bus->number != 0 || pdev->devfn != PCI_DEVFN(0x12, 0) ||
	    !match)
		return false;

	if (!match->driver_data)
		goto enable_64bit;

	dmi_get_date(DMI_BIOS_DATE, &year, &month, &date);
	snprintf(buf, sizeof(buf), "%04d%02d%02d", year, month, date);

	if (strcmp(buf, match->driver_data) >= 0)
		goto enable_64bit;
	else {
		dev_warn(&pdev->dev,
			 "%s: BIOS too old, forcing 32bit DMA, update BIOS\n",
			 match->ident);
		return false;
	}

enable_64bit:
	dev_warn(&pdev->dev, "%s: enabling 64bit DMA\n", match->ident);
	return true;
}

static bool ahci_broken_system_poweroff(struct pci_dev *pdev)
{
	static const struct dmi_system_id broken_systems[] = {
		{
			.ident = "HP Compaq nx6310",
			.matches = {
				DMI_MATCH(DMI_SYS_VENDOR, "Hewlett-Packard"),
				DMI_MATCH(DMI_PRODUCT_NAME, "HP Compaq nx6310"),
			},
			/* PCI slot number of the controller */
			.driver_data = (void *)0x1FUL,
		},
		{
			.ident = "HP Compaq 6720s",
			.matches = {
				DMI_MATCH(DMI_SYS_VENDOR, "Hewlett-Packard"),
				DMI_MATCH(DMI_PRODUCT_NAME, "HP Compaq 6720s"),
			},
			/* PCI slot number of the controller */
			.driver_data = (void *)0x1FUL,
		},

		{ }	/* terminate list */
	};
	const struct dmi_system_id *dmi = dmi_first_match(broken_systems);

	if (dmi) {
		unsigned long slot = (unsigned long)dmi->driver_data;
		/* apply the quirk only to on-board controllers */
		return slot == PCI_SLOT(pdev->devfn);
	}

	return false;
}

static bool ahci_broken_suspend(struct pci_dev *pdev)
{
	static const struct dmi_system_id sysids[] = {
		/*
		 * On HP dv[4-6] and HDX18 with earlier BIOSen, link
		 * to the harddisk doesn't become online after
		 * resuming from STR.  Warn and fail suspend.
		 *
		 * http://bugzilla.kernel.org/show_bug.cgi?id=12276
		 *
		 * Use dates instead of versions to match as HP is
		 * apparently recycling both product and version
		 * strings.
		 *
		 * http://bugzilla.kernel.org/show_bug.cgi?id=15462
		 */
		{
			.ident = "dv4",
			.matches = {
				DMI_MATCH(DMI_SYS_VENDOR, "Hewlett-Packard"),
				DMI_MATCH(DMI_PRODUCT_NAME,
					  "HP Pavilion dv4 Notebook PC"),
			},
			.driver_data = "20090105",	/* F.30 */
		},
		{
			.ident = "dv5",
			.matches = {
				DMI_MATCH(DMI_SYS_VENDOR, "Hewlett-Packard"),
				DMI_MATCH(DMI_PRODUCT_NAME,
					  "HP Pavilion dv5 Notebook PC"),
			},
			.driver_data = "20090506",	/* F.16 */
		},
		{
			.ident = "dv6",
			.matches = {
				DMI_MATCH(DMI_SYS_VENDOR, "Hewlett-Packard"),
				DMI_MATCH(DMI_PRODUCT_NAME,
					  "HP Pavilion dv6 Notebook PC"),
			},
			.driver_data = "20090423",	/* F.21 */
		},
		{
			.ident = "HDX18",
			.matches = {
				DMI_MATCH(DMI_SYS_VENDOR, "Hewlett-Packard"),
				DMI_MATCH(DMI_PRODUCT_NAME,
					  "HP HDX18 Notebook PC"),
			},
			.driver_data = "20090430",	/* F.23 */
		},
		/*
		 * Acer eMachines G725 has the same problem.  BIOS
		 * V1.03 is known to be broken.  V3.04 is known to
		 * work.  Between, there are V1.06, V2.06 and V3.03
		 * that we don't have much idea about.  For now,
		 * blacklist anything older than V3.04.
		 *
		 * http://bugzilla.kernel.org/show_bug.cgi?id=15104
		 */
		{
			.ident = "G725",
			.matches = {
				DMI_MATCH(DMI_SYS_VENDOR, "eMachines"),
				DMI_MATCH(DMI_PRODUCT_NAME, "eMachines G725"),
			},
			.driver_data = "20091216",	/* V3.04 */
		},
		{ }	/* terminate list */
	};
	const struct dmi_system_id *dmi = dmi_first_match(sysids);
	int year, month, date;
	char buf[9];

	if (!dmi || pdev->bus->number || pdev->devfn != PCI_DEVFN(0x1f, 2))
		return false;

	dmi_get_date(DMI_BIOS_DATE, &year, &month, &date);
	snprintf(buf, sizeof(buf), "%04d%02d%02d", year, month, date);

	return strcmp(buf, dmi->driver_data) < 0;
}

static bool ahci_broken_online(struct pci_dev *pdev)
{
#define ENCODE_BUSDEVFN(bus, slot, func)			\
	(void *)(unsigned long)(((bus) << 8) | PCI_DEVFN((slot), (func)))
	static const struct dmi_system_id sysids[] = {
		/*
		 * There are several gigabyte boards which use
		 * SIMG5723s configured as hardware RAID.  Certain
		 * 5723 firmware revisions shipped there keep the link
		 * online but fail to answer properly to SRST or
		 * IDENTIFY when no device is attached downstream
		 * causing libata to retry quite a few times leading
		 * to excessive detection delay.
		 *
		 * As these firmwares respond to the second reset try
		 * with invalid device signature, considering unknown
		 * sig as offline works around the problem acceptably.
		 */
		{
			.ident = "EP45-DQ6",
			.matches = {
				DMI_MATCH(DMI_BOARD_VENDOR,
					  "Gigabyte Technology Co., Ltd."),
				DMI_MATCH(DMI_BOARD_NAME, "EP45-DQ6"),
			},
			.driver_data = ENCODE_BUSDEVFN(0x0a, 0x00, 0),
		},
		{
			.ident = "EP45-DS5",
			.matches = {
				DMI_MATCH(DMI_BOARD_VENDOR,
					  "Gigabyte Technology Co., Ltd."),
				DMI_MATCH(DMI_BOARD_NAME, "EP45-DS5"),
			},
			.driver_data = ENCODE_BUSDEVFN(0x03, 0x00, 0),
		},
		{ }	/* terminate list */
	};
#undef ENCODE_BUSDEVFN
	const struct dmi_system_id *dmi = dmi_first_match(sysids);
	unsigned int val;

	if (!dmi)
		return false;

	val = (unsigned long)dmi->driver_data;

	return pdev->bus->number == (val >> 8) && pdev->devfn == (val & 0xff);
}

#ifdef CONFIG_ATA_ACPI
static void ahci_gtf_filter_workaround(struct ata_host *host)
{
	static const struct dmi_system_id sysids[] = {
		/*
		 * Aspire 3810T issues a bunch of SATA enable commands
		 * via _GTF including an invalid one and one which is
		 * rejected by the device.  Among the successful ones
		 * is FPDMA non-zero offset enable which when enabled
		 * only on the drive side leads to NCQ command
		 * failures.  Filter it out.
		 */
		{
			.ident = "Aspire 3810T",
			.matches = {
				DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
				DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 3810T"),
			},
			.driver_data = (void *)ATA_ACPI_FILTER_FPDMA_OFFSET,
		},
		{ }
	};
	const struct dmi_system_id *dmi = dmi_first_match(sysids);
	unsigned int filter;
	int i;

	if (!dmi)
		return;

	filter = (unsigned long)dmi->driver_data;
	dev_info(host->dev, "applying extra ACPI _GTF filter 0x%x for %s\n",
		 filter, dmi->ident);

	for (i = 0; i < host->n_ports; i++) {
		struct ata_port *ap = host->ports[i];
		struct ata_link *link;
		struct ata_device *dev;

		ata_for_each_link(link, ap, EDGE)
			ata_for_each_dev(dev, link, ALL)
				dev->gtf_filter |= filter;
	}
}
#else
static inline void ahci_gtf_filter_workaround(struct ata_host *host)
{}
#endif

#if defined(CONFIG_SYNO_LSP_ALPINE) && defined(CONFIG_ARCH_ALPINE)
#define al_ahci_iofic_base(base)	((base) + 0x2000)

static ssize_t al_ahci_show_msix_moder(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct ata_host *host = dev_get_drvdata(dev);
	struct ahci_host_priv *hpriv = host->private_data;
	ssize_t rc = 0;

	rc = sprintf(buf, "%d\n", hpriv->int_moderation);

	return rc;
}

static ssize_t al_ahci_store_msix_moder(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t len)
{
	struct ata_host *host = dev_get_drvdata(dev);
	struct ahci_host_priv *hpriv = host->private_data;
	unsigned long interval;
	int err;
	int i;

	err = kstrtoul(buf, 10, &interval);
	if (err < 0)
		return err;

	for (i = 0; i < ahci_nr_ports(hpriv->cap); i++)
		al_iofic_msix_moder_interval_config(
				al_ahci_iofic_base(hpriv->mmio),
				1 /*GROUP_B*/,
				i,
				interval);

	hpriv->int_moderation = interval;

	return len;
}

static struct device_attribute dev_attr_moder = {
	.attr = {.name = "msix_moder", .mode = (S_IRUGO | S_IWUSR)},
	.show = al_ahci_show_msix_moder,
	.store = al_ahci_store_msix_moder,
};

int al_ahci_sysfs_init(
	struct device *dev)
{
	if (device_create_file(dev, &dev_attr_moder))
		dev_err(dev, "failed to create msix interrupt moderation sysfs entry");

	return 0;
}

/******************************************************************************
 *****************************************************************************/
void al_ahci_sysfs_terminate(
	struct device *dev)
{
	device_remove_file(dev, &dev_attr_moder);
}

irqreturn_t ahci_hw_port_interrupt_handler(int irq, void *dev_instance)
{
	struct ata_port *ap_this = dev_instance;
	struct ata_host *host = ap_this->host;
	struct ahci_host_priv *hpriv = host->private_data;
	void __iomem *iofic_base = al_ahci_iofic_base(hpriv->mmio);
	VPRINTK("ENTER\n");

	spin_lock(ap_this->lock);
	ahci_port_intr(ap_this);

	spin_unlock(ap_this->lock);

	spin_lock(&host->lock);
	/* clean host cause */
	writel(1 << ap_this->port_no, hpriv->mmio + HOST_IRQ_STAT);

	/* unmask the interrupt in the iofic (auto-masked) */
	al_iofic_unmask(iofic_base, 1, 1 << ap_this->port_no);
	spin_unlock(&host->lock);

	VPRINTK("EXIT\n");

	return IRQ_HANDLED;
}

int al_ahci_init_msix(struct pci_dev *pdev, struct ahci_host_priv *hpriv)
{
	unsigned int msix_vecs = ahci_nr_ports(readl(hpriv->mmio + HOST_CAP));
	int i;
	int rc;
	void __iomem *iofic_base = al_ahci_iofic_base(hpriv->mmio);

	hpriv->msix_entries = NULL;

	dev_info(&pdev->dev, "use MSIX for ahci controller. vectors: %u\n",
			msix_vecs);
	hpriv->msix_entries = kcalloc(msix_vecs, sizeof(struct msix_entry), GFP_KERNEL);

	if (!hpriv->msix_entries) {
		dev_err(&pdev->dev, "failed to allocate msix_entries, vectors %d\n",
				msix_vecs);
		return -ENOMEM;
	}

	for (i = 0; i < msix_vecs; i++) {
		hpriv->msix_entries[i].entry = 3 + i;
		hpriv->msix_entries[i].vector = 0;
	}

	rc = pci_enable_msix(pdev, hpriv->msix_entries, msix_vecs);

	if (rc) {
		dev_info(&pdev->dev,"failed to enable MSIX, vectors %d rc %d\n",
				msix_vecs, rc);
		kfree(hpriv->msix_entries);
		hpriv->msix_entries = NULL;
		/* maybe we shoudl fall back to intx*/
		return -EPERM;
	}

	/* we use only group B */
	al_iofic_config(iofic_base, 1 /*GROUP_B*/,
			INT_CONTROL_GRP_SET_ON_POSEDGE |
			INT_CONTROL_GRP_AUTO_CLEAR |
			INT_CONTROL_GRP_AUTO_MASK |
			INT_CONTROL_GRP_CLEAR_ON_READ);

	al_iofic_moder_res_config(iofic_base, 1, 15);

        al_iofic_unmask(iofic_base, 1, (1 << msix_vecs) - 1);

	hpriv->msix_vecs = msix_vecs;

	al_ahci_sysfs_init(&pdev->dev);

	return 0;
}
#endif /* CONFIG_SYNO_LSP_ALPINE && CONFIG_ARCH_ALPINE */

int ahci_init_interrupts(struct pci_dev *pdev, struct ahci_host_priv *hpriv)
{
	int rc;
	unsigned int maxvec;

#if defined(CONFIG_SYNO_LSP_ALPINE) && defined(CONFIG_ARCH_ALPINE)
	if (hpriv->flags & AHCI_HFLAG_MSIX) {
		if (!al_ahci_init_msix(pdev, hpriv))
			return hpriv->msix_vecs;
	}
#endif /* CONFIG_SYNO_LSP_ALPINE && CONFIG_ARCH_ALPINE */

	if (!(hpriv->flags & AHCI_HFLAG_NO_MSI)) {
		rc = pci_enable_msi_block_auto(pdev, &maxvec);
		if (rc > 0) {
			if ((rc == maxvec) || (rc == 1))
				return rc;
			/*
			 * Assume that advantage of multipe MSIs is negated,
			 * so fallback to single MSI mode to save resources
			 */
			pci_disable_msi(pdev);
			if (!pci_enable_msi(pdev))
				return 1;
		}
	}

	pci_intx(pdev, 1);
	return 0;
}

/**
 *	ahci_host_activate - start AHCI host, request IRQs and register it
 *	@host: target ATA host
 *	@irq: base IRQ number to request
 *	@n_msis: number of MSIs allocated for this host
 *	@irq_handler: irq_handler used when requesting IRQs
 *	@irq_flags: irq_flags used when requesting IRQs
 *
 *	Similar to ata_host_activate, but requests IRQs according to AHCI-1.1
 *	when multiple MSIs were allocated. That is one MSI per port, starting
 *	from @irq.
 *
 *	LOCKING:
 *	Inherited from calling layer (may sleep).
 *
 *	RETURNS:
 *	0 on success, -errno otherwise.
 */
int ahci_host_activate(struct ata_host *host, int irq, unsigned int n_msis)
{
	int i, rc;
#if defined(CONFIG_SYNO_LSP_ALPINE)
	struct ahci_host_priv *hpriv = host->private_data;
	int port_irq;
#endif /* CONFIG_SYNO_LSP_ALPINE */

	/* Sharing Last Message among several ports is not supported */
	if (n_msis < host->n_ports)
		return -EINVAL;

	rc = ata_host_start(host);
	if (rc)
		return rc;

	for (i = 0; i < host->n_ports; i++) {
#if defined(CONFIG_SYNO_LSP_ALPINE)
		struct ata_port *ap = host->ports[i];
		struct ahci_port_priv *pp = ap->private_data;

		if (hpriv->msix_entries) {
#if defined(CONFIG_SYNO_LSP_ALPINE)
#ifdef CONFIG_ARCH_ALPINE
			port_irq = hpriv->msix_entries[i].vector;
			snprintf(pp->msix_name, sizeof(pp->msix_name), "ahci_%u",
					ap->port_no);
			rc = devm_request_irq(host->dev,
					port_irq, ahci_hw_port_interrupt_handler,
					0, pp->msix_name, ap);
#else /* CONFIG_ARCH_ALPINE */
			BUG();
#endif /* CONFIG_ARCH_ALPINE */
#endif /* CONFIG_SYNO_LSP_ALPINE */
		} else {
			port_irq = irq + i;
			rc = devm_request_threaded_irq(host->dev,
					port_irq, ahci_hw_interrupt,
					ahci_thread_fn, IRQF_SHARED,
					dev_driver_string(host->dev), host->ports[i]);
		}
#else /* CONFIG_SYNO_LSP_ALPINE */
		rc = devm_request_threaded_irq(host->dev,
			irq + i, ahci_hw_interrupt, ahci_thread_fn, IRQF_SHARED,
			dev_driver_string(host->dev), host->ports[i]);
#endif /* CONFIG_SYNO_LSP_ALPINE */

		if (rc)
			goto out_free_irqs;
	}

#if defined(CONFIG_SYNO_LSP_ALPINE)
	for (i = 0; i < host->n_ports; i++) {
		if (hpriv->msix_entries)
#ifdef CONFIG_ARCH_ALPINE
			port_irq = hpriv->msix_entries[i].vector;
#else /* CONFIG_ARCH_ALPINE */
			BUG();
#endif /* CONFIG_ARCH_ALPINE */
		else
			port_irq = irq + i;
		ata_port_desc(host->ports[i], "irq %d", port_irq);
	}
#else /* CONFIG_SYNO_LSP_ALPINE */
	for (i = 0; i < host->n_ports; i++)
		ata_port_desc(host->ports[i], "irq %d", irq + i);
#endif /* CONFIG_SYNO_LSP_ALPINE */

	rc = ata_host_register(host, &ahci_sht);
	if (rc)
		goto out_free_all_irqs;

	return 0;

out_free_all_irqs:
	i = host->n_ports;
out_free_irqs:
#if defined(CONFIG_SYNO_LSP_ALPINE)
	for (i--; i >= 0; i--) {
		if (hpriv->msix_entries)
#ifdef CONFIG_ARCH_ALPINE
			port_irq = hpriv->msix_entries[i].vector;
#else /* CONFIG_ARCH_ALPINE */
			BUG();
#endif /* CONFIG_ARCH_ALPINE */
		else
			port_irq = irq + i;
		devm_free_irq(host->dev, port_irq, host->ports[i]);
	}
#else /* CONFIG_SYNO_LSP_ALPINE */
	for (i--; i >= 0; i--)
		devm_free_irq(host->dev, irq + i, host->ports[i]);
#endif /* CONFIG_SYNO_LSP_ALPINE */
	return rc;
}

static int ahci_init_one(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	unsigned int board_id = ent->driver_data;
	struct ata_port_info pi = ahci_port_info[board_id];
	const struct ata_port_info *ppi[] = { &pi, NULL };
	struct device *dev = &pdev->dev;
	struct ahci_host_priv *hpriv;
	struct ata_host *host;
#if defined(CONFIG_SYNO_LSP_ALPINE) && defined(CONFIG_ARCH_ALPINE)
	struct device_node *np;
#endif /* CONFIG_SYNO_LSP_ALPINE && CONFIG_ARCH_ALPINE */
	int n_ports, n_msis, i, rc;
	int ahci_pci_bar = AHCI_PCI_BAR_STANDARD;

	VPRINTK("ENTER\n");

#ifdef CONFIG_SYNO_AHCI_SWITCH
	if('0' == g_ahci_switch) {
		printk("AHCI is disabled.\n");
		return 0;
	}

#endif /* CONFIG_SYNO_AHCI_SWITCH */

	WARN_ON((int)ATA_MAX_QUEUE > AHCI_MAX_CMDS);

	ata_print_version_once(&pdev->dev, DRV_VERSION);

	/* The AHCI driver can only drive the SATA ports, the PATA driver
	   can drive them all so if both drivers are selected make sure
	   AHCI stays out of the way */
	if (pdev->vendor == PCI_VENDOR_ID_MARVELL && !marvell_enable)
		return -ENODEV;

	/*
	 * For some reason, MCP89 on MacBook 7,1 doesn't work with
	 * ahci, use ata_generic instead.
	 */
	if (pdev->vendor == PCI_VENDOR_ID_NVIDIA &&
	    pdev->device == PCI_DEVICE_ID_NVIDIA_NFORCE_MCP89_SATA &&
	    pdev->subsystem_vendor == PCI_VENDOR_ID_APPLE &&
	    pdev->subsystem_device == 0xcb89)
		return -ENODEV;

	/* Promise's PDC42819 is a SAS/SATA controller that has an AHCI mode.
	 * At the moment, we can only use the AHCI mode. Let the users know
	 * that for SAS drives they're out of luck.
	 */
	if (pdev->vendor == PCI_VENDOR_ID_PROMISE)
		dev_info(&pdev->dev,
			 "PDC42819 can only drive SATA devices with this driver\n");

	/* Both Connext and Enmotus devices use non-standard BARs */
	if (pdev->vendor == PCI_VENDOR_ID_STMICRO && pdev->device == 0xCC06)
		ahci_pci_bar = AHCI_PCI_BAR_STA2X11;
	else if (pdev->vendor == 0x1c44 && pdev->device == 0x8000)
		ahci_pci_bar = AHCI_PCI_BAR_ENMOTUS;

	/* acquire resources */
	rc = pcim_enable_device(pdev);
	if (rc)
		return rc;

	/* AHCI controllers often implement SFF compatible interface.
	 * Grab all PCI BARs just in case.
	 */
	rc = pcim_iomap_regions_request_all(pdev, 1 << ahci_pci_bar, DRV_NAME);
	if (rc == -EBUSY)
		pcim_pin_device(pdev);
	if (rc)
		return rc;

	if (pdev->vendor == PCI_VENDOR_ID_INTEL &&
	    (pdev->device == 0x2652 || pdev->device == 0x2653)) {
		u8 map;

		/* ICH6s share the same PCI ID for both piix and ahci
		 * modes.  Enabling ahci mode while MAP indicates
		 * combined mode is a bad idea.  Yield to ata_piix.
		 */
		pci_read_config_byte(pdev, ICH_MAP, &map);
		if (map & 0x3) {
			dev_info(&pdev->dev,
				 "controller is in combined mode, can't enable AHCI mode\n");
			return -ENODEV;
		}
	}

	hpriv = devm_kzalloc(dev, sizeof(*hpriv), GFP_KERNEL);
	if (!hpriv)
		return -ENOMEM;
	hpriv->flags |= (unsigned long)pi.private_data;

	/* MCP65 revision A1 and A2 can't do MSI */
	if (board_id == board_ahci_mcp65 &&
	    (pdev->revision == 0xa1 || pdev->revision == 0xa2))
		hpriv->flags |= AHCI_HFLAG_NO_MSI;

	/* SB800 does NOT need the workaround to ignore SERR_INTERNAL */
	if (board_id == board_ahci_sb700 && pdev->revision >= 0x40)
		hpriv->flags &= ~AHCI_HFLAG_IGN_SERR_INTERNAL;

	/* only some SB600s can do 64bit DMA */
	if (ahci_sb600_enable_64bit(pdev))
		hpriv->flags &= ~AHCI_HFLAG_32BIT_ONLY;

	hpriv->mmio = pcim_iomap_table(pdev)[ahci_pci_bar];

	n_msis = ahci_init_interrupts(pdev, hpriv);
	if (n_msis > 1)
		hpriv->flags |= AHCI_HFLAG_MULTI_MSI;

	/* save initial config */
	ahci_pci_save_initial_config(pdev, hpriv);

	/* prepare host */
	if (hpriv->cap & HOST_CAP_NCQ) {
		pi.flags |= ATA_FLAG_NCQ;
		/*
		 * Auto-activate optimization is supposed to be
		 * supported on all AHCI controllers indicating NCQ
		 * capability, but it seems to be broken on some
		 * chipsets including NVIDIAs.
		 */
		if (!(hpriv->flags & AHCI_HFLAG_NO_FPDMA_AA))
			pi.flags |= ATA_FLAG_FPDMA_AA;
	}

	if (hpriv->cap & HOST_CAP_PMP)
		pi.flags |= ATA_FLAG_PMP;

	ahci_set_em_messages(hpriv, &pi);

#if defined(CONFIG_SYNO_LSP_ALPINE) && defined(CONFIG_ARCH_ALPINE)
	for (i = 0; i < AHCI_MAX_PORTS; i++)
		hpriv->led_gpio[i] = -1;

	np = of_find_compatible_node(NULL, NULL, "annapurna-labs,al-sata-sw-leds");
	if (np) {
		int err;
		struct device_node *child;
		u32	domain;
		u32	pci_bus;
		u32	pci_dev;
		u32	port;

		for_each_child_of_node(np, child) {
			err = of_property_read_u32(child, "pci_domain", &domain);
			if (err)
				continue;

			if (domain != pci_domain_nr(pdev->bus))
				continue;

			err = of_property_read_u32(child, "pci_bus", &pci_bus);
			if (err)
				continue;

			if (pci_bus != pdev->bus->number)
				continue;

			err = of_property_read_u32(child, "pci_dev", &pci_dev);
			if (err)
				continue;

			if (pci_dev != PCI_SLOT(pdev->devfn))
				continue;

			err = of_property_read_u32(child, "port", &port);
			if (err)
				continue;

			err = of_get_named_gpio(child, "gpios", 0);
			if (IS_ERR_VALUE(err))
				continue;

			hpriv->led_gpio[port] = err;
			err = gpio_request(hpriv->led_gpio[port], "sata led gpio");
			if (err) {
				dev_err(&pdev->dev, "al ahci gpio_request %d failed: %d\n",
						hpriv->led_gpio[port], err);
				continue;
			}
			gpio_direction_output(hpriv->led_gpio[port], 1);
			hpriv->em_msg_type = EM_MSG_TYPE_LED;
			pi.flags |= ATA_FLAG_EM | ATA_FLAG_SW_ACTIVITY;
		}
	}
#endif /* CONFIG_SYNO_LSP_ALPINE && CONFIG_ARCH_ALPINE */

	if (ahci_broken_system_poweroff(pdev)) {
		pi.flags |= ATA_FLAG_NO_POWEROFF_SPINDOWN;
		dev_info(&pdev->dev,
			"quirky BIOS, skipping spindown on poweroff\n");
	}

	if (ahci_broken_suspend(pdev)) {
		hpriv->flags |= AHCI_HFLAG_NO_SUSPEND;
		dev_warn(&pdev->dev,
			 "BIOS update required for suspend/resume\n");
	}

	if (ahci_broken_online(pdev)) {
		hpriv->flags |= AHCI_HFLAG_SRST_TOUT_IS_OFFLINE;
		dev_info(&pdev->dev,
			 "online status unreliable, applying workaround\n");
	}

	/* CAP.NP sometimes indicate the index of the last enabled
	 * port, at other times, that of the last possible port, so
	 * determining the maximum port number requires looking at
	 * both CAP.NP and port_map.
	 */
#ifdef CONFIG_SYNO_SATA_PORT_MAP
	if(gSynoSataHostCnt < sizeof(gszSataPortMap) && 0 != gszSataPortMap[gSynoSataHostCnt]) {
		n_ports = gszSataPortMap[gSynoSataHostCnt] - '0';
	}else{
#endif /* CONFIG_SYNO_SATA_PORT_MAP */
	n_ports = max(ahci_nr_ports(hpriv->cap), fls(hpriv->port_map));
#ifdef CONFIG_SYNO_SATA_PORT_MAP
	}
#endif /* CONFIG_SYNO_SATA_PORT_MAP */

	host = ata_host_alloc_pinfo(&pdev->dev, ppi, n_ports);
	if (!host)
		return -ENOMEM;
	host->private_data = hpriv;

#ifdef CONFIG_SYNO_MV_9235_PORTING
	if (pdev->vendor == 0x1b4b && pdev->device == 0x9235) {
		hpriv->flags |= AHCI_HFLAG_YES_MV9235_FIX;

		for (i = 0; i < host->n_ports; i++) {
			struct ata_port *ap = host->ports[i];
			ap->link.uiStsFlags |= SYNO_STATUS_IS_MV9235;
		}
	}
#endif /* CONFIG_SYNO_MV_9235_PORTING */

	if (!(hpriv->cap & HOST_CAP_SSS) || ahci_ignore_sss)
		host->flags |= ATA_HOST_PARALLEL_SCAN;
	else
		printk(KERN_INFO "ahci: SSS flag set, parallel bus scan disabled\n");

	if (pi.flags & ATA_FLAG_EM)
		ahci_reset_em(host);

	for (i = 0; i < host->n_ports; i++) {
		struct ata_port *ap = host->ports[i];

		ata_port_pbar_desc(ap, ahci_pci_bar, -1, "abar");
		ata_port_pbar_desc(ap, ahci_pci_bar,
				   0x100 + ap->port_no * 0x80, "port");

		/* set enclosure management message type */
		if (ap->flags & ATA_FLAG_EM)
			ap->em_message_type = hpriv->em_msg_type;

		/* disabled/not-implemented port */
		if (!(hpriv->port_map & (1 << i)))
			ap->ops = &ata_dummy_port_ops;
	}

#ifdef CONFIG_SYNO_AHCI_PMP_SII3x26_DEFER_CMD
	if (syno_is_hw_version(HW_DS1616p) ||
			syno_is_hw_version(HW_DS1515) ||
			syno_is_hw_version(HW_DS416p) ||
			syno_is_hw_version(HW_DS716p)) {
		for (i = 0; i < host->n_ports; i++) {
			printk("Change defer qc mode on external port for compatibility\n", i);
			struct ata_port *ap = host->ports[i];
			ap->ops->qc_defer = &ahci_syno_pmp_3x26_qc_defer;
		}
	}
#endif /* CONFIG_SYNO_AHCI_PMP_SII3x26_DEFER_CMD */

	/* apply workaround for ASUS P5W DH Deluxe mainboard */
	ahci_p5wdh_workaround(host);

	/* apply gtf filter quirk */
	ahci_gtf_filter_workaround(host);

	/* initialize adapter */
	rc = ahci_configure_dma_masks(pdev, hpriv->cap & HOST_CAP_64);
	if (rc)
		return rc;

	rc = ahci_pci_reset_controller(host);
	if (rc)
		return rc;

	ahci_pci_init_controller(host);
	ahci_pci_print_info(host);

	pci_set_master(pdev);
#ifdef CONFIG_SYNO_MV_9235_GPIO_CTRL
	if (pdev->vendor == 0x1b4b && (pdev->device == 0x9235 || pdev->device == 0x9215)) {
		syno_mv_9235_gpio_active_init(host);
	}
	if (pdev->vendor == 0x1b4b && (pdev->device == 0x9235 || pdev->device == 0x9215 || pdev->device == 0x9170)) {
		syno_mv_9xxx_amp_adjust(host);
	}
#endif /* CONFIG_SYNO_MV_9235_GPIO_CTRL */

	if (hpriv->flags & AHCI_HFLAG_MULTI_MSI)
		return ahci_host_activate(host, pdev->irq, n_msis);

	return ata_host_activate(host, pdev->irq, ahci_interrupt, IRQF_SHARED,
				 &ahci_sht);
}

module_pci_driver(ahci_pci_driver);

MODULE_AUTHOR("Jeff Garzik");
MODULE_DESCRIPTION("AHCI SATA low-level driver");
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE(pci, ahci_pci_tbl);
MODULE_VERSION(DRV_VERSION);
