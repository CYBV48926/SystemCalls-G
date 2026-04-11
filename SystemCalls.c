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

    int mboxStartup; // mailbox used to synchronize the start of this process after it is spawned
    int mboxWait; // mailbox used to block/wake this process when it is waiting for a child to finish or being waited on by a parent that is quitting
    int mboxSem; // mailbox used to block/wake this process when it is blocked on a semaphore
    int mboxMutex; // mailbox used to provide mutual exclusion when accessing this process's data (e.g. its children list)
} UserProcess;

#define MAXSEMS 200
#define SLOT_RESERVED (-1)  /* sentinel: slot is claimed but pid not yet assigned */

/* -------------------------- Globals ------------------------------------- */
static UserProcess userProcTable[MAXPROC];
static SemData semTable[MAXSEMS];
static int nextsem_id = 0;
static int procTableMutex;

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
static void getpid_syscall_handler(system_call_arguments_t* args);
static void gettimeofday_syscall_handler(system_call_arguments_t* args);
static void cputime_syscall_handler(system_call_arguments_t* args);

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
    systemCallVector[SYS_SPAWN] = spawn_syscall_handler;     // set handler for SYS_SPAWN system call
    systemCallVector[SYS_WAIT] = wait_syscall_handler;      // set handler for SYS_WAIT system call
    systemCallVector[SYS_EXIT] = exit_syscall_handler;      // set handler for SYS_EXIT system call
    systemCallVector[SYS_SEMCREATE] = semcreate_syscall_handler; // set handler for SYS_SEMCREATE system call
    systemCallVector[SYS_SEMP] = semp_syscall_handler;      // set handler for SYS_SEMP system call
    systemCallVector[SYS_SEMV] = semv_syscall_handler;      // set handler for SYS_SEMV system call
    systemCallVector[SYS_SEMFREE] = semfree_syscall_handler;   // set handler for SYS_SEMFREE system call
    systemCallVector[SYS_GETPID] = getpid_syscall_handler;    // set handler for SYS_GETPID system call
    systemCallVector[SYS_GETTIMEOFDAY] = gettimeofday_syscall_handler; // set handler for SYS_GETTIMEOFDAY system call
    systemCallVector[SYS_CPUTIME] = cputime_syscall_handler;   // set handler for SYS_CPUTIME system call


    handlers = get_interrupt_handlers(); // get the array of interrupt handlers from the THREADS library
    handlers[THREADS_SYS_CALL_INTERRUPT] = syscall_interrupt_handler; // set our syscall_interrupt_handler to handle system call interrupts

    /* Initialize the process table */
    memset(userProcTable, 0, sizeof(userProcTable)); // set all bytes to 0, which sets pid to 0, parentPid to 0, startFunc to NULL, arg to NULL, status to STATUS_EMPTY, and all pointers to NULL
    procTableMutex = mailbox_create(1, 0); // 1-slot mailbox for mutual exclusion

    for (i = 0; i < MAXPROC; i++)
    {
        userProcTable[i].mboxStartup = mailbox_create(1, 0); // create a mailbox for this process to synchronize its startup after being spawned. This mailbox will be sent a message by sys_spawn after the process is created, and the new process will wait to receive that message before it starts executing its startFunc.
        userProcTable[i].mboxMutex = mailbox_create(1, 0); // create a mailbox for this process to provide mutual exclusion when accessing this process's data (e.g. its children list). To acquire the mutex, a process will call mailbox_send to send a message to this mailbox, which will block if another process is currently holding the mutex. To release the mutex, a process will call mailbox_receive to receive the message from this mailbox, which will unblock one of the processes waiting to acquire the mutex if there are any.
        userProcTable[i].mboxSem = mailbox_create(1, 0);  // create a mailbox for this process to block/wake on when it is blocked on a semaphore. This mailbox will be sent a message by the semp_syscall_handler when this process is blocked on a semaphore, and the semv_syscall_handler will send a message to this mailbox to wake this process up when the semaphore becomes available.

        TListInitialize(&userProcTable[i].children,
            offsetof(UserProcess, pNextSibling), // pNextSibling/pPrevSibling are the link fields for the children list
            NULL); // no ordering function -- children are kept in insertion order
    }

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

    pid = k_getpid();
    pProc = &userProcTable[pid % MAXPROC]; // find the UserProcess struct for this process using the modulo strategy; this is safe because the parent process will not send the startup message until after it has filled out the PCB for this process, so we can be sure that the correct slot is filled out for this pid.
    /* Wait for the startup message from sys_spawn before proceeding, to ensure that the PCB is fully set up before we start executing the user function. */
    mailbox_receive(pProc->mboxStartup, NULL, 0, TRUE); // wait for a message on the mboxStartup mailbox, which will be sent by sys_spawn after the PCB for this process is fully set up. We can ignore the contents of the message and just use this as a synchronization point.

    /* if signaled when in the sys handler, then Exit */
    if (signaled())
    {
        console_output(FALSE, "%s - Process signaled in launch.\n", "launchUserProcess");
        sys_exit(0);
    }

    set_psr(get_psr() & ~PSR_KERNEL_MODE);

    /*launch the function*/
    if (pProc->startFunc != NULL)
    {
		resultCode = pProc->startFunc((char*)pProc->arg); // execute the startFunc for this process, passing the arg for this process as the argument to startFunc
    }
    else
    {
        resultCode = 0;
    }

    set_psr(get_psr() | PSR_KERNEL_MODE); // return to kernel for exit
	sys_exit(resultCode); // exit with the result code from startFunc

    return 0;
}


