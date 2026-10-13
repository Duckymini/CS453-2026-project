/**
 * @file   tm.c
 * @author [Pierre Bernadet]
 *
 * @section LICENSE
 *
 * [...]
 *
 * @section DESCRIPTION
 *
 * Implementation of your own transaction manager.
 * You can completely rewrite this file (and create more files) as you wish.
 * Only the interface (i.e. exported symbols and semantic) must be preserved.
**/

// Requested features
#define _GNU_SOURCE
#define _POSIX_C_SOURCE   200809L
#ifdef __STDC_NO_ATOMICS__
    #error Current C11 compiler does not support atomic operations
#endif

// External headers
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

// Internal headers
#include <tm.h>
#include "address.h"
#include "batcher.h"
#include "macros.h"
#include "segment.h"
#include "vec.h"

struct transaction {
    uint64_t id;                 // Unique id (from 1), 0 for read-only transactions
    bool is_ro;                  // Whether the transaction only reads
    bool committed;              // Set when the transaction ends: committed or aborted
    struct vec written;          // Controls (_Atomic uint64_t*) of the words written, each once
    struct vec touched;          // Controls (_Atomic uint64_t*) of the words read, whose access set was changed
    struct vec allocated;        // Indexes of the segments allocated
    struct vec freed;            // Indexes of the segments to free if committed
    struct transaction* next;    // Next transaction in the region's list of finished transactions
};

struct region {
    size_t size;                                  // Size of the first segment (in bytes)
    size_t align;                                 // Alignment and word size of the region
    struct batcher batcher;                       // Groups the transactions into epochs
    struct segment* segments[ADDR_MAX_SEGMENTS];  // Segment table, indexed by segment index (index 0 unused)
    uint64_t next_segment;                        // Next never-used segment index, for tm_alloc
    uint64_t free_indexes[ADDR_MAX_SEGMENTS];     // Stack of freed segment indexes, reusable by tm_alloc
    size_t nb_free_indexes;                       // Number of indexes in free_indexes
    pthread_mutex_t alloc_lock;                   // Protects next_segment, free_indexes and nb_free_indexes
    _Atomic uint64_t next_tx_id;                  // Next transaction id (ids start at 1, 0 means "nobody")
    struct transaction* finished;                 // Read-write transactions that ended during the current epoch
    pthread_mutex_t finished_lock;                // Protects finished
};


/** Create (i.e. allocate + init) a new shared memory region, with one first non-free-able allocated segment of the requested size and alignment.
 * @param size  Size of the first shared segment of memory to allocate (in bytes), must be a positive multiple of the alignment
 * @param align Alignment (in bytes, must be a power of 2) that the shared memory region must support
 * @return Opaque shared memory region handle, 'invalid_shared' on failure
**/
shared_t tm_create(size_t size, size_t align) {
    struct region* region = calloc(1, sizeof(struct region));
    if (!region) {
        return invalid_shared;
    }
    region->size = size;
    region->align = align;
    if (!batcher_init(&region->batcher)){
        free(region);
        return invalid_shared;
    }
    if (pthread_mutex_init(&region->alloc_lock, NULL) != 0) {
        batcher_cleanup(&region->batcher);
        free(region);
        return invalid_shared;
    }
    if (pthread_mutex_init(&region->finished_lock, NULL) != 0) {
        pthread_mutex_destroy(&region->alloc_lock);
        batcher_cleanup(&region->batcher);
        free(region);
        return invalid_shared;
    }
    struct segment* first = segment_create(size, align);
    if (!first) {
        pthread_mutex_destroy(&region->finished_lock);
        pthread_mutex_destroy(&region->alloc_lock);
        batcher_cleanup(&region->batcher);
        free(region);
        return invalid_shared;
    }
    region->segments[1] = first;
    region->next_segment = 2;
    region->next_tx_id = 1;

    return region;
}

