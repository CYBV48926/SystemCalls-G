#define SYSTEM_CALLS_PROJECT
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <THREADSLib.h>
#include <Messaging.h>
#include <Scheduler.h>
#include <TList.h>
#include "SystemCalls.h"
#include "libuser.h"

/* -------------------- Typedefs and Structs ------------------------------- */
typedef struct sem_data
{
    int status;
    int sem_id;
    int count;
    int waitQueue[MAXPROC]; // pids of processes blocked on this semaphore, in FIFO order
    int waitHead;           // index of the next pid to unblock (front of queue)
    int waitTail;           // index where the next pid will be inserted (back of queue)
    int waitCount;          // number of processes currently blocked on this semaphore
} SemData;

typedef struct user_proc
{
    struct user_proc* pNextSibling;
    struct user_proc* pPrevSibling;
    struct user_proc* pNextSem;
    struct user_proc* pPrevSem;
    TList             children;
    int              (*startFunc) (char*); // function where process begins
    int               pid;// process id
    int               parentPid; // pid of parent process, or 0 if no parent
    char* arg; // argument to pass to startFunc
    int               status; // STATUS_EMPTY, STATUS_RUNNING, STATUS_READY, STATUS_QUIT, STATUS_WAIT_BLOCKED, or STATUS_JOIN_BLOCKED
} UserProcess;

#define MAXSEMS 200

/* -------------------------- Globals ------------------------------------- */
static UserProcess userProcTable[MAXPROC];
static SemData semTable[MAXSEMS];
static int nextsem_id = 0;

/* ------------------------- Prototypes ----------------------------------- */
void sys_exit(int resultCode);
int sys_wait(int* pStatus);
int sys_spawn(char* name, int (*startFunc)(char*), char* arg, int stackSize, int priority);
int MessagingEntryPoint(char*);
extern int SystemCallsEntryPoint(void* pArgs);

static int launchUserProcess(char* pArg);

/* syscall handler prototypes */
static void sysNull(system_call_arguments_t* args);
static void spawn_syscall_handler(system_call_arguments_t* args);
static void wait_syscall_handler(system_call_arguments_t* args);
static void exit_syscall_handler(system_call_arguments_t* args);
static void semcreate_syscall_handler(system_call_arguments_t* args);
static void semp_syscall_handler(system_call_arguments_t* args);
static void semv_syscall_handler(system_call_arguments_t* args);
static void semfree_syscall_handler(system_call_arguments_t* args);


static int findFreeProcSlot(void);
static UserProcess* findProcByPid(int pid);
static void syscall_interrupt_handler(char deviceId[32], uint8_t command, uint32_t status, void* pArgs);


int MessagingEntryPoint(char* arg)
{
    int status = -1;
    int pid;
    int i; // loop index variable
    interrupt_handler_t* handlers; // array of interrupt handlers


    if (!(get_psr() & PSR_KERNEL_MODE)) // checkKernelMode(__func__) will stop the system if not in kernel mode, but we'll also output a message here for clarity.
    {
        console_output(FALSE, "MessagingEntryPoint(): not in kernel mode\n");
        stop(1);
    }

    /* Initialize the semaphore table */
    memset(semTable, 0, sizeof(semTable)); // set all bytes to 0, which sets sem_id to -1, count to 0, and status to STATUS_EMPTY
    for (i = 0; i < MAXSEMS; i++) //  set sem_id and status for each entry
    {
        semTable[i].sem_id = -1; // invalid sem_id to indicate this entry is not in use
        semTable[i].status = STATUS_EMPTY; // mark this entry as empty
    }
    nextsem_id = 0; // initialize nextsem_id to 0 so the first created semaphore will have sem_id 0

    /* initialize the system call vector */
    for (i = 0; i < THREADS_MAX_SYSCALLS; i++) // set all entries to sysNull to handle invalid system calls
    {
        systemCallVector[i] = sysNull; // default handler for invalid system calls
    }
    systemCallVector[SYS_SPAWN] = spawn_syscall_handler;       // set handler for SYS_SPAWN system call
    systemCallVector[SYS_WAIT] = wait_syscall_handler;         // set handler for SYS_WAIT system call
    systemCallVector[SYS_EXIT] = exit_syscall_handler;         // set handler for SYS_EXIT system call
    systemCallVector[SYS_SEMCREATE] = semcreate_syscall_handler; // set handler for SYS_SEMCREATE system call
    systemCallVector[SYS_SEMP] = semp_syscall_handler;         // set handler for SYS_SEMP system call
    systemCallVector[SYS_SEMV] = semv_syscall_handler;         // set handler for SYS_SEMV system call
    systemCallVector[SYS_SEMFREE] = semfree_syscall_handler;   // set handler for SYS_SEMFREE system call

    handlers = get_interrupt_handlers(); // get the array of interrupt handlers from the THREADS library
    handlers[THREADS_SYS_CALL_INTERRUPT] = syscall_interrupt_handler; // set our syscall_interrupt_handler to handle system call interrupts

    /* Initialize the process table */
    memset(userProcTable, 0, sizeof(userProcTable)); // set all bytes to 0, which sets pid to 0, parentPid to 0, startFunc to NULL, arg to NULL, status to STATUS_EMPTY, and all pointers to NULL


    pid = sys_spawn("SystemCalls", (int (*)(char*))SystemCallsEntryPoint, NULL, THREADS_MIN_STACK_SIZE * 4, 3); // create the SystemCalls process, which will execute the SystemCallsEntryPoint function. 

    /* Wait for all child processes to finish */
    while (sys_wait(&status) != -1)
        ; // keep waiting until there are no more children to wait for

    

    return 0;
} /* MessagingEntryPoint */


