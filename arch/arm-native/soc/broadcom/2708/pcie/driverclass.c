/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: BCM2711 PCIe host bridge driver.
*/

#define __OOP_NOATTRBASES__

#include <exec/types.h>
#include <hidd/pci.h>
#include <oop/oop.h>

#include <utility/tagitem.h>

#include <proto/exec.h>
#include <proto/utility.h>
#include <proto/oop.h>

#include <aros/symbolsets.h>

#include <string.h>

#include "pcie.h"

#define DEBUG 1
#include <aros/debug.h>

#undef HiddPCIDriverAttrBase
#undef HiddAttrBase

#define HiddPCIDriverAttrBase   (PSD(cl)->hiddPCIDriverAB)
#define HiddAttrBase            (PSD(cl)->hiddAB)

static inline uint32_t rd32(struct pci_staticdata *psd, uint32_t reg)
{
    return *(volatile uint32_t *)(psd->regs + reg);
}

static inline void wr32(struct pci_staticdata *psd, uint32_t reg, uint32_t val)
{
    *(volatile uint32_t *)(psd->regs + reg) = val;
}

OOP_Object *PCIBcm2711__Root__New(OOP_Class *cl, OOP_Object *o, struct pRoot_New *msg)
{
    struct pRoot_New mymsg;

    struct TagItem mytags[] = {
        { aHidd_Name, (IPTR)"PCINative" },
        { aHidd_HardwareName, (IPTR)"BCM2711 PCIe host bridge" },
        { TAG_DONE, 0 }
    };

    mymsg.mID = msg->mID;
    mymsg.attrList = (struct TagItem *)&mytags;

    if (msg->attrList)
    {
        mytags[2].ti_Tag = TAG_MORE;
        mytags[2].ti_Data = (IPTR)msg->attrList;
    }

    msg = &mymsg;

    return (OOP_Object *)OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
}

/*
 * Configuration space access. The root complex header is memory mapped
 * at offset 0 of the register block; everything on the external bus is
 * reached through the indexed window. The link is a single point to
 * point lane, so only device 0 exists on the external bus.
 */
static ULONG ReadConfigLong(struct pci_staticdata *psd, UBYTE bus, UBYTE dev, UBYTE sub, UWORD reg)
{
    ULONG val = 0xffffffff;

    if (bus == 0)
    {
        if (dev == 0 && sub == 0)
            val = rd32(psd, reg & 0xffc);
    }
    else if (dev == 0)
    {
        Disable();
        wr32(psd, PCIE_EXT_CFG_INDEX, EXT_CFG_ADDR(bus, dev, sub));
        val = rd32(psd, PCIE_EXT_CFG_DATA + (reg & 0xffc));
        Enable();
    }

    return val;
}

static void WriteConfigLong(struct pci_staticdata *psd, UBYTE bus, UBYTE dev, UBYTE sub, UWORD reg, ULONG val)
{
    if (bus == 0)
    {
        if (dev == 0 && sub == 0)
            wr32(psd, reg & 0xffc, val);
    }
    else if (dev == 0)
    {
        /* The endpoint runs no code until its driver enables it; that
           enable is the agreed moment to have its firmware loaded. */
        if ((reg & 0xffc) == PCI_CMD && (val & (PCI_CMD_MEMORY | PCI_CMD_MASTER)))
            EnsureEndpointFirmware(psd);

        Disable();
        wr32(psd, PCIE_EXT_CFG_INDEX, EXT_CFG_ADDR(bus, dev, sub));
        wr32(psd, PCIE_EXT_CFG_DATA + (reg & 0xffc), val);
        Enable();
    }
}

ULONG PCIBcm2711__Hidd_PCIDriver__ReadConfigLong(OOP_Class *cl, OOP_Object *o,
    struct pHidd_PCIDriver_ReadConfigLong *msg)
{
    return ReadConfigLong(PSD(cl), msg->bus, msg->dev, msg->sub, msg->reg);
}

UWORD PCIBcm2711__Hidd_PCIDriver__ReadConfigWord(OOP_Class *cl, OOP_Object *o,
    struct pHidd_PCIDriver_ReadConfigWord *msg)
{
    ULONG val = ReadConfigLong(PSD(cl), msg->bus, msg->dev, msg->sub, msg->reg);

    return (val >> ((msg->reg & 2) * 8)) & 0xffff;
}

UBYTE PCIBcm2711__Hidd_PCIDriver__ReadConfigByte(OOP_Class *cl, OOP_Object *o,
    struct pHidd_PCIDriver_ReadConfigByte *msg)
{
    ULONG val = ReadConfigLong(PSD(cl), msg->bus, msg->dev, msg->sub, msg->reg);

    return (val >> ((msg->reg & 3) * 8)) & 0xff;
}

void PCIBcm2711__Hidd_PCIDriver__WriteConfigLong(OOP_Class *cl, OOP_Object *o,
    struct pHidd_PCIDriver_WriteConfigLong *msg)
{
    WriteConfigLong(PSD(cl), msg->bus, msg->dev, msg->sub, msg->reg, msg->val);
}

void PCIBcm2711__Hidd_PCIDriver__WriteConfigWord(OOP_Class *cl, OOP_Object *o,
    struct pHidd_PCIDriver_WriteConfigWord *msg)
{
    ULONG val = ReadConfigLong(PSD(cl), msg->bus, msg->dev, msg->sub, msg->reg);
    ULONG shift = (msg->reg & 2) * 8;

