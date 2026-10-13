/**
 * @file   vec.h
 *
 * @section DESCRIPTION
 *
 * Minimal growable array of uintptr_t, used by the transactions to remember
 * the words they accessed and the segments they (de)allocated.
 *
 * An all-zero struct vec is a valid empty array.
**/

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define VEC_INITIAL_CAPACITY 16

struct vec {
    uintptr_t* items; // Elements (on the heap), NULL while empty
    size_t len;       // Number of elements stored
    size_t cap;       // Number of elements that fit before growing
};

/** Append an element, doubling the capacity when full.
 * @param vec   Array to append to
 * @param value Element to append
 * @return Whether the operation was a success (false: out of memory, array unchanged)
**/
static inline bool vec_push(struct vec* vec, uintptr_t value) {
    if (vec->len == vec->cap) {
        size_t cap = vec->cap == 0 ? VEC_INITIAL_CAPACITY : 2 * vec->cap;
        uintptr_t* items = realloc(vec->items, cap * sizeof(uintptr_t));
        if (!items)
            return false;
        vec->items = items;
        vec->cap   = cap;
    }
    vec->items[vec->len++] = value;
    return true;
}

/** Free the elements of an array, leaving it empty (and reusable).
 * @param vec Array to free
**/
static inline void vec_free(struct vec* vec) {
    free(vec->items);
    vec->items = NULL;
    vec->len   = 0;
    vec->cap   = 0;
}