static int launchUserProcess(char* pArg)
{
    int pid;
    int resultCode;
    UserProcess* pProc;

    /* if signaled when in the sys handler, then Exit */
    if (signaled())
    {
        console_output(FALSE, "%s - Process signaled in launch.\n", "launchUserProcess");
        sys_exit(0);
    }


    pid = k_getpid(); // get the pid of this process so we can find the corresponding UserProcess struct
    pProc = findProcByPid(pid); // find the UserProcess struct for this process using its pid.


    set_psr(get_psr() & ~PSR_KERNEL_MODE); // set mode to user mode by clearing the PSR_KERNEL_MODE bit in the processor status register


    if (pProc != NULL && pProc->startFunc != NULL)// call the startup function for this process if it exists
    {
        resultCode = pProc->startFunc((char*)pProc->arg); // call the startFunc for this process, passing in the arg for this process. Store the return value as the result code for this process.
    }
    else
    {
        resultCode = 0; // if we couldn't find the UserProcess struct or the startFunc, just use 0 as the result code
    }

    /* exit if the startup function returns -- use sys_exit so the process table is cleaned up */
    set_psr(get_psr() | PSR_KERNEL_MODE); // restore kernel mode before calling sys_exit
    sys_exit(resultCode); // clean up the process table entry and exit

    return 0;
}


int k_semp(int sem_id)
{
    int i;// loop index variable
    int result = -1;

    if (sem_id < 0 || sem_id >= MAXSEMS) // validate sem_id
        return -1;

    for (i = 0; i < MAXSEMS; i++) // find the semaphore with the given sem_id
    {
        if (semTable[i].sem_id == sem_id) // if we found the semaphore with the given sem_id
        {
            if (semTable[i].count > 0) // if the count is greater than 0, we can decrement it and proceed
            {
                semTable[i].count--; // decrement the count to indicate we are using one unit of the semaphore
                result = 0; // set result to 0 to indicate success
            }
            else
            {
                /* Enqueue this process's pid into the semaphore's wait queue before blocking */
                int myPid = k_getpid();
                semTable[i].waitQueue[semTable[i].waitTail] = myPid; // store our pid at the tail of the wait queue
                semTable[i].waitTail = (semTable[i].waitTail + 1) % MAXPROC; // advance the tail index, wrapping around if necessary
                semTable[i].waitCount++; // increment the count of blocked processes

                block(STATUS_WAIT_BLOCKED); // block this process until the semaphore is available.
                if (signaled()) // if we were signaled while blocked, then we should return -1 to indicate an error
                    result = -1;
                else
                    result = 0;
            }
            break;
        }
    }
    return result;
}

int k_semv(int sem_id) // V operation on the semaphore with the given sem_id
{
    int i;
    int result = -1;

    if (sem_id < 0 || sem_id >= MAXSEMS) // validate sem_id 
        return -1;

    for (i = 0; i < MAXSEMS; i++)
    {
        if (semTable[i].sem_id == sem_id) // if we found the semaphore with the given sem_id
        {
            if (semTable[i].waitCount > 0) // if there is a process blocked on this semaphore, wake it up instead of incrementing the count
            {
                int pidToUnblock = semTable[i].waitQueue[semTable[i].waitHead]; // get the pid of the process at the front of the wait queue
                semTable[i].waitHead = (semTable[i].waitHead + 1) % MAXPROC; // advance the head index, wrapping around if necessary
                semTable[i].waitCount--; // decrement the count of blocked processes
                unblock(pidToUnblock); // wake up the blocked process so it can proceed past its block() call
            }
            else
            {
                semTable[i].count++; // no blocked processes -- increment the count to indicate we are releasing one unit of the semaphore
            }
            result = 0;
            break;
        }
    }
    return result;
}