    val = (val & ~(0xffffUL << shift)) | ((ULONG)msg->val << shift);
    WriteConfigLong(PSD(cl), msg->bus, msg->dev, msg->sub, msg->reg, val);
}

void PCIBcm2711__Hidd_PCIDriver__WriteConfigByte(OOP_Class *cl, OOP_Object *o,
    struct pHidd_PCIDriver_WriteConfigByte *msg)
{
    ULONG val = ReadConfigLong(PSD(cl), msg->bus, msg->dev, msg->sub, msg->reg);
    ULONG shift = (msg->reg & 3) * 8;

    val = (val & ~(0xffUL << shift)) | ((ULONG)msg->val << shift);
    WriteConfigLong(PSD(cl), msg->bus, msg->dev, msg->sub, msg->reg, val);
}

/*
 * A bus master reaches system memory through the inbound window, which
 * the firmware does not place at address zero: what the CPU calls
 * address X, the bus calls X plus this offset. Drivers ask for the
 * translation through these two methods.
 */
APTR PCIBcm2711__Hidd_PCIDriver__CPUtoPCI(OOP_Class *cl, OOP_Object *o,
    struct pHidd_PCIDriver_CPUtoPCI *msg)
{
    return (APTR)((uintptr_t)msg->address + PSD(cl)->dma_offset);
}

APTR PCIBcm2711__Hidd_PCIDriver__PCItoCPU(OOP_Class *cl, OOP_Object *o,
    struct pHidd_PCIDriver_PCItoCPU *msg)
{
    return (APTR)((uintptr_t)msg->address - PSD(cl)->dma_offset);
}

/*
 * BAR addresses live on the PCI bus at BCM2711_PCIE_PCI_WIN; the CPU
 * reaches them through the outbound window. The window is already
 * mapped by the bootstrap.
 */
void *PCIBcm2711__Hidd_PCIDriver__MapPCI(OOP_Class *cl, OOP_Object *o,
    struct pHidd_PCIDriver_MapPCI *msg)
{
    uintptr_t addr = (uintptr_t)msg->PCIAddress;

    if (addr >= BCM2711_PCIE_PCI_WIN &&
        addr - BCM2711_PCIE_PCI_WIN < BCM2711_PCIE_WIN_SIZE)
        return (void *)(BCM2711_PCIE_CPU_WIN + (addr - BCM2711_PCIE_PCI_WIN));

    return (void *)addr;
}

/*
 * DMA descriptor memory. PCIe bus masters are not cache coherent on
 * this SoC and the kernel cannot change page attributes at runtime, so
 * allocations are served from the uncached block the bootstrap set
 * aside. Page granular first-fit; the page count of an allocation is
 * kept at its head slot.
 */
APTR PCIBcm2711__Hidd_PCIDriver__AllocPCIMem(OOP_Class *cl, OOP_Object *o,
    struct pHidd_PCIDriver_AllocPCIMem *msg)
{
    struct pci_staticdata *psd = PSD(cl);
    uint32_t pages = (msg->Size + UCMEM_PAGESIZE - 1) / UCMEM_PAGESIZE;
    APTR result = NULL;

    if (pages == 0 || pages >= UCMEM_CONT || psd->ucmem_base == 0)
    {
        bug("[PCIBcm2711] AllocPCIMem(%u): bad request\n", (unsigned int)msg->Size);
        return NULL;
    }

    Disable();
    for (uint32_t i = 0; i + pages <= psd->ucmem_pages; i++)
    {
        uint32_t j;

        for (j = 0; j < pages; j++)
            if (psd->ucmem_used[i + j])
                break;

        if (j == pages)
        {
            psd->ucmem_used[i] = pages;
            for (j = 1; j < pages; j++)
                psd->ucmem_used[i + j] = UCMEM_CONT;
            result = (APTR)(psd->ucmem_base + (uintptr_t)i * UCMEM_PAGESIZE);
            break;
        }

        if (psd->ucmem_used[i + j])
            i += j; /* skip the rest of that allocation */
    }
    Enable();

    if (result)
        memset(result, 0, (size_t)pages * UCMEM_PAGESIZE);
    else
        bug("[PCIBcm2711] AllocPCIMem(%u): out of uncached memory\n",
            (unsigned int)msg->Size);

    D(bug("[PCIBcm2711] AllocPCIMem(%u) = %p\n", (unsigned int)msg->Size, result));

    return result;
}

VOID PCIBcm2711__Hidd_PCIDriver__FreePCIMem(OOP_Class *cl, OOP_Object *o,
    struct pHidd_PCIDriver_FreePCIMem *msg)
{
    struct pci_staticdata *psd = PSD(cl);
    uintptr_t addr = (uintptr_t)msg->Address;

    if (addr < psd->ucmem_base || addr >= psd->ucmem_base + psd->ucmem_size)
        return;

    uint32_t idx = (addr - psd->ucmem_base) / UCMEM_PAGESIZE;
    uint32_t pages = psd->ucmem_used[idx];

    if (pages == 0 || pages == UCMEM_CONT)
        return; /* not the head of an allocation */

    Disable();
    for (uint32_t j = 0; j < pages; j++)
        psd->ucmem_used[idx + j] = 0;
    Enable();
}
