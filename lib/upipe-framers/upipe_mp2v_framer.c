/*
 * Copyright (C) 2013 OpenHeadend S.A.R.L.
 *
 * Authors: Christophe Massiot
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 */

/** @file
 * @short Upipe module building frames from chunks of an ISO 13818-2 stream
 */

#include <upipe/ubase.h>
#include <upipe/urefcount.h>
#include <upipe/ulist.h>
#include <upipe/uprobe.h>
#include <upipe/uref.h>
#include <upipe/uref_flow.h>
#include <upipe/uref_block.h>
#include <upipe/uref_block_flow.h>
#include <upipe/uref_pic.h>
#include <upipe/uref_pic_flow.h>
#include <upipe/uref_clock.h>
#include <upipe/uclock.h>
#include <upipe/ubuf.h>
#include <upipe/upipe.h>
#include <upipe/upipe_helper_upipe.h>
#include <upipe/upipe_helper_sync.h>
#include <upipe/upipe_helper_octet_stream.h>
#include <upipe/upipe_helper_output.h>
#include <upipe-framers/upipe_mp2v_framer.h>
#include <upipe-framers/uref_mp2v.h>
#include <upipe-framers/uref_mp2v_flow.h>

#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>

#include <bitstream/mpeg/mp2v.h>

/** we only accept the ISO 13818-2 elementary stream */
#define EXPECTED_FLOW_DEF "block.mpeg2video."

/** token to find MPEG-2 start codes */
#define FIND_START      3, 0, 0, 1

/** token to find MPEG-2 GOP header start codes */
#define FIND_GOP        4, 0, 0, 1, MP2VGOP_START_CODE

/** token to find MPEG-2 extension start codes */
#define FIND_EXTENSION  4, 0, 0, 1, MP2VX_START_CODE

/** @internal @This translates the MPEG frame_rate_code to double */
static const struct urational frame_rate_from_code[] = {
    { .num = 0, .den = 0 }, /* invalid */
    { .num = 24000, .den = 1001 },
    { .num = 24, .den = 1 },
    { .num = 25, .den = 1 },
    { .num = 30000, .den = 1001 },
    { .num = 30, .den = 1 },
    { .num = 50, .den = 1 },
    { .num = 60000, .den = 1001 },
    { .num = 60, .den = 1 },
    /* Xing */
    { .num = 15000, .den = 1001 },
    /* libmpeg3 */
    { .num = 5000, .den = 1001 },
    { .num = 10000, .den = 1001 },
    { .num = 12000, .den = 1001 },
    { .num = 15000, .den = 1001 },
    /* invalid */
    { .num = 0, .den = 0 },
    { .num = 0, .den = 0 }
};

/** @internal @This is the private context of an mp2vf pipe. */
struct upipe_mp2vf {
    /* output stuff */
    /** pipe acting as output */
    struct upipe *output;
    /** output flow definition packet */
    struct uref *flow_def;
    /** true if the flow definition has already been sent */
    bool flow_def_sent;
    /** input flow definition packet */
    struct uref *flow_def_input;
    /** last random access point */
    uint64_t systime_rap;

    /* picture parsing stuff */
    /** last output picture number */
    uint64_t last_picture_number;
    /** last temporal reference read from the stream, or -1 */
    int last_temporal_reference;
    /** true we have had a discontinuity recently */
    bool got_discontinuity;
    /** true if the user wants us to insert sequence headers before I frames,
     * if it is not already present */
    bool insert_sequence;
    /** pointer to a sequence header */
    struct ubuf *sequence_header;
    /** pointer to a sequence header extension */
    struct ubuf *sequence_ext;
    /** pointer to a sequence display extension */
    struct ubuf *sequence_display;
    /** true if the flag progressive sequence is true */
    bool progressive_sequence;
    /** frames per second */
    struct urational fps;

    /* octet stream stuff */
    /** next uref to be processed */
    struct uref *next_uref;
    /** original size of the next uref */
    size_t next_uref_size;
    /** urefs received after next uref */
    struct ulist urefs;

    /* octet stream parser stuff */
    /** current size of next frame (in next_uref) */
    size_t next_frame_size;
    /** true if the next uref begins with a sequence header */
    bool next_frame_sequence;
    /** offset of the picture header in next_uref, or -1 */
    ssize_t next_frame_offset;
    /** true if we have found at least one slice header */
    bool next_frame_slice;
    /** original PTS of the next picture, or UINT64_MAX */
    uint64_t next_frame_pts_orig;
    /** PTS of the next picture, or UINT64_MAX */
    uint64_t next_frame_pts;
    /** system PTS of the next picture, or UINT64_MAX */
    uint64_t next_frame_pts_sys;
    /** original DTS of the next picture, or UINT64_MAX */
    uint64_t next_frame_dts_orig;
    /** DTS of the next picture, or UINT64_MAX */
    uint64_t next_frame_dts;
    /** system DTS of the next picture, or UINT64_MAX */
    uint64_t next_frame_dts_sys;
    /** true if we have thrown the sync_acquired event (that means we found a
     * sequence header) */
    bool acquired;

    /** refcount management structure */
    urefcount refcount;
    /** public upipe structure */
    struct upipe upipe;
};

/** @hidden */
static void upipe_mp2vf_promote_uref(struct upipe *upipe);

UPIPE_HELPER_UPIPE(upipe_mp2vf, upipe)
UPIPE_HELPER_SYNC(upipe_mp2vf, acquired)
UPIPE_HELPER_OCTET_STREAM(upipe_mp2vf, next_uref, next_uref_size, urefs,
                          upipe_mp2vf_promote_uref)

UPIPE_HELPER_OUTPUT(upipe_mp2vf, output, flow_def, flow_def_sent)