int k_semcreate(int initial_value)
{
    int i;
    int sem_id = -1;

    if (initial_value < 0) // validate initial_value
        return -1;

    for (i = 0; i < MAXSEMS; i++) // find an empty slot in the semaphore table
    {
        if (semTable[i].status == STATUS_EMPTY) // if we found an empty slot
        {
            sem_id = nextsem_id++; // assign the next available sem_id to this semaphore and increment nextsem_id for the next semaphore that gets created
            semTable[i].sem_id = sem_id; // set the sem_id for this semaphore
            semTable[i].count = initial_value; // set the count for this semaphore to the initial value
            semTable[i].status = STATUS_RUNNING; // set the status for this semaphore to STATUS_RUNNING to indicate it is in use
            semTable[i].waitHead = 0; // initialize the wait queue head index to 0
            semTable[i].waitTail = 0; // initialize the wait queue tail index to 0
            semTable[i].waitCount = 0; // initialize the count of blocked processes to 0
            break;
        }
    }
    return sem_id;
}

int k_semfree(int sem_id)
{
    int i;
    int result = -1;

    for (i = 0; i < MAXSEMS; i++) // find the semaphore with the given sem_id
    {
        if (semTable[i].sem_id == sem_id) // if we found the semaphore with the given sem_id
        {
            /* Unblock any processes still waiting on this semaphore before freeing it */
            while (semTable[i].waitCount > 0)
            {
                int pidToUnblock = semTable[i].waitQueue[semTable[i].waitHead]; // get the pid of the process at the front of the wait queue
                semTable[i].waitHead = (semTable[i].waitHead + 1) % MAXPROC; // advance the head index
                semTable[i].waitCount--; // decrement the count of blocked processes
                unblock(pidToUnblock); // wake up the blocked process; it will detect signaled() or check the result
            }

            semTable[i].sem_id = -1; // set sem_id to -1 to indicate this entry is not in use
            semTable[i].count = 0; // reset count to 0
            semTable[i].waitHead = 0; // reset wait queue head index
            semTable[i].waitTail = 0; // reset wait queue tail index
            semTable[i].waitCount = 0; // reset count of blocked processes
            semTable[i].status = STATUS_EMPTY; // set status to STATUS_EMPTY to indicate this entry is empty
            result = 0;
            break;
        }
    }
    return result;
}

int sys_spawn(char* name, int (*startFunc)(char*), char* arg, int stackSize, int priority)
{
    int parentPid;
    int pid = -1;
    int procSlot; // index of the slot in the userProcTable for this new process
    UserProcess* pProcess;

    /* validate the parameters */
    if (name == NULL || startFunc == NULL || stackSize < THREADS_MIN_STACK_SIZE || priority < LOWEST_PRIORITY || priority > HIGHEST_PRIORITY) // validate parameters and return -1 if any are invalid
    {
        return -1;
    }
    /* we are the parent*/
    parentPid = k_getpid();

    procSlot = findFreeProcSlot(); // find a free slot in the userProcTable for this new process
    if (procSlot < 0)
    {
        console_output(FALSE, "sys_spawn(): no free process slots.\n");
        return -1;
    }

    pProcess = &userProcTable[procSlot]; // get a pointer to the UserProcess struct for this new process using the procSlot index
    pProcess->startFunc = startFunc; // set the startFunc for this new process to the startFunc parameter
    pProcess->arg = arg; // set the arg for this new process to the arg parameter
    pProcess->parentPid = parentPid; // set the parentPid for this new process to the pid of the parent process
    pProcess->status = STATUS_READY; // set the status for this new process to STATUS_READY to indicate it is ready to run.

    /* Pass the procSlot index as pArg so launchUserProcess can find its table entry
       immediately, without depending on pid being written back first. */
    pid = k_spawn(name, launchUserProcess, arg, stackSize, priority);
    if (pid < 0)
    {
        console_output(FALSE, "Failed to create user process.");
        memset(pProcess, 0, sizeof(UserProcess)); // reset the UserProcess struct for this new process to clear out any data we set before we found out we couldn't create the process
    }
    else
    {
        pProcess->pid = pid; // set the pid for this new process to the pid returned by k_spawn
    }
    return pid;
}

