/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: picture.datatype subclass that decodes the first (or only) video frame
          of any container/codec libavcodec can handle -- so MultiView and the
          desktop can open media the built-in image datatypes do not cover
          (mp4/mov/mkv/avi first frame, plus formats like h264/hevc/vp9 stills).

          The heavy lifting is libavformat + libavcodec + our hand-written
          planar-YUV -> RGB24 converter (the same one FFView uses, because
          libswscale's yuv2rgb24 C writer faults on this target). File I/O goes
          through a custom AVIOContext backed by the datatype's own dos file
          handle (DTA_Handle), so we read through dos like the rest of the port.

          Decode-only: DTM_WRITE falls through to the superclass (no encode).
*/

#include <aros/debug.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/utility.h>
#include <proto/datatypes.h>
#include <proto/graphics.h>

#include <exec/exec.h>
#include <dos/dos.h>
#include <utility/utility.h>
#include <datatypes/pictureclass.h>

#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/mem.h>
#include <libavutil/error.h>

ADD2LIBS("datatypes/picture.datatype", 0, struct Library *, PictureBase);

/**************************************************************************************************/

/* ---- custom AVIOContext over the datatype's already-open dos handle -------- */

#define AVIO_BUFSZ 32768

struct dt_io { BPTR fh; };          /* the handle is owned by datatypes.library */

static int dt_read(void *opaque, uint8_t *buf, int size)
{
    struct dt_io *io = opaque;
    LONG n = Read(io->fh, buf, size);
    if (n < 0)  return AVERROR(EIO);
    if (n == 0) return AVERROR_EOF;
    return (int)n;
}

/* Classic 32-bit dos Seek(): returns the position held BEFORE the move (-1 on
   error). 32-bit is fine for a picture datatype (files < 2GB). */
static int64_t dt_seek(void *opaque, int64_t off, int whence)
{
    struct dt_io *io = opaque;
    LONG mode;

    whence &= ~AVSEEK_FORCE;
    if (whence == AVSEEK_SIZE) {
        LONG cur = Seek(io->fh, 0, OFFSET_CURRENT);
        LONG sz;
        Seek(io->fh, 0, OFFSET_END);
        sz = Seek(io->fh, cur, OFFSET_BEGINNING);
        return (int64_t)sz;
    }
    mode = (whence == SEEK_SET) ? OFFSET_BEGINNING :
           (whence == SEEK_CUR) ? OFFSET_CURRENT  : OFFSET_END;
    if (Seek(io->fh, (LONG)off, mode) < 0) return AVERROR(EIO);
    return (int64_t)Seek(io->fh, 0, OFFSET_CURRENT);
}

/* Open avformat over the dos handle. The handle is NOT closed here (datatypes
   owns it); the caller frees the AVIO via dt_avio_close(). */
static AVFormatContext *dt_avio_open(BPTR fh)
{
    struct dt_io    *io;
    unsigned char   *buf;
    AVIOContext     *avio;
    AVFormatContext *fmt;

    io = av_mallocz(sizeof(*io));
    if (!io) return NULL;
    io->fh = fh;

    buf = av_malloc(AVIO_BUFSZ);
    if (!buf) { av_free(io); return NULL; }

    avio = avio_alloc_context(buf, AVIO_BUFSZ, 0, io, dt_read, NULL, dt_seek);
    if (!avio) { av_free(buf); av_free(io); return NULL; }

    fmt = avformat_alloc_context();
    if (!fmt) { av_freep(&avio->buffer); avio_context_free(&avio); av_free(io); return NULL; }
    fmt->pb = avio;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;

    if (avformat_open_input(&fmt, NULL, NULL, NULL) < 0) {
        av_freep(&avio->buffer); avio_context_free(&avio); av_free(io);
        return NULL;
    }
    if (avformat_find_stream_info(fmt, NULL) < 0) {
        AVIOContext *pb = fmt->pb;
        avformat_close_input(&fmt);
        if (pb) { av_freep(&pb->buffer); avio_context_free(&pb); }
        av_free(io);
        return NULL;
    }
    return fmt;
}

static void dt_avio_close(AVFormatContext *fmt)
{
    AVIOContext  *avio;
    struct dt_io *io;

    if (!fmt) return;
    avio = fmt->pb;
    io   = avio ? avio->opaque : NULL;
    avformat_close_input(&fmt);          /* CUSTOM_IO: leaves pb for us */
    if (avio) { av_freep(&avio->buffer); avio_context_free(&avio); }
    if (io)   av_free(io);               /* NOT Close(io->fh): datatypes owns it */
}

/* ---- planar-YUV (any 8-bit subsampling) -> RGB24, full/limited BT.601 ------ */

static unsigned char clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : (unsigned char)v); }

