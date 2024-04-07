#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>

typedef struct mchunk_hdr {
    u_int8_t used;
    size_t size;
    struct mchunk_hdr* next;
} mchunk_hdr;

typedef struct freelist {
    size_t size;
    mchunk_hdr* head;
    pthread_mutex_t mu;
} freelist;

#define HEAP_CAP 128 * 1024 // 128 KB
#define INITIAL_CHUNK_SIZE 128 // 128 B
#define CHUNK_HDR_SIZE sizeof(mchunk_hdr)
#define USED 1
#define FREE 0

struct freelist __freelist = {
    .size = 0,
    .head = NULL
};

// TODO: handle the case when the list is not empty but has not 
// enough memory for the request so it needs to be grown as well
int __grow_freelist(int size) {
    // grow freelist in multiples of HEAP_CAP
    // with respect to the requested memory amount
    size = ceil((double) size / HEAP_CAP) * HEAP_CAP;
    void* base = sbrk(size);
    if ((intptr_t)base == -1 && errno == ENOMEM) {
        return 0;
    }

    char* chunk = (char*)base;
    struct mchunk_hdr dummy_head = {
        .size = 0,
        .next = (struct mchunk_hdr*)base
    };
    struct mchunk_hdr* prev = &dummy_head;

    for (int i = 0; i < size / INITIAL_CHUNK_SIZE; i++) {
        struct mchunk_hdr* curr = (struct mchunk_hdr*)chunk;
        curr->size = INITIAL_CHUNK_SIZE - CHUNK_HDR_SIZE;
        curr->used = FREE;
        prev->next = curr;
        prev = curr;
        chunk += INITIAL_CHUNK_SIZE;
    }

    __freelist.size += size;
    __freelist.head = (struct mchunk_hdr*)base;

    return 1;
}

void* my_malloc(size_t size) {
    if (!size) {
        return NULL;
    }

    pthread_mutex_lock(&__freelist.mu);
    if (__freelist.head == NULL) {
        // grow freelist at first allocation request or
        // when the user allocated all of the existing chunks
        if (!__grow_freelist(size)) {
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
            // split chunk in order to reduce internal fragmantation
            while (curr->size >> 1 >= size && curr->size >> 1 > CHUNK_HDR_SIZE) {
                curr->size >>= 1;
                struct mchunk_hdr* new_chunk = (struct mchunk_hdr*)((char*)(curr + 1) + curr->size);
                new_chunk->used = FREE;
                new_chunk->size = curr->size - CHUNK_HDR_SIZE;
                new_chunk->next = curr->next;
                curr->next = new_chunk;
            }

            if (prev == NULL) {
                __freelist.head = __freelist.head->next;
            } else {
                prev->next = curr->next;
            }

            curr->used = USED;
            curr->next = NULL;
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
            // replace merge head since a non adjacent chunk found on the way
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
            merge_head->used = USED;
            merge_head->next = NULL;

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
    if (hdr->used == FREE) {
        // chunk is already freed
        exit(1);
    }

    hdr->used = FREE;

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