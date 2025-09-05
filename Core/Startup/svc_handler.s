.syntax unified
.cpu cortex-m4
.fpu softvfp
.thumb

.global SVC_Handler
.global start_first_task
.global PendSV_Handler

/*
 * Supervisor Call Handler
 * Routes system calls from user tasks to the kernel
 * Determines which stack (MSP/PSP) contains the exception frame
 */
.thumb_func
SVC_Handler:
    TST lr, #4              // Test bit 2 of LR to determine stack used
    ITE EQ                  // If-Then-Else block
    MRSEQ r0, MSP          // If EQ: exception used MSP, load MSP into r0
    MRSNE r0, PSP          // If NE: exception used PSP, load PSP into r0
    B SVC_Handler_Main      // Branch to C handler with stack pointer in r0

/*
 * First Task Launcher
 * Specialized routine for launching the initial task
 * Restores task context and switches to user mode execution
 */
.thumb_func
start_first_task:
    // Call C function to set up PSP properly
    BL perform_context_switch

    // Set up for thread mode with PSP
    MOV LR, #0xFFFFFFFD     // Set magic return value for thread mode with PSP
    MRS R0, PSP             // Load current Process Stack Pointer
    LDMIA R0!,{R4-R11}     // Load task's register context (R4-R11) from stack
    MSR PSP, R0             // Update PSP to point past loaded registers
    BX LR                   // Return to task using special LR value

/*
 * Pending Service Handler - Context Switch Implementation
 * Performs the actual register save/restore during task switching
 * Called automatically when PendSV exception is triggered
 */
.thumb_func
PendSV_Handler:
    // Save current task context if PSP is valid
    MRS R0, PSP                    // Get current task's stack pointer
    CBZ R0, PendSV_Handler_nosave  // Skip save if PSP is 0 (first task)
    STMDB R0!,{R4-R11}            // Save current task's registers (R4-R11) to stack
    MSR PSP, R0                    // Update PSP with new stack position

PendSV_Handler_nosave:
    // Call C function to handle task switching logic
    BL perform_context_switch

    // Load new task context
    MRS R0, PSP                    // Get new task's stack pointer
    LDMIA R0!,{R4-R11}            // Restore new task's registers from stack
    MSR PSP, R0                    // Update PSP for new task
    MOV LR, #0xFFFFFFFD           // Set return value for thread mode with PSP
    BX LR                          // Return to new task
