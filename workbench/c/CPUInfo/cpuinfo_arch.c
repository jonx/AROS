/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Portable (non-x86) CPUInfo backend.

    The original CPUInfo walks an x86-only cpu.resource and decodes CPUID. This
    backend instead uses the architecture-neutral processor.resource (the same
    GetCPUInfo() API ShowConfig uses) for the facts the running system can report
    (processor count, architecture, endianness, model), and on AArch64 adds the
    architectural facts that are true by the ABI this binary is built for
    (execution state, register file, address width, mandatory SIMD/FP).

    It builds and runs on any AROS target. Output goes through dos.library
    (PutStr/Printf) so the command depends only on dos.library + processor.resource
    and runs in a minimal boot (no stdc/posixc).

    Note: per-model identification (implementer/part/revision via MIDR_EL1 and the
    ID_AA64* feature registers) is EL1-only. On the hosted AROS port the kernel
    runs as an EL0 process, so those registers are not readable here and the
    generic processor.resource has no AArch64 probe backend yet; model/vendor and
    the feature bits therefore read "Unknown". A native AArch64 processor.resource
    backend would fill them in with no change to this command.
*/

#include "cpuinfo.h"

#include <resources/processor.h>
#include <proto/processor.h>

/* processor.resource base referenced by the GetCPUInfo() inline. */
APTR ProcessorBase = NULL;

static CONST_STRPTR arch_name(ULONG a)
{
    switch (a)
    {
    case PROCESSORARCH_M68K: return "M68K";
    case PROCESSORARCH_PPC:  return "PowerPC";
    case PROCESSORARCH_X86:  return "x86";
    case PROCESSORARCH_ARM:  return "ARM";
    default:                 return "Unknown";
    }
}

static CONST_STRPTR endian_name(ULONG e)
{
    switch (e)
    {
    case ENDIANNESS_LE: return "little-endian";
    case ENDIANNESS_BE: return "big-endian";
    default:            return "unknown";
    }
}

#if defined(__aarch64__)
/*
 * Ask the host OS for the real CPU identity (e.g. "Apple M1 Pro") and core
 * counts. On Apple Silicon AROS runs at EL0, where MIDR_EL1 / the ID_AA64*
 * registers are not readable, so querying the host is the only way to learn the
 * part. This path is inert on every other platform: it goes through
 * hostlib.resource (absent on native AROS), dlopens libSystem.dylib (absent on
 * non-darwin hosts) and calls sysctlbyname (absent in e.g. glibc) -- any link in
 * that chain that is missing makes it return without printing anything.
 *
 * Why this lives in CPUInfo and not in processor.resource:
 *   The architecturally "proper" home would be a darwin processor.resource
 *   backend (cf. arch/all-linux/processor/, which reads /proc/cpuinfo via the
 *   host libc), so that GetCPUInfo() -- and hence ShowConfig and every other
 *   caller -- would see the real model too. But the generic rom/processor only
 *   serves GCIT_ModelString as the constant "Unknown"; surfacing a host-derived
 *   model through it means adding a model-override hook to rom/processor
 *   (getcpuinfo.c + processor_intern.h) AND a new arch-specific module wired into
 *   the kernel-processor build -- a cross-cutting change to a core resource.
 *   Doing the host query here keeps it self-contained, fully gated, and
 *   verifiable in isolation, and puts the answer exactly where a user looks for
 *   it. The processor.resource block above still prints what the portable API
 *   actually reports ("Model: Unknown"), so the two layers stay honest; if a
 *   darwin processor.resource backend is added later, that line fills in and this
 *   block can move into it unchanged.
 *
 * darwin/AArch64 size_t is 64-bit, matching IPTR. The hw.* integer sysctls are
 * 32-bit ints, matching AROS LONG; a width mismatch just makes the call fail and
 * the value is skipped.
 */
#include <proto/hostlib.h>

typedef int (*sysctlbyname_t)(const char *, void *, IPTR *, const void *, IPTR);