/** @internal @This flushes all PTS timestamps.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_mp2vf_flush_pts(struct upipe *upipe)
{
    struct upipe_mp2vf *upipe_mp2vf = upipe_mp2vf_from_upipe(upipe);
    upipe_mp2vf->next_frame_pts_orig = UINT64_MAX;
    upipe_mp2vf->next_frame_pts = UINT64_MAX;
    upipe_mp2vf->next_frame_pts_sys = UINT64_MAX;
}

/** @internal @This flushes all DTS timestamps.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_mp2vf_flush_dts(struct upipe *upipe)
{
    struct upipe_mp2vf *upipe_mp2vf = upipe_mp2vf_from_upipe(upipe);
    upipe_mp2vf->next_frame_dts_orig = UINT64_MAX;
    upipe_mp2vf->next_frame_dts = UINT64_MAX;
    upipe_mp2vf->next_frame_dts_sys = UINT64_MAX;
}

/** @internal @This increments all DTS timestamps by the duration of the frame.
 *
 * @param upipe description structure of the pipe
 * @param duration duration of the frame
 */
static void upipe_mp2vf_increment_dts(struct upipe *upipe, uint64_t duration)
{
    struct upipe_mp2vf *upipe_mp2vf = upipe_mp2vf_from_upipe(upipe);
    if (upipe_mp2vf->next_frame_dts_orig != UINT64_MAX)
        upipe_mp2vf->next_frame_dts_orig += duration;
    if (upipe_mp2vf->next_frame_dts != UINT64_MAX)
        upipe_mp2vf->next_frame_dts += duration;
    if (upipe_mp2vf->next_frame_dts_sys != UINT64_MAX)
        upipe_mp2vf->next_frame_dts_sys += duration;
}

/** @internal @This allocates an mp2vf pipe.
 *
 * @param mgr common management structure
 * @param uprobe structure used to raise events
 * @return pointer to upipe or NULL in case of allocation error
 */
static struct upipe *upipe_mp2vf_alloc(struct upipe_mgr *mgr,
                                       struct uprobe *uprobe)
{
    struct upipe_mp2vf *upipe_mp2vf = malloc(sizeof(struct upipe_mp2vf));
    if (unlikely(upipe_mp2vf == NULL))
        return NULL;
    struct upipe *upipe = upipe_mp2vf_to_upipe(upipe_mp2vf);
    upipe_init(upipe, mgr, uprobe);
    upipe_mp2vf_init_sync(upipe);
    upipe_mp2vf_init_octet_stream(upipe);
    upipe_mp2vf_init_output(upipe);
    upipe_mp2vf->flow_def_input = NULL;
    upipe_mp2vf->systime_rap = UINT64_MAX;
    upipe_mp2vf->last_picture_number = 0;
    upipe_mp2vf->last_temporal_reference = -1;
    upipe_mp2vf->got_discontinuity = false;
    upipe_mp2vf->insert_sequence = false;
    upipe_mp2vf->next_frame_size = 0;
    upipe_mp2vf->next_frame_sequence = false;
    upipe_mp2vf->next_frame_offset = -1;
    upipe_mp2vf->next_frame_slice = false;
    upipe_mp2vf_flush_pts(upipe);
    upipe_mp2vf_flush_dts(upipe);
    upipe_mp2vf->sequence_header = upipe_mp2vf->sequence_ext =
        upipe_mp2vf->sequence_display = NULL;
    upipe_mp2vf->acquired = false;
    urefcount_init(&upipe_mp2vf->refcount);
    upipe_throw_ready(upipe);
    return upipe;
}

/** @internal @This finds an MPEG-2 start code and returns its value.
 *
 * @param upipe description structure of the pipe
 * @param start value of the start code
 * @return true if a start code was found
 */
static bool upipe_mp2vf_find(struct upipe *upipe, uint8_t *start)
{
    struct upipe_mp2vf *upipe_mp2vf = upipe_mp2vf_from_upipe(upipe);
    return uref_block_find(upipe_mp2vf->next_uref,
                           &upipe_mp2vf->next_frame_size, FIND_START) &&
           uref_block_extract(upipe_mp2vf->next_uref,
                              upipe_mp2vf->next_frame_size + 3, 1, start);
}

/** @internal @This finds an MPEG-2 extension start code and returns its value.
 *
 * @param upipe description structure of the pipe
 * @param uref uref containing a frame
 * @param offset_p reference to offset at which to start the scan, filled in
 * with the position of the start code
 * @param start value of the extension start code
 * @return true if a start code was found
 */
static bool upipe_mp2vf_find_ext(struct upipe *upipe, struct uref *uref,
                                 size_t *offset_p, uint8_t *start)
{
    bool ret = uref_block_find(uref, offset_p, FIND_EXTENSION) &&
               uref_block_extract(uref, *offset_p + 4, 1, start);
    if (ret)
        *start >>= 4;
    return ret;
}

/** @internal @This parses a new sequence header, and outputs a flow
 * definition
 *
 * @param upipe description structure of the pipe
 * @return false in case of error
 */
