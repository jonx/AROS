/* Native oracle for the emu68k OpenCatalogA crossing. Keep the three arguments
 * byte-for-byte equivalent to hosted/emu68k/nativelib/genobject.s. */

#include <exec/types.h>
#include <aros/debug.h>
#include <libraries/locale.h>
#include <utility/tagitem.h>

#include <proto/dos.h>
#include <proto/locale.h>

int main(void)
{
    static const char name[] = "System/Libs/dos.catalog";
    static const char language[] = "czech";
    struct TagItem tags[] =
    {
        { OC_Language, (IPTR)language },
        { TAG_DONE, 0 }
    };
    struct Catalog *catalog;

    bug("[T3CAT-NATIVE] before OpenCatalogA(NULL, %s, OC_Language=%s)\n",
        name, language);
    catalog = OpenCatalogA(NULL, name, tags);
    bug("[T3CAT-NATIVE] returned %s\n", catalog ? "NONNULL" : "NULL");

    Printf("[T3CAT-NATIVE] OpenCatalogA(NULL, %s, OC_Language=%s) -> %s\n",
           (IPTR)name, (IPTR)language,
           (IPTR)(catalog ? "NONNULL" : "NULL"));
    if (catalog) CloseCatalog(catalog);
    return catalog ? 0 : 5;
}
