#include "k_mem.h"
#include "common.h"
#include "k_task.h"

// Ext func for getting TID of task to use without using nested svc calls
extern task_t osGetTID_internal(void);

// Define NULL since we can't use standard library
#ifndef NULL
#define NULL ((void*)0)
#endif

//  mem block header
typedef struct mem_block {
    U32 size;
    U8 is_allocated;
    task_t owner_tid;
    struct mem_block* next;
    struct mem_block* prev;
} mem_block_t;

// Global mem management variables
static U8* heap_start = NULL;
static U8* heap_end = NULL;
static mem_block_t* free_list_head = NULL;
static U8 memory_initialized = 0;

// External linker symbols
extern U32 _img_end;
extern U32 _estack;
extern U32 _Min_Stack_Size;

// Helper function prototypes
static mem_block_t* find_free_block(size_t size);
static void split_block(mem_block_t* block, size_t size);
static void coalesce_free_blocks(mem_block_t* block);
static void add_to_free_list(mem_block_t* block);
static void remove_from_free_list(mem_block_t* block);
static U8 is_valid_pointer(void* ptr);



// Initializes  mem manager
int k_mem_init_impl(void) {
    // Checks kernel if is initialized
    if (!g_kernel_initialized) {
        return RTX_ERR;
    }

    // reset everything if already initialized
    if (memory_initialized) {

        if (heap_start != NULL && heap_end != NULL) {
            size_t heap_size = heap_end - heap_start;

            for (size_t i = 0; i < heap_size; i++) {
                heap_start[i] = 0;
            }
        }

        // Reset all states
        heap_start = NULL;
        heap_end = NULL;
        free_list_head = NULL;
        memory_initialized = 0;
    }

    // Calculate heap boundaries with small offset for safety
    heap_start = (U8*)&_img_end + 0x200;
    heap_end = (U8*)&_estack - (U32)&_Min_Stack_Size;


    if ((U32)heap_start % 4 != 0) {
        heap_start += 4 - ((U32)heap_start % 4);
    }

    // check to see if we have enough space for at least one more block
    size_t total_heap_size = heap_end - heap_start;
    if (total_heap_size < sizeof(mem_block_t) + 16) {
        return RTX_ERR;
    }

    // Initialize the first free blocsk
    free_list_head = (mem_block_t*)heap_start;
    free_list_head->size = total_heap_size;
    free_list_head->is_allocated = 0;
    free_list_head->owner_tid = TID_NULL;
    free_list_head->next = NULL;
    free_list_head->prev = NULL;

    memory_initialized = 1;
    return RTX_OK;
}

int k_mem_init(void) {
    int result;
    __asm("SVC #7" : "=r" (result));
    return result;
}






// Allocate memory using First Fit algorithm

void* k_mem_alloc_impl(size_t size) {
    if (!memory_initialized || size == 0) {
        return NULL;
    }

    // Make sure size is 4 byte aligned
    if (size % 4 != 0) {
        size += 4 - (size % 4);
    }

    // Space for header
    size_t total_size = size + sizeof(mem_block_t);

    // Find free block
    mem_block_t* block = find_free_block(total_size);
    if (block == NULL) {
        return NULL;
    }

    // Remove from free list FIRST
    remove_from_free_list(block);

    // Split block if bigger than needed
    if (block->size >= total_size + sizeof(mem_block_t) + 16) {


        // Create new free block from the remainder
        mem_block_t* new_block = (mem_block_t*)((U8*)block + total_size);
        new_block->size = block->size - total_size;
        new_block->is_allocated = 0;
        new_block->owner_tid = TID_NULL;
        new_block->next = NULL;
        new_block->prev = NULL;

        // update original block size
        block->size = total_size;

        // Add the new free block to the free list
        add_to_free_list(new_block);
    }

    // marks block as allocated
    block->is_allocated = 1;
    block->owner_tid = osGetTID_internal();

    // Clear the next/prev pointers since they are no longer in the list
    block->next = NULL;
    block->prev = NULL;

    // return pointer to usable memory
    return (void*)((U8*)block + sizeof(mem_block_t));
}

