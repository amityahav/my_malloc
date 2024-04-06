#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>

typedef struct mchunk_hdr {
    // 0 bit - USED/FREE chunk
    u_int8_t flags;
    size_t size;
    struct mchunk_hdr* next;
} mchunk_hdr;

typedef struct freelist {
    size_t size;
    mchunk_hdr* head;
    pthread_mutex_t mu;
} freelist;

#define HEAP_CAP 64 * 1024 // 64 KB
#define INITIAL_CHUNK_SIZE 64 // 64 B
#define CHUNK_HDR_SIZE sizeof(mchunk_hdr)
#define USED 1
#define FREE 0

struct freelist __freelist = {
    .size = 0,
    .head = NULL
};

int __grow_freelist() {
    void* base = sbrk(HEAP_CAP);
    if ((uint32_t)base == -1 && errno == ENOMEM) {
        return 0;
    }

    char* chunk = (char*)base;
    struct mchunk_hdr dummy_head = {
        .size = 0,
        .next = (struct mchunk_hdr*)base
    };
    struct mchunk_hdr* prev = &dummy_head;

    for (int i = 0; i < HEAP_CAP / INITIAL_CHUNK_SIZE; i++) {
        struct mchunk_hdr* curr = (struct mchunk_hdr*)chunk;
        curr->size = INITIAL_CHUNK_SIZE - CHUNK_HDR_SIZE;
        prev->next = curr;
        prev = curr;
        chunk += INITIAL_CHUNK_SIZE;
    }

    __freelist.size += HEAP_CAP;
    __freelist.head = (struct mchunk_hdr*)base;

    return 1;
}

// 2. handle splitting chunk to reduce internal fragmantation
void* my_malloc(size_t size) {
    if (!size) {
        return NULL;
    }

    pthread_mutex_lock(&__freelist.mu);
    if (__freelist.head == NULL) {
        if (!__grow_freelist()) {
            errno = ENOMEM;
            pthread_mutex_unlock(&__freelist.mu);
            return NULL;
        }
    }

    struct mchunk_hdr* prev;
    struct mchunk_hdr* merge_head;
    struct mchunk_hdr* merge_head_prev;
    size_t acc_size = 0;
    for (struct mchunk_hdr* curr = __freelist.head; curr != NULL; curr = curr->next) {
        if (curr->size >= size) {
            if (prev == NULL) {
                __freelist.head = __freelist.head->next;
            } else {
                prev->next = curr->next;
            }

            curr->flags |= USED;
            pthread_mutex_unlock(&__freelist.mu);
            return curr + 1;
        } 

        // try merging adjacent chunks
        if (merge_head == NULL) {
            merge_head_prev = prev;
            merge_head = curr;
            acc_size += curr->size;
            continue;
        }

        if ((char*)prev + CHUNK_HDR_SIZE + prev->size != (char*)curr) {
            // replace merge head since a non adjacent chunk found in the way
            acc_size = 0;
            merge_head_prev = prev;
            merge_head = curr;
            acc_size += curr->size;
            continue;
        }

        acc_size += curr->size + CHUNK_HDR_SIZE;
        if (acc_size >= size) {
            // found enough chunks to merge 
            merge_head->size = acc_size;
            merge_head->flags |= USED;

            if (merge_head_prev == NULL) {
                __freelist.head = curr->next;
            } else {
                merge_head_prev->next = curr->next;
            }

            pthread_mutex_unlock(&__freelist.mu);
            return merge_head + 1;
        }
        
        prev = curr;
    }

    pthread_mutex_unlock(&__freelist.mu);
    return NULL;
}

void* my_free(void* ptr) {    
    struct mchunk_hdr* hdr = (struct mchunk_hdr*)ptr - 1;
    if (hdr->flags >> 1 == FREE) {
        // chunk is already freed
        exit(1);
    }

    hdr->flags ^= FREE;

    pthread_mutex_lock(&__freelist.mu);

    struct mchunk_hdr* prev;
    for (struct mchunk_hdr* curr = __freelist.head; curr != NULL; curr = curr->next) {
        if (hdr < curr) {
            if (prev == NULL) {
                // to be freed chunk is the first chunk in the freelist
                hdr->next = __freelist.head;
                __freelist.head = hdr;
            } else {
                hdr->next = prev->next;
                prev->next = hdr;
            }   

            pthread_mutex_unlock(&__freelist.mu);
            return NULL;
        }

        prev = curr;
    }

    if (prev == NULL) {
        // freelist is empty
        __freelist.head = hdr;
    } else {
        prev->next = hdr;
    }

    pthread_mutex_unlock(&__freelist.mu);
    return NULL;
}

int main(int argc, char **argv) {
    printf("hello world");
}