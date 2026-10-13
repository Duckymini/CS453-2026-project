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

uint64_t batcher_get_epoch(struct batcher* batcher) {
    return batcher->counter;
}

void batcher_enter(struct batcher* batcher) {
    pthread_mutex_lock(&(batcher->mutex));
    if (batcher->remaining == 0) {
        batcher->remaining = 1;
    } else {
        uint64_t epoch = batcher->counter;
        batcher->blocked++;
        while (batcher->counter == epoch)
            pthread_cond_wait(&(batcher->cond), &(batcher->mutex));
    }
    pthread_mutex_unlock(&(batcher->mutex));
}

void batcher_leave(struct batcher* batcher) {
    pthread_mutex_lock(&(batcher->mutex));
    batcher->remaining--;
    if (batcher->remaining == 0) {
        batcher->counter++;
        batcher->remaining = batcher->blocked;
        batcher->blocked = 0;
        pthread_cond_broadcast(&(batcher->cond));
    }
    pthread_mutex_unlock(&(batcher->mutex));
}
