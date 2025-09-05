#include "k_task.h"
#include "k_mem.h"
#include "common.h"
#include <stdbool.h>

//  NULL cause can't use standard library
#ifndef NULL
#define NULL ((void*)0)
#endif



// definitions to replace stm32f4xx.h
#define SCB_ICSR_PENDSVSET_Msk (1UL << 28)

//  SCB structure
typedef struct {
    volatile const uint32_t CPUID;
    volatile uint32_t ICSR;
} SCB_Type;

#define SCB_BASE (0xE000ED00UL)
#define SCB ((SCB_Type *) SCB_BASE)



// SysTick struct
typedef struct {
    volatile uint32_t CTRL;
    volatile uint32_t LOAD;
    volatile uint32_t VAL;
    volatile const uint32_t CALIB;
} SysTick_Type;

#define SysTick_BASE (0xE000E010UL)
#define SysTick ((SysTick_Type *) SysTick_BASE)

// Manual inline functions for special registers in ARM
static inline uint32_t __get_PSP(void) {
    uint32_t result;
    __asm volatile ("MRS %0, psp" : "=r" (result));
    return(result);
}

static inline void __set_PSP(uint32_t topOfProcStack) {
    __asm volatile ("MSR psp, %0" : : "r" (topOfProcStack) : "sp");
}


static inline void __DSB(void) {
    __asm volatile ("dsb 0xF":::"memory");
}



static inline void __disable_irq(void) {
    __asm volatile ("cpsid i" : : : "memory");
}





static inline void __enable_irq(void) {
    __asm volatile ("cpsie i" : : : "memory");
}




// Global var
TCB g_tasks[MAX_TASKS];
task_t g_active_task_id = TID_NULL;
task_t target_task_id = TID_NULL;
U32 *task_stack_ptrs[MAX_TASKS];
int g_num_tasks = 0;
U8 g_kernel_initialized = 0;
U8 g_kernel_running = 0;
volatile U32 g_system_time = 0;

// ext declarations
extern volatile U32 g_system_time;

// ext functions
extern void start_first_task(void);
extern void perform_context_switch(void);

// mem block struct
typedef struct mem_block {
    U32 size;
    U8 is_allocated;
    task_t owner_tid;
    struct mem_block* next;
    struct mem_block* prev;
} mem_block_t;

//  null task that just yields more efficiently
void null_task_func(void *args) {
    while (1) {
        // just wait for interrupt
        __asm("wfi");
    }
}




// implementation for oskerenlinit where set everything up to clean initial state
void osKernelInit_impl(void) {

	// clear everything and start dormant
    for (int i = 0; i < MAX_TASKS; i++) {
        g_tasks[i].state = DORMANT;
        g_tasks[i].tid = i;
        g_tasks[i].ptask = NULL;
        g_tasks[i].stack_high = 0;
        g_tasks[i].stack_size = 0;
        g_tasks[i].stack_ptr = NULL;
        g_tasks[i].is_fresh_task = TASK_NEW;
        g_tasks[i].time_left = 0;
        g_tasks[i].deadline_value = 5;
        g_tasks[i].sleep_time = 0;
        g_tasks[i].period = 0;
        g_tasks[i].next_period_start = 0;
        g_tasks[i].is_periodic = 0;
        task_stack_ptrs[i] = NULL;
    }

    //  null task setup
    g_tasks[0].state = READY;
    g_tasks[0].ptask = &null_task_func;
    g_tasks[0].deadline_value = 0xFFFFFFFF;
    g_tasks[0].time_left = 0xFFFFFFFF;
    g_tasks[0].is_periodic = 0;

    // reset all global state
    g_num_tasks = 0;
    g_active_task_id = TID_NULL;
    g_kernel_initialized = 1;
    g_kernel_running = 0;

    // initalize mem mgmt after kernel is initialized
    if (k_mem_init_impl() != RTX_OK) {

    }
}

void osKernelInit(void) {
    __asm("SVC #18");
}

