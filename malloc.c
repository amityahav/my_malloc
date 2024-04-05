#include <stdio.h>
#include <unistd.h>
#include <errno.h>

#define HEAP_CAP 1024 * 1024 * 1024 // 1 GB
#define INITIAL_CHUNK_SIZE 8 * 1024 // 8 KB

typedef struct mchunk_hdr {
    size_t size;
    struct mchunk_hdr* next;
} mchunk_hdr;

#define CHUNK_HDR_SIZE sizeof(mchunk_hdr)

struct mchunk_hdr* __freelist = NULL;

int __init_freelist() {
    void* base = sbrk(HEAP_CAP);
    if ((uint32_t)base == -1) {
         return 0;
    }

    char* chunk = base;
    struct mchunk_hdr dummy_head = {
        .size = 0,
        .next = base
    };
    struct mchunk_hdr* prev = &dummy_head;

    for (int i = 0; i < HEAP_CAP / INITIAL_CHUNK_SIZE; i++) {
        struct mchunk_hdr* curr = chunk;
        curr->size = INITIAL_CHUNK_SIZE - CHUNK_HDR_SIZE;
        prev->next = curr;
        prev = curr;
        chunk += INITIAL_CHUNK_SIZE;
    }

    __freelist = base;

    return 1;
}

// 1. handle merging adj chunks
// 2. handle splitting chunk to reduce internal fragmantation
// 3. handle the case where freelist is null due to user allocated all memory
void* my_malloc(size_t size) {
    if (__freelist == NULL) {
        if (!__init_freelist()) {
            errno = ENOMEM;
            return NULL;
        }
    }

    struct mchunk_hdr* prev;
    for (struct mchunk_hdr* curr = __freelist; curr != NULL; curr = curr->next) {
        if (curr->size >= size) {
            if (prev == NULL) {
                __freelist = __freelist->next;
            } else {
                prev->next = curr->next;
            }

            return (char*)curr + CHUNK_HDR_SIZE;
        }

        prev = curr;
    }

    return NULL;
}

// TODO: validate that the to-be-freed ptr is from the freelist pool
void* free(void* ptr) {
    struct mchunk_hdr* hdr = (char*)ptr - CHUNK_HDR_SIZE;
    struct mchunk_hdr* prev;
    for (struct mchunk_hdr* curr = __freelist; curr != NULL; curr = curr->next) {
        if (hdr < curr) {
            if (prev == NULL) {
                // to be freed chunk is the first chunk in the freelist
                hdr->next = __freelist;
                __freelist = hdr;
            } else {
                hdr->next = prev->next;
                prev->next = hdr;
            }

            return NULL;
        }

        prev = curr;
    }

    if (prev == NULL) {
        // freelist is empty
        __freelist = hdr;
    } else {
        prev->next = hdr;
    }

    return NULL;
}

int main(int argc, char **argv) {
    printf("hello world");
}