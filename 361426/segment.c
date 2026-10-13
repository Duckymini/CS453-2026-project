/**
 * @file   segment.c
 *
 * @section DESCRIPTION
 *
 * Allocation and deallocation of the shared memory segments.
**/

// Requested features
#define _GNU_SOURCE
#define _POSIX_C_SOURCE   200809L

// External headers
#include <stdalign.h>
#include <stdlib.h>
#include <string.h>

// Internal headers
#include "segment.h"

struct segment* segment_create(size_t size, size_t align) {
    struct segment* segment = malloc(sizeof(struct segment));
    if (!segment)
        return NULL;
    segment->size     = size;
    segment->nb_words = size / align;
    if (align <= alignof(max_align_t)) {
        // calloc is aligned enough, and zeroes large blocks lazily (pages
        // untouched by the transactions are never actually allocated)
        segment->copy_a = calloc(1, size);
        if (!segment->copy_a)
            goto fail_a;
        segment->copy_b = calloc(1, size);
        if (!segment->copy_b)
            goto fail_b;
    } else {
        // posix_memalign requires an alignment that is a multiple of sizeof(void*)
        if (posix_memalign(&(segment->copy_a), align, size) != 0)
            goto fail_a;
        if (posix_memalign(&(segment->copy_b), align, size) != 0)
            goto fail_b;
        memset(segment->copy_a, 0, size);
        memset(segment->copy_b, 0, size);
    }
    // All-zero bytes is a valid zero value for a lock-free atomic integer
    segment->controls = calloc(segment->nb_words, sizeof(_Atomic uint64_t));
    if (!segment->controls)
        goto fail_controls;
    return segment;
fail_controls:
    free(segment->copy_b);
fail_b:
    free(segment->copy_a);
fail_a:
    free(segment);
    return NULL;
}

void segment_free(struct segment* segment) {
    free(segment->controls);
    free(segment->copy_b);
    free(segment->copy_a);
    free(segment);
}