/** Destroy (i.e. clean-up + free) a given shared memory region.
 * @param shared Shared memory region to destroy, with no running transaction
**/
void tm_destroy(shared_t shared) {
    struct region* region = (struct region*) shared;
    for (uint64_t i = 1; i < ADDR_MAX_SEGMENTS; i++){
        if (region->segments[i]) {
            segment_free(region->segments[i]);
        }
    }
    pthread_mutex_destroy(&region->finished_lock);
    pthread_mutex_destroy(&region->alloc_lock);
    batcher_cleanup(&region->batcher);
    free(region);
}

/** [thread-safe] Return the start address of the first allocated segment in the shared memory region.
 * @param shared Shared memory region to query
 * @return Start address of the first allocated segment
**/
void* tm_start(shared_t unused(shared)) {
    return addr_encode(1, 0);
}

/** [thread-safe] Return the size (in bytes) of the first allocated segment of the shared memory region.
 * @param shared Shared memory region to query
 * @return First allocated segment size
**/
size_t tm_size(shared_t shared) {
    return ((struct region*) shared)->size;
}

/** [thread-safe] Return the alignment (in bytes) of the memory accesses on the given shared memory region.
 * @param shared Shared memory region to query
 * @return Alignment used globally
**/
size_t tm_align(shared_t shared) {
    return ((struct region*) shared)->align;
}

/** Free a segment and make its index reusable by tm_alloc.
 * @param region Region of the segment
 * @param index  Index of the segment to free
**/
static void release_segment(struct region* region, uint64_t index) {
    pthread_mutex_lock(&region->alloc_lock);
    struct segment* segment = region->segments[index];
    if (segment) { // Guard against a segment freed twice
        region->segments[index] = NULL;
        region->free_indexes[region->nb_free_indexes++] = index;
    }
    pthread_mutex_unlock(&region->alloc_lock);
    if (segment)
        segment_free(segment);
}

/** End of an epoch, called by the last thread leaving the batcher: no transaction is running.
 * Publishes the writes of the committed transactions, resets the controls of every word accessed
 * during the epoch, frees the segments that must be freed, and frees the finished transactions.
 * @param arg Region (struct region*)
**/
static void end_of_epoch(void* arg) {
    struct region* region = (struct region*) arg;
    // First every control, since some of them may belong to segments freed below
    for (struct transaction* tx = region->finished; tx; tx = tx->next) {
        for (size_t i = 0; i < tx->written.len; i++) {
            _Atomic uint64_t* ctrl = (_Atomic uint64_t*) tx->written.items[i];
            uint64_t c = atomic_load(ctrl);
            if (tx->committed)
                c ^= CTRL_READABLE_B; // Swap: the written copy becomes the readable one
            atomic_store(ctrl, ctrl_reset(c));
        }
        for (size_t i = 0; i < tx->touched.len; i++) {
            _Atomic uint64_t* ctrl = (_Atomic uint64_t*) tx->touched.items[i];
            atomic_store(ctrl, ctrl_reset(atomic_load(ctrl)));
        }
    }
    // Then the segments: freed by committed transactions, allocated by aborted ones
    struct transaction* tx = region->finished;
    while (tx) {
        struct vec* to_free = tx->committed ? &tx->freed : &tx->allocated;
        for (size_t i = 0; i < to_free->len; i++)
            release_segment(region, to_free->items[i]);
        struct transaction* next = tx->next;
        vec_free(&tx->written);
        vec_free(&tx->touched);
        vec_free(&tx->allocated);
        vec_free(&tx->freed);
        free(tx);
        tx = next;
    }
    region->finished = NULL;
}

/** End a transaction (commit or abort) and leave the batcher.
 * @param region    Region of the transaction
 * @param tx        Transaction to end, must not be used afterwards
 * @param committed Whether the transaction committed (true) or aborted (false)
**/
static void finish(struct region* region, struct transaction* tx, bool committed) {
    if (tx->is_ro) {
        free(tx); // Read-only transactions leave nothing to do at the end of the epoch
    } else {
        tx->committed = committed;
        pthread_mutex_lock(&region->finished_lock);
        tx->next = region->finished;
        region->finished = tx;
        pthread_mutex_unlock(&region->finished_lock);
    }
    batcher_leave(&region->batcher, end_of_epoch, region);
}

