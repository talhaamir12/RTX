/*
 * k_mem.h
 *
 *  Created on: Jan 5, 2024
 *      Author: nexususer
 *
 *      NOTE: any C functions you write must go into a corresponding c file that you create in the Core->Src folder
 */

#ifndef INC_K_MEM_H_
#define INC_K_MEM_H_

#include "common.h"




// Initialize memory manager
int k_mem_init(void);


// Allocate memory with First Fit algorithm
void* k_mem_alloc(size_t size);



// Deallocate memory
int k_mem_dealloc(void* ptr);


// counts external frag for free blocks
int k_mem_count_extfrag(size_t size);


// impl functions for svc
void* k_mem_alloc_impl(size_t size);
int k_mem_init_impl(void);
int k_mem_count_extfrag_impl(size_t size);
int k_mem_dealloc_impl(void* ptr);



// debugging functions
U8 k_mem_is_initialized(void);
void* k_mem_get_heap_start(void);
void* k_mem_get_heap_end(void);
void* k_mem_get_free_list_head(void);
void k_mem_debug_state(const char* when);
void k_mem_force_reset(void);

#endif