//  scheduler that handles both periodic and non periodic
task_t edf_scheduler(void) {
    U32 earliest_deadline = 0xFFFFFFFF;
    task_t selected_task = 0;
    int tasks_with_same_deadline = 0;
    task_t first_equal_deadline_task = TID_NULL;

    // find the earlest deadline
    for (task_t i = 1; i < MAX_TASKS; i++) {
        if (g_tasks[i].state == READY) {
            if (g_tasks[i].deadline_value < earliest_deadline) {
                earliest_deadline = g_tasks[i].deadline_value;
                selected_task = i;
                tasks_with_same_deadline = 1;
                first_equal_deadline_task = i;
            } else if (g_tasks[i].deadline_value == earliest_deadline) {
                tasks_with_same_deadline++;
                if (first_equal_deadline_task == TID_NULL) {
                    first_equal_deadline_task = i;
                }
            }
        }
    }

    // If multiple tasks have same earliest deadline then use RR
    if (tasks_with_same_deadline > 1) {
    	task_t current_task = osGetTID_internal();

        // next task with same deadline after current task
        for (task_t i = current_task + 1; i < MAX_TASKS; i++) {
            if (g_tasks[i].state == READY && g_tasks[i].deadline_value == earliest_deadline) {
                return i;
            }
        }

        // if didn't find one after current then look before
        for (task_t i = 1; i <= current_task; i++) {
            if (g_tasks[i].state == READY && g_tasks[i].deadline_value == earliest_deadline) {
                return i;
            }
        }
    }

    // return 0
    return selected_task;
}








// Context switch handler
void perform_context_switch(void) {
	task_t current_task = osGetTID_internal();

    // save where current task stack pointer is
    if (current_task != TID_NULL) {

        task_stack_ptrs[current_task] = (U32*)__get_PSP();
    }

    // get next task stack pointer and make it the current task
    if (target_task_id != TID_NULL) {
        __set_PSP((uint32_t)task_stack_ptrs[target_task_id]);
        g_active_task_id = target_task_id;
    }
}

// Trigger context switch
void trigger_context_switch(void) {


	task_t current_task = osGetTID_internal();

    target_task_id = edf_scheduler();

    if (target_task_id == TID_NULL || target_task_id == current_task) {
        return;
    }

    // sets up new task
    if (g_tasks[target_task_id].is_fresh_task == TASK_NEW) {
        task_stack_ptrs[target_task_id] = (U32 *)g_tasks[target_task_id].stack_high;

        // For xPSR, PC and LR
        *(--task_stack_ptrs[target_task_id]) = (1 << 24);
        *(--task_stack_ptrs[target_task_id]) = (U32)(g_tasks[target_task_id].ptask);
        *(--task_stack_ptrs[target_task_id]) = (U32)(osTaskExit);

        // For R12, R3, R2, R1, R0
        for (int i = 0; i < 5; i++) {
            *(--task_stack_ptrs[target_task_id]) = 0xAAAAAAAA;
        }

        // For R11, R10, R9, R8, R7, R6, R5, R4
        for (int i = 0; i < 8; i++) {
            *(--task_stack_ptrs[target_task_id]) = 0xAAAAAAAA;
        }
    }

    // Update task states
    if (current_task != TID_NULL && g_tasks[current_task].state == RUNNING) {
        g_tasks[current_task].state = READY;
        // Rst timer for preempted task if deadline-expired
        if (g_tasks[current_task].time_left == 0) {
            g_tasks[current_task].time_left = g_tasks[current_task].deadline_value;
        }
    }

    g_tasks[target_task_id].state = RUNNING;
    g_tasks[target_task_id].is_fresh_task = TASK_EXISTING;

    // Rst target tasks deadline timer when starts running
    if (g_tasks[target_task_id].time_left == 0) {
        g_tasks[target_task_id].time_left = g_tasks[target_task_id].deadline_value;
    }

    // Trigger hardware context switch
    SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;
    __asm volatile ("ISB");
}

