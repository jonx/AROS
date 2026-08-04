/*
 * exfat-handler - prototypes
 *
 * Copyright (C) 2026 The AROS Development Team
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the same terms as AROS itself.
 */

#ifndef EXFAT_PROTOS_H
#define EXFAT_PROTOS_H

/* disk.c */
void ProcessDiskChange(struct Globals *glob);
void UpdateDisk(struct Globals *glob);
void Probe64BitSupport(struct Globals *glob);

/* volume.c: supplied by the mount path */
void DoDiskInsert(struct Globals *glob);
void DoDiskRemove(struct Globals *glob);

/* stream.c */
LONG ExfatReadStream(struct FSSuper *sb, const struct exfat_stream *stream,
    UQUAD offset, APTR buffer, ULONG length, ULONG *actual);
LONG ExfatLoadStream(struct FSSuper *sb, const struct exfat_stream *stream,
    UBYTE **buffer, ULONG *length);

/* directory.c */
LONG ExfatLoadMetadata(struct FSSuper *sb);
LONG ExfatNextEntry(struct FSSuper *sb, const struct exfat_stream *directory,
    ULONG *index, struct exfat_entry *entry);
LONG ExfatFindEntry(struct FSSuper *sb, const struct exfat_stream *directory,
    CONST_STRPTR name, ULONG name_length, struct exfat_entry *entry);
void ExfatNameToLocal(const struct exfat_entry *entry, STRPTR out,
    ULONG out_size);

/* packet.c */
void ExfatProcessPackets(struct Globals *glob);

/* support.c */
LONG ErrorMessageArgs(struct Globals *glob, char *options,
    CONST_STRPTR format, ...);

#define ErrorMessage(fmt, options, ...) \
    ErrorMessageArgs(glob, options, fmt, __VA_ARGS__)

#endif /* EXFAT_PROTOS_H */