static bool upipe_mp2vf_parse_sequence(struct upipe *upipe)
{
    struct upipe_mp2vf *upipe_mp2vf = upipe_mp2vf_from_upipe(upipe);
    uint8_t sequence_buffer[MP2VSEQ_HEADER_SIZE];
    const uint8_t *sequence;
    if (unlikely((sequence = ubuf_block_peek(upipe_mp2vf->sequence_header,
                                             0, MP2VSEQ_HEADER_SIZE,
                                             sequence_buffer)) == NULL)) {
        upipe_throw_aerror(upipe);
        return false;
    }
    uint16_t horizontal = mp2vseq_get_horizontal(sequence);
    uint16_t vertical = mp2vseq_get_vertical(sequence);
    uint8_t aspect = mp2vseq_get_aspect(sequence);
    uint8_t framerate = mp2vseq_get_framerate(sequence);
    uint32_t bitrate = mp2vseq_get_bitrate(sequence);
    uint32_t vbvbuffer = mp2vseq_get_vbvbuffer(sequence);
    if (unlikely(!ubuf_block_peek_unmap(upipe_mp2vf->sequence_header, 0,
                                        MP2VSEQ_HEADER_SIZE, sequence_buffer,
                                        sequence))) {
        upipe_throw_aerror(upipe);
        return false;
    }

    struct urational frame_rate = frame_rate_from_code[framerate];
    if (!frame_rate.num) {
        upipe_err_va(upipe, "invalid frame rate %d", framerate);
        return false;
    }

    struct uref *flow_def = uref_dup(upipe_mp2vf->flow_def_input);
    if (unlikely(flow_def == NULL)) {
        upipe_throw_aerror(upipe);
        return false;
    }
    bool ret = true;

    uint64_t max_octetrate = 1500000 / 8;
    if (upipe_mp2vf->sequence_ext != NULL) {
        uint8_t ext_buffer[MP2VSEQX_HEADER_SIZE];
        const uint8_t *ext;
        if (unlikely((ext = ubuf_block_peek(upipe_mp2vf->sequence_ext,
                                            0, MP2VSEQX_HEADER_SIZE,
                                            ext_buffer)) == NULL)) {
            uref_free(flow_def);
            upipe_throw_aerror(upipe);
            return false;
        }

        uint8_t profilelevel = mp2vseqx_get_profilelevel(ext);
        bool progressive = mp2vseqx_get_progressive(ext);
        uint8_t chroma = mp2vseqx_get_chroma(ext);
        horizontal |= mp2vseqx_get_horizontal(ext) << 12;
        vertical |= mp2vseqx_get_vertical(ext) << 12;
        bitrate |= mp2vseqx_get_bitrate(ext) << 18;
        vbvbuffer |= mp2vseqx_get_vbvbuffer(ext) << 10;
        bool lowdelay = mp2vseqx_get_lowdelay(ext);
        frame_rate.num *= mp2vseqx_get_frameraten(ext) + 1;
        frame_rate.den *= mp2vseqx_get_framerated(ext) + 1;
        urational_simplify(&frame_rate);

        if (unlikely(!ubuf_block_peek_unmap(upipe_mp2vf->sequence_ext, 0,
                                            MP2VSEQX_HEADER_SIZE, ext_buffer,
                                            ext))) {
            uref_free(flow_def);
            upipe_throw_aerror(upipe);
            return false;
        }

        ret = ret && uref_mp2v_flow_set_profilelevel(flow_def, profilelevel);
        switch (profilelevel & MP2VSEQX_LEVEL_MASK) {
            case MP2VSEQX_LEVEL_LOW:
                max_octetrate = 4000000 / 8;
                break;
            case MP2VSEQX_LEVEL_MAIN:
                max_octetrate = 15000000 / 8;
                break;
            case MP2VSEQX_LEVEL_HIGH1440:
                max_octetrate = 60000000 / 8;
                break;
            case MP2VSEQX_LEVEL_HIGH:
                max_octetrate = 80000000 / 8;
                break;
            default:
                upipe_err_va(upipe, "invalid level %d",
                             profilelevel & MP2VSEQX_LEVEL_MASK);
                uref_free(flow_def);
                return false;
        }
        ret = ret && uref_block_flow_set_max_octetrate(flow_def, max_octetrate);
        if (progressive)
            ret = ret && uref_pic_set_progressive(flow_def);
        upipe_mp2vf->progressive_sequence = progressive;
        ret = ret && uref_pic_flow_set_macropixel(flow_def, 1);
        ret = ret && uref_pic_flow_set_planes(flow_def, 0);
        ret = ret && uref_pic_flow_add_plane(flow_def, 1, 1, 1, "y8");
        switch (chroma) {
            case MP2VSEQX_CHROMA_420:
                ret = ret && uref_pic_flow_add_plane(flow_def, 2, 2, 1, "u8");
                ret = ret && uref_pic_flow_add_plane(flow_def, 2, 2, 1, "v8");
                ret = ret && uref_flow_set_def(flow_def,
                        EXPECTED_FLOW_DEF "pic.planar8_420.");
                break;
            case MP2VSEQX_CHROMA_422:
                ret = ret && uref_pic_flow_add_plane(flow_def, 2, 1, 1, "u8");
                ret = ret && uref_pic_flow_add_plane(flow_def, 2, 1, 1, "v8");
                ret = ret && uref_flow_set_def(flow_def,
                        EXPECTED_FLOW_DEF "pic.planar8_422.");
                break;
            case MP2VSEQX_CHROMA_444:
                ret = ret && uref_pic_flow_add_plane(flow_def, 1, 1, 1, "u8");
                ret = ret && uref_pic_flow_add_plane(flow_def, 1, 1, 1, "v8");
                ret = ret && uref_flow_set_def(flow_def,
                        EXPECTED_FLOW_DEF "pic.planar8_444.");
                break;
            default:
                upipe_err_va(upipe, "invalid chroma format %d", chroma);
                uref_free(flow_def);
                return false;
        }
        if (lowdelay)
            ret = ret && uref_mp2v_flow_set_lowdelay(flow_def);
    } else
        upipe_mp2vf->progressive_sequence = false;

    ret = ret && uref_pic_set_hsize(flow_def, horizontal);
    ret = ret && uref_pic_set_vsize(flow_def, vertical);
    struct urational sar;
    switch (aspect) {
        case MP2VSEQ_ASPECT_SQUARE:
            sar.num = sar.den = 1;
            break;
        case MP2VSEQ_ASPECT_4_3:
            sar.num = vertical * 4;
            sar.den = horizontal * 3;
            urational_simplify(&sar);
            break;
        case MP2VSEQ_ASPECT_16_9:
            sar.num = vertical * 16;
            sar.den = horizontal * 9;
            urational_simplify(&sar);
            break;
        case MP2VSEQ_ASPECT_2_21:
            sar.num = vertical * 221;
            sar.den = horizontal * 100;
            urational_simplify(&sar);
            break;
        default:
            upipe_err_va(upipe, "invalid aspect ratio %d", aspect);
            uref_free(flow_def);
            return false;
    }
    ret = ret && uref_pic_set_aspect(flow_def, sar);
    ret = ret && uref_pic_flow_set_fps(flow_def, frame_rate);
    upipe_mp2vf->fps = frame_rate;
    ret = ret && uref_block_flow_set_octetrate(flow_def, bitrate * 400 / 8);
    ret = ret && uref_block_flow_set_cpb_buffer(flow_def,
                                                vbvbuffer * 16 * 1024 / 8);

    if (upipe_mp2vf->sequence_display != NULL) {
        size_t size;
        uint8_t display_buffer[MP2VSEQDX_HEADER_SIZE + MP2VSEQDX_COLOR_SIZE];
        const uint8_t *display;
        if (unlikely(!ubuf_block_size(upipe_mp2vf->sequence_display, &size) ||
                     (display = ubuf_block_peek(upipe_mp2vf->sequence_display,
                                                0, size,
                                                display_buffer)) == NULL)) {
            uref_free(flow_def);
            upipe_throw_aerror(upipe);
            return false;
        }

        uint16_t display_horizontal = mp2vseqdx_get_horizontal(display);
        uint16_t display_vertical = mp2vseqdx_get_vertical(display);

        if (unlikely(!ubuf_block_peek_unmap(upipe_mp2vf->sequence_display, 0,
                                            size, display_buffer, display))) {
            uref_free(flow_def);
            upipe_throw_aerror(upipe);
            return false;
        }

        ret = ret && uref_pic_set_hsize_visible(flow_def, display_horizontal);
        ret = ret && uref_pic_set_vsize_visible(flow_def, display_vertical);
    }

    if (unlikely(!ret)) {
        upipe_throw_aerror(upipe);
        return false;
    }
    upipe_mp2vf_store_flow_def(upipe, flow_def);
    return true;
}

