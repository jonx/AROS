#ifndef DOS_EXFAT_H
#define DOS_EXFAT_H

/* Small, allocation-free content probe shared by partition and device
   automounters. This identifies which handler should receive the medium;
   it intentionally does not replace the handler's complete boot-region,
   geometry, and checksum validation. */

#define ID_EXFAT_DISK 0x46415458UL /* 'FATX' */
#define EXFAT_HANDLER_NAME "exfat-handler"

/* MBR type 0x07 and the GPT Basic Data UUID are shared by NTFS and exFAT.
   Once a content probe has selected FATX, do not replace it with the generic
   partition-type mapping used as the fallback for other filesystems. */
static inline unsigned long ExfatSelectPartitionDosType(
    unsigned long content_type, unsigned long mapped_type)
{
    return content_type == ID_EXFAT_DISK ? content_type : mapped_type;
}

static inline int IsExfatBootSector(const void *buffer, unsigned long length)
{
    const unsigned char *b = (const unsigned char *)buffer;
    static const unsigned char jump[3] = { 0xeb, 0x76, 0x90 };
    static const unsigned char name[8] =
        { 'E','X','F','A','T',' ',' ',' ' };
    unsigned long i;

    if (b == NULL || length < 512)
        return 0;
    for (i = 0; i < sizeof(jump); i++)
        if (b[i] != jump[i])
            return 0;
    for (i = 0; i < sizeof(name); i++)
        if (b[3 + i] != name[i])
            return 0;
    /* A legacy FAT BPB occupies this area; exFAT requires it to be zero. */
    for (i = 11; i < 64; i++)
        if (b[i] != 0)
            return 0;
    return b[510] == 0x55 && b[511] == 0xaa;
}

#endif /* DOS_EXFAT_H */