int sys_wait(int* pStatus)
{
    int pid;
    pid = k_wait(pStatus);
    return pid;
}

void sys_exit(int resultCode)
{
	int procSlot; // index of the slot in the userProcTable for this process
    
    int childStatus; // buffer to receive the child's exit status
    int pid = k_getpid();
    UserProcess* pProcess;
    //UserProcess* pChild;

    pid = k_getpid();
    procSlot = pid % MAXPROC;
    pProcess = &userProcTable[procSlot];
	// signal and join all children of this process before exiting
     


	// remove process from list of children of its parent
    if (pProcess->parentPid != 0) // if this process has a parent (parentPid of 0 indicates no parent)
    {
        UserProcess* pParent = findProcByPid(pProcess->parentPid); // find the UserProcess struct for the parent process using the parentPid
        if (pParent != NULL) // if we found the parent process
        {
            TListRemoveNode(&pParent->children, pProcess); // remove this process from the list of children in the parent process's UserProcess struct
		}
    }
        /* Wait for all signaled children to finish before this process exits */
    while (k_wait(&childStatus) != -1); // pass valid pointer -- k_wait writes exit status here
            

        pProcess->status = STATUS_QUIT;
        pProcess->pid = 0;
    
    k_exit(resultCode);
}



static void sysNull(system_call_arguments_t* args) // default handler for invalid system calls
{
    console_output(FALSE, "sysNull(): Invalid system_call %d\n", args->call_id); // output an error message indicating the invalid system call
    console_output(FALSE, "sysNull(): process %d terminating\n", k_getpid()); // output a message indicating that this process is terminating due to the invalid system call
    set_psr(get_psr() & ~PSR_KERNEL_MODE); //  set mode to user mode before exiting so that we exit from user mode instead of kernel mode.
    sys_exit(1);
}

static void spawn_syscall_handler(system_call_arguments_t* args)
{
    int (*startFunc)(char*) = (int (*)(char*)) args->arguments[0]; // cast the first argument to a function pointer with the correct signature
    char* arg = (char*)args->arguments[1]; // cast the second argument to a char* for the argument to pass to the startFunc
    int   stackSize = (int)args->arguments[2]; // cast the third argument to an int for the stack size
    int   priority = (int)args->arguments[3]; // cast the fourth argument to an int for the priority
    char* name = (char*)args->arguments[4]; // cast the fifth argument to a char* for the name of the new process
    int   pid;

    pid = sys_spawn(name, startFunc, arg, stackSize, priority);

    args->arguments[0] = (intptr_t)pid; // store the pid of the new process in the first argument to return it to the user process that made the system call
    args->arguments[3] = (intptr_t)(pid < 0 ? -1 : 0); // store the result code (0 for success, -1 for failure) in the fourth argument to return it to the user process that made the system call.

    set_psr(get_psr() & ~PSR_KERNEL_MODE); // set mode to user mode before returning so that we return to user mode instead of kernel mode.
}

static void wait_syscall_handler(system_call_arguments_t* args) // handler for the SYS_WAIT system call
{
    int status = 0; // variable to store the exit status of the child process that we are waiting for
    int pid; // variable to store the pid of the child process that we are waiting for, which is returned by sys_wait

    pid = sys_wait(&status); // call sys_wait with a pointer to the status variable to wait for a child process to finish and get its exit status.

    args->arguments[0] = (intptr_t)pid; // store the pid of the child process that finished in the first argument to return it to the user process that made the system call
    args->arguments[1] = (intptr_t)status; // store the exit status of the child process that finished in the second argument to return it to the user process that made the system call
    args->arguments[3] = (intptr_t)(pid < 0 ? -1 : 0); // store the result code (0 for success, -1 for failure) in the fourth argument to return it to the user process that made the system call.   

    set_psr(get_psr() & ~PSR_KERNEL_MODE); // set mode to user mode before returning so that we return to user mode instead of kernel mode.
}

static void exit_syscall_handler(system_call_arguments_t* args) // handler for the SYS_EXIT system call
{
    int status = (int)args->arguments[0]; // cast the first argument to an int for the exit status to pass to sys_exit

    sys_exit(status); // call sys_exit with the exit status to terminate this process and return the exit status to any parent process that is waiting for this process to finish.

}

