/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: BCM2711 PCIe host bridge bring-up and driver registration.
*/

#define __OOP_NOATTRBASES__

#include <aros/symbolsets.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <hidd/pci.h>
#include <oop/oop.h>

#include <proto/exec.h>
#include <proto/oop.h>
#include <proto/mbox.h>
#include <proto/openfirmware.h>
#include <proto/bootloader.h>

#include <aros/bootloader.h>

#define __NOLIBBASE__
#include <proto/kernel.h>

IPTR __arm_periiobase __attribute__((used)) = 0;
#define ARM_PERIIOBASE __arm_periiobase

#include <hardware/bcm2708.h>
#include <hardware/videocore.h>

#include <aros/macros.h>

#include <string.h>

#include "pcie.h"

#define DEBUG 1
#include <aros/debug.h>

APTR MBoxBase;
void *OpenFirmwareBase;
APTR BootLoaderBase;

/* "PCIE=disable" on the kernel command line keeps the bridge untouched. */
static BOOL PCIeEnabled(void)
{
    struct List *list;
    struct Node *node;

    BootLoaderBase = OpenResource("bootloader.resource");
    if (!BootLoaderBase)
        return TRUE;

    list = (struct List *)GetBootInfo(BL_Args);
    if (!list)
        return TRUE;

    ForeachNode(list, node)
    {
        if (strncmp(node->ln_Name, "PCIE=", 5) == 0 &&
            strstr(&node->ln_Name[5], "disable"))
        {
            D(bug("[PCIBcm2711] disabled on the command line\n"));
            return FALSE;
        }
    }

    return TRUE;
}

/*
 * The bridge registers raise an external abort when the block is absent
 * (emulators mark the device tree node disabled instead of modelling
 * it), so consult the tree before the first register access.
 */
static BOOL PCIeNodeUsable(void)
{
    void *key, *prop;
    const char *val;

    OpenFirmwareBase = OpenResource("openfirmware.resource");
    if (!OpenFirmwareBase)
        return FALSE;

    key = OF_OpenKey("/scb/pcie@7d500000");
    if (!key)
    {
        D(bug("[PCIBcm2711] no PCIe node in the device tree\n"));
        return FALSE;
    }

    /* OF_OpenKey falls back to the last resolved node; make sure this
     * really is the PCIe controller. */
    prop = OF_FindProperty(key, "compatible");
    if (!prop)
    {
        D(bug("[PCIBcm2711] PCIe node has no compatible property\n"));
        return FALSE;
    }
    val = OF_GetPropValue(prop);
    if (!val || !strstr(val, "2711-pcie"))
    {
        D(bug("[PCIBcm2711] node is not the PCIe controller (%s)\n", val ? val : "?"));
        return FALSE;
    }

    prop = OF_FindProperty(key, "status");
    if (prop)
    {
        val = OF_GetPropValue(prop);
        if (val && strcmp(val, "okay") != 0)
        {
            D(bug("[PCIBcm2711] node status '%s', leaving the bridge alone\n", val));
            return FALSE;
        }
    }

    return TRUE;
}

static inline uint32_t rd32(volatile uint8_t *regs, uint32_t reg)
{
    return *(volatile uint32_t *)(regs + reg);
}

static inline void wr32(volatile uint8_t *regs, uint32_t reg, uint32_t val)
{
    *(volatile uint32_t *)(regs + reg) = val;
}

/* Busy wait on the generic counter; init runs before timer.device. */
static void delay_us(uint32_t us)
{
    uint64_t freq, now, end;

    asm volatile("mrs %0, CNTFRQ_EL0" : "=r"(freq));
    asm volatile("mrs %0, CNTPCT_EL0" : "=r"(now));
    end = now + (freq * us) / 1000000;

    do {
        asm volatile("mrs %0, CNTPCT_EL0" : "=r"(now));
    } while (now < end);
}

/*
 * Ask the firmware to load the VL805 xHCI controller firmware. On
 * boards whose bootloader EEPROM already carries it this returns
 * without effect. The argument encodes the controller's PCI address.
 */
