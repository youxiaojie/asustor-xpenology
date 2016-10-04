/*
 *  arch/arm/mach-sti/smp.h
 *
 * Copyright (C) 2013 STMicroelectronics (R&D) Limited.
 *		http://www.st.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __MACH_STI_SMP_H
#define __MACH_STI_SMP_H

extern struct smp_operations	sti_smp_ops;
void sti_secondary_startup(void);
#ifdef CONFIG_SYNO_LSP_MONACO_SDK2_15_4
int sti_abap_c_start(void);
extern unsigned int sti_abap_c_size;
#endif /* CONFIG_SYNO_LSP_MONACO_SDK2_15_4 */

#endif