// SVC Handler for system calls
void SVC_Handler_Main(unsigned int *svc_args) {

    uint8_t svc_number = ((char *)svc_args[6])[-2];

    switch (svc_number) {
    // Start kernel
        case 0:
            __set_PSP((uint32_t)task_stack_ptrs[target_task_id]);
            start_first_task();
            break;

         // yield
        case 1:
            // if switching to a new task then set up stack
            if (target_task_id != TID_NULL && g_tasks[target_task_id].is_fresh_task == TASK_NEW) {
                task_stack_ptrs[target_task_id] = (U32 *)g_tasks[target_task_id].stack_high;

                *(--task_stack_ptrs[target_task_id]) = (1 << 24);
                *(--task_stack_ptrs[target_task_id]) = (U32)(g_tasks[target_task_id].ptask);
                *(--task_stack_ptrs[target_task_id]) = (U32)(osTaskExit);

                // For R12, R3, R2, R1, R0
                for (int i = 0; i < 5; i++) {
                    *(--task_stack_ptrs[target_task_id]) = 0xAAAAAAAA;
                }

                // For R11, R10, R9, R8, R7, R6, R5, R4
                for (int i = 0; i < 8; i++) {
                    *(--task_stack_ptrs[target_task_id]) = 0xAAAAAAAA;
                }

                g_tasks[target_task_id].is_fresh_task = TASK_EXISTING;
            }

            // set target task to running and rst its time_left
            if (target_task_id != TID_NULL) {
                g_tasks[target_task_id].state = RUNNING;
                if (g_tasks[target_task_id].time_left == 0) {
                    g_tasks[target_task_id].time_left = g_tasks[target_task_id].deadline_value;
                }
            }

            // trigger PendSV
            SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;
            __asm volatile ("ISB");
            break;


        // Createtask
        case 2:
        {
            TCB* task = (TCB*)svc_args[0];
            svc_args[0] = osCreateTask_impl(task);
        }
        break;


        // CreateDeadlinetask
        case 3:
            {
                int deadline = svc_args[0];
                TCB* task = (TCB*)svc_args[1];
                svc_args[0] = osCreateDeadlineTask_impl(deadline, task);
            }
            break;


        // Setdeadline
        case 4:
            {
                int deadline = svc_args[0];
                task_t tid = svc_args[1];
                // defaults to error
                svc_args[0] = RTX_ERR;

                if (deadline > 0 && tid < MAX_TASKS &&
                    (g_tasks[tid].state == READY || g_tasks[tid].state == RUNNING)) {

                	// blocks timer interrupts
                    __disable_irq();
                    g_tasks[tid].deadline_value = deadline;
                    g_tasks[tid].time_left = deadline;

                    // check if preemption is needed
                    if (g_active_task_id != TID_NULL &&
                        g_tasks[tid].deadline_value < g_tasks[g_active_task_id].deadline_value) {
                        __enable_irq();
                        trigger_context_switch();
                        return;
                    }
                    __enable_irq();
                    svc_args[0] = RTX_OK;
                }
            }
            break;


        // Task Info
        case 5:
            {
                task_t tid = svc_args[0];
                TCB* task_copy = (TCB*)svc_args[1];
                svc_args[0] = osTaskInfo_impl(tid, task_copy);
            }
            break;





        // Meminit
        case 7:
            svc_args[0] = k_mem_init_impl();
            break;



        // Memalloc
        case 8:
            {
                size_t size = svc_args[0];
                svc_args[0] = (unsigned int)k_mem_alloc_impl(size);
            }
            break;

        // Memdealloc
        case 9:
            {
                void* ptr = (void*)svc_args[0];
                svc_args[0] = k_mem_dealloc_impl(ptr);
            }
            break;


         // mem cnt extfrag
        case 10:
            {
                size_t size = svc_args[0];
                svc_args[0] = k_mem_count_extfrag_impl(size);
            }
            break;










         //  getTID
        case 15:
            svc_args[0] = g_active_task_id;
            break;

        // Taskexit
        case 17:
            if (g_active_task_id != TID_NULL) {
                // Free the stack using the stored base pointer
                if (k_mem_dealloc_impl(g_tasks[g_active_task_id].stack_base) != RTX_OK) {
                    // Handle error but continue cleanup
                }

                g_tasks[g_active_task_id].state = DORMANT;
                g_tasks[g_active_task_id].ptask = NULL;
                g_tasks[g_active_task_id].stack_high = 0;
                g_tasks[g_active_task_id].stack_size = 0;
                g_tasks[g_active_task_id].stack_base = NULL;
                g_tasks[g_active_task_id].stack_ptr = NULL;
                g_tasks[g_active_task_id].is_fresh_task = TASK_NEW;
                task_stack_ptrs[g_active_task_id] = NULL;

                g_num_tasks--;
                trigger_context_switch();
            }
            break;

        // Kernelinit
        case 18:
            osKernelInit_impl();
            break;

        default:
            break;
    }
}

// Functions for lab

// osgetTID function which just calls svc case
task_t osGetTID(void) {
    task_t result;
    __asm volatile (
        "svc #15\n\t"
        "mov %0, r0"
        : "=r" (result)
        :
        : "r0"
    );
    return result;
}

 // Internal copy of getostid so can be backed by svc call
	task_t osGetTID_internal(void) {
	    return g_active_task_id;
	}