static void NotifyXHCIReset(void)
{
    unsigned int *msg_, *msg;

    if ((MBoxBase = OpenResource("mbox.resource")) == NULL)
        return;

    msg_ = AllocMem(8 * 4 + 16, MEMF_CLEAR);
    if (!msg_)
        return;
    msg = (unsigned int *)(((uintptr_t)msg_ + 15) & ~15);

    msg[0] = AROS_LONG2LE(7 * 4);
    msg[1] = AROS_LONG2LE(VCTAG_REQ);
    msg[2] = AROS_LONG2LE(VCTAG_NOTIFYXHCI);
    msg[3] = AROS_LONG2LE(4);
    msg[4] = AROS_LONG2LE(4);
    msg[5] = AROS_LONG2LE(EXT_CFG_ADDR(1, 0, 0));
    msg[6] = 0;

    MBoxWrite((APTR)VCMB_BASE, VCMB_PROPCHAN, msg);
    if (MBoxRead((APTR)VCMB_BASE, VCMB_PROPCHAN) == msg)
        D(bug("[PCIBcm2711] VL805 firmware notify replied %08x\n", AROS_LE2LONG(msg[5])));
    else
        D(bug("[PCIBcm2711] VL805 firmware notify got no reply\n"));

    FreeMem(msg_, 8 * 4 + 16);
}

