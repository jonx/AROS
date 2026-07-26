/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
*/

#ifndef PCIE_BCM2711_H
#define PCIE_BCM2711_H

#include <inttypes.h>
#include <exec/types.h>
#include <exec/libraries.h>
#include <oop/oop.h>

#include LC_LIBDEFS_FILE

/*
 * BCM2711 PCIe host bridge (single root port, gen2 x1). The register
 * block sits outside the main peripheral window, at a fixed place in
 * the SoC address map.
 */
#define BCM2711_PCIE_REG_BASE           0xFD500000UL
#define BCM2711_PCIE_REG_SIZE           0x9310

/* CPU-side outbound window (fixed in the SoC address map) and the PCI
 * bus address programmed into it. */
#define BCM2711_PCIE_CPU_WIN            0x600000000ULL
#define BCM2711_PCIE_PCI_WIN            0xC0000000UL
#define BCM2711_PCIE_WIN_SIZE           0x04000000UL    /* 64MB */

/* The root complex configuration header is memory mapped at offset 0;
 * external configuration space is reached through an indexed window. */
#define PCIE_RC_CFG_PRIV1_ID_VAL3       0x043c  /* class code override */
#define PCIE_RC_CFG_PRIV1_LINK_CAP      0x04dc

#define PCIE_MISC_MISC_CTRL             0x4008
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LO 0x400c
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_HI 0x4010
#define PCIE_MISC_RC_BAR1_CONFIG_LO     0x402c
#define PCIE_MISC_RC_BAR2_CONFIG_LO     0x4034
#define PCIE_MISC_RC_BAR2_CONFIG_HI     0x4038
#define PCIE_MISC_RC_BAR3_CONFIG_LO     0x403c
#define PCIE_MISC_PCIE_CTRL             0x4064
#define PCIE_MISC_PCIE_STATUS           0x4068
#define PCIE_MISC_REVISION              0x406c
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT 0x4070
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI    0x4080
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI   0x4084
#define PCIE_MISC_HARD_PCIE_HARD_DEBUG  0x4204

#define PCIE_EXT_CFG_DATA               0x8000
#define PCIE_EXT_CFG_INDEX              0x9000
#define PCIE_RGR1_SW_INIT_1             0x9210

/* MISC_CTRL bits */
#define MISC_CTRL_SCB_ACCESS_EN         (1 << 12)
#define MISC_CTRL_CFG_READ_UR_MODE      (1 << 13)
#define MISC_CTRL_MAX_BURST_SIZE_128    (0 << 20)
#define MISC_CTRL_SCB0_SIZE(exp)        (((exp) - 15) << 27)   /* log2 bytes */

/* RC_BAR config: size exponent in the low 5 bits (log2 bytes - 15) */
#define RC_BAR_SIZE(exp)                ((exp) - 15)

/* PCIE_STATUS bits */
#define STATUS_PHYLINKUP                (1 << 4)
#define STATUS_DL_ACTIVE                (1 << 5)

/* HARD_DEBUG bits */
#define HARD_DEBUG_SERDES_IDDQ          (1 << 27)

/* RGR1_SW_INIT_1 bits. While SW_INIT is asserted the bridge core is held
 * in reset and every register outside this block raises a bus error. */
#define RGR1_PERST                      (1 << 0)
#define RGR1_SW_INIT                    (1 << 1)

/* External config index encoding */
#define EXT_CFG_ADDR(bus, dev, func)    (((bus) << 20) | ((dev) << 15) | ((func) << 12))

/* Legacy INTA of the single root port arrives at GIC INTID 175 (SPI 143). */
#define BCM2711_PCIE_INTA               175

/* Uncached DMA memory allocator: page-granular bitmap over the block the
 * bootstrap reserved. */
#define UCMEM_PAGESIZE                  4096
#define UCMEM_MAXPAGES                  512     /* 2MB */

struct pci_staticdata {
    OOP_AttrBase    hiddPCIDriverAB;
    OOP_AttrBase    hiddAB;
    OOP_Class       *driverClass;

    volatile uint8_t *regs;

    /* Uncached DMA memory block */
    uintptr_t       ucmem_base;
    uintptr_t       ucmem_size;
    uint32_t        ucmem_pages;
    uint8_t         ucmem_used[UCMEM_MAXPAGES];  /* pages in an allocation, at its head */
};

struct pcibcm2711base {
    struct Library          LibNode;
    struct pci_staticdata   psd;
};

#define BASE(lib) ((struct pcibcm2711base *)(lib))
#define PSD(cl)   (&((struct pcibcm2711base *)cl->UserData)->psd)

#endif