// osYield
void osYield(void) {
	task_t current_task = osGetTID_internal();

    if (current_task != TID_NULL) {
        __disable_irq();

        // Set current task back to READY
        g_tasks[current_task].state = READY;

        // Only reset timer for nonperiodic tasks
        if (!g_tasks[current_task].is_periodic) {
            g_tasks[current_task].time_left = g_tasks[current_task].deadline_value;
        }


        // find next task to run
        target_task_id = edf_scheduler();

        __enable_irq();

        if (target_task_id != TID_NULL) {
            __asm("SVC #1");
        }
    }
}




// osSleep which uses SVC#1 for yield logic since similar
void osSleep(int timeInMs) {
	task_t current_task = osGetTID_internal();
    if (current_task != TID_NULL && timeInMs > 0) {
        __disable_irq();

        // Sets task to sleeping
        g_tasks[current_task].state = SLEEPING;
        g_tasks[current_task].time_left = timeInMs;
        target_task_id = edf_scheduler();

        __enable_irq();

        if (target_task_id != TID_NULL) {
        	// yield SVC call
            __asm("SVC #1");
        } else {
            while (g_tasks[current_task].state == SLEEPING) {
                __asm("wfi");
            }
        }
    }
}




// OsPeriodyield just calls osSleep with tasks deadline value
void osPeriodYield(void) {
	task_t current_tid = osGetTID_internal();


    if (current_tid != TID_NULL) {
        if (g_tasks[current_tid].is_periodic) {
            int remaining_time = g_tasks[current_tid].time_left;
            if (remaining_time > 0) {
            	// sleep until period ends
            	osSleep(remaining_time);


            } else {
            	// reset for next period
                g_tasks[current_tid].time_left = g_tasks[current_tid].deadline_value;
            }
        } else {
        	// for nonperiodic task just sleep till deadline
            osSleep(g_tasks[current_tid].deadline_value);
        }
    }
}



// ossetdeadline which just calls svc call
int osSetDeadline(int deadline, task_t TID) {
    int result;
    __asm("SVC #4" : "=r" (result) : "r" (deadline), "r" (TID));
    return result;
}







// implementation of oscreatetask
int osCreateTask_impl(TCB *task) {
	// cant make tasks until kernel is ready
    if (!g_kernel_initialized) {
        return RTX_ERR;
    }
   // invalid params
    if (task == NULL || task->ptask == NULL || task->stack_size == 0) {
        return RTX_ERR;
    }
    // stack too small
    if (task->stack_size < STACK_SIZE) {
        return RTX_ERR;
    }

    // find empty task slot to use
    task_t new_tid = TID_NULL;
    for (int i = 1; i < MAX_TASKS; i++) {
        if (g_tasks[i].state == DORMANT) {
            new_tid = i;
            break;
        }
    }

    if (new_tid == TID_NULL || g_num_tasks >= (MAX_TASKS - 1)) {
        return RTX_ERR;
    }

    // alloc mem for task stack
    void* allocated_stack = k_mem_alloc_impl(task->stack_size);
    if (allocated_stack == NULL) {
        return RTX_ERR;
    }

    // initialize all TCB fields
    g_tasks[new_tid].ptask = task->ptask;
    g_tasks[new_tid].stack_size = task->stack_size;
    g_tasks[new_tid].stack_high = (U32)allocated_stack + task->stack_size;
    g_tasks[new_tid].stack_base = allocated_stack;
    g_tasks[new_tid].tid = new_tid;
    g_tasks[new_tid].state = READY;
    g_tasks[new_tid].stack_ptr = NULL;
    g_tasks[new_tid].is_fresh_task = TASK_NEW;
    g_tasks[new_tid].deadline_value = 5;
    g_tasks[new_tid].time_left = 5;
    g_tasks[new_tid].sleep_time = 0;
    g_tasks[new_tid].period = 0;
    g_tasks[new_tid].next_period_start = 0;
    g_tasks[new_tid].is_periodic = 0;

    // update mem block to new task
    mem_block_t* block = (mem_block_t*)((U8*)allocated_stack - sizeof(mem_block_t));
    block->owner_tid = new_tid;

    // update input task with assigned TID and stack info
    task->tid = new_tid;
    task->stack_high = g_tasks[new_tid].stack_high;

    g_num_tasks++;

    // check preemption if kernel is running
    if (g_kernel_running && g_active_task_id != TID_NULL) {
        if (g_tasks[new_tid].deadline_value < g_tasks[g_active_task_id].deadline_value) {
            trigger_context_switch();
        }
    }

    return RTX_OK;
}