/** @internal @This extracts the sequence header from a uref.
 *
 * @param upipe description structure of the pipe
 * @param uref uref containing a frame, beginning with a sequence header
 * @param offset_p filled in with the length of the sequence header on
 * execution
 * @return pointer to ubuf containing only the sequence header
 */
static struct ubuf *upipe_mp2vf_extract_sequence(struct upipe *upipe,
                                                 struct uref *uref,
                                                 size_t *offset_p)
{
    struct ubuf *sequence_header = ubuf_dup(uref->ubuf);
    uint8_t word;
    if (unlikely(sequence_header == NULL ||
                 !ubuf_block_extract(sequence_header, 11, 1, &word))) {
        if (sequence_header != NULL)
            ubuf_free(sequence_header);
        upipe_throw_aerror(upipe);
        return NULL;
    }

    size_t sequence_header_size = MP2VSEQ_HEADER_SIZE;
    if (word & 0x2) {
        /* intra quantiser matrix */
        sequence_header_size += 64;
        if (unlikely(!ubuf_block_extract(sequence_header, 11 + 64, 1, &word))) {
            ubuf_free(sequence_header);
            upipe_throw_aerror(upipe);
            return NULL;
        }
    }
    if (word & 0x1) {
        /* non-intra quantiser matrix */
        sequence_header_size += 64;
    }

    if (unlikely(!ubuf_block_resize(sequence_header, 0,
                                    sequence_header_size))) {
        ubuf_free(sequence_header);
        upipe_throw_aerror(upipe);
        return NULL;
    }
    *offset_p = sequence_header_size;
    return sequence_header;
}

/** @internal @This extracts the sequence extension from a uref.
 *
 * @param upipe description structure of the pipe
 * @param uref uref containing a frame, beginning with a sequence header
 * @param offset_p offset of the sequence extension in the uref, adds its
 * length on execution
 * @return pointer to ubuf containing only the sequence extension
 */
static struct ubuf *upipe_mp2vf_extract_extension(struct upipe *upipe,
                                                  struct uref *uref,
                                                  size_t *offset_p)
{
    struct ubuf *sequence_ext = ubuf_dup(uref->ubuf);
    if (unlikely(sequence_ext == NULL ||
                 !ubuf_block_resize(sequence_ext, *offset_p,
                                    MP2VSEQX_HEADER_SIZE))) {
        if (sequence_ext != NULL)
            ubuf_free(sequence_ext);
        upipe_throw_aerror(upipe);
        return NULL;
    }
    *offset_p += MP2VSEQX_HEADER_SIZE;
    return sequence_ext;
}

/** @internal @This extracts the sequence display extension from a uref.
 *
 * @param upipe description structure of the pipe
 * @param uref uref containing a frame, beginning with a sequence header
 * @param offset_p offset of the sequence display extension in the uref,
 * adds its length on execution
 * @return pointer to ubuf containing only the sequence extension
 */
static struct ubuf *upipe_mp2vf_extract_display(struct upipe *upipe,
                                                struct uref *uref,
                                                size_t *offset_p)
{
    struct ubuf *sequence_display = ubuf_dup(uref->ubuf);
    uint8_t word;
    if (unlikely(sequence_display == NULL ||
                 !ubuf_block_extract(sequence_display, *offset_p, 1, &word))) {
        if (sequence_display != NULL)
            ubuf_free(sequence_display);
        upipe_throw_aerror(upipe);
        return NULL;
    }
    size_t sequence_display_size = MP2VSEQDX_HEADER_SIZE + 
                                   ((word & 0x1) ? MP2VSEQDX_COLOR_SIZE : 0);
    if (unlikely(!ubuf_block_resize(sequence_display, *offset_p,
                                    sequence_display_size))) {
        ubuf_free(sequence_display);
        upipe_throw_aerror(upipe);
        return NULL;
    }
    *offset_p += sequence_display_size;
    return sequence_display;
}

/** @internal @This handles a uref containing a sequence header.
 *
 * @param upipe description structure of the pipe
 * @param uref uref containing a frame, beginning with a sequence header
 * @return false in case of error
 */