static void semcreate_syscall_handler(system_call_arguments_t* args) // handler for the SYS_SEMCREATE system call
{
    int initial_value = (int)args->arguments[0]; // cast the first argument to an int for the initial semaphore value
    int sem_id;

    sem_id = k_semcreate(initial_value); // create the semaphore with the given initial value

    args->arguments[0] = (intptr_t)sem_id;          // return the sem_id to the caller
    args->arguments[3] = (intptr_t)(sem_id < 0 ? -1 : 0); // return 0 on success, -1 on failure

    set_psr(get_psr() & ~PSR_KERNEL_MODE); // restore user mode before returning
}

static void semp_syscall_handler(system_call_arguments_t* args) // handler for the SYS_SEMP system call
{
    int sem_id = (int)args->arguments[0]; // cast the first argument to an int for the semaphore handle
    int result;

    result = k_semp(sem_id); // perform the P (wait) operation on the semaphore

    args->arguments[3] = (intptr_t)(result < 0 ? -1 : 0); // return 0 on success, -1 on failure

    set_psr(get_psr() & ~PSR_KERNEL_MODE); // restore user mode before returning
}

static void semv_syscall_handler(system_call_arguments_t* args) // handler for the SYS_SEMV system call
{
    int sem_id = (int)args->arguments[0]; // cast the first argument to an int for the semaphore handle
    int result;

    result = k_semv(sem_id); // perform the V (signal) operation on the semaphore

    args->arguments[3] = (intptr_t)(result < 0 ? -1 : 0); // return 0 on success, -1 on failure

    set_psr(get_psr() & ~PSR_KERNEL_MODE); // restore user mode before returning
}

static void semfree_syscall_handler(system_call_arguments_t* args) // handler for the SYS_SEMFREE system call
{
    int sem_id = (int)args->arguments[0]; // cast the first argument to an int for the semaphore handle
    int result;

    result = k_semfree(sem_id); // free the semaphore with the given sem_id

    args->arguments[3] = (intptr_t)(result < 0 ? -1 : 0); // return 0 on success, -1 on failure

    set_psr(get_psr() & ~PSR_KERNEL_MODE); // restore user mode before returning
}


static int findFreeProcSlot(void) // returns index of free slot in userProcTable, or -1 if no free slot
{
    int i;
    for (i = 0; i < MAXPROC; i++) // linear search -- not efficient, but MAXPROC is small
    {
        if (userProcTable[i].pid == 0) // pid of 0 means slot is free -- no process will have pid of 0
            return i;
    }
    return -1;
}

static UserProcess* findProcByPid(int pid) // returns pointer to UserProcess with given pid, or NULL if no such process
{
    int i;
    for (i = 0; i < MAXPROC; i++) // linear search -- not efficient, but MAXPROC is small
    {
        if (userProcTable[i].pid == pid) // pid of 0 means slot is free -- no process will have pid of 0
            return &userProcTable[i];
    }
    return NULL;
}
static void syscall_interrupt_handler(char deviceId[32], uint8_t command, uint32_t status, void* pArgs) // handler for system call interrupts
{
    LARGE_INTEGER unit;
    int id;

    unit.QuadPart = (LONGLONG)deviceId; // use LARGE_INTEGER to safely extract the device id from the pointer
    id = unit.LowPart;                  // get the low 32-bit part as the integer device id

    if (id != THREADS_SYSTEM_CALL_ID) // if the deviceId does not match the expected value for system call interrupts, output an error message and return without handling this interrupt
    {
        console_output(FALSE, "syscall_interrupt_handler(): unexpected deviceId %d\n", id);
        return;
    }

    if (pArgs != NULL)
    {
        system_call_arguments_t* pSysCallArgs = (system_call_arguments_t*)pArgs; // cast the pArgs to a pointer to a system_call_arguments_t struct

        if (pSysCallArgs->call_id >= THREADS_MAX_SYSCALLS) // if the call_id in the system call arguments is out of range, output an error message and exit this process with an error code
        {
            console_output(FALSE, "syscall_interrupt_handler(): call_id %d out of range\n",
                pSysCallArgs->call_id);
            sys_exit(1);
            return;
        }

        if (systemCallVector[pSysCallArgs->call_id] != NULL) // if there is a handler for this system call, call the handler with the system call arguments
        {
            systemCallVector[pSysCallArgs->call_id](pSysCallArgs); // call the handler for this system call, passing in the system call arguments
        }
    }
}