static BOOL BridgeInit(struct pci_staticdata *psd)
{
    volatile uint8_t *regs = psd->regs;
    uint32_t tmp;
    int i;

    /*
     * The register core may be held in SW_INIT out of power-on; while it
     * is, everything but the reset block answers with a bus error. Only
     * the RGR1 block may be touched before the core is released, and
     * with a plain write, not a read-modify-write.
     */
    D(bug("[PCIBcm2711] resetting the bridge\n"));
    wr32(regs, PCIE_RGR1_SW_INIT_1, RGR1_PERST | RGR1_SW_INIT);
    delay_us(200);

    /* Release the bridge core, keep PERST# asserted */
    wr32(regs, PCIE_RGR1_SW_INIT_1, RGR1_PERST);
    delay_us(200);

    D(bug("[PCIBcm2711] bridge revision %08x\n", rd32(regs, PCIE_MISC_REVISION)));

    /* Power up the PHY */
    tmp = rd32(regs, PCIE_MISC_HARD_PCIE_HARD_DEBUG);
    wr32(regs, PCIE_MISC_HARD_PCIE_HARD_DEBUG, tmp & ~HARD_DEBUG_SERDES_IDDQ);
    delay_us(200);

    /*
     * SCB/inbound setup: one region at PCI address 0 covering system
     * memory. The size is an exponent and has to describe the memory
     * that is really fitted, not the largest a controller could take,
     * or inbound addresses do not decode.
     */
    {
        uint64_t ramtop = psd->ucmem_base + psd->ucmem_size;
        unsigned int exp = 63 - __builtin_clzll(ramtop);

        if ((1ULL << exp) < ramtop)
            exp++;              /* round up to the next power of two */

        D(bug("[PCIBcm2711] system memory %uMB, inbound size exponent %u\n",
              (unsigned)(ramtop >> 20), exp));

        wr32(regs, PCIE_MISC_MISC_CTRL,
             MISC_CTRL_SCB_ACCESS_EN | MISC_CTRL_CFG_READ_UR_MODE |
             MISC_CTRL_MAX_BURST_SIZE_128 | MISC_CTRL_SCB0_SIZE(exp));

        wr32(regs, PCIE_MISC_RC_BAR2_CONFIG_LO, RC_BAR_SIZE(exp));
        wr32(regs, PCIE_MISC_RC_BAR2_CONFIG_HI, 0);
    }
    wr32(regs, PCIE_MISC_RC_BAR1_CONFIG_LO, 0);
    wr32(regs, PCIE_MISC_RC_BAR3_CONFIG_LO, 0);

    /* Outbound window: CPU 0x6_0000_0000 -> PCI 0xC0000000, 64MB */
    wr32(regs, PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LO, BCM2711_PCIE_PCI_WIN);
    wr32(regs, PCIE_MISC_CPU_2_PCIE_MEM_WIN0_HI, 0);
    {
        uint32_t base_mb  = BCM2711_PCIE_CPU_WIN >> 20;
        uint32_t limit_mb = (BCM2711_PCIE_CPU_WIN + BCM2711_PCIE_WIN_SIZE - 1) >> 20;

        wr32(regs, PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT,
             ((base_mb & 0xfff) << 4) | ((limit_mb & 0xfff) << 20));
        wr32(regs, PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI, base_mb >> 12);
        wr32(regs, PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI, limit_mb >> 12);
    }

    /* Deassert PERST# and wait for the link */
    D(bug("[PCIBcm2711] releasing PERST#\n"));
    wr32(regs, PCIE_RGR1_SW_INIT_1, 0);

    for (i = 0; i < 100; i++)
    {
        tmp = rd32(regs, PCIE_MISC_PCIE_STATUS);
        if ((tmp & (STATUS_PHYLINKUP | STATUS_DL_ACTIVE)) ==
            (STATUS_PHYLINKUP | STATUS_DL_ACTIVE))
            break;
        delay_us(5000);
    }

    if ((tmp & (STATUS_PHYLINKUP | STATUS_DL_ACTIVE)) !=
        (STATUS_PHYLINKUP | STATUS_DL_ACTIVE))
    {
        bug("[PCIBcm2711] link did not come up (status %08x)\n", tmp);
        return FALSE;
    }

    D(bug("[PCIBcm2711] link up after %dms (status %08x)\n", i * 5, tmp));

    /* Present the root port as a PCI-PCI bridge */
    wr32(regs, PCIE_RC_CFG_PRIV1_ID_VAL3, 0x060400);

    /* Bus numbers: primary 0, secondary 1, subordinate 1 */
    tmp = rd32(regs, 0x18);
    wr32(regs, 0x18, (tmp & 0xff000000) | 0x00010100);

    /* Forward the memory window through the bridge */
    wr32(regs, 0x20, ((BCM2711_PCIE_PCI_WIN + BCM2711_PCIE_WIN_SIZE - 0x100000) & 0xfff00000)
                     | (BCM2711_PCIE_PCI_WIN >> 16));

    /* Enable memory decode and bus mastering on the root port, and let it
       pass legacy interrupts up */
    tmp = rd32(regs, PCI_CMD);
    wr32(regs, PCI_CMD, (tmp | PCI_CMD_MEMORY | PCI_CMD_MASTER) & ~PCI_CMD_INTX_DISABLE);

    /* Let the legacy interrupt lines through to the interrupt controller. */
    wr32(regs, PCIE_INTR2_CPU_MASK_CLR, PCIE_INTR2_INTX_MASK);
    D(bug("[PCIBcm2711] INTx mask now %08x\n",
          rd32(regs, PCIE_INTR2_CPU_MASK_STATUS)));

    return TRUE;
}

/*
 * The endpoint has no firmware-assigned resources; place its BAR at
 * the bottom of the window, route its interrupt and enable it.
 */