/** Read one word in a read-write transaction.
 * @param tx      Transaction
 * @param segment Segment of the word
 * @param word    Index of the word in the segment
 * @param align   Word size of the region
 * @param target  Where to copy the word (private memory)
 * @return Whether the transaction can continue
**/
static bool read_word(struct transaction* tx, struct segment* segment, size_t word, size_t align, void* target) {
    _Atomic uint64_t* ctrl = &segment->controls[word];
    uint64_t c = atomic_load(ctrl);
    bool recorded = false;
    while (true) {
        uint64_t access = ctrl_access(c);
        if (ctrl_written(c)) {
            if (access != tx->id)
                return false; // Written by another transaction
            memcpy(target, segment_word(segment, !ctrl_readable_b(c), word, align), align); // Our own write
            return true;
        }
        uint64_t new_access;
        if (access == tx->id || access == CTRL_ACCESS_MANY)
            new_access = access;
        else if (access == CTRL_ACCESS_NONE)
            new_access = tx->id;
        else
            new_access = CTRL_ACCESS_MANY;
        if (new_access != access) {
            // Record the word before claiming it, so its control is always reset at the end of the epoch
            if (!recorded) {
                if (!vec_push(&tx->touched, (uintptr_t) ctrl))
                    return false;
                recorded = true;
            }
            if (!atomic_compare_exchange_weak(ctrl, &c, ctrl_with_access(c, new_access)))
                continue; // Control changed meanwhile ('c' was updated): try again
        }
        memcpy(target, segment_word(segment, ctrl_readable_b(c), word, align), align);
        return true;
    }
}

/** Write one word in a read-write transaction.
 * @param tx      Transaction
 * @param segment Segment of the word
 * @param word    Index of the word in the segment
 * @param align   Word size of the region
 * @param source  Where to copy the word from (private memory)
 * @return Whether the transaction can continue
**/
static bool write_word(struct transaction* tx, struct segment* segment, size_t word, size_t align, void const* source) {
    _Atomic uint64_t* ctrl = &segment->controls[word];
    uint64_t c = atomic_load(ctrl);
    bool recorded = false;
    while (true) {
        uint64_t access = ctrl_access(c);
        if (ctrl_written(c)) {
            if (access != tx->id)
                return false; // Written by another transaction
            memcpy(segment_word(segment, !ctrl_readable_b(c), word, align), source, align); // Overwrite our own write
            return true;
        }
        if (access != CTRL_ACCESS_NONE && access != tx->id)
            return false; // Read by another transaction
        // Record the word before claiming it (once: a word must not be swapped twice)
        if (!recorded) {
            if (!vec_push(&tx->written, (uintptr_t) ctrl))
                return false;
            recorded = true;
        }
        if (!atomic_compare_exchange_weak(ctrl, &c, ctrl_with_access(c | CTRL_WRITTEN, tx->id)))
            continue; // Control changed meanwhile ('c' was updated): try again
        memcpy(segment_word(segment, !ctrl_readable_b(c), word, align), source, align);
        return true;
    }
}

/** [thread-safe] Begin a new transaction on the given shared memory region.
 * @param shared Shared memory region to start a transaction on
 * @param is_ro  Whether the transaction is read-only
 * @return Opaque transaction ID, 'invalid_tx' on failure
**/
tx_t tm_begin(shared_t shared, bool is_ro) {
    struct region* region = (struct region*) shared;
    struct transaction* tx = calloc(1, sizeof(struct transaction)); // Allocated before entering, to spend less time in the batch
    if (!tx)
        return invalid_tx;
    tx->is_ro = is_ro;
    batcher_enter(&region->batcher);
    if (!is_ro)
        tx->id = atomic_fetch_add(&region->next_tx_id, 1);
    return (tx_t) tx;
}

/** [thread-safe] End the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to end
 * @return Whether the whole transaction committed
**/
bool tm_end(shared_t shared, tx_t tx) {
    // Conflicts are detected at each access, so a transaction that reached its end always commits
    finish((struct region*) shared, (struct transaction*) tx, true);
    return true;
}

