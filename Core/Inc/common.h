/*
 * common.h
 *
 *  Created on: Jan 5, 2024
 *      Author: nexususer
 *
 *      NOTE: If you feel that there are common
 *      C functions corresponding to this
 *      header, then any C functions you write must go into a corresponding c file that you create in the Core->Src folder
 */

#ifndef INC_COMMON_H_
#define INC_COMMON_H_

#include <stdint.h>

// Define size_t ourselves since we can't use stddef.h
typedef unsigned int size_t;

// Task states
#define DORMANT 0
#define READY 1
#define RUNNING 2
#define SLEEPING 3

#define TASK_NEW 0
#define TASK_EXISTING 1

// Task IDs
#define TID_NULL 0

// System limits
#define MAX_TASKS 16
#define STACK_SIZE 0x400   // 1kb minimum stack size per task

// Type aliases for clarity
typedef uint32_t U32;
typedef uint16_t U16;
typedef uint8_t  U8;
typedef U32 task_t;

// Task Control Block (TCB)
typedef struct task_control_block {
    void (*ptask)(void* args);   // Pointer to task entry function - offset 0
    U32 stack_high;              // Start address (high) of task stack - offset 4
    task_t tid;                  // Task ID - offset 8
    U8 state;                    // Task state (DORMANT, READY, RUNNING, SLEEPING) - offset 12
    U16 stack_size;              // Size of stack (must be multiple of 8) - offset 14
    // Add your own fields below if needed
    U32 *stack_ptr;              // Current stack pointer position - offset 16 (but due to alignment will be 20)
    U8 is_fresh_task;            // TASK_NEW or TASK_EXISTING - offset 20/24
    U32 time_left;               // Time remaining for task - offset 24/28
    U32 deadline_value;          // Original deadline/timeslice value - offset 28/32
    U32 sleep_time;              // Time remaining to sleep (0 if not sleeping) - offset 32/36
    U32 period;                  // Period for periodic tasks (0 if not periodic) - offset 36/40
    U32 next_period_start;       // When the next period should start - offset 40/44
    U8 is_periodic;              // 0 for regular tasks, 1 for periodic tasks - offset 44/48
    void* stack_base;            // Base pointer returned by k_mem_alloc for freeing
} __attribute__((packed)) TCB;

// Return codes for kernel functions
#define RTX_OK  0
#define RTX_ERR (-1)

extern int g_num_tasks;
extern U8 g_kernel_initialized;
extern volatile U32 g_system_time;

#endif /* INC_COMMON_H_ */
