/*
 * Copyright (C) 2012 OpenHeadend S.A.R.L.
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
 * @short Upipe inline manager of dictionary of attributes
 * This manager stores all attributes inline inside a single umem block.
 * This is designed in order to minimize calls to memory allocators, and
 * to transmit dictionaries over streams.
 */

#include <upipe/ubase.h>
#include <upipe/urefcount.h>
#include <upipe/ulifo.h>
#include <upipe/umem.h>
#include <upipe/udict.h>
#include <upipe/udict_inline.h>

#include <stdlib.h>
#include <assert.h>

/** default minimal size of the dictionary */
#define UDICT_MIN_SIZE 128
/** default extra space added on udict expansion */
#define UDICT_EXTRA_SIZE 64

/** @hidden */
static void udict_inline_free_inner(struct udict *udict);

/** @internal @This represents a shorthand attribute type. */
struct inline_shorthand {
    const char *name;
    enum udict_type base_type;
};

/** @This stores a list of inline_shorthands attributes.
 *
 * Please note that the code expects the first line to be
 * UDICT_TYPE_SHORTHAND + 1.
 */
static const struct inline_shorthand inline_shorthands[] = {
    { "f.disc", UDICT_TYPE_VOID },
    { "f.random", UDICT_TYPE_VOID },
    { "f.error", UDICT_TYPE_VOID },
    { "f.def", UDICT_TYPE_STRING },
    { "f.rawdef", UDICT_TYPE_STRING },
    { "f.program", UDICT_TYPE_STRING },
    { "f.lang", UDICT_TYPE_STRING },

    { "k.systime", UDICT_TYPE_UNSIGNED },
    { "k.systime.rap", UDICT_TYPE_UNSIGNED },
    { "k.pts", UDICT_TYPE_UNSIGNED },
    { "k.pts.orig", UDICT_TYPE_UNSIGNED },
    { "k.pts.sys", UDICT_TYPE_UNSIGNED },
    { "k.dts", UDICT_TYPE_UNSIGNED },
    { "k.dts.orig", UDICT_TYPE_UNSIGNED },
    { "k.dts.sys", UDICT_TYPE_UNSIGNED },
    { "k.vbvdelay", UDICT_TYPE_UNSIGNED },
    { "k.duration", UDICT_TYPE_UNSIGNED },

    { "b.start", UDICT_TYPE_VOID },
    { "b.end", UDICT_TYPE_VOID },

    { "p.num", UDICT_TYPE_UNSIGNED },
    { "p.hsize", UDICT_TYPE_UNSIGNED },
    { "p.vsize", UDICT_TYPE_UNSIGNED },
    { "p.hsizevis", UDICT_TYPE_UNSIGNED },
    { "p.vsizevis", UDICT_TYPE_UNSIGNED },
    { "p.hposition", UDICT_TYPE_UNSIGNED },
    { "p.vposition", UDICT_TYPE_UNSIGNED },
    { "p.aspect", UDICT_TYPE_RATIONAL },
    { "p.progressive", UDICT_TYPE_VOID },
    { "p.tf", UDICT_TYPE_VOID },
    { "p.bf", UDICT_TYPE_VOID },
    { "p.tff", UDICT_TYPE_VOID },
};

/** @This stores the size of the value of basic attribute types. */
static const size_t attr_sizes[] = { 0, 0, 0, 0, 1, 1, 1, 8, 8, 16, 8 };

/** super-set of the udict_mgr structure with additional local members */
struct udict_inline_mgr {
    /** minimum space at allocation */
    size_t min_size;
    /** extra space added when the umem is expanded */
    size_t extra_size;

    /** udict pool */
    struct ulifo udict_pool;
    /** umem allocator */
    struct umem_mgr *umem_mgr;

    /** common management structure */
    struct udict_mgr mgr;
};

/** super-set of the udict structure with additional local members */
struct udict_inline {
    /** umem structure pointing to buffer */
    struct umem umem;
    /** used size */
    size_t size;

