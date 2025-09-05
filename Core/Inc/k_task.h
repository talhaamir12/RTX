/*
 * k_task.h
 *
 *  Created on: Jan 5, 2024
 *      Author: nexususer
 *
 *      NOTE: any C functions you write must go into a corresponding c file that you create in the Core->Src folder
 */

#ifndef INC_K_TASK_H_
#define INC_K_TASK_H_
#include "common.h"
#define TASK_NEW 0
#define TASK_EXISTING 1

extern TCB g_tasks[MAX_TASKS];
extern task_t g_current_tid;

// functions for Part 1
void osKernelInit(void);
int osCreateTask(TCB* task);
int osKernelStart(void);
void osYield(void);
int osTaskInfo(task_t tid, TCB* task_copy);
task_t osGetTID(void);
int osTaskExit(void);

// functions for Part 3
void osSleep(int timeInMs);
void osPeriodYield(void);
int osSetDeadline(int deadline, task_t TID);
int osCreateDeadlineTask(int deadline, TCB* task);

// Implementation functions for SVC backing
int osCreateTask_impl(TCB* task);
int osTaskInfo_impl(task_t tid, TCB* task_copy);
void osKernelInit_impl(void);
int osCreateDeadlineTask_impl(int deadline, TCB* task);
task_t osGetTID_internal(void);

// Internal scheduler functions
task_t get_current_task_id(void);
void prepare_task_switch(void);
void trigger_context_switch(void);
void initialize_new_task_stack(task_t task_id);
task_t select_next_task(void);
void run_task_scheduler(void);
task_t edf_scheduler(void);
void update_task_times(void);
void handle_sleeping_tasks(void);

#endif /* INC_K_TASK_H_ */
