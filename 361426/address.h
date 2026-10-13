/**
 * @file   address.h
 *
 * @section DESCRIPTION
 *
 * Encoding of the "virtual" addresses handed to the user of the STM.
 *
 * The user never dereferences the addresses we return: it only adds offsets to
 * them and passes them back to tm_read/tm_write/tm_free. We can therefore pack
 * into a single 64-bit value both the segment index and the offset (in bytes)
 * inside that segment:
 *
 *   63            48 47                                   0
 *  ┌────────────────┬──────────────────────────────────────┐
 *  │ segment index  │ offset in the segment (in bytes)     │
 *  └────────────────┴──────────────────────────────────────┘
 *
 * Segment index 0 is never used, so that no address is ever NULL. The first
 * segment (the one returned by tm_start) has index 1.
**/

#pragma once

#include <stddef.h>
#include <stdint.h>

#define ADDR_OFFSET_BITS 48
#define ADDR_OFFSET_MASK ((UINT64_C(1) << ADDR_OFFSET_BITS) - 1) // 48 low bits set
#define ADDR_MAX_SEGMENTS (UINT64_C(1) << (64 - ADDR_OFFSET_BITS))  // 2^16 segment indexes

/** Build the address of the byte at 'offset' in segment 'segment'.
 * @param segment Segment index (in [1, ADDR_MAX_SEGMENTS[)
 * @param offset  Offset in bytes inside the segment
 * @return Encoded address
**/
static inline void* addr_encode(uint64_t segment, uint64_t offset) {
    return (void*) (uintptr_t) ((segment << ADDR_OFFSET_BITS) | offset);
}

/** Get the segment index of an encoded address.
 * @param addr Encoded address
 * @return Segment index
**/
static inline uint64_t addr_segment(void const* addr) {
    return (uint64_t) (uintptr_t) addr >> ADDR_OFFSET_BITS;
}

/** Get the offset (in bytes) inside its segment of an encoded address.
 * @param addr Encoded address
 * @return Offset in bytes
**/
static inline uint64_t addr_offset(void const* addr) {
    return (uint64_t) (uintptr_t) addr & ADDR_OFFSET_MASK;
}

/** Get the index of the word an encoded address points to, inside its segment.
 * @param addr  Encoded address
 * @param align Word size (alignment of the region)
 * @return Word index
**/
static inline uint64_t addr_word(void const* addr, size_t align) {
    return addr_offset(addr) / align;
}
