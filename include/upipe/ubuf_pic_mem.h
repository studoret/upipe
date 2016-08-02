/*
 * Copyright (C) 2012-2016 OpenHeadend S.A.R.L.
 *
 * Authors: Christophe Massiot
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject
 * to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/** @file
 * @short Upipe ubuf manager for picture formats with umem storage
 */

#ifndef _UPIPE_UBUF_PIC_MEM_H_
/** @hidden */
#define _UPIPE_UBUF_PIC_MEM_H_
#ifdef __cplusplus
extern "C" {
#endif

#include <upipe/ubase.h>
#include <upipe/ubuf.h>
#include <upipe/ubuf_pic.h>

#include <stdint.h>
#include <stdbool.h>

/** @This is a simple signature to make sure the ubuf_control internal API
 * is used properly. */
#define UBUF_PIC_MEM_SIGNATURE UBASE_FOURCC('m','e','m','p')

/** @hidden */
struct ubuf_mem_shared;

/** @This extends ubuf_command with specific commands for pic mem manager. */
enum ubuf_pic_mem_command {
    UBUF_PIC_MEM_SENTINEL = UBUF_CONTROL_LOCAL,

    /** returns the shared substructure (const char *,
     * struct ubuf_mem_shared **, size_t *, size_t *) */
    UBUF_PIC_MEM_GET_SHARED
};

/** @This returns the underlying shared buffer. The reference counter is not
 * incremented.
 *
 * @param ubuf pointer to ubuf
 * @param chroma chroma type (see chroma reference)
 * @param shared_p filled in with a pointer to the underlying shared buffer
 * @param offset_p filled in with the offset in the shared buffer for the plane
 * @param size_p filled in with the size of the plane
 * @return an error code
 */
static inline int ubuf_pic_mem_get_shared(struct ubuf *ubuf,
        const char *chroma, struct ubuf_mem_shared **shared_p,
        size_t *offset_p, size_t *size_p)
{
    return ubuf_control(ubuf, UBUF_PIC_MEM_GET_SHARED,
                        UBUF_PIC_MEM_SIGNATURE, chroma, shared_p,
                        offset_p, size_p);
}

/** @This allocates a new instance of the ubuf manager for picture formats
 * using umem.
 *
 * @param ubuf_pool_depth maximum number of ubuf structures in the pool
 * @param shared_pool_depth maximum number of shared structures in the pool
 * @param umem_mgr memory allocator to use for buffers
 * @param macropixel number of pixels in a macropixel (typically 1)
 * @param hprepend extra pixels added before each line (if set to -1, a
 * default sensible value is used)
 * @param happend extra pixels added after each line (if set to -1, a
 * default sensible value is used)
 * @param vprepend extra lines added before buffer (if set to -1, a
 * default sensible value is used)
 * @param vappend extra lines added after buffer (if set to -1, a
 * default sensible value is used)
 * @param align alignment in octets (if set to 0, no line will be voluntarily
 * aligned)
 * @param align_hmoffset horizontal offset of the aligned macropixel in a line
 * (may be negative)
 * @return pointer to manager, or NULL in case of error
 */
struct ubuf_mgr *ubuf_pic_mem_mgr_alloc(uint16_t ubuf_pool_depth,
                                        uint16_t shared_pool_depth,
                                        struct umem_mgr *umem_mgr,
                                        uint8_t macropixel,
                                        int hprepend, int happend,
                                        int vprepend, int vappend,
                                        int align, int align_hmoffset);

/** @This adds a new plane to a ubuf manager for picture formats using umem.
 * It may only be called on initializing the manager, before any ubuf is
 * allocated.
 *
 * @param mgr pointer to a ubuf_mgr structure
 * @param chroma chroma type (see chroma reference)
 * @param hsub horizontal subsamping
 * @param vsub vertical subsamping
 * @param macropixel_size size of a macropixel in octets
 * @return an error code
 */
int ubuf_pic_mem_mgr_add_plane(struct ubuf_mgr *mgr, const char *chroma,
                               uint8_t hsub, uint8_t vsub,
                               uint8_t macropixel_size);

/** @This allocates a new instance of the ubuf manager for picture formats
 * using umem, from a fourcc image format.
 *
 * @param ubuf_pool_depth maximum number of ubuf structures in the pool
 * @param shared_pool_depth maximum number of shared structures in the pool
 * @param umem_mgr memory allocator to use for buffers
 * @param fcc fourcc to use to create the manager
 * @param hmprepend extra macropixels added before each line (if set to -1, a
 * default sensible value is used)
 * @param hmappend extra macropixels added after each line (if set to -1, a
 * default sensible value is used)
 * @param vprepend extra lines added before buffer (if set to -1, a
 * default sensible value is used)
 * @param vappend extra lines added after buffer (if set to -1, a
 * default sensible value is used)
 * @param align alignment in octets (if set to 0, no line will be voluntarily
 * aligned)
 * @param align_hmoffset horizontal offset of the aligned macropixel in a line
 * (may be negative)
 * @return pointer to manager, or NULL in case of error
 */
struct ubuf_mgr *ubuf_pic_mem_mgr_alloc_fourcc(uint16_t ubuf_pool_depth,
                                               uint16_t shared_pool_depth,
                                               struct umem_mgr *umem_mgr,
                                               const char *fcc,
                                               int hmprepend, int hmappend,
                                               int vprepend, int vappend,
                                               int align, int align_hmoffset);

#ifdef __cplusplus
}
#endif
#endif
