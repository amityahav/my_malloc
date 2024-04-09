# my_malloc
I wanted to better understand memory allocation so i've decided to implement a simple 
malloc/free mechanisms.

- memory is managed in a doubly linked list of chunks.
- each chunk comprises of a metadata header (4 bytes) and the memory block to be used.
- when a user requests for memory, we first traverse the linked list to find a big enough chunk.
   - if one is found then we check if we can split it in order to reduce internal fragmentation. and return it.
   - if not found then we grow our heap and extend our linked list.
- when a user frees a chunk of memory, we return it to the linked list and merge it with adjacent chunks if any.
