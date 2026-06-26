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
