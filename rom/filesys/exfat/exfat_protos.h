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

/* support.c */
LONG ErrorMessageArgs(struct Globals *glob, char *options,
    CONST_STRPTR format, ...);

#define ErrorMessage(fmt, options, ...) \
    ErrorMessageArgs(glob, options, fmt, __VA_ARGS__)

#endif /* EXFAT_PROTOS_H */
