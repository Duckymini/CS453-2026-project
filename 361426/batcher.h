/**
 * @file   batcher.h
 *
 * @section DESCRIPTION
 *
 * Thread batcher of the dual-versioned STM.
 *
 * Like a mutex, but instead of letting threads in one at a time, it lets in
 * all the threads that were waiting together, once the last thread of the
 * current batch has left. The time span during which one batch runs is called
 * an epoch.
**/

#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct batcher {
    uint64_t counter;      // Current epoch, incremented when the last thread of a batch leaves
    size_t remaining;      // Number of threads still running in the current batch
    size_t blocked;        // Number of threads waiting for the next batch
    pthread_mutex_t mutex; // Protects the fields above
    pthread_cond_t cond;   // Where blocked threads wait for the next batch
};

/** Initialize a batcher.
 * @param batcher Batcher to initialize
 * @return Whether the operation was a success
**/
bool batcher_init(struct batcher* batcher);

/** Clean up a batcher, with no thread inside or waiting.
 * @param batcher Batcher to clean up
**/
void batcher_cleanup(struct batcher* batcher);

/** Get the current epoch; only valid between enter and leave.
 * @param batcher Batcher to query
 * @return Current epoch
**/
uint64_t batcher_get_epoch(struct batcher* batcher);

/** Enter the batcher: join the current batch if none is running, otherwise wait for the next one.
 * @param batcher Batcher to enter
**/
void batcher_enter(struct batcher* batcher);

/** Leave the batcher; the last thread of the batch ends the epoch and lets the next batch in.
 * @param batcher Batcher to leave
**/
void batcher_leave(struct batcher* batcher);
