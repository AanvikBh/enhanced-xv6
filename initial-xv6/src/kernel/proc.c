#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

#ifdef COW
extern struct cow_info page_details[];
extern struct spinlock page_cow_lock;
#endif

struct proc *Queue[4][NPROC];
// int front_ptrs[4];
// int rear_ptrs[4];
int numProcPerQueue[4];
int ticksPerQue[] = {1, 3, 9, 15};
int ageingTime = 30;

int enqueueProc(struct proc *p, int priority)
{
  if (p->state != RUNNABLE)
    return -1;
  for (int i = 0; i < numProcPerQueue[priority]; i++)
    if (Queue[priority][i]->pid == p->pid)
      return -1;

  // Otherwise we put it at the back of the queue
  p->curr_ticks = 0;
  p->currPriority = priority;
  Queue[priority][numProcPerQueue[priority]++] = p;
  // numProcPerQueue[priority]++;
  p->is_in_mlfq = 1;

  return 0;
}

int dequeueProc(struct proc *p, int priority)
{
  if (numProcPerQueue[priority] == 0)
    return -1;
  for (int i = 0; i < numProcPerQueue[priority]; i++)
  {
    if (Queue[priority][i]->pid == p->pid)
    {
      Queue[priority][i] = 0;
      for (int j = i; j < numProcPerQueue[priority] - 1; j++)
      {
        Queue[priority][j] = Queue[priority][j + 1];
      }
      numProcPerQueue[priority]--;
      p->is_in_mlfq = 0;
      return 0;
    }
  }
  return -1;
}