void* k_mem_alloc(size_t size) {
    void* result;
    __asm("SVC #8" : "=r" (result) : "r" (size));
    return result;
}







// deallocate memory
int k_mem_dealloc_impl(void* ptr) {


    if (ptr == NULL) {

        return RTX_OK;
    }

    if (!memory_initialized || !is_valid_pointer(ptr)) {

        return RTX_ERR;
    }

    // Get block header
    mem_block_t* block = (mem_block_t*)((U8*)ptr - sizeof(mem_block_t));


    // check if block is allocated
    if (!block->is_allocated) {

        return RTX_ERR;
    }

    // Check owner since only owner can request to free memory
    task_t current_tid = osGetTID_internal();

    if (block->owner_tid != current_tid && current_tid != TID_NULL) {

        return RTX_ERR;
    }

    // mark as free
    block->is_allocated = 0;
    block->owner_tid = TID_NULL;

    // Add back to free list
    add_to_free_list(block);

    // Coalesce with adjacent free blocks
    coalesce_free_blocks(block);


    return RTX_OK;
}

int k_mem_dealloc(void* ptr) {
    int result;
    __asm("SVC #9" : "=r" (result) : "r" (ptr));
    return result;
}









// Count external fragmentation
int k_mem_count_extfrag_impl(size_t size) {
    if (!memory_initialized) {
        return 0;
    }



    int count = 0;
    mem_block_t* current = free_list_head;

    while (current != NULL) {
        if (!current->is_allocated) {
            // Calculate usable size (excluding header)
            size_t usable_size = current->size - sizeof(mem_block_t);
            if (usable_size < size) {
                count++;
            }
        }
        current = current->next;
    }

    return count;
}

int k_mem_count_extfrag(size_t size) {
    int result;
    __asm("SVC #10" : "=r" (result) : "r" (size));
    return result;
}



// Finds free block using First Fit algorithm
static mem_block_t* find_free_block(size_t size) {
    mem_block_t* current = free_list_head;

    while (current != NULL) {
        // Double-check that blocks in free list are actually free
        if (!current->is_allocated && current->size >= size) {
            return current;
        }
        current = current->next;
    }

    return NULL;
}


// Split block into 2 if larger than needed
static void split_block(mem_block_t* block, size_t size) {
    if (block->size < size + sizeof(mem_block_t) + 16) {
        return;
    }

    // Create new block from the remainder
    mem_block_t* new_block = (mem_block_t*)((U8*)block + size);
    new_block->size = block->size - size;
    new_block->is_allocated = 0;
    new_block->owner_tid = TID_NULL;

    // Update original block size
    block->size = size;

    // Insert new block into the list after current block
    new_block->next = block->next;
    new_block->prev = block;

    if (block->next != NULL) {
        block->next->prev = new_block;
    }
    block->next = new_block;



    // Add new block to free list
    add_to_free_list(new_block);
}





// Coalesce free blocks
static void coalesce_free_blocks(mem_block_t* block) {
    // Try to coalesce with next block
    if (block->next != NULL && !block->next->is_allocated) {
        mem_block_t* next_block = block->next;

        // Check if blocks are adjacent
        if ((U8*)block + block->size == (U8*)next_block) {
            block->size += next_block->size;
            block->next = next_block->next;
            if (next_block->next != NULL) {
                next_block->next->prev = block;
            }


            remove_from_free_list(next_block);
        }
    }

    // Try to coalesce with previous block
    if (block->prev != NULL && !block->prev->is_allocated) {
        mem_block_t* prev_block = block->prev;

        // Check if blocks are adjacent
        if ((U8*)prev_block + prev_block->size == (U8*)block) {
            prev_block->size += block->size;
            prev_block->next = block->next;
            if (block->next != NULL) {
                block->next->prev = prev_block;
            }
            remove_from_free_list(block);
        }
    }


}