/** [thread-safe] Read operation in the given transaction, source in the shared region and target in a private region.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param source Source start address (in the shared region)
 * @param size   Length to copy (in bytes), must be a positive multiple of the alignment
 * @param target Target start address (in a private region)
 * @return Whether the whole transaction can continue
**/
bool tm_read(shared_t shared, tx_t tx, void const* source, size_t size, void* target) {
    struct region* region = (struct region*) shared;
    struct transaction* transaction = (struct transaction*) tx;
    struct segment* segment = region->segments[addr_segment(source)];
    size_t align = region->align;
    size_t first = addr_word(source, align);
    size_t nb_words = size / align;
    char* dest = (char*) target;
    if (transaction->is_ro) {
        // Readable copies do not change during an epoch: always consistent, never aborts
        for (size_t i = 0; i < nb_words; i++) {
            uint64_t c = atomic_load(&segment->controls[first + i]);
            memcpy(dest + i * align, segment_word(segment, ctrl_readable_b(c), first + i, align), align);
        }
        return true;
    }
    for (size_t i = 0; i < nb_words; i++) {
        if (!read_word(transaction, segment, first + i, align, dest + i * align)) {
            finish(region, transaction, false);
            return false;
        }
    }
    return true;
}

/** [thread-safe] Write operation in the given transaction, source in a private region and target in the shared region.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param source Source start address (in a private region)
 * @param size   Length to copy (in bytes), must be a positive multiple of the alignment
 * @param target Target start address (in the shared region)
 * @return Whether the whole transaction can continue
**/
bool tm_write(shared_t shared, tx_t tx, void const* source, size_t size, void* target) {
    struct region* region = (struct region*) shared;
    struct transaction* transaction = (struct transaction*) tx;
    struct segment* segment = region->segments[addr_segment(target)];
    size_t align = region->align;
    size_t first = addr_word(target, align);
    size_t nb_words = size / align;
    char const* src = (char const*) source;
    for (size_t i = 0; i < nb_words; i++) {
        if (!write_word(transaction, segment, first + i, align, src + i * align)) {
            finish(region, transaction, false);
            return false;
        }
    }
    return true;
}

/** [thread-safe] Memory allocation in the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param size   Allocation requested size (in bytes), must be a positive multiple of the alignment
 * @param target Pointer in private memory receiving the address of the first byte of the newly allocated, aligned segment
 * @return Whether the whole transaction can continue (success/nomem), or not (abort_alloc)
**/
alloc_t tm_alloc(shared_t shared, tx_t tx, size_t size, void** target) {
    struct region* region = (struct region*) shared;
    struct transaction* transaction = (struct transaction*) tx;
    // No other transaction knows the new segment before we commit: it can be created right away
    struct segment* segment = segment_create(size, region->align);
    if (!segment)
        return nomem_alloc;
    uint64_t index;
    pthread_mutex_lock(&region->alloc_lock);
    if (region->nb_free_indexes > 0) {
        index = region->free_indexes[--region->nb_free_indexes];
    } else if (region->next_segment < ADDR_MAX_SEGMENTS) {
        index = region->next_segment++;
    } else {
        pthread_mutex_unlock(&region->alloc_lock);
        segment_free(segment);
        return nomem_alloc; // No segment index left
    }
    region->segments[index] = segment;
    pthread_mutex_unlock(&region->alloc_lock);
    if (!vec_push(&transaction->allocated, index)) {
        release_segment(region, index); // Cannot be tracked: undo the allocation now
        finish(region, transaction, false);
        return abort_alloc;
    }
    *target = addr_encode(index, 0);
    return success_alloc;
}

/** [thread-safe] Memory freeing in the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param target Address of the first byte of the previously allocated segment to deallocate
 * @return Whether the whole transaction can continue
**/
bool tm_free(shared_t shared, tx_t tx, void* target) {
    struct region* region = (struct region*) shared;
    struct transaction* transaction = (struct transaction*) tx;
    // Other transactions of this epoch may still access the segment: free it at the end of the epoch, if we commit
    if (!vec_push(&transaction->freed, addr_segment(target))) {
        finish(region, transaction, false);
        return false;
    }
    return true;
}