static void print_host_cpu_info(void)
{
    APTR           HostLibBase;
    void          *libc;
    sysctlbyname_t sc;
    char           brand[128];
    LONG           phys = 0, logical = 0, pcore = 0, ecore = 0;
    BOOL           haveBrand = FALSE, havePhys = FALSE, haveLog = FALSE, havePE = FALSE;
    IPTR           len;

    HostLibBase = OpenResource("hostlib.resource");
    if (!HostLibBase)                       /* not a hosted AROS */
        return;

    libc = HostLib_Open("libSystem.dylib", NULL);
    if (!libc)                              /* not a darwin host */
        return;

    sc = (sysctlbyname_t)HostLib_GetPointer(libc, "sysctlbyname", NULL);
    if (sc)
    {
        LONG p = 0, e = 0;

        brand[0] = '\0';

        /* Gather everything under one host lock, then print (no AROS I/O while
           the host lock is held). */
        HostLib_Lock();

        len = sizeof(brand);
        if (sc("machdep.cpu.brand_string", brand, &len, NULL, 0) == 0 && brand[0])
            haveBrand = TRUE;

        len = sizeof(LONG);
        if (sc("hw.physicalcpu", &phys, &len, NULL, 0) == 0)
            havePhys = TRUE;

        len = sizeof(LONG);
        if (sc("hw.logicalcpu", &logical, &len, NULL, 0) == 0)
            haveLog = TRUE;

        len = sizeof(LONG);
        if (sc("hw.perflevel0.physicalcpu", &p, &len, NULL, 0) == 0)
        {
            pcore = p;
            len = sizeof(LONG);
            if (sc("hw.perflevel1.physicalcpu", &e, &len, NULL, 0) == 0)
                ecore = e;
            havePE = TRUE;
        }

        HostLib_Unlock();
    }

    HostLib_Close(libc, NULL);

    if (haveBrand || havePhys)
    {
        PutStr("  Host (macOS, via sysctl) reports:\n");
        if (haveBrand)
            Printf("    CPU model    : %s\n", (IPTR)brand);
        if (havePhys)
        {
            if (havePE && pcore && ecore)
                Printf("    Physical CPUs: %ld (%ld performance + %ld efficiency)\n",
                       (LONG)phys, (LONG)pcore, (LONG)ecore);
            else
                Printf("    Physical CPUs: %ld\n", (LONG)phys);
        }
        if (haveLog)
            Printf("    Logical CPUs : %ld\n", (LONG)logical);
    }
}
#endif /* __aarch64__ */

int cpuinfo_print(BOOL verbose)
{
    ULONG count = 1, i;

    PutStr(APPNAME " - CPU Information tool v" VERSSTRING ".\n");
    PutStr("(C) Copyright the AROS Dev Team.\n");
    PutStr("-------------------------------------\n\n");

    ProcessorBase = OpenResource(PROCESSORNAME);
    if (!ProcessorBase)
    {
        PutStr("ERROR: Couldn't open " PROCESSORNAME ".\n");
        return RETURN_FAIL;
    }

    {
        struct TagItem ct[] =
        {
            { GCIT_NumberOfProcessors, (IPTR)&count },
            { TAG_DONE,                TAG_DONE     }
        };
        GetCPUInfo(ct);
    }

    if (count < 1)
        count = 1;
    Printf("%lu processor%s present in this system.\n\n",
           (ULONG)count, (count == 1) ? (IPTR)"" : (IPTR)"s");

    for (i = 0; i < count; i++)
    {
        ULONG        architecture = PROCESSORARCH_UNKNOWN;
        ULONG        endianness   = ENDIANNESS_UNKNOWN;
        CONST_STRPTR model        = NULL;

        struct TagItem pt[] =
        {
            { GCIT_SelectedProcessor, (IPTR)i           },
            { GCIT_ModelString,       (IPTR)&model       },
            { GCIT_Architecture,      (IPTR)&architecture},
            { GCIT_Endianness,        (IPTR)&endianness  },
            { TAG_DONE,               TAG_DONE           }
        };
        GetCPUInfo(pt);

        if (!model)
            model = "Unknown";

        Printf("PROCESSOR %lu:\n", (ULONG)(i + 1));
        PutStr("  As reported by processor.resource:\n");
        Printf("    Architecture : %s\n", (IPTR)arch_name(architecture));
        Printf("    Endianness   : %s\n", (IPTR)endian_name(endianness));
        Printf("    Model        : %s\n", (IPTR)model);

#if defined(__aarch64__)
        PutStr("  AArch64 (architecture facts for this build):\n");
        PutStr("    Execution state : AArch64 (ARMv8-A, 64-bit)\n");
        PutStr("    Endianness      : ");
        PutStr((__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__) ? "big-endian\n" : "little-endian\n");
        Printf("    Address width   : %lu-bit\n", (ULONG)(sizeof(void *) * 8));
        PutStr("    Integer regs    : 31 x 64-bit (X0-X30) + SP, PC\n");
        PutStr("    SIMD/FP regs    : 32 x 128-bit (V0-V31)\n");
        PutStr("    Advanced SIMD/FP: present (NEON; required by the AROS AArch64 ABI)\n");

        /* The real silicon identity comes from the host (EL0 can't read the CPU
           ID registers); inert on non-darwin / native AROS. Host info is
           system-wide, so emit it once. */
        if (i == 0)
            print_host_cpu_info();

        if (verbose)
        {
            PutStr("  Notes:\n");
            PutStr("    Per-model identification (implementer/part/revision from MIDR_EL1\n");
            PutStr("    and the ID_AA64* feature registers) is EL1-only and is not\n");
            PutStr("    readable from the hosted AROS process (EL0); a native AArch64\n");
            PutStr("    processor.resource backend would supply it.\n");
        }
#else
        if (verbose)
            PutStr("  (No architecture-specific detail for this target.)\n");
#endif
        PutStr("\n");
    }

    return RETURN_OK;
}