    /** common structure */
    struct udict udict;
};

/** @internal @This returns the high-level udict structure.
 *
 * @param inl pointer to the udict_inline structure
 * @return pointer to the udict structure
 */
static inline struct udict *udict_inline_to_udict(struct udict_inline *inl)
{
    return &inl->udict;
}

/** @internal @This returns the private udict_inline structure.
 *
 * @param mgr description structure of the udict mgr
 * @return pointer to the udict_inline structure
 */
static inline struct udict_inline *udict_inline_from_udict(struct udict *udict)
{
    return container_of(udict, struct udict_inline, udict);
}

/** @internal @This returns the high-level udict_mgr structure.
 *
 * @param inline_mgr pointer to the udict_inline_mgr structure
 * @return pointer to the udict_mgr structure
 */
static inline struct udict_mgr *udict_inline_mgr_to_udict_mgr(struct udict_inline_mgr *inline_mgr)
{
    return &inline_mgr->mgr;
}

/** @internal @This returns the private udict_inline_mgr structure.
 *
 * @param mgr description structure of the udict mgr
 * @return pointer to the udict_inline_mgr structure
 */
static inline struct udict_inline_mgr *udict_inline_mgr_from_udict_mgr(struct udict_mgr *mgr)
{
    return container_of(mgr, struct udict_inline_mgr, mgr);
}

/** @This allocates a udict with attributes space.
 *
 * @param mgr common management structure
 * @param size initial size of the attribute space
 * @return pointer to udict or NULL in case of allocation error
 */
static struct udict *udict_inline_alloc(struct udict_mgr *mgr, size_t size)
{
    struct udict_inline_mgr *inline_mgr = udict_inline_mgr_from_udict_mgr(mgr);
    struct udict *udict = ulifo_pop(&inline_mgr->udict_pool, struct udict *);
    struct udict_inline *inl;
    if (unlikely(udict == NULL)) {
        inl = malloc(sizeof(struct udict_inline));
        if (unlikely(inl == NULL))
            return NULL;
        udict = udict_inline_to_udict(inl);
        inl->udict.mgr = mgr;
    } else
        inl = udict_inline_from_udict(udict);

    if (size < inline_mgr->min_size)
        size = inline_mgr->min_size;
    if (unlikely(!umem_alloc(inline_mgr->umem_mgr, &inl->umem, size))) {
        if (unlikely(!ulifo_push(&inline_mgr->udict_pool, udict)))
            udict_inline_free_inner(udict);
        return NULL;
    }

    uint8_t *buffer = umem_buffer(&inl->umem);
    buffer[0] = UDICT_TYPE_END;
    inl->size = 1;

    udict_mgr_use(mgr);
    return udict;
}

/** @This duplicates a given udict.
 *
 * @param udict pointer to udict
 * @param new_udict_p reference written with a pointer to the newly allocated
 * udict
 * @return false in case of error
 */
static bool udict_inline_dup(struct udict *udict, struct udict **new_udict_p)
{
    assert(new_udict_p != NULL);
    struct udict_inline *inl = udict_inline_from_udict(udict);
    struct udict *new_udict = udict_inline_alloc(udict->mgr, inl->size);
    if (unlikely(new_udict == NULL))
        return false;

    *new_udict_p = new_udict;

    struct udict_inline *new_inl = udict_inline_from_udict(new_udict);
    memcpy(umem_buffer(&new_inl->umem), umem_buffer(&inl->umem), inl->size);
    new_inl->size = inl->size;
    return true;
}

/** @internal @This looks up a shorthand attribute in the list of shorthands.
 *
 * @param type shorthand attribute
 * @return pointer to the found shorthand entry, or NULL
 */
static const struct inline_shorthand *
    udict_inline_shorthand(enum udict_type type)
{
    if (unlikely(type > UDICT_TYPE_SHORTHAND + 1 + sizeof(inline_shorthands) /
                                               sizeof(struct inline_shorthand)))
        return NULL;
    return &inline_shorthands[type - UDICT_TYPE_SHORTHAND - 1];
}