int k_semp(int sem_id)
{
	int i; // loop index variable
	int result = -1; // assume failure until we successfully acquire the semaphore
	int sem_idx = -1; // index in the semaphore table of the target semaphore, set to -1 to indicate not found

	if (sem_id < 0 || sem_id >= MAXSEMS) return -1; // validate sem_id

	for (i = 0; i < MAXSEMS; i++) // find the semaphore with the given sem_id
    {
		if (semTable[i].sem_id == sem_id) // found target semaphore
        {
			sem_idx = i; // save the index of the target semaphore for later use in checking if we were woken up because the semaphore was freed while we were asleep
			if (semTable[i].count > 0) // if the count is greater than 0, we can acquire the semaphore without blocking
            {
                semTable[i].count--; // We got the semaphore
				result = 0; // indicate success
            }
            else
            {
                
				int myPid = k_getpid(); // get our pid to add to the wait queue for this semaphore and to know which mailbox to block on
				semTable[i].waitQueue[semTable[i].waitTail] = myPid; // add to the wait queue for this semaphore at the tail index
                semTable[i].waitTail = (semTable[i].waitTail + 1) % MAXPROC; // update the tail index for the wait queue
                semTable[i].waitCount++; // increment the count of blocked processes

                
				mailbox_receive(userProcTable[myPid % MAXPROC].mboxSem, NULL, 0, TRUE); // block on our mboxSem mailbox until we are woken up by the semv_syscall_handler when this semaphore becomes available

				if (semTable[sem_idx].status == STATUS_EMPTY || semTable[sem_idx].sem_id != sem_id) // check if the semaphore we were waiting on was freed while we were asleep 
                {
                    result = -1; // The semaphore was freed while we were asleep
                }
                else if (signaled())
                {
                    result = -1; // We were killed while asleep
                }
                else
                {
                    result = 0; // Successfully acquired!
                }
            }
            break;
        }
    }
    return result;
}