int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    char *pa = kalloc();
    if (pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int)(p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
void procinit(void)
{
  struct proc *p;

  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for (p = proc; p < &proc[NPROC]; p++)
  {
    initlock(&p->lock, "proc");
    p->state = UNUSED;
    p->kstack = KSTACK((int)(p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu *
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc *
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int allocpid()
{
  int pid;

  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc *
allocproc(void)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if (p->state == UNUSED)
    {
      goto found;
    }
    else
    {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  // Allocate a trapframe page.
  if ((p->trapframe = (struct trapframe *)kalloc()) == 0)
  {
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if (p->pagetable == 0)
  {
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;
  p->rtime = 0;
  p->etime = 0;
  p->ctime = ticks;
  p->wtime = 0;
  p->stime = 0;
  p->Rtime=0;
  p->RBI = 25;
  p->staticPriority = 50;
  p->dynamicPriority = 75;

  p->sleep_start=p->sleep_end=0;


  p->alarmticks = 0;
  p->alarmhandler = 0;
  p->curr_ticks = 0;

  p->currPriority = 0;
  for (int i = 0; i < 4; i++)
  {
    p->ticksProcPerQue[i] = 0;
  }
  p->lastScheduledOnTick = ticks;
  p->is_in_mlfq = 0;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if (p->trapframe)
    kfree((void *)p->trapframe);
  p->trapframe = 0;
  if (p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if (pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if (mappages(pagetable, TRAMPOLINE, PGSIZE,
               (uint64)trampoline, PTE_R | PTE_X) < 0)
  {
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if (mappages(pagetable, TRAPFRAME, PGSIZE,
               (uint64)(p->trapframe), PTE_R | PTE_W) < 0)
  {
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// assembled from ../user/initcode.S
// od -t xC ../user/initcode
uchar initcode[] = {
    0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
    0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
    0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
    0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
    0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
    0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00};

// Set up first user process.
void userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;

  // allocate one user page and copy initcode's instructions
  // and data into it.
  uvmfirst(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;     // user program counter
  p->trapframe->sp = PGSIZE; // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;

#ifdef MLFQ
  // printf("Here\n");
  enqueueProc(p, 0);
#endif

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if (n > 0)
  {
    if ((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0)
    {
      return -1;
    }
  }
  else if (n < 0)
  {
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if ((np = allocproc()) == 0)
  {
    return -1;
  }

// Copy user memory from parent to child.
#ifndef COW
  if (uvmcopy(p->pagetable, np->pagetable, p->sz) < 0)
  {
    freeproc(np);
    release(&np->lock);
    return -1;
  }
#endif

#ifdef COW
  pte_t *pte;
  uint64 pa, j;
  uint flags;
  // char *mem;

  for (j = 0; j < p->sz; j += PGSIZE)
  {
    if ((pte = walk(p->pagetable, j, 0)) == 0)
      panic("cow_fork: pte should exist");
    if ((*pte & PTE_V) == 0)
      panic("cow_fork: page not present");
    pa = PTE2PA(*pte);
    acquire(&page_cow_lock);
    int page_num = ((uint64)pa / PGSIZE);
    page_details[page_num].numReferences++;
    page_details[page_num].p = np;
    page_details[page_num].page_number = page_num;
    release(&page_cow_lock);
    flags = PTE_FLAGS(*pte);
    flags &= (~PTE_W);
    flags |= PTE_CoW;
    *pte = PA2PTE(pa) | flags;

    // if ((mem = kalloc()) == 0)
    //   goto err;
    // memmove(mem, (char *)pa, PGSIZE);
    if (mappages(np->pagetable, j, PGSIZE, (uint64)pa, flags) != 0)
    {
      // kfree(mem);
      uvmunmap(np->pagetable, 0, j / PGSIZE, 1);
      freeproc(np);
      release(&np->lock);
      return -1;
    }
  }

  // return 0;
#endif
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for (i = 0; i < NOFILE; i++)
    if (p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  np->lastScheduledOnTick = ticks;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  #ifdef MLFQ

  if (!np->is_in_mlfq)
  {
    enqueueProc(np, 0);
  }

#endif

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void reparent(struct proc *p)
{
  struct proc *pp;

  for (pp = proc; pp < &proc[NPROC]; pp++)
  {
    if (pp->parent == p)
    {
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void exit(int status)
{
  struct proc *p = myproc();

  if (p == initproc)
    panic("init exiting");

  // Close all open files.
  for (int fd = 0; fd < NOFILE; fd++)
  {
    if (p->ofile[fd])
    {
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);

  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;
  p->etime = ticks;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int wait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for (;;)
  {
    // Scan through table looking for exited children.
    havekids = 0;
    for (pp = proc; pp < &proc[NPROC]; pp++)
    {
      if (pp->parent == p)
      {
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if (pp->state == ZOMBIE)
        {
          // Found one.
          pid = pp->pid;
          if (addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                   sizeof(pp->xstate)) < 0)
          {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if (!havekids || killed(p))
    {
      release(&wait_lock);
      return -1;
    }

#ifdef MLFQ
    p->is_in_mlfq = 0;
    p->curr_ticks = 0;
    p->ticksProcPerQue[p->currPriority] = 0;
    dequeueProc(p, p->currPriority);
#endif


    // Wait for a child to exit.
    sleep(p, &wait_lock); // DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.

#ifndef PBS
void scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  #ifdef FCFS
  for (;;)
  {
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();
    struct proc *temp = 0;
    struct proc *minProc = 0;
    int minTime = 100000;
    for (temp = proc; temp < &proc[NPROC]; temp++)
    {
      acquire(&temp->lock);
      if (temp->state == RUNNABLE)
      {
        if (minTime > temp->ctime)
        {
          minProc = temp;
          minTime = temp->ctime;
        }
      }
      release(&temp->lock);
    }

    // Switch to chosen process.  It is the process's job
    // to release its lock and then reacquire it
    // before jumping back to us.
    if (minProc != 0)
    {
      acquire(&minProc->lock);
      if (minProc->state == RUNNABLE)
      {
        minProc->state = RUNNING;
        c->proc = minProc;
        swtch(&c->context, &minProc->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      release(&minProc->lock);
    }
  }
//}
#endif
#ifdef MLFQ
  struct proc *p;
  for (;;)
  {
    intr_on();
    // update_time();

    for (p = proc; p < &proc[NPROC]; p++)
    {
      if (p->state == RUNNABLE)
      {

        // add it in highest priority queue
        enqueueProc(p, p->currPriority);
        // if(!p->is_in_mlfq){
        //   enqueueProc(p, 0);
        // }
      }
    }

    // change some index here

    for (int i = 1; i < 4; i++)
    {
      for (int j = 0; j < numProcPerQueue[i]; j++)
      {
        struct proc *starve = Queue[i][j];
        acquire(&starve->lock);

        if (ticks - starve->lastScheduledOnTick >= ageingTime)
        {
          // Promote the process to a higher-priority queue
          dequeueProc(starve, starve->currPriority);
          starve->currPriority = starve->currPriority - 1;
          starve->ticksProcPerQue[starve->currPriority] = 0;
          p->lastScheduledOnTick = ticks;
          enqueueProc(starve, starve->currPriority+1);

          // Reset the lastScheduledOnTick counter
        }
        release(&starve->lock);
      }
    }

    struct proc *highestPriorProc = 0;
    for (int i = 0; i < 4; i++)
    {
      if (numProcPerQueue[i] > 0)
      {
        highestPriorProc = Queue[i][0];
        dequeueProc(highestPriorProc, i);
        break;
      }
    }
    if (highestPriorProc)
    {
      // printf("Process: %p", highestPriorProc);
      acquire(&highestPriorProc->lock);
      if (highestPriorProc->state == RUNNABLE)
      {
        highestPriorProc->lastScheduledOnTick = ticks;
        highestPriorProc->state = RUNNING;
        c->proc = highestPriorProc;
        swtch(&c->context, &highestPriorProc->context);
        c->proc = 0;
      }
      release(&highestPriorProc->lock);
    }
  }

#endif

  for (;;)
  {
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

    for (p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);
      if (p->state == RUNNABLE)
      {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        p->numScheduled++;
        c->proc = p;
        swtch(&c->context, &p->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      release(&p->lock);
    }
  }
}
#endif

#ifdef PBS

void calculatePriorities(struct proc *p)
{

  int rbi_proc = ((3 * p->Rtime) - (p->stime) - (p->wtime)) * 50;
  rbi_proc /= (p->Rtime + p->wtime + p->stime + 1);
  if (rbi_proc < 0)
  {
    rbi_proc = 0;
  }
  p->RBI = rbi_proc;
  p->dynamicPriority = 100;
  if (p->dynamicPriority > p->staticPriority + p->RBI)
  {
    p->dynamicPriority = p->staticPriority + p->RBI;
  }
  return;
}

void scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for (;;)
  {
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();
    struct proc *max_priority_proc = 0;
    // int count_max_priority = 0;
    // int lowest_dp_value = 0;

    for (p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);
      if (p->state == RUNNABLE)
      {

        // if (max_priority_proc==0 || max_priority_proc->ctime < p->ctime)
        // {
        //   max_priority_proc = p;
        //   // count_max_priority++;
        // }
        // p->stime=p->sleep_end-p->sleep_start;

        calculatePriorities(p);
        if (max_priority_proc == 0 || p->dynamicPriority < max_priority_proc->dynamicPriority)
        {
          max_priority_proc = p;
          // lowest_dp_value = p->dynamicPriority;
          // count_max_priority++;
        }
        else if (p->state == RUNNABLE && p->dynamicPriority == max_priority_proc->dynamicPriority)
        {
          if (max_priority_proc->numScheduled > p->numScheduled)
          {
            max_priority_proc = p;
            // count_max_priority++;
            // printf("%d %d %d %d\n", max_priority_proc->pid, max_priority_proc->staticPriority, max_priority_proc->RBI, max_priority_proc->dynamicPriority);
          }
          else if (p->state == RUNNABLE && p->dynamicPriority == max_priority_proc->dynamicPriority && p->numScheduled == max_priority_proc->numScheduled)
          {
            if (max_priority_proc->ctime < p->ctime)
            {
              max_priority_proc = p;
              // count_max_priority++;
            }
          }
        }
      }
      release(&p->lock);
    }

    if (max_priority_proc != 0)
    {
      // printf("%d\n", max_priority_proc->pid);
      acquire(&max_priority_proc->lock);
      // printf("%d\n", max_priority_proc->pid);
      if (max_priority_proc->state == RUNNABLE)
      {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        
        // printf("%d\n",max_priority_proc->RBI);
        max_priority_proc->state = RUNNING;
        max_priority_proc->numScheduled++;
        max_priority_proc->Rtime = 0;
        max_priority_proc->stime = 0;
        c->proc = max_priority_proc;
        swtch(&c->context, &max_priority_proc->context);
        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      release(&max_priority_proc->lock);
    }
  }
}
#endif

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void sched(void)
{
  int intena;
  struct proc *p = myproc();

  if (!holding(&p->lock))
    panic("sched p->lock");
  if (mycpu()->noff != 1)
    panic("sched locks");
  if (p->state == RUNNING)
    panic("sched running");
  if (intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first)
  {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();

  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock); // DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  // p->sleep_start=ticks;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void wakeup(void *chan)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    if (p != myproc())
    {
      acquire(&p->lock);
      if (p->state == SLEEPING && p->chan == chan)
      {
        p->state = RUNNABLE;
        // p->sleep_end=ticks;
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int kill(int pid)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if (p->pid == pid)
    {
      p->killed = 1;
      if (p->state == SLEEPING)
      {
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int killed(struct proc *p)
{
  int k;

  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if (user_dst)
  {
    return copyout(p->pagetable, dst, src, len);
  }
  else
  {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if (user_src)
  {
    return copyin(p->pagetable, dst, src, len);
  }
  else
  {
    memmove(dst, (char *)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void procdump(void)
{
  static char *states[] = {
      [UNUSED] "unused",
      [USED] "used",
      [SLEEPING] "sleep ",
      [RUNNABLE] "runble",
      [RUNNING] "run   ",
      [ZOMBIE] "zombie"};
  struct proc *p;
  char *state;

  printf("\n");
  for (p = proc; p < &proc[NPROC]; p++)
  {
    if (p->state == UNUSED)
      continue;
    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}

// waitx
int waitx(uint64 addr, uint *wtime, uint *rtime)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for (;;)
  {
    // Scan through table looking for exited children.
    havekids = 0;
    for (np = proc; np < &proc[NPROC]; np++)
    {
      if (np->parent == p)
      {
        // make sure the child isn't still in exit() or swtch().
        acquire(&np->lock);

        havekids = 1;
        if (np->state == ZOMBIE)
        {
          // Found one.
          pid = np->pid;
          *rtime = np->rtime;
          *wtime = np->etime - np->ctime - np->rtime;
          if (addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                   sizeof(np->xstate)) < 0)
          {
            release(&np->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(np);
          release(&np->lock);
          release(&wait_lock);
          return pid;
        }
        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if (!havekids || p->killed)
    {
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(p, &wait_lock); // DOC: wait-sleep
  }
}

void update_time()
{
  struct proc *p;
  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if (p->state == RUNNING)
    {
      p->rtime++;
      // p->Rtime++;
    }
    release(&p->lock);
  }
  #ifdef MLFQ
      // acquire
    for (p = proc; p < &proc[NPROC]; p++)
    {
      if (p->pid >= 9 && p->pid <= 13)
      {
        if (p->state == RUNNABLE || p->state == RUNNING)
          printf("%d %d %d \n",p->pid,ticks,p->currPriority);
      }
    }
  #endif
}

uint64 set_priority_proc(int pid_change, int new_priority){
  uint64 old_priority;
  
  for(struct proc* p=proc; p<&p[NPROC]; p++){
    acquire(&p->lock);
      if (p->pid==pid_change)
      {
        // printf("%d %d\n", pid_change, new_priority);
        old_priority=p->staticPriority;
        p->staticPriority=new_priority;
        // resetting times too
        // p->stime=0;
        // p->rtime=0;
        // p->wtime=0;
        p->RBI=25;
        release(&p->lock);
        break;
      }
    release(&p->lock);
  }
  if(old_priority>new_priority){
    yield();
  }
  return old_priority;
}