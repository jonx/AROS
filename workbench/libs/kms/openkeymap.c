
#include <aros/debug.h>

#include <proto/dos.h>
#include <proto/exec.h>

#include <string.h>

#include "kms_intern.h"
#include "kms_akmd.h"

/*****************************************************************************

    NAME */
#include <proto/kms.h>

	AROS_LH1(struct KeyMapNode *, OpenKeymap,

/*  SYNOPSIS */
	AROS_LHA(STRPTR, name, A0),

/*  LOCATION */
	struct KMSLibrary *, KMSBase, 5, Kms)

/*  FUNCTION
	Open a keymap by name.

    INPUTS
	name - Keymap name. Can be a full pathname or just a name.
	       In the latter case DEVS:Keymaps is assumed to be a
	       path to the file.

    RESULT
    	A pointer to the loaded keymap.

    NOTES
	This function automatically keeps track of loaded keymaps
	via keymap.resource. No more than a single copy of the keymap
	will be loaded.

	On 64-bit little-endian systems, this function automatically
	converts 68K hunk-format keymaps to native format. See
	parsekeymapseg.c for implementation details (GitHub issue #74).

	If no usable compiled keymap is found (no file, a non-hunk
	seglist, or a hunk file the running system cannot convert, e.g.
	when the keymap loads above 4GB), this function falls back to
	parsing the plain-text descriptor <name>.akmd from the same
	directory and building a native keymap from it at runtime.

    EXAMPLE

    BUGS

    SEE ALSO

    INTERNALS
	Uses parsekeymapseg() to convert hunk-format seglists on
	non-big-endian or non-32-bit systems.

    HISTORY

*****************************************************************************/	
{
    AROS_LIBFUNC_INIT

    struct KeyMapResource *kmr = ((struct kms_base *)KMSBase)->kmr;
    struct KeyMapNode *kmn = NULL, *kmn2 = NULL;
    ULONG buflen;
    STRPTR km_name, fullname;
    BPTR km_seg;
    IPTR hunkinfo = 0;
    struct TagItem segtags[2] =
    {
        { GSLI_68KHUNK, (IPTR)&hunkinfo },
        { TAG_DONE,     0               }
    };
    BOOL ishunk = FALSE, isshort;

    km_name  = FilePart(name);
    isshort  = (km_name == name);
    if (isshort)
    {
        if (kmr)
        {
            /*
             * A short name was given.
             * Check if the keymap is already resident.
             * Unfortunately we still have to use Forbid()/Permit() locking
             * because there can be lots of software which does the same.
             * AmigaOS(tm) never provided a centralized semaphore for this.
             */
            Forbid();
            kmn = (struct KeyMapNode *)FindName(&kmr->kr_List, name);
            Permit();
        }

	/* If found, return it */
	if (kmn)
	    return kmn;
    }

    /* Resolve the full path (room for the ".akmd" fallback suffix too) */
    buflen = strlen(name) + PREFIX_LEN + 6;
    fullname = AllocMem(buflen, MEMF_ANY);
    if (!fullname)
	return NULL;

    if (isshort)
    {
        strcpy(fullname, PREFIX_STR);
        AddPart(fullname, km_name, buflen);
    }
    else
        strcpy(fullname, name);

    km_seg = LoadSeg(fullname);
    if (km_seg)
    {
        D(bug("[KMS] %s: loaded seglist @ 0x%p\n", __func__, km_seg);)

        if (GetSegListInfo(km_seg, segtags))
        {
            D(bug("[KMS] %s: hunkinfo == 0x%p\n", __func__, hunkinfo);)
            if (hunkinfo)
                ishunk = TRUE;
        }

        if (!ishunk)
        {
            /* Not a keymap we can use as-is; try the .akmd fallback below */
            UnLoadSeg(km_seg);
            km_seg = BNULL;
        }
#if !AROS_BIG_ENDIAN || (__WORDSIZE != 32)
        else
        {
            /* Returns BNULL when the conversion cannot be done (e.g. the
               seglist loaded above 4GB); fall back to the text descriptor */
            km_seg = parsekeymapseg(km_seg);
        }
#endif

        if (km_seg)
        {
            D(bug("[KMS] %s: using seglist @ 0x%p\n", __func__, km_seg);)
            kmn = BADDR(km_seg) + sizeof(BPTR);
        }
    }

    if (!kmn)
    {
        /* Fall back to the plain-text .akmd descriptor */
        ULONG l = strlen(fullname);

        if (l < 5 || strcmp(fullname + l - 5, ".akmd") != 0)
            strcat(fullname, ".akmd");

        kmn = kms_LoadAkmdKeymap((struct kms_base *)KMSBase, fullname,
                                 isshort ? km_name : NULL);
        D(bug("[KMS] %s: .akmd fallback (%s) -> 0x%p\n", __func__, fullname, kmn);)
    }

    FreeMem(fullname, buflen);

    if (!kmn)
	return NULL;

    if (kmr)
    {
        Forbid();

        /*
         * Check if this keymap is already loaded once more before installing it.
         * Now use name contained in the KeyMapNode instead of user-supplied one.
         * This is needed for two cases:
         * 1. Several programs running concurrently tried to load and install
         *    the same keymap (rare but theoretically possible case).
         * 2. We are given full file path. We need to load the file and take
         *    keymap name from it.
         */
        kmn2 = (struct KeyMapNode *)FindName(&kmr->kr_List, kmn->kn_Node.ln_Name);
        if (!kmn2)
            Enqueue(&kmr->kr_List, &kmn->kn_Node);

        Permit();
    }

    /* If the keymap was already loaded, use the resident copy and drop our one */
    if (kmn2)
    {
	if (km_seg)
	    UnLoadSeg(km_seg);
	else
	    FreeVec(kmn);           /* the .akmd-built block */
	kmn = kmn2;
    }

    return kmn;

    AROS_LIBFUNC_EXIT
}
