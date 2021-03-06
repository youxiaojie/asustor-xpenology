/*
 * Copyright (C) 2013 Marvell
 *
 * Gregory Clement <gregory.clement@free-electrons.com>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.  This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#ifndef __LINUX_XHCI_MVEBU_H
#define __LINUX_XHCI_MVEBU_H

#if defined(CONFIG_USB_XHCI_MVEBU) || defined(CONFIG_SYNO_ARMADA)
int xhci_mvebu_probe(struct platform_device *pdev);
int xhci_mvebu_remove(struct platform_device *pdev);
void xhci_mvebu_resume(struct device *dev);
#else
#define xhci_mvebu_probe NULL
#define xhci_mvebu_remove NULL
#define xhci_mvebu_resume NULL
#endif
#endif /* __LINUX_XHCI_MVEBU_H */
