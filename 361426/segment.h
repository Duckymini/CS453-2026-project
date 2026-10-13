/**
 * @file   segment.h
 *
 * @section DESCRIPTION
 *
 * Shared memory segment of the dual-versioned STM.
 *
 * Every word of a segment is stored twice (copy A and copy B), and has one
 * control word that tells which copy is readable and who accessed the word
 * during the current epoch:
 *
 *   63                                       2      1          0
 *  ┌──────────────────────────────────────────┬─────────┬──────────┐
 *  │ access                                   │ written │ readable │
 *  └──────────────────────────────────────────┴─────────┴──────────┘
 *
 *  - readable: 0 = copy A holds the committed value, 1 = copy B does
 *  - written:  1 if a transaction wrote the word during the current epoch
 *  - access:   CTRL_ACCESS_NONE = no read-write transaction accessed the word,
 *              CTRL_ACCESS_MANY = several did,
 *              otherwise        = id of the only transaction that accessed it
 *
 * A control equal to 0 thus means: copy A readable, not written, no access.
**/

#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CTRL_READABLE_B   (UINT64_C(1) << 0)
#define CTRL_WRITTEN      (UINT64_C(1) << 1)
#define CTRL_ACCESS_SHIFT 2
#define CTRL_ACCESS_NONE  UINT64_C(0)
#define CTRL_ACCESS_MANY  (UINT64_MAX >> CTRL_ACCESS_SHIFT) // all access bits set

struct segment {
    size_t size;                // Size of the segment (in bytes)
    size_t nb_words;            // Number of words (size / align)
    void* copy_a;               // Copy A of every word
    void* copy_b;               // Copy B of every word
    _Atomic uint64_t* controls; // One control per word
};

/** Get whether copy B (rather than copy A) is the readable copy.
 * @param ctrl Control value
 * @return Whether copy B is readable
**/
static inline bool ctrl_readable_b(uint64_t ctrl) {
    return (ctrl & CTRL_READABLE_B) != 0;
}

/** Get whether the word has been written during the current epoch.
 * @param ctrl Control value
 * @return Whether the word was written
**/
static inline bool ctrl_written(uint64_t ctrl) {
    return (ctrl & CTRL_WRITTEN) != 0;
}

/** Get the access field of a control.
 * @param ctrl Control value
 * @return CTRL_ACCESS_NONE, CTRL_ACCESS_MANY or the id of the only accessor
**/
static inline uint64_t ctrl_access(uint64_t ctrl) {
    return ctrl >> CTRL_ACCESS_SHIFT;
}

/** Replace the access field of a control, keeping its readable and written bits.
 * @param ctrl   Control value
 * @param access New access field (CTRL_ACCESS_NONE, CTRL_ACCESS_MANY or a transaction id)
 * @return New control value
**/
static inline uint64_t ctrl_with_access(uint64_t ctrl, uint64_t access) {
    return (ctrl & (CTRL_READABLE_B | CTRL_WRITTEN)) | (access << CTRL_ACCESS_SHIFT);
}

/** Get the control of a word at the start of a new epoch: same readable copy, not written, no access.
 * @param ctrl Control value
 * @return New control value
**/
static inline uint64_t ctrl_reset(uint64_t ctrl) {
    return ctrl & CTRL_READABLE_B;
}

/** Get the address of a word in one of the two copies of a segment.
 * @param segment Segment of the word
 * @param copy_b  Whether to use copy B (rather than copy A)
 * @param word    Index of the word in the segment
 * @param align   Word size of the region
 * @return Address of the word in the requested copy
**/
static inline void* segment_word(struct segment* segment, bool copy_b, size_t word, size_t align) {
    return (char*) (copy_b ? segment->copy_b : segment->copy_a) + word * align;
}

/** Allocate a new segment, with both copies of every word and every control set to zero.
 * @param size  Size of the segment (in bytes), a positive multiple of align
 * @param align Alignment (and word size) of the region
 * @return New segment, NULL on failure
**/
struct segment* segment_create(size_t size, size_t align);

/** Free a segment previously returned by segment_create.
 * @param segment Segment to free
**/
void segment_free(struct segment* segment);