/** @internal @This jumps to the next attribute.
 *
 * @param attr attribute to iterate from
 * @return pointer to the next valid attribute, or NULL
 */
static uint8_t *udict_inline_next(uint8_t *attr)
{
    if (*attr == UDICT_TYPE_END)
        return NULL;

    if (likely(*attr > UDICT_TYPE_SHORTHAND)) {
        const struct inline_shorthand *shorthand =
            udict_inline_shorthand(*attr);
        if (unlikely(shorthand == NULL))
            return NULL;
        if (shorthand->base_type != UDICT_TYPE_OPAQUE &&
            shorthand->base_type != UDICT_TYPE_STRING)
            return attr + attr_sizes[shorthand->base_type] + 1;
    }

    uint16_t size = (attr[1] << 8) | attr[2];
    return attr + 3 + size;
}

/** @internal @This finds an attribute (shorthand or not) of the given name
 * and type and returns a pointer to its beginning.
 *
 * @param udict pointer to the udict
 * @param name name of the attribute
 * @param type type of the attribute (excluding inline_shorthands)
 * @return pointer to the attribute, or NULL
 */
static uint8_t *udict_inline_find(struct udict *udict, const char *name,
                                   enum udict_type type)
{
    struct udict_inline *inl = udict_inline_from_udict(udict);
    uint8_t *attr = umem_buffer(&inl->umem);
    while (attr != NULL) {
        if (*attr == type &&
             (type > UDICT_TYPE_SHORTHAND || type == UDICT_TYPE_END ||
              !strcmp((const char *)(attr + 3), name)))
            return attr;
        attr = udict_inline_next(attr);
    }
    return NULL;
}

/** @internal @This finds an attribute (shorthand or not) of the given name
 * and type and returns the name and type of the next attribute.
 *
 * @param udict pointer to the udict
 * @param name_p reference to the name of the attribute to find, changed during
 * execution to the name of the next attribute, or NULL if it is a shorthand
 * @param type_p reference to the type of the attribute, changed to
 * UDICT_TYPE_END at the end of the iteration; start with UDICT_TYPE_END as well
 */
static void udict_inline_iterate(struct udict *udict, const char **name_p,
                                 enum udict_type *type_p)
{
    assert(name_p != NULL);
    assert(type_p != NULL);
    struct udict_inline *inl = udict_inline_from_udict(udict);
    uint8_t *attr;

    if (likely(*type_p != UDICT_TYPE_END)) {
        attr = udict_inline_find(udict, *name_p, *type_p);
        if (likely(attr != NULL))
            attr = udict_inline_next(attr);
    } else
        attr = umem_buffer(&inl->umem);
    if (unlikely(attr == NULL || *attr == UDICT_TYPE_END)) {
        *type_p = UDICT_TYPE_END;
        return;
    }

    *type_p = *attr;
    *name_p = *attr > UDICT_TYPE_SHORTHAND ? NULL : (const char *)(attr + 3);
}

/** @internal @This finds an attribute (shorthand or not) of the given name
 * and type and returns a pointer to the beginning of its value.
 *
 * @param udict pointer to the udict
 * @param name name of the attribute
 * @param type type of the attribute (excluding inline_shorthands)
 * @param size_p size of the value, written on execution (can be NULL)
 * @return pointer to the value of the found attribute, or NULL
 */
static uint8_t *_udict_inline_get(struct udict *udict, const char *name,
                                  enum udict_type type, size_t *size_p)
{
    uint8_t *attr = udict_inline_find(udict, name, type);
    if (unlikely(attr == NULL))
        return NULL;