static void SetupEndpoint(struct pci_staticdata *psd)
{
    volatile uint8_t *regs = psd->regs;
    uint32_t id, tmp;

    Disable();
    wr32(regs, PCIE_EXT_CFG_INDEX, EXT_CFG_ADDR(1, 0, 0));
    id = rd32(regs, PCIE_EXT_CFG_DATA + 0x00);

    if (id != 0xffffffff && id != 0)
    {
        /* 64-bit memory BAR0 */
        wr32(regs, PCIE_EXT_CFG_DATA + 0x10, BCM2711_PCIE_PCI_WIN);
        wr32(regs, PCIE_EXT_CFG_DATA + 0x14, 0);

        /* Interrupt line = GIC INTID of INTA */
        tmp = rd32(regs, PCIE_EXT_CFG_DATA + 0x3c);
        wr32(regs, PCIE_EXT_CFG_DATA + 0x3c, (tmp & 0xffffff00) | BCM2711_PCIE_INTA);

        /* Memory decode + bus mastering, and let it raise its interrupt:
           the firmware leaves legacy interrupts disabled. */
        tmp = rd32(regs, PCIE_EXT_CFG_DATA + PCI_CMD);
        tmp = (tmp | PCI_CMD_MEMORY | PCI_CMD_MASTER) & ~PCI_CMD_INTX_DISABLE;
        wr32(regs, PCIE_EXT_CFG_DATA + PCI_CMD, tmp);
        D(bug("[PCIBcm2711] endpoint command now %04x\n",
              (unsigned)(rd32(regs, PCIE_EXT_CFG_DATA + PCI_CMD) & 0xffff)));
    }
    Enable();

    D(bug("[PCIBcm2711] endpoint 1:0.0 id %08x\n", id));
}

static int PCIBcm2711_InitClass(LIBBASETYPEPTR LIBBASE)
{
    struct pci_staticdata *psd = &LIBBASE->psd;
    APTR KernelBase;
    OOP_Object *pci;

    KernelBase = OpenResource("kernel.resource");
    if (!KernelBase)
        return TRUE;

    __arm_periiobase = (IPTR)KrnGetSystemAttr(KATTR_PeripheralBase);

    /* BCM2711 only */
    if (__arm_periiobase != BCM2711_PERIIOBASE)
        return TRUE;

    if (!PCIeEnabled() || !PCIeNodeUsable())
        return TRUE;

    psd->regs = (volatile uint8_t *)BCM2711_PCIE_REG_BASE;

    psd->ucmem_base = (uintptr_t)KrnGetSystemAttr(KATTR_UncachedMemBase);
    psd->ucmem_size = (uintptr_t)KrnGetSystemAttr(KATTR_UncachedMemSize);
    psd->ucmem_pages = psd->ucmem_size / UCMEM_PAGESIZE;
    if (psd->ucmem_pages > UCMEM_MAXPAGES)
        psd->ucmem_pages = UCMEM_MAXPAGES;
    D(bug("[PCIBcm2711] uncached DMA memory: %p, %u pages\n",
          (void *)psd->ucmem_base, psd->ucmem_pages));

    if (!BridgeInit(psd))
        return TRUE;    /* no link; nothing to drive, but do not stop boot */

    NotifyXHCIReset();
    delay_us(100000);

    SetupEndpoint(psd);

    psd->hiddPCIDriverAB = OOP_ObtainAttrBase(IID_Hidd_PCIDriver);
    psd->hiddAB = OOP_ObtainAttrBase(IID_Hidd);
    if (psd->hiddPCIDriverAB == 0 || psd->hiddAB == 0)
    {
        D(bug("[PCIBcm2711] ObtainAttrBases failed\n"));
        return TRUE;
    }

    struct pHidd_PCI_AddHardwareDriver msg;

    msg.driverClass = psd->driverClass;
    msg.mID = OOP_GetMethodID(IID_Hidd_PCI, moHidd_PCI_AddHardwareDriver);

    pci = OOP_NewObject(NULL, CLID_Hidd_PCI, NULL);
    if (pci)
    {
        OOP_DoMethod(pci, (OOP_Msg)&msg);
        OOP_DisposeObject(pci);
        D(bug("[PCIBcm2711] driver registered\n"));
    }

    return TRUE;
}

static int PCIBcm2711_ExpungeClass(LIBBASETYPEPTR LIBBASE)
{
    OOP_ReleaseAttrBase(IID_Hidd_PCIDriver);
    OOP_ReleaseAttrBase(IID_Hidd);

    return TRUE;
}

ADD2INITLIB(PCIBcm2711_InitClass, 0)
ADD2EXPUNGELIB(PCIBcm2711_ExpungeClass, 0)