// Add block to free list
static void add_to_free_list(mem_block_t* block) {
    // Clear any existing pointers
    block->next = NULL;
    block->prev = NULL;

    // If free list is empty then this becomes the head
    if (free_list_head == NULL) {
        free_list_head = block;
        return;
    }

    // Insert in address order for better coalescing
    mem_block_t* current = free_list_head;
    mem_block_t* prev = NULL;

    // Find correct position to insert
    while (current != NULL && current < block) {
        prev = current;
        current = current->next;
    }



    if (prev == NULL) {
        // Insert at beginning
        block->next = free_list_head;
        if (free_list_head != NULL) {
            free_list_head->prev = block;
        }


        free_list_head = block;
    } else {
        // Insert between previous and current
        block->next = current;
        block->prev = prev;
        prev->next = block;
        if (current != NULL) {
            current->prev = block;
        }
    }
}



//remove block from free list
static void remove_from_free_list(mem_block_t* block) {
    if (block->prev != NULL) {
        block->prev->next = block->next;
    } else {
        // Head of the free list
        free_list_head = block->next;
    }

    if (block->next != NULL) {
        block->next->prev = block->prev;
    }

    // Clear pointers
    block->next = NULL;
    block->prev = NULL;
}



// checks if pointer is valid for deallocation
static U8 is_valid_pointer(void* ptr) {
    U8* byte_ptr = (U8*)ptr;

    // Check if pointer is within heap bounds
    if (byte_ptr < heap_start + sizeof(mem_block_t) || byte_ptr >= heap_end) {
        return 0;
    }

    // Check if block header is within heap bounds
    mem_block_t* block = (mem_block_t*)(byte_ptr - sizeof(mem_block_t));
    if ((U8*)block < heap_start || (U8*)block >= heap_end) {
        return 0;
    }

    // Check if block size is reasonable
    if (block->size < sizeof(mem_block_t) + 4 ||
        (U8*)block + block->size > heap_end) {
        return 0;
    }

    return 1;
}








//functions fpr debugging
U8 k_mem_is_initialized(void) {
    return memory_initialized;
}

void* k_mem_get_heap_start(void) {
    return heap_start;
}

void* k_mem_get_heap_end(void) {
    return heap_end;
}

void* k_mem_get_free_list_head(void) {
    return free_list_head;
}

void k_mem_debug_state(const char* when) {
    printf("=== MEMORY STATE %s ===\n", when);
    printf("memory_initialized = %d\n", memory_initialized);
    printf("heap_start = %p\n", heap_start);
    printf("heap_end = %p\n", heap_end);
    printf("free_list_head = %p\n", free_list_head);

    if (free_list_head != NULL) {
        printf("free_list_head->size = %lu\n", free_list_head->size);
        printf("free_list_head->is_allocated = %d\n", free_list_head->is_allocated);
    }

    // Count free blocks
    int free_count = 0;
    mem_block_t* current = free_list_head;
    while (current != NULL && free_count < 10) {
        if (!current->is_allocated) {
            free_count++;
        }
        current = current->next;
    }
    printf("Free blocks in list: %d\n", free_count);
    printf("=========================\n");
}


// force reset all memory manager state
void k_mem_force_reset(void) {
    // If heap was initialized then clear the heap memory
    if (heap_start != NULL && heap_end != NULL) {
        size_t heap_size = heap_end - heap_start;
        // Clear entire heap memory
        for (size_t i = 0; i < heap_size; i++) {
            heap_start[i] = 0;
        }
        printf("Cleared %zu bytes of heap memory\n", heap_size);
    }

    // resets all global state
    heap_start = NULL;
    heap_end = NULL;
    free_list_head = NULL;
    memory_initialized = 0;

    printf("Memory manager forcibly reset\n");
}