/* Returns 1 if it produced RGB, 0 if the frame is not 8-bit planar YUV. */
static int yuv_to_rgb24(AVFrame *fr, unsigned char *dst, int dstride, int w, int h)
{
    const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(fr->format);
    const unsigned char *Y, *U, *V;
    int yls, uls, vls, cw, ch, full, x, y;

    if (!d || d->nb_components < 3 || !(d->flags & AV_PIX_FMT_FLAG_PLANAR) ||
        (d->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL)) || d->comp[0].depth != 8)
        return 0;

    Y = fr->data[0]; U = fr->data[1]; V = fr->data[2];
    yls = fr->linesize[0]; uls = fr->linesize[1]; vls = fr->linesize[2];
    cw  = d->log2_chroma_w; ch = d->log2_chroma_h;
    full = (fr->color_range == AVCOL_RANGE_JPEG)
        || fr->format == AV_PIX_FMT_YUVJ420P || fr->format == AV_PIX_FMT_YUVJ422P
        || fr->format == AV_PIX_FMT_YUVJ444P || fr->format == AV_PIX_FMT_YUVJ440P;

    for (y = 0; y < h; y++) {
        const unsigned char *yr = Y + y * yls;
        const unsigned char *ur = U + (y >> ch) * uls;
        const unsigned char *vr = V + (y >> ch) * vls;
        unsigned char *o = dst + y * dstride;
        for (x = 0; x < w; x++) {
            int yy = yr[x], uu = ur[x >> cw] - 128, vv = vr[x >> cw] - 128, r, g, b;
            if (full) {
                r = yy + ((91881 * vv) >> 16);
                g = yy - ((22554 * uu + 46802 * vv) >> 16);
                b = yy + ((116130 * uu) >> 16);
            } else {
                int c = 76284 * (yy - 16);
                r = (c + 104595 * vv) >> 16;
                g = (c - 25624 * uu - 53281 * vv) >> 16;
                b = (c + 132251 * uu) >> 16;
            }
            o[0] = clamp8(r); o[1] = clamp8(g); o[2] = clamp8(b);
            o += 3;
        }
    }
    return 1;
}

/* ---- decode the first video frame and hand it to picture.datatype --------- */