    if (likely(type > UDICT_TYPE_SHORTHAND)) {
        const struct inline_shorthand *shorthand =
            udict_inline_shorthand(*attr);
        if (unlikely(shorthand == NULL))
            return NULL;

        if (shorthand->base_type != UDICT_TYPE_OPAQUE &&
            shorthand->base_type != UDICT_TYPE_STRING) {
            if (likely(size_p != NULL))
                *size_p = attr_sizes[shorthand->base_type];
            attr++;
        } else {
            uint16_t size = (attr[1] << 8) | attr[2];
            if (likely(size_p != NULL))
                *size_p = size;
            attr += 3;
        }
    } else {
        uint16_t size = (attr[1] << 8) | attr[2];
        size_t namelen = strlen(name);
        assert(size > namelen);
        if (likely(size_p != NULL))
            *size_p = size - namelen - 1;
        attr += 4 + namelen;
    }
    return attr;
}

/** @internal @This finds an attribute (shorthand or not) of the given name
 * and type and returns a pointer to the beginning of its value (const version).
 *
 * @param udict pointer to the udict
 * @param name name of the attribute
 * @param type type of the attribute (excluding inline_shorthands)
 * @param size_p size of the value, written on execution (can be NULL)
 * @param attr_p pointer to the value of the found attribute, written on
 * execution
 * @return false in case of error
 */
static bool udict_inline_get(struct udict *udict, const char *name,
                             enum udict_type type, size_t *size_p,
                             const uint8_t **attr_p)
{
    uint8_t *attr = _udict_inline_get(udict, name, type, size_p);
    if (unlikely(attr == NULL))
        return false;
    *attr_p = attr;
    return true;
}

/** @internal @This deletes an attribute.
 *
 * @param udict pointer to the udict
 * @param name name of the attribute
 * @param type type of the attribute
 * @return true if the attribute existed before
 */
static bool udict_inline_delete(struct udict *udict, const char *name,
                                enum udict_type type)
{
    assert(type != UDICT_TYPE_END);
    struct udict_inline *inl = udict_inline_from_udict(udict);
    uint8_t *attr = udict_inline_find(udict, name, type);
    if (unlikely(attr == NULL))
        return false;

    uint8_t *end = udict_inline_next(attr);
    memmove(attr, end, umem_buffer(&inl->umem) + inl->size - end);
    inl->size -= end - attr;
    return true;
}

/** @internal @This adds or changes an attribute (excluding the value itself).
 *
 * @param udict pointer to the udict
 * @param name name of the attribute
 * @param type type of the attribute
 * @param attr_size size needed to store the value of the attribute
 * @return pointer to the value of the attribute
 */
static bool udict_inline_set(struct udict *udict, const char *name,
                             enum udict_type type, size_t attr_size,
                             uint8_t **attr_p)
{
    struct udict_inline *inl = udict_inline_from_udict(udict);
    const struct inline_shorthand *shorthand = NULL;
    enum udict_type base_type = type;
    if (likely(type > UDICT_TYPE_SHORTHAND)) {
        shorthand = udict_inline_shorthand(type);
        if (unlikely(shorthand == NULL))
            return false;
        base_type = shorthand->base_type;
    }

    /* check if it already exists */
    size_t current_size;
    uint8_t *attr = _udict_inline_get(udict, name, type, &current_size);
    if (unlikely(attr != NULL)) {
        if ((base_type != UDICT_TYPE_OPAQUE &&
             base_type != UDICT_TYPE_STRING) ||
            current_size == attr_size) {
            *attr_p = attr;
            return true;
        }
        if (likely(base_type == UDICT_TYPE_STRING &&
                   current_size > attr_size)) {
            /* Just zero out superfluous bytes */
            memset(attr + attr_size, 0, current_size - attr_size);
            *attr_p = attr;
            return true;
        }
        udict_inline_delete(udict, name, type);
    }

    /* calculate header size */
    size_t header_size = 1;
    size_t namelen;
    if (likely(shorthand != NULL)) {
        if (base_type == UDICT_TYPE_OPAQUE || base_type == UDICT_TYPE_STRING)
            header_size += 2;
    } else {
        namelen = strlen(name);
        header_size += 2 + namelen + 1;
    }

    /* check total attributes size */
    attr = umem_buffer(&inl->umem) + inl->size - 1;
    size_t total_size = (attr - umem_buffer(&inl->umem)) + header_size +
                        attr_size + 1;
    if (unlikely(total_size >= umem_size(&inl->umem))) {
        struct udict_inline_mgr *inline_mgr =
            udict_inline_mgr_from_udict_mgr(udict->mgr);
        if (unlikely(!umem_realloc(&inl->umem, total_size +
                                               inline_mgr->extra_size)))
            return false;

        attr = umem_buffer(&inl->umem) + inl->size - 1;
    }
    assert(*attr == UDICT_TYPE_END);

    /* write attribute header */
    if (unlikely(shorthand == NULL)) {
        assert(namelen + 1 + attr_size <= UINT16_MAX);
        uint16_t size = namelen + 1 + attr_size;
        *attr++ = type;
        *attr++ = size >> 8;
        *attr++ = size & 0xff;
        memcpy(attr, name, namelen + 1);
        attr += namelen + 1;
   } else if (shorthand->base_type == UDICT_TYPE_OPAQUE ||
              shorthand->base_type == UDICT_TYPE_STRING) {
        assert(attr_size <= UINT16_MAX);
        uint16_t size = attr_size;
        *attr++ = type;
        *attr++ = size >> 8;
        *attr++ = size & 0xff;
   } else
        *attr++ = type;

    attr[attr_size] = UDICT_TYPE_END;
    *attr_p = attr;
    inl->size += header_size + attr_size;
    return true;
}

