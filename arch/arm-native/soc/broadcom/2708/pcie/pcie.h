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

/* Byte order the inbound window presents to a bus master. */
#define PCIE_RC_CFG_VENDOR_SPECIFIC_REG1        0x0188
#define VENDOR_SPECIFIC_REG1_ENDIAN_BAR2_MASK   0xc
#define VENDOR_SPECIFIC_REG1_LITTLE_ENDIAN      0x0
#define PCIE_RC_CFG_PRIV1_LINK_CAP      0x04dc
#define LINK_CAP_ASPM_SUPPORT_MASK      0xc00

/* MSI block, masked off since legacy interrupts are used */
#define PCIE_MSI_INTR2_BASE             0x4500
#define PCIE_MSI_INTR2_CLR              (PCIE_MSI_INTR2_BASE + 0x08)
#define PCIE_MSI_INTR2_MASK_SET         (PCIE_MSI_INTR2_BASE + 0x10)

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

/* Configuration header command register */
#define PCI_CMD                         0x04

/* The attached USB controller reports the firmware it is running here;
   zero means it has none and will execute no command. */
#define VL805_CFG_FWVERSION             0x50
#define PCI_CMD_MEMORY                  (1 << 1)
#define PCI_CMD_MASTER                  (1 << 2)
#define PCI_CMD_INTX_DISABLE            (1 << 10)

/* Legacy interrupt block. The four INTx lines are masked out of reset and
 * reach the interrupt controller only once unmasked here. */
#define PCIE_INTR2_CPU_STATUS           0x4300
#define PCIE_INTR2_CPU_CLR              0x4308
#define PCIE_INTR2_CPU_MASK_STATUS      0x430c
#define PCIE_INTR2_CPU_MASK_SET         0x4310
#define PCIE_INTR2_CPU_MASK_CLR         0x4314
#define PCIE_INTR2_INTX_MASK            0xf

#define PCIE_EXT_CFG_DATA               0x8000
#define PCIE_EXT_CFG_INDEX              0x9000
#define PCIE_RGR1_SW_INIT_1             0x9210

/* MISC_CTRL bits */
#define MISC_CTRL_RCB_64B_MODE          (1 << 7)
#define MISC_CTRL_RCB_MPS_MODE          (1 << 10)
#define MISC_CTRL_SCB_ACCESS_EN         (1 << 12)
#define MISC_CTRL_CFG_READ_UR_MODE      (1 << 13)
#define MISC_CTRL_MAX_BURST_SIZE_128    (0 << 20)
#define MISC_CTRL_SCB0_SIZE(exp)        (((exp) - 15) << 27)   /* log2 bytes */

/* RC_BAR config: size exponent in the low 5 bits (log2 bytes - 15) */
#define RC_BAR_SIZE(exp)                ((exp) - 15)

/* The SoC lets a bus master reach the low 3GB; the inbound window that
   covers it is the next power of two up. */
#define BCM2711_DMA_EXP                 32

/* PCIE_STATUS bits */
#define STATUS_PHYLINKUP                (1 << 4)
#define STATUS_DL_ACTIVE                (1 << 5)

/* HARD_DEBUG bits */
#define HARD_DEBUG_SERDES_IDDQ          (1 << 27)
#define HARD_DEBUG_CLKREQ_DEBUG_EN      (1 << 1)

/* RGR1_SW_INIT_1 bits. While SW_INIT is asserted the bridge core is held
 * in reset and every register outside this block raises a bus error. */
#define RGR1_PERST                      (1 << 0)
#define RGR1_SW_INIT                    (1 << 1)

/* External config index encoding */
#define EXT_CFG_ADDR(bus, dev, func)    (((bus) << 20) | ((dev) << 15) | ((func) << 12))

/* Legacy INTA of the single root port arrives at GIC INTID 175 (SPI 143). */
#define BCM2711_PCIE_INTA               175

/* Uncached DMA memory allocator: page-granular bitmap over the block the
 * bootstrap reserved. xHCI controllers may use a 64KB page size and ask
 * for scratchpad areas of a megabyte or more, so the block is sized in
 * tens of megabytes and a single allocation is not artificially capped. */
#define UCMEM_PAGESIZE                  4096
#define UCMEM_MAXPAGES                  4096    /* 16MB */
#define UCMEM_CONT                      0xffff  /* continuation slot marker */

struct pci_staticdata {
    OOP_AttrBase    hiddPCIDriverAB;
    OOP_AttrBase    hiddAB;
    OOP_Class       *driverClass;

    volatile uint8_t *regs;
    BOOL            preinitialised;     /* firmware left the link trained */

    /*
     * What a bus master must add to a system address to reach it. The
     * firmware picks this to suit the memory fitted and publishes it in
     * the device tree, so it is read at run time rather than assumed.
     */
    uint64_t        dma_offset;

    /* Uncached DMA memory block */
    uintptr_t       ucmem_base;
    uintptr_t       ucmem_size;
    uint32_t        ucmem_pages;
    uint16_t        ucmem_used[UCMEM_MAXPAGES];  /* pages in an allocation, at its head */
};

struct pcibcm2711base {
    struct Library          LibNode;
    struct pci_staticdata   psd;
};

#define BASE(lib) ((struct pcibcm2711base *)(lib))
#define PSD(cl)   (&((struct pcibcm2711base *)cl->UserData)->psd)

#endif
