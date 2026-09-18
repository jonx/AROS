/* hostvolume.h — flat C ABI for the macOS-folder-as-AROS-volume host glue.
 *
 * Implemented clean-room from docs/features/host-volume/spec.md (R-NORM,
 * R-CHARSET, R-ESCAPE, R-SIDECAR). No GPL emulator source (UAE family —
 * WinUAE/FS-UAE/Amiberry/E-UAE/Janus-UAE — or vAmiga) was read, searched, or
 * consulted. Sources: POSIX + Unicode UAX #15 (NFC/NFD) [PUB]; the AROS
 * emul-handler Do* contract as restated in the spec [AROS]; this project's
 * own hosted spikes + harness conventions [OURS].
 *
 * Hand-authored, neutral. This header is the contact surface the AROS-side
 * emul-handler per-host overlay (built by the AROS crosstools) calls into.
 * The glue pulls NO AROS headers and NO CoreFoundation/Cocoa: normalization
 * and charset are pure CPU work; the sidecar uses only libc file I/O that is
 * already present in the overlay's struct LibCInterface (open/read/write/
 * close/unlink/rename). Hence NO -framework dependency.
 */
#ifndef HOSTVOLUME_H
#define HOSTVOLUME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 *  R-NORM — Unicode NFC normalization (self-contained, table-driven)
 *  Operates on UTF-8 byte strings (the host side of the bridge). The
 *  handler's canonical form is NFC; everything compared or constructed
 *  passes through hv_to_nfc first so NFC vs NFD inputs collapse to one
 *  deterministic key regardless of what the filesystem stored.
 * ------------------------------------------------------------------ */

/* Normalize a NUL-terminated UTF-8 string to NFC. Writes at most dstcap
 * bytes (always NUL-terminates when dstcap>0). Returns the number of bytes
 * that NFC needs *excluding* the NUL (like snprintf): if the return value
 * is >= dstcap the output was truncated. Pure; no host call. */
size_t hv_to_nfc(const char *utf8, char *dst, size_t dstcap);

/* Convenience: normalize both to NFC and compare for byte-equality.
 * Returns 1 if the two strings denote the same NFC sequence, else 0. */
int hv_nfc_equal(const char *a, const char *b);

/* ------------------------------------------------------------------ *
 *  R-CHARSET / R-ESCAPE — AROS Latin-1 bytes <-> host UTF-8
 *  Latin-1 -> UTF-8 is total. UTF-8 -> Latin-1 is partial; code points
 *  outside U+0000..U+00FF are emitted as a reversible ASCII escape
 *  "%uXXXX" (our marker is '%'); the marker itself is escaped as "%u0025"
 *  so the transform is symmetric and round-trips.
 * ------------------------------------------------------------------ */

/* AROS Latin-1 name bytes -> host UTF-8. Also DECODES our "%uXXXX" escapes
 * (incl. the marker's own "%u0025") back to the real code point, so a host name
 * that was escaped on the way in is reconstructed byte-identically on the way
 * out (genuine round-trip). Returns bytes needed (excl. NUL), snprintf-style. */
size_t hv_latin1_to_utf8(const char *latin1, char *dst, size_t dstcap);

/* Host UTF-8 -> AROS Latin-1 name bytes, escaping un-mappable code points
 * and the marker byte. Returns bytes needed (excl. NUL), snprintf-style. */
size_t hv_utf8_to_latin1(const char *utf8, char *dst, size_t dstcap);

/* ------------------------------------------------------------------ *
 *  R-SIDECAR — ".ameta", one metadata file per host directory
 *
 *  AmigaOS fib_Protection layout (public AmigaOS ABI, restated [PUB]): the
 *  low nibble is ACTIVE-LOW delete/execute/write/read. The handler takes
 *  owner, group and other rwx from st_mode and every other bit from .ameta,
 *  the Delete bit included.
 * ------------------------------------------------------------------ */

/* fib_Protection bits (AmigaOS public ABI). Active-low for DEWR. */
#define HV_FIBF_DELETE   (1u << 0)
#define HV_FIBF_EXECUTE  (1u << 1)
#define HV_FIBF_WRITE    (1u << 2)
#define HV_FIBF_READ     (1u << 3)
#define HV_FIBF_ARCHIVE  (1u << 4)   /* AmigaOS-only (no POSIX rwx slot) */
#define HV_FIBF_PURE     (1u << 5)   /* AmigaOS-only */
#define HV_FIBF_SCRIPT   (1u << 6)   /* AmigaOS-only */
#define HV_FIBF_HOLD     (1u << 7)   /* AmigaOS-only */

/* The bits st_mode holds (owner, group and other rwx); .ameta holds the rest. */
#define HV_HOST_MASK 0xEE0Eu

/* The default protection a fresh file carries (rwxd available => all four
 * active-low bits CLEAR). A file with this protection and an empty comment
 * is "default" and gets NO sidecar. */
#define HV_PROT_DEFAULT  0u

#define HV_COMMENT_MAX   80          /* AmigaOS fib_Comment capacity */

typedef struct {
    uint32_t prot;                   /* full fib_Protection word */
    char     comment[HV_COMMENT_MAX + 1]; /* NUL-terminated Latin-1 (AROS) */
} HVMeta;

/* True (1) for .ameta and a writer's temporary ".ameta.*" file, which
 * enumeration skips. */
int hv_is_sidecar_name(const char *name);

/* True (1) if this metadata needs no entry in .ameta. */
int hv_meta_is_default(const HVMeta *m);

/* AROS overlay .ameta I/O (file access via the dlsym'd LibCInterface). */
struct emulbase;

/* Read filepath's entry from its directory's .ameta. Returns 1 if there is
 * one, 0 if not (out set to defaults), -1 when memory runs out. A malformed
 * line is reported and skipped. Caller must NOT hold HostLib_Lock(). */
int MetaRead(struct emulbase *emulbase, const char *filepath, HVMeta *out);

/* Record filepath's metadata in its directory's .ameta, dropping the entry
 * when it is default and the file when no entry is left. Returns 0 or -1. */
int MetaWrite(struct emulbase *emulbase, const char *filepath, const HVMeta *m);

/* Keep .ameta in step with a rename, a move or a delete, after the host did it. */
void MetaRename(struct emulbase *emulbase, const char *oldpath, const char *newpath);
void MetaDelete(struct emulbase *emulbase, const char *filepath);

#ifdef __cplusplus
}
#endif

#endif /* HOSTVOLUME_H */
