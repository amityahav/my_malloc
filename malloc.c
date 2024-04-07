#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>

typedef struct mchunk_hdr {
    u_int8_t used;
    size_t size;
    struct mchunk_hdr* prev;
    struct mchunk_hdr* next;
} mchunk_hdr;

typedef struct freelist {
    size_t available;
    mchunk_hdr* head;
    mchunk_hdr* tail;
    pthread_mutex_t mu;
} freelist;

#define HEAP_CAP 128 * 1024 // 128 KB
#define CHUNK_HDR_SIZE sizeof(mchunk_hdr)
#define USED 1
#define FREE 0

struct freelist __freelist = {
    .available = 0,
    .head = NULL,
    .tail = NULL
};

// TODO: handle the case when the list is not empty but has not 
// enough memory for the request so it needs to be grown as well
int __grow_freelist(int size) {
    // grow freelist in multiples of HEAP_CAP
    // with respect to the requested memory amount
    size = ceil((double) size / HEAP_CAP) * HEAP_CAP * 2;
    void* base = sbrk(size);
    if ((intptr_t)base == -1 && errno == ENOMEM) {
        return 0;
    }

    struct mchunk_hdr* new_chunk = (struct mchunk_hdr*)base;
    new_chunk->used = FREE;
    new_chunk->size = size - CHUNK_HDR_SIZE;
    new_chunk->prev = __freelist.tail;
    if (__freelist.tail != NULL) {
        __freelist.tail->next = new_chunk;
    }

    __freelist.tail = new_chunk;

    if (__freelist.head == NULL) {
        __freelist.head = new_chunk;
    }

    __freelist.available += new_chunk->size;
    
    // try merge backward
    if (__freelist.tail->prev != NULL && 
    (char*)__freelist.tail - __freelist.tail->prev->size - CHUNK_HDR_SIZE == (char*)__freelist.tail->prev) {
        __freelist.tail->prev->size += __freelist.tail->size + CHUNK_HDR_SIZE;
        __freelist.tail = __freelist.tail->prev;
        __freelist.tail->next = NULL;
        __freelist.available += CHUNK_HDR_SIZE;
    }

    return 1;
}

void* my_malloc(size_t size) {
    if (size <= 0) {
        return NULL;
    }

    pthread_mutex_lock(&__freelist.mu);
    if (__freelist.available < size) {
        // grow freelist whenever there is not enough memory 
        // for the request
        if (!__grow_freelist(size)) {
            errno = ENOMEM;
            pthread_mutex_unlock(&__freelist.mu);
            return NULL;
        }
    }

    struct mchunk_hdr* prev;
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
                __freelist.available -= CHUNK_HDR_SIZE;
            }

            if (prev == NULL) {
                __freelist.head = __freelist.head->next;
            } else {
                prev->next = curr->next;
            }

            curr->used = USED;
            curr->next = NULL;
            __freelist.available -= curr->size;
            pthread_mutex_unlock(&__freelist.mu);
            return curr + 1;
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
    hdr->next = NULL;
    hdr-> prev = NULL;
    __freelist.available += hdr->size;

    pthread_mutex_lock(&__freelist.mu);
    
    int found = 0;
    for (struct mchunk_hdr* curr = __freelist.head; curr != NULL; curr = curr->next) {
        if (hdr < curr) {
            if (curr == __freelist.head) {
                // to be freed chunk is the first chunk in the freelist
                hdr->next = __freelist.head;
                __freelist.head->prev = hdr;
                __freelist.head = hdr;
            } else {
                hdr->next = curr;
                curr->prev->next = hdr; 
                hdr->prev = curr->prev;
                curr->prev = hdr;
            }   

            found = 1;
        }
    }

    if (!found) {
        if (__freelist.head == NULL) {
            // freelist is empty
            __freelist.head = hdr;
            __freelist.tail = hdr;
        } else {
            // freed chunk is the tail of the freelist
            __freelist.tail->next = hdr;
            hdr->prev = __freelist.tail;
            __freelist.tail = hdr;
        }
    }

    // merge adjacent chunks if possible
    int acc_fwd = hdr->size;
    for (struct mchunk_hdr* curr = hdr->next; curr != NULL; curr = curr->next) {
        if ((char*)curr - curr->prev->size - CHUNK_HDR_SIZE != (char*)curr->prev) {
            hdr->next = curr;
            hdr->size = acc_fwd;
            curr->prev = hdr;
            break;
        }

        acc_fwd += curr->size + CHUNK_HDR_SIZE;
        __freelist.available += CHUNK_HDR_SIZE;

        if (curr == __freelist.tail) {
            hdr->next = NULL;
            hdr->size = acc_fwd;
            __freelist.tail = hdr;
            break;
        }
    }

    int acc_bck = hdr->size;
    struct mchunk_hdr* bck_head = hdr;
    for (struct mchunk_hdr* curr = hdr->prev; curr != NULL; curr = curr->prev) {
        if ((char*)curr + curr->next->size + CHUNK_HDR_SIZE != (char*)curr->next) {
            bck_head->size = acc_bck;
            bck_head->next = hdr->next;
            hdr->next->prev = bck_head;
            break;
        }

        bck_head = curr;
        acc_bck += bck_head->size + CHUNK_HDR_SIZE;
        __freelist.available += CHUNK_HDR_SIZE;

        if (bck_head == __freelist.head) {
            bck_head->size = acc_bck;
            break;
        }
    }

    if (bck_head != hdr) {
        // we've merged backwards
        bck_head->next = hdr->next;
        if (hdr->next != NULL) {
            hdr->next->prev = bck_head;
        }
    }

    pthread_mutex_unlock(&__freelist.mu);
    return NULL;
}

int main(int argc, char **argv) {
    printf("hello world");
}