int k_semv(int sem_id)
{
    int i;
    int result = -1;

	if (sem_id < 0 || sem_id >= MAXSEMS) return -1; // validate sem_id

	for (i = 0; i < MAXSEMS; i++) // find the semaphore with the given sem_id
    {
        if (semTable[i].sem_id == sem_id)
        {
            if (semTable[i].waitCount > 0)
            {
                
				int pidToUnblock = semTable[i].waitQueue[semTable[i].waitHead]; // get the pid of the process at the head of the wait queue for this semaphore, which is the next process to unblock
				semTable[i].waitHead = (semTable[i].waitHead + 1) % MAXPROC; // update the head index for the wait queue
				semTable[i].waitCount--; // decrement the count of blocked processes

               
				mailbox_send(userProcTable[pidToUnblock % MAXPROC].mboxSem, NULL, 0, FALSE); // send a message to the mboxSem mailbox for this process to wake it up. This will unblock the process that was blocked in k_semp waiting for this semaphore.
            }
            else
            {
				semTable[i].count++; // if there are no processes waiting, just increment the count for this semaphore to indicate it is available. 
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
    int hadBlocked = 0;

    if (sem_id < 0 || sem_id >= MAXSEMS)
        return -1;

    for (i = 0; i < MAXSEMS; i++) // find the semaphore with the given sem_id
    {
        if (semTable[i].sem_id == sem_id) // found target semaphore
        {
           
			while (semTable[i].waitCount > 0)// if there are any processes blocked on this semaphore, unblock all of them and set hadBlocked to 1 to indicate that we unblocked at least one process
            {
				int pidToUnblock = semTable[i].waitQueue[semTable[i].waitHead]; // get the pid of the process at the head of the wait queue for this semaphore, which is the next process to unblock
				semTable[i].waitHead = (semTable[i].waitHead + 1) % MAXPROC; // update the head index for the wait queue
				semTable[i].waitCount--; // decrement the count of blocked processes

                
				mailbox_send(userProcTable[pidToUnblock % MAXPROC].mboxSem, NULL, 0, FALSE); // send a message to the mboxSem mailbox for this process to wake it up. This will unblock the process that was blocked in k_semp waiting for this semaphore.
				hadBlocked = 1; // set hadBlocked to 1 to indicate that we unblocked at least one process
            }

			semTable[i].sem_id = -1; // set sem_id to -1 to indicate this entry is not in use
			semTable[i].count = 0; // reset count to 0 for cleanliness, even though it shouldn't matter since this entry is now marked as empty
			semTable[i].waitHead = 0; // reset wait queue head index to 0 for cleanliness, even though it shouldn't matter since this entry is now marked as empty
			semTable[i].waitTail = 0; // reset wait queue tail index to 0 for cleanliness, even though it shouldn't matter since this entry is now marked as empty
			semTable[i].waitCount = 0; // reset count of blocked processes to 0 for cleanliness, even though it shouldn't matter since this entry is now marked as empty
            semTable[i].status = STATUS_EMPTY;

            /* Test convention: 1 if any waiters were released, else 0 */
            result = hadBlocked ? 1 : 0;
            break;
        }
    }

    return result;
}

int sys_spawn(char* name, int (*startFunc)(char*), char* arg, int stackSize, int priority)
{
    int parentPid;
    int pid = -1;
    UserProcess* pProcess;
    UserProcess* pParent;

    
	if (name == NULL || startFunc == NULL || stackSize < THREADS_MIN_STACK_SIZE || priority < LOWEST_PRIORITY || priority > HIGHEST_PRIORITY) // check for null pointers and validate stack size and priority
    {
        return -1;
    }

    parentPid = k_getpid();

    
	pid = k_spawn(name, launchUserProcess, arg, stackSize, priority); // spawn a new process to run the launchUserProcess function, which will wait for the startup message from this sys_spawn function before executing the user function specified by startFunc.

	if (pid < 0) // if k_spawn failed to create the new process, return -1 to indicate failure
    {
        console_output(FALSE, "Failed to create user process.");
        return -1;
    }

   
	mailbox_send(procTableMutex, NULL, 0, TRUE); // acquire mutex for process table to safely update the PCB for the new process and add it to the children list of the parent process
     
	pProcess = &userProcTable[pid % MAXPROC]; // find the UserProcess struct for the new process

   
	pProcess->pid = pid; // set the pid for this process in its PCB
	pProcess->startFunc = startFunc; // set the startFunc for this process in its PCB to the function specified by the caller of sys_spawn, which will be called by launchUserProcess 
	pProcess->arg = arg; // set the arg for this process in its PCB to the argument specified by the caller of sys_spawn, which will be passed to startFunc when launchUserProcess calls it
	pProcess->parentPid = parentPid; // set the parentPid for this process in its PCB to the pid of the parent process, which we got at the beginning of this function. 
    pProcess->status = STATUS_READY;

	TListInitialize(&pProcess->children, offsetof(UserProcess, pNextSibling), NULL); // initialize the children list for this process

   
    if (parentPid > 0) // Prevents issues if the very first process has no parent
    {
        pParent = &userProcTable[parentPid % MAXPROC];
        TListAddNode(&pParent->children, pProcess);
    }

   
	mailbox_receive(procTableMutex, NULL, 0, FALSE); // release mutex for process table now that we're done updating the PCB for the new process and adding it to the children list of the parent process

	mailbox_send(pProcess->mboxStartup, NULL, 0, FALSE); // send the startup message to the mboxStartup mailbox for this process to allow it to proceed with executing its startFunc now that the PCB is fully set up

    return pid;
}

int sys_wait(int* pStatus)
{
    int pid;
	pid = k_wait(pStatus); // call k_wait to wait for a child process to finish and get its exit status. k_wait will return the pid of the child that finished, or -1 if there are no children to wait for.
    return pid;
}

void sys_exit(int resultCode)
{
    int pid;
	UserProcess* pProcess; // pointer to the UserProcess struct for this process
	int childStatus;// variable to store the exit status of child processes when we wait for them to finish

	pid = k_getpid(); // get our pid to find our PCB and to know which mailbox to use for mutual exclusion
	pProcess = findProcByPid(pid); // find the UserProcess struct for this process using its pid. 
    if (pProcess == NULL)
    {
        k_exit(resultCode);
        return;
    }

    /* Signal all children to terminate, but keep them in the children list */
    {
        UserProcess* pChild = (UserProcess*)TListGetNextNode(&pProcess->children, NULL);
        while (pChild != NULL)
        {
            if (pChild->pid > 0)
            {
                
				k_kill(pChild->pid, SIG_TERM); // send a termination signal to the child process to ask it to terminate.
            }
			pChild = (UserProcess*)TListGetNextNode(&pProcess->children, pChild); // get the next child in the list
        }
    }

    
	while (TListGetNextNode(&pProcess->children, NULL) != NULL) // keep waiting until there are no more children in the children list
    {
		k_wait(&childStatus); // wait for a child process to finish. 
        
    }

	if (pProcess->parentPid != 0) // if this process has a parent, remove this process from its parent's children list
    {
		UserProcess* pParent = findProcByPid(pProcess->parentPid); // find the UserProcess struct for the parent process using the parentPid field in this process's PCB. 
        if (pParent != NULL)
        {
			TListRemoveNode(&pParent->children, pProcess); // remove this process from its parent's children list 
        }
    }

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
    int sem_id = (int)args->arguments[0];
    int result;

	result = k_semfree(sem_id); // free the semaphore with the given sem_id

    
	args->arguments[0] = (intptr_t)result;// return the result code from k_semfree (1 if any waiters were released, 0 if no waiters were released, -1 on failure)
    args->arguments[3] = (intptr_t)result; 

    set_psr(get_psr() & ~PSR_KERNEL_MODE);
}

static void getpid_syscall_handler(system_call_arguments_t* args) // handler for the SYS_GETPID system call
{
    args->arguments[0] = (intptr_t)k_getpid(); // return the pid of the current process to the caller

    set_psr(get_psr() & ~PSR_KERNEL_MODE); // restore user mode before returning
}

static void gettimeofday_syscall_handler(system_call_arguments_t* args)
{
	args->arguments[0] = (intptr_t)system_clock(); // return the current system time to the caller. 
    set_psr(get_psr() & ~PSR_KERNEL_MODE);
}

static void cputime_syscall_handler(system_call_arguments_t* args) // handler for the SYS_CPUTIME system call
{
  

    args->arguments[0] = (intptr_t)(k_getpid() * 100 + 50); // Dummy: some calculations based on pid

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