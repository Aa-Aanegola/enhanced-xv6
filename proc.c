#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

// The 5 queues
static struct proc *queue[5][NPROC];
// Pointers to the last elements in the queues
static int num_queued[5] = {0, 0, 0, 0, 0};
char states[6][10] = {"UNUSED", "EMBRYO", "SLEEPING", "RUNNABLE", "RUNNING", "ZOMBIE"};

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

int temp = 0;

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  p->ctime = ticks;
  p->etime = -1;
  p->rtime = 0;
  p->wtime = 0;

  p->priority = 60;
  p->rounds = 0;
  
  p->queue_number = 0;
  for(int i = 0; i<5; i++)
  	p->ticks[i] = 0;
  p->cur_ticks = 0;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;
  p->made_runnable = ticks;

  #ifdef MLFQ
  enqueue(p, 0);
  #endif

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;
  np->made_runnable = ticks;

  #ifdef MLFQ
  enqueue(np, 0);
  #ifdef DEBUG
  cprintf("Appending process with pid %d to queue 0\n", np->pid);
  #endif  
  #endif

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  curproc->etime = ticks;

  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);

        #ifdef MLFQ
        dequeue(p, p->queue_number);
        #endif

        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct cpu *c = mycpu();
  c->proc = 0;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    
    #ifdef RR
    //cprintf("Using RR scheduling.\n");
    for(struct proc *p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;
      p->wtime += ticks - p->made_runnable;

      /*#ifdef DEBUG
      cprintf("Switching to process with pid %d\n", p->pid);
      #endif*/
      // Counts number of times process is given CPU time
      p->rounds++;

      swtch(&(c->scheduler), p->context);
      
      /*#ifdef DEBUG
      cprintf("Switching back to kernel space.\n");
      #endif*/

      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
    #endif
    
    #ifdef FCFS
    struct proc *chosen = 0;

    for(struct proc *p = ptable.proc; p<&ptable.proc[NPROC]; p++)
    {
      if(p->state != RUNNABLE)
        continue;
      if(chosen == 0 || p->ctime < chosen->ctime)
        chosen = p;
    }

    if(chosen != 0 && chosen->state == RUNNABLE)
    {
      // From original scheduler, change to user space
      c->proc = chosen;
      switchuvm(chosen);
      chosen->state = RUNNING;
      chosen->wtime += ticks - chosen->made_runnable;

      // Increment number of CPU rounds
      chosen->rounds++;
      
      #ifdef DEBUG
      cprintf("Switching to process with pid %d with ctime %d\n", chosen->pid, chosen->ctime);
      #endif
      

      // Switch back to kernel
      swtch(&(c->scheduler), chosen->context);

      #ifdef DEBUG
      cprintf("Switching back to kernel space.\n");
      #endif

      switchkvm();

      // Reset to init
      c->proc = 0;
    }
    #endif 

    #ifdef PBS
    int min_priority = -1;

    for(struct proc *p = ptable.proc; p<&ptable.proc[NPROC]; p++)
    {
      if(p->state != RUNNABLE)
        continue;

      if(min_priority == -1 || min_priority >= p->priority)
        min_priority = p->priority;
    }

    if(min_priority == -1)
    {
      release(&ptable.lock);
      continue;
    }

    for(struct proc *p = ptable.proc; p<&ptable.proc[NPROC]; p++)
    {
      // If there is a new lower priority
      int reset = 0;

      for(struct proc *temp = ptable.proc; temp<&ptable.proc[NPROC]; temp++)
        if(temp->priority < min_priority && temp->state == RUNNABLE)
          reset = 1;
      
      if(reset)
        break;

      if(p->state != RUNNABLE)
        continue;

      if(p->priority == min_priority)
      {
        c->proc = p;
        switchuvm(p);
        p->state = RUNNING;
        p->wtime += ticks - p->made_runnable;


        p->rounds++;

        #ifdef DEBUG
        cprintf("Switching to process with pid %d with priority %d\n", p->pid, p->priority);
        #endif

        swtch(&(c->scheduler), p->context);
        switchkvm();

        #ifdef DEBUG
        cprintf("Switching back to kernel space.\n");
        #endif

        c->proc = 0;
      }
    }
    #endif

    #ifdef MLFQ

    // First check for new processes
    for(struct proc *p = ptable.proc; p<&ptable.proc[NPROC]; p++)
      if(p->state == RUNNABLE)
        enqueue(p, p->queue_number);

    // Then we check for queue promotions
    for(int i = 1; i<5; i++)
    {
    	for(int j = 0; j<num_queued[i]; j++)
    	{
    		int age = ticks - queue[i][j]->made_runnable;
    		if(age > MAX_AGE)
    		{
    			struct proc *p = queue[i][j];
    			#ifdef DEBUG
    			cprintf("Moving process with pid %d from %d to %d\n", p->pid, i, i-1);
    			#endif
    			dequeue(p, i);
    			enqueue(p, i-1);
    		}
    	}
    }

    // Try and find a process to execute
    struct proc *p = 0;

    for(int i = 0; i<5; i++)
    {	
    	// Take the first process in the queue
    	if(num_queued[i] > 0)
    	{
    		p = queue[i][0];
    		dequeue(p, i);
    		break;
    	}
    }

    if(p == 0 || p->state != RUNNABLE)
    {
    	release(&ptable.lock);
    	continue;
    }

  	p->rounds++;

  	#ifdef DEBUG
  	cprintf("Running pid %d queue %d ticks %d\n", p->pid, p->queue_number, p->cur_ticks);
  	#endif

  	c->proc = p;
    	
  	switchuvm(p);
		p->state = RUNNING;
		p->wtime += ticks - p->made_runnable;

		swtch(&c->scheduler, p->context);
		switchkvm();
		
		c->proc = 0;

		// If it's still runnable then shift it to a lower queue
		// If it's not runnable, then add it to the same queue
		if(p->state == RUNNABLE)
		{
			if(p->exceeded)
			{
        //cprintf("Updating\n");
				p->exceeded = 0;
				p->cur_ticks = 0;
				dequeue(p, p->queue_number);
        if(p->queue_number != 4)
					p->queue_number++;
			}
      else
        p->cur_ticks = 0;
      enqueue(p, p->queue_number);
		}
    else
      dequeue(p, p->queue_number);
    #endif

    release(&ptable.lock);
  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  myproc()->made_runnable = ticks;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  
  if(p->state == RUNNABLE)
  	p->wtime += ticks - p->made_runnable;

  p->state = SLEEPING;
  
  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
    {
    	#ifdef MLFQ
      p->cur_ticks = 0;
      enqueue(p, p->queue_number);
      #endif
      p->state = RUNNABLE;
      p->made_runnable = ticks;
    }
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
      {
      	#ifdef MLFQ
      	enqueue(p, p->queue_number);
      	p->cur_ticks = 0;
      	#endif
        p->state = RUNNABLE;
        p->made_runnable = ticks;
      }
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

int 
waitx(int *wtime, int *rtime)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();

  acquire(&ptable.lock);
  for (;;)
  {
    // Scan through table looking for exited children.
    havekids = 0;
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
      if (p->parent != curproc)
        continue;

      havekids = 1;
      
      if (p->state == ZOMBIE)
      {
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
      
        #ifdef MLFQ
        dequeue(p, p->queue_number);
        #endif

        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
      
        *rtime = p->rtime;
        *wtime = p->wtime;

        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if (!havekids || curproc->killed)
    {
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock); //DOC: wait-sleep
  }
}


int set_priority(int new_priority, int pid)
{
	if(new_priority<0 || new_priority>100)
    return -1;

  int old_priority = -1;
  int release_CPU = 0;

  for(struct proc *p = ptable.proc; p<&ptable.proc[NPROC]; p++)
  {
    if(p->pid == pid)
    {
      acquire(&ptable.lock);
      old_priority = p->priority;
      p->priority = new_priority;

      if(old_priority > new_priority)
        release_CPU = 1;
      
      release(&ptable.lock);
      break;
    }
  }

  if(release_CPU)
    yield();

  return old_priority;
}

int enqueue(struct proc *p, int pos)
{	
	if(p->state != RUNNABLE)
		return -1;
	// Check if the process is already in that queue
	for(int i = 0; i<num_queued[pos]; i++)
		if(queue[pos][i]->pid == p->pid)
			return -1;

	// Otherwise we put it at the back of the queue
	p->cur_ticks = 0;
	p->queue_number = pos;
	queue[pos][num_queued[pos]++] = p;

  #ifdef DEBUG
	cprintf("Proc with pid %d added to queue %d and state %d num %d\n", p->pid, pos, p->state, num_queued[pos]);
  #endif

	return 0;
}

int dequeue(struct proc *p, int pos)
{
	// Remove it from the queue, and shift the other elements forward
  for(int i = 0; i<num_queued[pos]; i++)
	{
		if(queue[pos][i]->pid == p->pid)
		{
			queue[pos][i] = 0;
			for(int j = i; j<num_queued[pos]-1; j++)
			{
				queue[pos][j] = queue[pos][j+1];
			}
			num_queued[pos] -= 1;
			return 0;
		}
	}
	return -1;
}	


void set_exceeded(struct proc *p)
{
	p->exceeded = 1;
}

void update_ticks(struct proc *p)
{
	p->cur_ticks += 1;
	p->ticks[p->queue_number] += 1;
}


int ps()
{
	cprintf("PID \t PRIORITY \t STATE \t\t R_TIME \t W_TIME \t N_RUN \t CUR_Q \t Q0 \t Q1 \t Q2 \t Q3 \t Q4\n\n");

	for(struct proc *p = ptable.proc; p<&ptable.proc[NPROC]; p++)
	{
		if(p->state == UNUSED)
			continue;
		int wtime= 0;
		
		if(p->state == RUNNABLE)
			wtime = ticks - p->made_runnable;
	
		cprintf("%d \t %d \t\t %s \t ", p->pid, p->priority, states[p->state]);
		cprintf("%d \t\t %d \t\t %d \t %d \t ", p->rtime, wtime, p->rounds, p->queue_number);
		cprintf("%d \t %d \t %d \t %d \t %d\n", p->ticks[0], p->ticks[1], p->ticks[2], p->ticks[3], p->ticks[4]);
	}
  return 0;
}


int return_ticks()
{
	return ticks;
}