static LONG FFMPEG_Decode(Class *cl, Object *o, BPTR file, struct BitMapHeader *bmh)
{
    AVFormatContext *fmt = NULL;
    AVCodecContext  *ctx = NULL;
    const AVCodec   *dec = NULL;
    AVPacket        *pkt = NULL;
    AVFrame         *frame = NULL;
    unsigned char   *rgb = NULL;
    LONG             error = DTERROR_INVALID_DATA;
    LONG             level = 0, dterr = 0;
    int              vs, w, h, stride, got = 0;

    D(bug("[ffmpeg.datatype] %s()\n", __func__));

    Seek(file, 0, OFFSET_BEGINNING);
    fmt = dt_avio_open(file);
    if (!fmt) return DTERROR_INVALID_DATA;

    vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (vs < 0 || !dec) goto done;

    ctx = avcodec_alloc_context3(dec);
    if (!ctx) { error = ERROR_NO_FREE_STORE; goto done; }
    if (avcodec_parameters_to_context(ctx, fmt->streams[vs]->codecpar) < 0 ||
        avcodec_open2(ctx, dec, NULL) < 0)
        goto done;

    pkt   = av_packet_alloc();
    frame = av_frame_alloc();
    if (!pkt || !frame) { error = ERROR_NO_FREE_STORE; goto done; }

    /* pull the first decodable video frame */
    while (!got && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == vs && avcodec_send_packet(ctx, pkt) == 0)
            if (avcodec_receive_frame(ctx, frame) == 0) got = 1;
        av_packet_unref(pkt);
    }
    if (!got) {                                  /* drain at EOF */
        avcodec_send_packet(ctx, NULL);
        if (avcodec_receive_frame(ctx, frame) == 0) got = 1;
    }
    if (!got) goto done;

    w = frame->width; h = frame->height;
    if (w <= 0 || h <= 0) goto done;
    stride = w * 3;

    rgb = AllocVec((ULONG)stride * h, MEMF_ANY);
    if (!rgb) { error = ERROR_NO_FREE_STORE; goto done; }

    if (!yuv_to_rgb24(frame, rgb, stride, w, h)) {
        /* Not planar YUV (e.g. an RGB/paletted still). Leave it to the built-in
           datatypes rather than guessing; report a clean "wrong type". */
        error = DTERROR_INVALID_DATA;
        goto done;
    }

    bmh->bmh_Width  = w;
    bmh->bmh_Height = h;
    bmh->bmh_Depth  = 24;

    SetDTAttrs(o, NULL, NULL,
        DTA_NominalHoriz, w,
        DTA_NominalVert,  h,
        PDTA_SourceMode,  PMODE_V43,
        DTA_ErrorLevel,   &level,
        DTA_ErrorNumber,  &dterr,
        TAG_END);

    DoSuperMethod(cl, o,
        PDTM_WRITEPIXELARRAY, (IPTR)rgb, PBPAFMT_RGB,
        stride, 0, 0, w, h);

    error = 0;

done:
    if (rgb)   FreeVec(rgb);
    if (frame) av_frame_free(&frame);
    if (pkt)   av_packet_free(&pkt);
    if (ctx)   avcodec_free_context(&ctx);
    dt_avio_close(fmt);
    return error;
}

static LONG FFMPEG_Import(Class *cl, Object *o, struct TagItem *tags)
{
    struct BitMapHeader *bmh = NULL;
    char  *filename = (char *)GetTagData(DTA_Name, 0, tags);
    SIPTR  srctype = 0;
    BPTR   file = BNULL;
    LONG   error = ERROR_OBJECT_NOT_FOUND;

    D(bug("[ffmpeg.datatype] %s()\n", __func__));

    GetDTAttrs(o,
        PDTA_BitMapHeader, &bmh,
        DTA_Handle,        &file,
        DTA_SourceType,    &srctype,
        TAG_END);

    if (srctype == DTST_RAM)
        return 0;                                /* empty object */

    if (bmh && file && srctype == DTST_FILE) {
        error = FFMPEG_Decode(cl, o, file, bmh);
        if (error == 0 && filename)
            SetDTAttrs(o, NULL, NULL, DTA_ObjName, (IPTR)FilePart(filename), TAG_END);
    }
    return error;
}

/**************************************************************************************************/

IPTR FFMPEG__OM_NEW(struct IClass *cl, Object *o, struct opSet *msg)
{
    IPTR retval;
    LONG error;

    D(bug("[ffmpeg.datatype] %s()\n", __func__));

    retval = DoSuperMethodA(cl, o, (Msg)msg);
    if (retval) {
        error = FFMPEG_Import(cl, (Object *)retval, msg->ops_AttrList);
        if (error != 0) {
            CoerceMethod(cl, (Object *)retval, OM_DISPOSE);
            SetIoErr(error);
            retval = (IPTR)NULL;
        }
    }
    return retval;
}

IPTR FFMPEG__DTM_WRITE(Class *cl, Object *o, struct dtWrite *dtw)
{
    /* Decode-only: DTM_RAW export is not supported; let the superclass handle
       DTM_WRITE (it can still write the decoded bitmap out as an ILBM etc.). */
    D(bug("[ffmpeg.datatype] %s()\n", __func__));
    return DoSuperMethodA(cl, o, (Msg)dtw);
}