/** @internal @This names a shorthand attribute.
 *
 * @param udict pointer to the udict
 * @param type shorthand type
 * @param name_p filled in with the name of the shorthand attribute
 * @param base_type_p filled in with the base type of the shorthand attribute
 * @return false in case the shorthand doesn't exist
 */
static bool udict_inline_name(struct udict *udict, enum udict_type type,
                              const char **name_p, enum udict_type *base_type_p)
{
    if (type <= UDICT_TYPE_SHORTHAND)
        return false;

    const struct inline_shorthand *shorthand = udict_inline_shorthand(type);
    if (unlikely(shorthand == NULL))
        return false;

    *name_p = shorthand->name;
    *base_type_p = shorthand->base_type;
    return true;
}

/** @This handles control commands.
 *
 * @param udict pointer to udict
 * @param command type of command to process
 * @param args arguments of the command
 * @return false in case of error
 */
static bool udict_inline_control(struct udict *udict,
                                 enum udict_command command, va_list args)
{
    switch (command) {
        case UDICT_DUP: {
            struct udict **udict_p = va_arg(args, struct udict **);
            return udict_inline_dup(udict, udict_p);
        }
        case UDICT_ITERATE: {
            const char **name_p = va_arg(args, const char **);
            enum udict_type *type_p = va_arg(args, enum udict_type *);
            udict_inline_iterate(udict, name_p, type_p);
            return true;
        }
        case UDICT_GET: {
            const char *name = va_arg(args, const char *);
            enum udict_type type = va_arg(args, enum udict_type);
            size_t *size_p = va_arg(args, size_t *);
            const uint8_t **attr_p = va_arg(args, const uint8_t **);
            return udict_inline_get(udict, name, type, size_p, attr_p);
        }
        case UDICT_SET: {
            const char *name = va_arg(args, const char *);
            enum udict_type type = va_arg(args, enum udict_type);
            size_t size = va_arg(args, size_t);
            uint8_t **attr_p = va_arg(args, uint8_t **);
            return udict_inline_set(udict, name, type, size, attr_p);
        }
        case UDICT_DELETE: {
            const char *name = va_arg(args, const char *);
            enum udict_type type = va_arg(args, enum udict_type);
            return udict_inline_delete(udict, name, type);
        }
        case UDICT_NAME: {
            enum udict_type type = va_arg(args, enum udict_type);
            const char **name_p = va_arg(args, const char **);
            enum udict_type *base_type_p = va_arg(args, enum udict_type *);
            return udict_inline_name(udict, type, name_p, base_type_p);
        }
        default:
            return false;
    }
}