// oscreatetask which just calls svc call
int osCreateTask(TCB *task) {
    int result;
    __asm volatile (
        "mov r0, %1\n\t"
        "svc #2\n\t"
        "mov %0, r0"
        : "=r" (result)
        : "r" (task)
        : "r0"
    );
    return result;
}











// osCreateDeadlineTask implementation
int osCreateDeadlineTask_impl(int deadline, TCB* task) {
    if (deadline <= 0 || task == NULL || task->stack_size < STACK_SIZE) {
        return RTX_ERR;
    }

    // create task using oscreate task logic
    int result = osCreateTask_impl(task);
    if (result != RTX_OK) {
        return result;
    }

    // update deadline and mark as periodic
    task_t new_tid = task->tid;
    g_tasks[new_tid].deadline_value = deadline;
    g_tasks[new_tid].time_left = deadline;
    g_tasks[new_tid].next_period_start = 0;
    // marks as periodic
    g_tasks[new_tid].is_periodic = 1;

    // Check for preemption
    if (g_kernel_running && g_active_task_id != TID_NULL) {
        if (deadline < g_tasks[g_active_task_id].deadline_value) {
            trigger_context_switch();
        }
    }

    return RTX_OK;
}

// oscreatedeadlinetask which just calls svc call
int osCreateDeadlineTask(int deadline, TCB* task) {
    int result;
    // Use the same approach that worked for osTaskInfo
    __asm volatile (
        "mov r0, %1\n\t"
        "mov r1, %2\n\t"
        "svc #3\n\t"
        "mov %0, r0"
        : "=r" (result)
        : "r" (deadline), "r" (task)
        : "r0", "r1"
    );
    return result;
}






// oskernelstart
int osKernelStart(void) {
    if (!g_kernel_initialized || g_kernel_running) {
        return RTX_ERR;
    }

    // find first task to run
    target_task_id = edf_scheduler();
    if (target_task_id == TID_NULL) {
        return RTX_ERR;
    }




    // set up first task
    g_active_task_id = target_task_id;
    task_stack_ptrs[target_task_id] = (U32 *)g_tasks[target_task_id].stack_high;
    // set up stack frame
    *(--task_stack_ptrs[target_task_id]) = (1 << 24);
    *(--task_stack_ptrs[target_task_id]) = (U32)(g_tasks[target_task_id].ptask);
    *(--task_stack_ptrs[target_task_id]) = (U32)(osTaskExit);
    // fill in bogus register values
    for (int j = 0; j < 13; j++) {
        *(--task_stack_ptrs[target_task_id]) = 0xAAAAAAAA;
    }

    g_tasks[target_task_id].state = RUNNING;
    g_tasks[target_task_id].is_fresh_task = TASK_EXISTING;
    g_tasks[target_task_id].time_left = g_tasks[target_task_id].deadline_value;

    // make sure all tasks start with fresh deadlines
    for (int i = 1; i < MAX_TASKS; i++) {
        if (g_tasks[i].state == READY) {
            g_tasks[i].time_left = g_tasks[i].deadline_value;
        }
    }

    g_kernel_running = 1;

    // timer resets
    g_system_time = 0;
    SysTick->VAL = 0;
    extern volatile uint32_t uwTick;
    uwTick = 0;

    __asm("SVC #0");
    return RTX_ERR;
}



// implementation for ostaskinfo
int osTaskInfo_impl(task_t tid, TCB* task_copy) {
    if (tid >= MAX_TASKS || task_copy == NULL) {
        return RTX_ERR;
    }

    // copies entire TCB
    *task_copy = g_tasks[tid];
    return RTX_OK;
}

// ostaskinfo function which just calls svc call
int osTaskInfo(task_t tid, TCB* task_copy) {
    int result;

    __asm volatile (
        "mov r0, %1\n\t"
        "mov r1, %2\n\t"
        "svc #5\n\t"
        "mov %0, r0"
        : "=r" (result)
        : "r" (tid), "r" (task_copy)
        : "r0", "r1"
    );
    return result;
}




// ostaskexit which just calls svc call
int osTaskExit(void) {
    __asm("SVC #17");
    return RTX_OK;
}