static bool upipe_mp2vf_handle_sequence(struct upipe *upipe, struct uref *uref)
{
    struct upipe_mp2vf *upipe_mp2vf = upipe_mp2vf_from_upipe(upipe);
    size_t ext_offset;
    struct ubuf *sequence_ext = NULL;
    struct ubuf *sequence_display = NULL;
    struct ubuf *sequence_header = upipe_mp2vf_extract_sequence(upipe, uref,
                                                                &ext_offset);
    if (unlikely(sequence_header == NULL))
        return false;

    uint8_t ext_header;
    if (upipe_mp2vf_find_ext(upipe, uref, &ext_offset, &ext_header)) {
        if (unlikely(ext_header != MP2VX_ID_SEQX)) {
            /* if extensions are in use, we are in MPEG-2 mode, and therefore
             * we must have a sequence extension */
            ubuf_free(sequence_header);
            upipe_err_va(upipe, "wrong header extension %"PRIu8, ext_header);
            return false;
        }

        sequence_ext = upipe_mp2vf_extract_extension(upipe, uref, &ext_offset);
        if (unlikely(sequence_ext == NULL)) {
            ubuf_free(sequence_header);
            return false;
        }

        if (upipe_mp2vf_find_ext(upipe, uref, &ext_offset, &ext_header) &&
            ext_header == MP2VX_ID_SEQDX) {
            sequence_display = upipe_mp2vf_extract_display(upipe, uref,
                                                           &ext_offset);
            if (unlikely(sequence_display == NULL)) {
                ubuf_free(sequence_header);
                ubuf_free(sequence_ext);
                return false;
            }
        }
    }

    if (likely(upipe_mp2vf->sequence_header != NULL &&
               ubuf_block_compare(sequence_header,
                                  upipe_mp2vf->sequence_header) &&
               ((upipe_mp2vf->sequence_ext == NULL && sequence_ext == NULL) ||
                (upipe_mp2vf->sequence_ext != NULL && sequence_ext != NULL &&
                 ubuf_block_compare(sequence_ext,
                                    upipe_mp2vf->sequence_ext))) &&
               ((upipe_mp2vf->sequence_display == NULL &&
                 sequence_display == NULL) ||
                (upipe_mp2vf->sequence_display != NULL &&
                 sequence_display != NULL &&
                 ubuf_block_compare(sequence_display,
                                    upipe_mp2vf->sequence_display))))) {
        /* identical sequence header, extension and display, but we rotate them
         * to free older buffers */
        ubuf_free(upipe_mp2vf->sequence_header);
        if (upipe_mp2vf->sequence_ext != NULL)
            ubuf_free(upipe_mp2vf->sequence_ext);
        if (upipe_mp2vf->sequence_display != NULL)
            ubuf_free(upipe_mp2vf->sequence_display);
        upipe_mp2vf->sequence_header = sequence_header;
        upipe_mp2vf->sequence_ext = sequence_ext;
        upipe_mp2vf->sequence_display = sequence_display;
        return true;
    }

    if (upipe_mp2vf->sequence_header != NULL)
        ubuf_free(upipe_mp2vf->sequence_header);
    if (upipe_mp2vf->sequence_ext != NULL)
        ubuf_free(upipe_mp2vf->sequence_ext);
    if (upipe_mp2vf->sequence_display != NULL)
        ubuf_free(upipe_mp2vf->sequence_display);
    upipe_mp2vf->sequence_header = sequence_header;
    upipe_mp2vf->sequence_ext = sequence_ext;
    upipe_mp2vf->sequence_display = sequence_display;

    return upipe_mp2vf_parse_sequence(upipe);
}

/** @internal @This parses a new picture header, and outputs a flow
 * definition
 *
 * @param upipe description structure of the pipe
 * @param uref uref containing a frame
 * @return false in case of error
 */
