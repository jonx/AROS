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
BOOL UpdateDisk(struct Globals *glob);
BOOL SyncDisk(struct Globals *glob);
void Probe64BitSupport(struct Globals *glob);
LONG ExfatFormatVolume(struct Globals *glob, CONST_STRPTR name,
    ULONG name_length);

/* volume.c: supplied by the mount path */
void DoDiskInsert(struct Globals *glob);
void DoDiskRemove(struct Globals *glob);
BOOL ExfatAttemptDestroyVolume(struct FSSuper *sb);

/* stream.c */
LONG ExfatReadStream(struct FSSuper *sb, const struct exfat_stream *stream,
    UQUAD offset, APTR buffer, ULONG length, ULONG *actual);
LONG ExfatClusterAt(struct FSSuper *sb, const struct exfat_stream *stream,
    UQUAD ordinal, ULONG *result);
LONG ExfatFatNext(struct FSSuper *sb, ULONG cluster, ULONG *next);
LONG ExfatLoadStream(struct FSSuper *sb, const struct exfat_stream *stream,
    UBYTE **buffer, ULONG *length);
LONG ExfatWriteStream(struct FSSuper *sb, const struct exfat_stream *stream,
    UQUAD offset, CONST_APTR buffer, ULONG length, ULONG *actual);
LONG ExfatZeroStream(struct FSSuper *sb, const struct exfat_stream *stream,
    UQUAD offset, UQUAD length);
LONG ExfatZeroClusterRun(struct FSSuper *sb, ULONG first, ULONG count);
LONG ExfatSetFatEntry(struct FSSuper *sb, ULONG cluster, ULONG value);

/* directory.c */
LONG ExfatLoadMetadata(struct FSSuper *sb);
LONG ExfatNextEntry(struct FSSuper *sb, const struct exfat_stream *directory,
    ULONG *index, struct exfat_entry *entry);
LONG ExfatFindEntry(struct FSSuper *sb, const struct exfat_stream *directory,
    CONST_STRPTR name, ULONG name_length, struct exfat_entry *entry);
void ExfatNameToLocal(const struct exfat_entry *entry, STRPTR out,
    ULONG out_size);
LONG ExfatUpdateEntryStream(struct FSSuper *sb,
    const struct exfat_stream *directory, const struct exfat_entry *entry);
LONG ExfatCreateEntry(struct FSSuper *sb,
    struct exfat_entry *directory, const struct exfat_stream *parent,
    CONST_STRPTR name,
    ULONG name_length, struct exfat_entry *entry);
LONG ExfatCreateDirectoryEntry(struct FSSuper *sb,
    struct exfat_entry *directory, const struct exfat_stream *parent,
    CONST_STRPTR name,
    ULONG name_length, struct exfat_entry *entry);
LONG ExfatRenameEntry(struct FSSuper *sb,
    const struct exfat_stream *source_directory,
    const struct exfat_entry *source_entry,
    struct exfat_entry *target_directory,
    const struct exfat_stream *target_parent,
    CONST_STRPTR name, ULONG name_length, struct exfat_entry *target_entry);
LONG ExfatUpdateEntryAttributes(struct FSSuper *sb,
    const struct exfat_stream *directory, struct exfat_entry *entry,
    UWORD attributes);
LONG ExfatUpdateEntryDate(struct FSSuper *sb,
    const struct exfat_stream *directory, struct exfat_entry *entry,
    ULONG timestamp, UBYTE ten_ms, BYTE utc, ULONG fields);
LONG ExfatTouchEntry(struct FSSuper *sb,
    const struct exfat_stream *directory, struct exfat_entry *entry,
    ULONG fields);
LONG ExfatSetVolumeLabel(struct FSSuper *sb, CONST_STRPTR name,
    ULONG name_length);
LONG ExfatDeleteEntry(struct FSSuper *sb,
    const struct exfat_stream *directory, const struct exfat_entry *entry);
LONG ExfatPrepareDirectoryDelete(struct FSSuper *sb,
    const struct exfat_stream *directory, BOOL purge);

/* transaction.c */
LONG ExfatBeginWrite(struct FSSuper *sb);
LONG ExfatFlushWriteStage(struct FSSuper *sb);
LONG ExfatCommitWrite(struct FSSuper *sb);
LONG ExfatAbortWrite(struct FSSuper *sb);

/* allocation.c */
LONG ExfatPlanContiguous(struct FSSuper *sb, ULONG hint, ULONG count,
    ULONG *first);
LONG ExfatSetBitmapRange(struct FSSuper *sb, ULONG first, ULONG count,
    BOOL allocated);
LONG ExfatReserveBitmapRange(struct FSSuper *sb, ULONG first, ULONG count,
    BOOL allocated);
LONG ExfatWriteBitmapRange(struct FSSuper *sb, ULONG first, ULONG count);
LONG ExfatGrowFile(struct FSSuper *sb, struct exfat_entry *entry,
    UQUAD new_length);
LONG ExfatResizeFile(struct FSSuper *sb,
    const struct exfat_stream *directory, struct exfat_entry *entry,
    UQUAD new_length);
LONG ExfatFreeStream(struct FSSuper *sb,
    const struct exfat_stream *stream);

/* packet.c */
void ExfatProcessPackets(struct Globals *glob);

/* support.c */
void ExfatSendEvent(LONG event, struct Globals *glob);
BOOL ExfatGetGMTOffset(struct Globals *glob, LONG *minutes);
LONG ExfatPackDateStamp(struct Globals *glob, const struct DateStamp *date,
    ULONG *packed, UBYTE *ten_ms, UBYTE *utc);
void ExfatCurrentTimestamp(struct Globals *glob, ULONG *packed,
    UBYTE *ten_ms, UBYTE *utc);
BOOL ExfatUnpackDateStamp(struct Globals *glob, ULONG packed, UBYTE ten_ms,
    UBYTE utc, struct DateStamp *date);
LONG ErrorMessageArgs(struct Globals *glob, char *options,
    CONST_STRPTR format, ...);

#define ErrorMessage(fmt, options, ...) \
    ErrorMessageArgs(glob, options, fmt, __VA_ARGS__)

#endif /* EXFAT_PROTOS_H */
