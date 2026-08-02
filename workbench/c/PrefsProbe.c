/* Native oracle for the emu68k GetPrefs crossing. Reads the same fields, in the
 * same order, as hosted/emu68k/nativelib/genprefs.s reads from the guest copy.
 * The two lines must be identical: what the system actually holds is irrelevant
 * to the test, which is the point of having an oracle rather than constants. */

#include <exec/types.h>
#include <intuition/preferences.h>

#include <proto/dos.h>
#include <proto/intuition.h>

int main(void)
{
    struct Preferences p;
    const UBYTE *f;
    ULONG matrix = 0;
    int i;

    GetPrefs(&p, sizeof(p));

    for (i = 0; i < (int)(sizeof(p.PointerMatrix) / sizeof(p.PointerMatrix[0])); i++)
        matrix += p.PointerMatrix[i];

    f = (const UBYTE *)p.PrinterFilename;

    Printf("[T3PREF-NATIVE] %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx\n",
           (IPTR)(ULONG)(UBYTE)p.FontHeight,
           (IPTR)(ULONG)p.KeyRptSpeed.tv_micro,
           (IPTR)(ULONG)p.DoubleClick.tv_micro,
           (IPTR)matrix,
           (IPTR)(ULONG)p.PointerTicks,
           (IPTR)(ULONG)(UWORD)p.EnableCLI,
           (IPTR)(ULONG)p.PrinterType,
           (IPTR)(((ULONG)f[0] << 24) | ((ULONG)f[1] << 16) |
                  ((ULONG)f[2] << 8) | (ULONG)f[3]),
           (IPTR)(ULONG)p.ext_size);
    return 0;
}