static bool upipe_mp2vf_parse_picture(struct upipe *upipe, struct uref *uref)
{
    struct upipe_mp2vf *upipe_mp2vf = upipe_mp2vf_from_upipe(upipe);
    bool closedgop = false;
    bool brokenlink = false;
    if (upipe_mp2vf->next_frame_offset) {
        /* there is some header in front, there may be a GOP header */
        size_t gop_offset = 0;
        if (uref_block_find(uref, &gop_offset, FIND_GOP)) {
            uint8_t gop_buffer[MP2VGOP_HEADER_SIZE];
            const uint8_t *gop;
            if (unlikely((gop = uref_block_peek(uref, gop_offset,
                                                MP2VGOP_HEADER_SIZE,
                                                gop_buffer)) == NULL)) {
                upipe_throw_aerror(upipe);
                return false;
            }
            closedgop = mp2vgop_get_closedgop(gop);
            brokenlink = mp2vgop_get_brokenlink(gop);
            if (unlikely(!uref_block_peek_unmap(uref, gop_offset,
                                                MP2VGOP_HEADER_SIZE, gop_buffer,
                                                gop))) {
                upipe_throw_aerror(upipe);
                return false;
            }
            upipe_mp2vf->last_temporal_reference = -1;
        }
    }

    if ((brokenlink || (!closedgop && upipe_mp2vf->got_discontinuity)) &&
        !uref_flow_set_discontinuity(uref)) {
        upipe_throw_aerror(upipe);
        return false;
    }

    uint8_t picture_buffer[MP2VPIC_HEADER_SIZE];
    const uint8_t *picture;
    if (unlikely((picture = uref_block_peek(uref,
                                            upipe_mp2vf->next_frame_offset,
                                            MP2VPIC_HEADER_SIZE,
                                            picture_buffer)) == NULL)) {
        upipe_throw_aerror(upipe);
        return false;
    }
    uint16_t temporalreference = mp2vpic_get_temporalreference(picture);
    uint8_t codingtype = mp2vpic_get_codingtype(picture);
    uint16_t vbvdelay = mp2vpic_get_vbvdelay(picture);
    if (unlikely(!uref_block_peek_unmap(uref, upipe_mp2vf->next_frame_offset,
                                        MP2VPIC_HEADER_SIZE, picture_buffer,
                                        picture))) {
        upipe_throw_aerror(upipe);
        return false;
    }

    uint64_t picture_number = upipe_mp2vf->last_picture_number +
        (temporalreference - upipe_mp2vf->last_temporal_reference);
    if (temporalreference > upipe_mp2vf->last_temporal_reference) {
        upipe_mp2vf->last_temporal_reference = temporalreference;
        upipe_mp2vf->last_picture_number = picture_number;
    }
    if (unlikely(!uref_pic_set_number(uref, picture_number) ||
                 !uref_mp2v_set_type(uref, codingtype) ||
                 (vbvdelay != UINT16_MAX && !uref_clock_set_vbv_delay(uref,
                      (uint64_t)vbvdelay * UCLOCK_FREQ / 90000)))) {
        upipe_throw_aerror(upipe);
        return false;
    }

    size_t ext_offset = upipe_mp2vf->next_frame_offset + MP2VPIC_HEADER_SIZE;
    uint8_t ext_header;
    uint64_t duration = UCLOCK_FREQ * upipe_mp2vf->fps.den /
                                      upipe_mp2vf->fps.num;
    if (upipe_mp2vf_find_ext(upipe, uref, &ext_offset, &ext_header)) {
        if (unlikely(ext_header != MP2VX_ID_PICX)) {
            /* if extensions are in use, we are in MPEG-2 mode, and therefore
             * we must have a picture extension */
            upipe_err_va(upipe, "wrong header extension %"PRIu8, ext_header);
            return false;
        }

        uint8_t ext_buffer[MP2VPICX_HEADER_SIZE];
        const uint8_t *ext;
        if (unlikely((ext = uref_block_peek(uref, ext_offset,
                                            MP2VPICX_HEADER_SIZE,
                                            ext_buffer)) == NULL)) {
            upipe_throw_aerror(upipe);
            return false;
        }
        uint8_t intradc = mp2vpicx_get_intradc(ext);
        uint8_t structure = mp2vpicx_get_structure(ext);
        bool tff = mp2vpicx_get_tff(ext);
        bool rff = mp2vpicx_get_rff(ext);
        bool progressive = mp2vpicx_get_progressive(ext);
        if (unlikely(!uref_block_peek_unmap(uref, ext_offset,
                                            MP2VPICX_HEADER_SIZE, ext_buffer,
                                            ext))) {
            upipe_throw_aerror(upipe);
            return false;
        }

        if (intradc != 0)
            upipe_warn_va(upipe, "bit depth %"PRIu8" is possibly not supported",
                          intradc + 8);

        if (upipe_mp2vf->progressive_sequence) {
            if (rff)
                duration *= 1 + tff;
        } else {
            if (structure == MP2VPICX_FRAME_PICTURE) {
                if (rff)
                    duration += duration / 2;
            } else
                duration /= 2;
        }

        if (unlikely(((structure & MP2VPICX_TOP_FIELD) &&
                      !uref_pic_set_tf(uref)) ||
                     ((structure & MP2VPICX_BOTTOM_FIELD) &&
                      !uref_pic_set_bf(uref)) ||
                     (tff && !uref_pic_set_tff(uref)) ||
                     (!uref_clock_set_duration(uref, duration)) ||
                     (progressive && !uref_pic_set_progressive(uref)))) {
            upipe_throw_aerror(upipe);
            return false;
        }
    }

    bool ret = true;
#define SET_TIMESTAMP(name)                                                 \
    if (upipe_mp2vf->next_frame_##name != UINT64_MAX)                       \
        ret = ret &&                                                        \
              uref_clock_set_##name(uref, upipe_mp2vf->next_frame_##name);  \
    else                                                                    \
        uref_clock_delete_##name(uref);
    SET_TIMESTAMP(pts_orig)
    SET_TIMESTAMP(pts)
    SET_TIMESTAMP(pts_sys)
    SET_TIMESTAMP(dts_orig)
    SET_TIMESTAMP(dts)
    SET_TIMESTAMP(dts_sys)
#undef SET_TIMESTAMP
    upipe_mp2vf_flush_pts(upipe);
    upipe_mp2vf_increment_dts(upipe, duration);

    if (!ret) {
        uref_free(uref);
        upipe_throw_aerror(upipe);
        return true;
    }

    return true;
}

/** @internal @This handles a uref containing a picture header.
 *
 * @param upipe description structure of the pipe
 * @param uref uref containing a frame
 * @return false in case of error
 */
static bool upipe_mp2vf_handle_picture(struct upipe *upipe, struct uref *uref)
{
    if (unlikely(!upipe_mp2vf_parse_picture(upipe, uref)))
        return false;

    uint8_t type;
    if (!uref_mp2v_get_type(uref, &type))
        return false;

    if (type == MP2VPIC_TYPE_I) {
        struct upipe_mp2vf *upipe_mp2vf = upipe_mp2vf_from_upipe(upipe);
        uint64_t systime_rap = UINT64_MAX;
        uref_clock_get_systime_rap(uref, &systime_rap);

        if (upipe_mp2vf->next_frame_sequence) {
            uref_flow_set_random(uref);
            upipe_mp2vf->systime_rap = systime_rap;
        } else if (upipe_mp2vf->insert_sequence) {
            struct ubuf *ubuf;
            if (upipe_mp2vf->sequence_display != NULL) {
                ubuf = ubuf_dup(upipe_mp2vf->sequence_display);
                if (unlikely(ubuf == NULL)) {
                    upipe_throw_aerror(upipe);
                    return false;
                }
                uref_block_insert(uref, 0, ubuf);
            }
            if (upipe_mp2vf->sequence_ext != NULL) {
                ubuf = ubuf_dup(upipe_mp2vf->sequence_ext);
                if (unlikely(ubuf == NULL)) {
                    upipe_throw_aerror(upipe);
                    return false;
                }
                uref_block_insert(uref, 0, ubuf);
            }
            ubuf = ubuf_dup(upipe_mp2vf->sequence_header);
            if (unlikely(ubuf == NULL)) {
                upipe_throw_aerror(upipe);
                return false;
            }
            uref_block_insert(uref, 0, ubuf);
            uref_flow_set_random(uref);
            upipe_mp2vf->systime_rap = systime_rap;
        }
    }
    return true;
}

/** @internal @This handles and outputs a frame.
 *
 * @param upipe description structure of the pipe
 * @param upump pump that generated the buffer
 * @return false if the stream needs to be resync'd
 */
static bool upipe_mp2vf_output_frame(struct upipe *upipe, struct upump *upump)
{
    struct upipe_mp2vf *upipe_mp2vf = upipe_mp2vf_from_upipe(upipe);
    struct uref *uref = uref_dup(upipe_mp2vf->next_uref);
    if (unlikely(uref == NULL ||
                 !uref_block_resize(uref, 0,
                                    upipe_mp2vf->next_frame_size))) {
        upipe_throw_aerror(upipe);
        return true;
    }

    if (upipe_mp2vf->next_frame_sequence) {
        if (unlikely(!upipe_mp2vf_handle_sequence(upipe, uref))) {
            uref_free(uref);
            return false;
        }
    }

    if (unlikely(!upipe_mp2vf_handle_picture(upipe, uref))) {
        uref_free(uref);
        return false;
    }

    if (upipe_mp2vf->systime_rap != UINT64_MAX)
        uref_clock_set_systime_rap(uref, upipe_mp2vf->systime_rap);
    upipe_mp2vf_output(upipe, uref, upump);
    return true;
}

/** @internal @This is called back by @ref upipe_mp2vf_append_octet_stream
 * whenever a new uref is promoted in next_uref.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_mp2vf_promote_uref(struct upipe *upipe)
{
    struct upipe_mp2vf *upipe_mp2vf = upipe_mp2vf_from_upipe(upipe);
    uint64_t ts;
#define SET_TIMESTAMP(name)                                                 \
    if (uref_clock_get_##name(upipe_mp2vf->next_uref, &ts))                 \
        upipe_mp2vf->next_frame_##name = ts;
    SET_TIMESTAMP(pts_orig)
    SET_TIMESTAMP(pts)
    SET_TIMESTAMP(pts_sys)
    SET_TIMESTAMP(dts_orig)
    SET_TIMESTAMP(dts)
    SET_TIMESTAMP(dts_sys)
#undef SET_TIMESTAMP
}

/** @internal @This tries to output frames from the queue of input buffers.
 *
 * @param upipe description structure of the pipe
 * @param upump pump that generated the buffer
 */
static void upipe_mp2vf_work(struct upipe *upipe, struct upump *upump)
{
    struct upipe_mp2vf *upipe_mp2vf = upipe_mp2vf_from_upipe(upipe);
    while (upipe_mp2vf->next_uref != NULL) {
        uint8_t start;
        if (!upipe_mp2vf_find(upipe, &start))
            return;

        if (unlikely(!upipe_mp2vf->acquired)) {
            upipe_mp2vf_consume_octet_stream(upipe,
                                             upipe_mp2vf->next_frame_size);
            upipe_mp2vf->next_frame_size = 0;

            switch (start) {
                case MP2VPIC_START_CODE:
                    upipe_mp2vf_flush_pts(upipe);
                    upipe_mp2vf_flush_dts(upipe);
                    break;
                case MP2VSEQ_START_CODE:
                    upipe_mp2vf_sync_acquired(upipe);
                    upipe_mp2vf->next_frame_sequence = true;
                    break;
                default:
                    break;
            }
            upipe_mp2vf->next_frame_size += 4;
            continue;
        }

        if (unlikely(upipe_mp2vf->next_frame_offset == -1)) {
            if (start == MP2VPIC_START_CODE)
                upipe_mp2vf->next_frame_offset = upipe_mp2vf->next_frame_size;
            upipe_mp2vf->next_frame_size += 4;
            continue;
        }

        if (start == MP2VX_START_CODE) {
            upipe_mp2vf->next_frame_size += 4;
            continue;
        }

        if (start > MP2VPIC_START_CODE && start <= MP2VPIC_LAST_CODE) {
            /* slice header */
            upipe_mp2vf->next_frame_slice = true;
            upipe_mp2vf->next_frame_size += 4;
            continue;
        }

        if (start == MP2VEND_START_CODE)
            upipe_mp2vf->next_frame_size += 4;

        if (unlikely(!upipe_mp2vf_output_frame(upipe, upump))) {
            upipe_warn(upipe, "erroneous frame headers");
            upipe_mp2vf_consume_octet_stream(upipe,
                                             upipe_mp2vf->next_frame_size);
            upipe_mp2vf->next_frame_size = 0;
            upipe_mp2vf_sync_lost(upipe);
            upipe_mp2vf->next_frame_sequence = false;
            upipe_mp2vf->next_frame_offset = -1;
            upipe_mp2vf->next_frame_slice = false;
            continue;
        }
        upipe_mp2vf_consume_octet_stream(upipe, upipe_mp2vf->next_frame_size);
        upipe_mp2vf->next_frame_sequence = false;
        upipe_mp2vf->next_frame_offset = -1;
        upipe_mp2vf->next_frame_slice = false;
        upipe_mp2vf->next_frame_size = 4;
        switch (start) {
            case MP2VSEQ_START_CODE:
                upipe_mp2vf->next_frame_sequence = true;
                break;
            case MP2VGOP_START_CODE:
                break;
            case MP2VPIC_START_CODE:
                upipe_mp2vf->next_frame_offset = 0;
                break;
            case MP2VEND_START_CODE:
                upipe_mp2vf->next_frame_size = 0;
                /* intended pass-through */
            default:
                upipe_mp2vf_sync_lost(upipe);
                break;
        }
    }
}

/** @internal @This receives data.
 *
 * @param upipe description structure of the pipe
 * @param uref uref structure
 * @param upump pump that generated the buffer
 */
static void upipe_mp2vf_input(struct upipe *upipe, struct uref *uref,
                              struct upump *upump)
{
    struct upipe_mp2vf *upipe_mp2vf = upipe_mp2vf_from_upipe(upipe);
    const char *def;
    if (unlikely(uref_flow_get_def(uref, &def))) {
        if (unlikely(ubase_ncmp(def, EXPECTED_FLOW_DEF))) {
            uref_free(uref);
            if (upipe_mp2vf->flow_def_input != NULL) {
                uref_free(upipe_mp2vf->flow_def_input);
                upipe_mp2vf->flow_def_input = NULL;
            }
            upipe_mp2vf_store_flow_def(upipe, NULL);
            upipe_throw_flow_def_error(upipe, uref);
            return;
        }

        upipe_dbg_va(upipe, "flow definition: %s", def);
        upipe_mp2vf->flow_def_input = uref;
        if (upipe_mp2vf->sequence_header != NULL)
            upipe_mp2vf_parse_sequence(upipe);
        return;
    }

    if (unlikely(upipe_mp2vf->flow_def_input == NULL)) {
        uref_free(uref);
        upipe_throw_flow_def_error(upipe, uref);
        return;
    }

    if (unlikely(uref->ubuf == NULL)) {
        uref_free(uref);
        return;
    }

    if (unlikely(uref_flow_get_discontinuity(uref))) {
        if (!upipe_mp2vf->next_frame_slice) {
            /* we do not want discontinuities in the headers before the first
             * slice header; inside the slices it is less destructive */
            upipe_mp2vf_clean_octet_stream(upipe);
            upipe_mp2vf_init_octet_stream(upipe);
            upipe_mp2vf->got_discontinuity = true;
        } else
            uref_flow_set_error(upipe_mp2vf->next_uref);
    }

    upipe_mp2vf_append_octet_stream(upipe, uref);
    upipe_mp2vf_work(upipe, upump);
}

/** @This returns the current setting for sequence header insertion.
 *
 * @param upipe description structure of the pipe
 * @param val_p filled with the current setting
 * @return false in case of error
 */
static bool _upipe_mp2vf_get_sequence_insertion(struct upipe *upipe,
                                                int *val_p)
{
    struct upipe_mp2vf *upipe_mp2vf = upipe_mp2vf_from_upipe(upipe);
    *val_p = upipe_mp2vf->insert_sequence ? 1 : 0;
    return true;
}

/** @This sets or unsets the sequence header insertion. When true, a sequence
 * headers is inserted in front of every I frame if it is missing, as per
 * ISO-13818-2 specification.
 *
 * @param upipe description structure of the pipe
 * @param val true for sequence header insertion
 * @return false in case of error
 */
static bool _upipe_mp2vf_set_sequence_insertion(struct upipe *upipe,
                                                int val)
{
    struct upipe_mp2vf *upipe_mp2vf = upipe_mp2vf_from_upipe(upipe);
    upipe_mp2vf->insert_sequence = !!val;
    return true;
}

/** @internal @This processes control commands on a mp2vf pipe.
 *
 * @param upipe description structure of the pipe
 * @param command type of command to process
 * @param args arguments of the command
 * @return false in case of error
 */
static bool upipe_mp2vf_control(struct upipe *upipe,
                                enum upipe_command command, va_list args)
{
    switch (command) {
        case UPIPE_GET_OUTPUT: {
            struct upipe **p = va_arg(args, struct upipe **);
            return upipe_mp2vf_get_output(upipe, p);
        }
        case UPIPE_SET_OUTPUT: {
            struct upipe *output = va_arg(args, struct upipe *);
            return upipe_mp2vf_set_output(upipe, output);
        }

        case UPIPE_MP2VF_GET_SEQUENCE_INSERTION: {
            unsigned int signature = va_arg(args, unsigned int);
            assert(signature == UPIPE_MP2VF_SIGNATURE);
            int *val_p = va_arg(args, int *);
            return _upipe_mp2vf_get_sequence_insertion(upipe, val_p);
        }
        case UPIPE_MP2VF_SET_SEQUENCE_INSERTION: {
            unsigned int signature = va_arg(args, unsigned int);
            assert(signature == UPIPE_MP2VF_SIGNATURE);
            int val = va_arg(args, int);
            return _upipe_mp2vf_set_sequence_insertion(upipe, val);
        }
        default:
            return false;
    }
}

/** @This increments the reference count of a upipe.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_mp2vf_use(struct upipe *upipe)
{
    struct upipe_mp2vf *upipe_mp2vf = upipe_mp2vf_from_upipe(upipe);
    urefcount_use(&upipe_mp2vf->refcount);
}

/** @This decrements the reference count of a upipe or frees it.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_mp2vf_release(struct upipe *upipe)
{
    struct upipe_mp2vf *upipe_mp2vf = upipe_mp2vf_from_upipe(upipe);
    if (unlikely(urefcount_release(&upipe_mp2vf->refcount))) {
        upipe_throw_dead(upipe);

        upipe_mp2vf_clean_octet_stream(upipe);
        upipe_mp2vf_clean_output(upipe);
        upipe_mp2vf_clean_sync(upipe);

        if (upipe_mp2vf->flow_def_input != NULL)
            uref_free(upipe_mp2vf->flow_def_input);
        if (upipe_mp2vf->sequence_header != NULL)
            ubuf_free(upipe_mp2vf->sequence_header);
        if (upipe_mp2vf->sequence_ext != NULL)
            ubuf_free(upipe_mp2vf->sequence_ext);
        if (upipe_mp2vf->sequence_display != NULL)
            ubuf_free(upipe_mp2vf->sequence_display);

        upipe_clean(upipe);
        urefcount_clean(&upipe_mp2vf->refcount);
        free(upipe_mp2vf);
    }
}

/** module manager static descriptor */
static struct upipe_mgr upipe_mp2vf_mgr = {
    .signature = UPIPE_MP2VF_SIGNATURE,

    .upipe_alloc = upipe_mp2vf_alloc,
    .upipe_input = upipe_mp2vf_input,
    .upipe_control = upipe_mp2vf_control,
    .upipe_use = upipe_mp2vf_use,
    .upipe_release = upipe_mp2vf_release,

    .upipe_mgr_use = NULL,
    .upipe_mgr_release = NULL
};

/** @This returns the management structure for all mp2vf pipes.
 *
 * @return pointer to manager
 */
struct upipe_mgr *upipe_mp2vf_mgr_alloc(void)
{
    return &upipe_mp2vf_mgr;
}