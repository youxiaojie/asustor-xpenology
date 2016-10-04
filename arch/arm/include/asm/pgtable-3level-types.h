/*
 * arch/arm/include/asm/pgtable-3level-types.h
 *
 * Copyright (C) 2011 ARM Ltd.
 * Author: Catalin Marinas <catalin.marinas@arm.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */
#ifndef _ASM_PGTABLE_3LEVEL_TYPES_H
#define _ASM_PGTABLE_3LEVEL_TYPES_H

#include <asm/types.h>

typedef u64 pteval_t;
typedef u64 pmdval_t;
typedef u64 pgdval_t;

#undef STRICT_MM_TYPECHECKS

#ifdef STRICT_MM_TYPECHECKS

/*
 * These are used to make use of C type-checking..
 */
#if defined(CONFIG_SYNO_LSP_ALPINE)
typedef struct { pteval_t pte;
#if HW_PAGES_PER_PAGE > 1
	pteval_t unused[HW_PAGES_PER_PAGE-1];
#endif /* HW_PAGES_PER_PAGE > 1 */
} pte_t;
#else /* CONFIG_SYNO_LSP_ALPINE */
typedef struct { pteval_t pte; } pte_t;
#endif /* CONFIG_SYNO_LSP_ALPINE */
typedef struct { pmdval_t pmd; } pmd_t;
typedef struct { pgdval_t pgd; } pgd_t;
typedef struct { pteval_t pgprot; } pgprot_t;

#define pte_val(x)      ((x).pte)
#define pmd_val(x)      ((x).pmd)
#define pgd_val(x)	((x).pgd)
#define pgprot_val(x)   ((x).pgprot)

#if defined(CONFIG_SYNO_LSP_ALPINE)
#define __pte(x)        ({pte_t __pte = { .pte = (x) }; __pte; })
#else /* CONFIG_SYNO_LSP_ALPINE */
#define __pte(x)        ((pte_t) { (x) } )
#endif /* CONFIG_SYNO_LSP_ALPINE */
#define __pmd(x)        ((pmd_t) { (x) } )
#define __pgd(x)	((pgd_t) { (x) } )
#define __pgprot(x)     ((pgprot_t) { (x) } )

#else	/* !STRICT_MM_TYPECHECKS */

#if defined(CONFIG_SYNO_LSP_ALPINE)
typedef struct { pteval_t pte;
#if HW_PAGES_PER_PAGE > 1
	pteval_t unused[HW_PAGES_PER_PAGE-1];
#endif /* HW_PAGES_PER_PAGE > 1 */
} pte_t;
#else /* CONFIG_SYNO_LSP_ALPINE */
typedef pteval_t pte_t;
#endif /* CONFIG_SYNO_LSP_ALPINE */
typedef pmdval_t pmd_t;
typedef pgdval_t pgd_t;
typedef pteval_t pgprot_t;

#if defined(CONFIG_SYNO_LSP_ALPINE)
#define pte_val(x)	((x).pte)
#else /* CONFIG_SYNO_LSP_ALPINE */
#define pte_val(x)	(x)
#endif /* CONFIG_SYNO_LSP_ALPINE */
#define pmd_val(x)	(x)
#define pgd_val(x)	(x)
#define pgprot_val(x)	(x)

#if defined(CONFIG_SYNO_LSP_ALPINE)
#define __pte(x)	({pte_t __pte = { .pte = (x) }; __pte; })
#else /* CONFIG_SYNO_LSP_ALPINE */
#define __pte(x)	(x)
#endif /* CONFIG_SYNO_LSP_ALPINE */
#define __pmd(x)	(x)
#define __pgd(x)	(x)
#define __pgprot(x)	(x)

#endif	/* STRICT_MM_TYPECHECKS */

#endif	/* _ASM_PGTABLE_3LEVEL_TYPES_H */
