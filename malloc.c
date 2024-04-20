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
    mchunk_hdr* head;
    mchunk_hdr* tail;
    pthread_mutex_t mu;
} freelist;

#define ALIGN8(x) (((x - 1) >> 3) << 3) + 8
#define CHUNK_HDR_SIZE sizeof(mchunk_hdr)
#define USED 1
#define FREE 0

freelist __freelist = {
    .head = NULL,
    .tail = NULL
};

void debug_freelist() {
    int i = 0;
    printf("------------------------\n");
    for (mchunk_hdr* curr = __freelist.head; curr != NULL; curr = curr->next) {
        printf("CHUNK #%d: size: %lu\n", i, curr->size);
        i++;
    }
    printf("------------------------\n");
}

mchunk_hdr* __find_chunk(size_t size) {
    for (mchunk_hdr* curr = __freelist.head; curr != NULL; curr = curr->next) {
        if (curr->size >= size) {
            return curr;
        }
    }

    return NULL;
}

void __split_chunk(mchunk_hdr* chunk, size_t size) {
    size_t remaining_size = chunk->size - size;
    if (remaining_size > CHUNK_HDR_SIZE) {
        // we can split
        chunk->size = size;
        mchunk_hdr* new_chunk = (mchunk_hdr*)((char*)(chunk + 1) + size);
        new_chunk->used = FREE;
        new_chunk->size = remaining_size - CHUNK_HDR_SIZE;
        new_chunk->next = chunk->next;
        if (chunk->next != NULL) {
            chunk->next->prev = new_chunk;
        }

        new_chunk->prev = chunk;
        chunk->next = new_chunk;

        if (chunk == __freelist.tail) {
            __freelist.tail = new_chunk;
        }
    }
}

void __merge_forward(mchunk_hdr* chunk) {
    if (chunk->next && ((char*)chunk + CHUNK_HDR_SIZE + chunk->size == (char*)chunk->next)) {
        chunk->size += chunk->next->size + CHUNK_HDR_SIZE;
        if (chunk->next == __freelist.tail) {
            __freelist.tail = chunk;
        }

        chunk->next = chunk->next->next;
        if (chunk->next) {
            chunk->next->prev = chunk;
        }
    }
}

void __merge_backward(mchunk_hdr* chunk) {
    if (chunk->prev && ((char*)chunk - chunk->prev->size - CHUNK_HDR_SIZE == (char*)chunk->prev)) {
        chunk->prev->size += CHUNK_HDR_SIZE + chunk->size;
        chunk->prev->next = chunk->next;
        if (chunk->next) {
            chunk->next->prev = chunk->prev;
        }
    }
}

mchunk_hdr* __find_chunk_pos(mchunk_hdr* chunk) {
    for (mchunk_hdr* curr = __freelist.head; curr != NULL; curr = curr->next) {
        if (chunk < curr) {
            return curr;
        }
    }

    return NULL;
}

int __grow_freelist(int size) {
    // grow freelist in multiples of 128 KB
    // with respect to the requested memory amount
    size = ceil((double) size / ((128*1024))) * (128*1024) * 2;
    void* base = sbrk(size + CHUNK_HDR_SIZE);
    if (base == (void*)-1 && errno == ENOMEM) {
        return 0;
    }

    void* new_base = sbrk(0);
    if (new_base == (void*)-1) {
        return 0;
    }

    size = (uintptr_t)new_base - (uintptr_t)base;
    if (size <= CHUNK_HDR_SIZE) {
        return 0;
    }

    mchunk_hdr* new_chunk = (mchunk_hdr*)base;
    new_chunk->used = FREE;
    new_chunk->size = size - CHUNK_HDR_SIZE;
    new_chunk->prev = __freelist.tail;

    if (__freelist.tail) {
        __freelist.tail->next = new_chunk;
    }

    __freelist.tail = new_chunk;

    if (!__freelist.head) {
        __freelist.head = new_chunk;
    }

    // try merge backward
    if (__freelist.tail->prev && 
    (char*)__freelist.tail - __freelist.tail->prev->size - CHUNK_HDR_SIZE == (char*)__freelist.tail->prev) {
        __freelist.tail->prev->size += __freelist.tail->size + CHUNK_HDR_SIZE;
        __freelist.tail = __freelist.tail->prev;
        __freelist.tail->next = NULL;
    }

    return 1;
}

void* my_malloc(size_t size) {
    if (size <= 0) {
        return NULL;
    }

    size_t s = ALIGN8(size);

    for (;;) {
        mchunk_hdr* chunk = __find_chunk(s);
        if (chunk) {
            //split chunk in order to reduce internal fragmantation
            __split_chunk(chunk, s);

            // remove chunk from the freelist
            if (chunk->next) {
                chunk->next->prev = chunk->prev;
            }

            if (chunk == __freelist.tail) {
                __freelist.tail = chunk->prev;
            }

            if (chunk == __freelist.head) {
                __freelist.head = __freelist.head->next;
            } else {
                chunk->prev->next = chunk->next;
            }

            chunk->used = USED;
            // for security reasons
            chunk->next = NULL;
            chunk->prev = NULL;

            pthread_mutex_unlock(&__freelist.mu);
            return chunk + 1;
        } 

        // could'nt find enough memory for the request
        // allocate more memory
        if (!__grow_freelist(s)) {
            errno = ENOMEM;
            pthread_mutex_unlock(&__freelist.mu);
            return NULL;
        }
    }

    // should not get here ever once we allocate more memory
    // for the request
    pthread_mutex_unlock(&__freelist.mu);
    return NULL;
}

void my_free(void* ptr) {    
    mchunk_hdr* chunk = (mchunk_hdr*)ptr - 1;
    if (chunk->used == FREE) {
        // chunk is already freed
        return;
    }

    chunk->used = FREE;

    pthread_mutex_lock(&__freelist.mu);
    
    // link the to-be-freed chunk 
    mchunk_hdr* curr = __find_chunk_pos(chunk);
    if (curr) {
        if (curr == __freelist.head) {
            // to-be-freed chunk is the first chunk in the freelist
            chunk->next = __freelist.head;
            __freelist.head->prev = chunk;
            __freelist.head = chunk;
        } else {
            chunk->next = curr;
            curr->prev->next = chunk; 
            chunk->prev = curr->prev;
            curr->prev = chunk;
        }          
    } else {
        if (!__freelist.head) {
            // freelist is empty
            __freelist.head = chunk;
            __freelist.tail = chunk;
        } else {
            // freed chunk is the tail of the freelist
            __freelist.tail->next = chunk;
            chunk->prev = __freelist.tail;
            __freelist.tail = chunk;
        }        
    }

    __merge_forward(chunk);
    __merge_backward(chunk);

    pthread_mutex_unlock(&__freelist.mu);
    return;
}

int main(int argc, char **argv) {
    void* ptr = my_malloc(64);
    debug_freelist();

    void* ptr2 = my_malloc(2);
    debug_freelist();

    void *ptr3 = my_malloc(20);
    debug_freelist();

    my_free(ptr);
    debug_freelist();

    void *ptr4 = my_malloc(120);
    debug_freelist();

    my_free(ptr3);
    debug_freelist();

    my_free(ptr2);
    debug_freelist();

    my_free(ptr4);
    debug_freelist();

}