/** @internal @This frees a udict and all associated data structures.
 *
 * @param udict pointer to a udict structure to free
 */
static void udict_inline_free_inner(struct udict *udict)
{
    struct udict_inline *inl = udict_inline_from_udict(udict);
    free(inl);
}

/** @This frees a udict.
 *
 * @param udict pointer to a udict structure to free
 */
static void udict_inline_free(struct udict *udict)
{
    struct udict_inline_mgr *inline_mgr =
        udict_inline_mgr_from_udict_mgr(udict->mgr);
    struct udict_inline *inl = udict_inline_from_udict(udict);

    umem_free(&inl->umem);
    if (unlikely(!ulifo_push(&inline_mgr->udict_pool, udict)))
        udict_inline_free_inner(udict);

    udict_mgr_release(&inline_mgr->mgr);
}

/** @This instructs an existing udict manager to release all structures
 * currently kept in pools. It is intended as a debug tool only.
 *
 * @param mgr pointer to udict manager
 */
static void udict_inline_mgr_vacuum(struct udict_mgr *mgr)
{
    struct udict_inline_mgr *inline_mgr = udict_inline_mgr_from_udict_mgr(mgr);
    struct udict *udict;
    while ((udict = ulifo_pop(&inline_mgr->udict_pool, struct udict *)) != NULL)
        udict_inline_free_inner(udict);
}

/** @This frees a udict manager.
 *
 * @param mgr pointer to a udict manager
 */
static void udict_inline_mgr_free(struct udict_mgr *mgr)
{
    struct udict_inline_mgr *inline_mgr = udict_inline_mgr_from_udict_mgr(mgr);
    udict_inline_mgr_vacuum(mgr);
    ulifo_clean(&inline_mgr->udict_pool);
    umem_mgr_release(inline_mgr->umem_mgr);

    urefcount_clean(&inline_mgr->mgr.refcount);
    free(inline_mgr);
}

/** @This allocates a new instance of the inline udict manager.
 *
 * @param udict_pool_depth maximum number of udict structures in the pool
 * @param umem_mgr memory allocator to use for buffers
 * @param min_size minimum allocated space for the udict (if set to -1, a
 * default sensible value is used)
 * @param extra_size extra space added when the udict needs to be resized
 * (if set to -1, a default sensible value is used)
 * @return pointer to manager, or NULL in case of error
 */
struct udict_mgr *udict_inline_mgr_alloc(unsigned int udict_pool_depth,
                                         struct umem_mgr *umem_mgr,
                                         int min_size, int extra_size)
{
    struct udict_inline_mgr *inline_mgr =
        malloc(sizeof(struct udict_inline_mgr) +
               ulifo_sizeof(udict_pool_depth));
    if (unlikely(inline_mgr == NULL))
        return NULL;

    ulifo_init(&inline_mgr->udict_pool, udict_pool_depth,
               (void *)inline_mgr + sizeof(struct udict_inline_mgr));
    inline_mgr->umem_mgr = umem_mgr;
    umem_mgr_use(umem_mgr);

    inline_mgr->min_size = min_size > 0 ? min_size : UDICT_MIN_SIZE;
    inline_mgr->extra_size = extra_size > 0 ? extra_size : UDICT_EXTRA_SIZE;

    urefcount_init(&inline_mgr->mgr.refcount);
    inline_mgr->mgr.udict_alloc = udict_inline_alloc;
    inline_mgr->mgr.udict_control = udict_inline_control;
    inline_mgr->mgr.udict_free = udict_inline_free;
    inline_mgr->mgr.udict_mgr_vacuum = udict_inline_mgr_vacuum;
    inline_mgr->mgr.udict_mgr_free = udict_inline_mgr_free;
    
    return udict_inline_mgr_to_udict_mgr(inline_mgr);
}
