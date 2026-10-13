/**
 * @file   batcher.c
 *
 * @section DESCRIPTION
 *
 * Implementation of the thread batcher.
**/

// Requested features
#define _GNU_SOURCE
#define _POSIX_C_SOURCE   200809L

// Internal headers
#include "batcher.h"
#include "macros.h"

bool batcher_init(struct batcher* batcher) {
    batcher->counter   = 0;
    batcher->remaining = 0;
    batcher->blocked   = 0;
    if (pthread_mutex_init(&(batcher->mutex), NULL) != 0)
        return false;
    if (pthread_cond_init(&(batcher->cond), NULL) != 0) {
        pthread_mutex_destroy(&(batcher->mutex));
        return false;
    }
    return true;
}

void batcher_cleanup(struct batcher* batcher) {
    pthread_cond_destroy(&(batcher->cond));
    pthread_mutex_destroy(&(batcher->mutex));
}

uint64_t batcher_get_epoch(struct batcher* unused(batcher)) {
    // TODO: batcher_get_epoch
    return 0;
}

void batcher_enter(struct batcher* unused(batcher)) {
    // TODO: batcher_enter
}

void batcher_leave(struct batcher* unused(batcher)) {
    // TODO: batcher_leave
}
