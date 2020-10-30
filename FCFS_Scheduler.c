#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

#define NULL 0

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;


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
    
    struct proc *chosen = NULL;

    for(struct proc *p = ptable.proc; p<&ptable.proc[NPROC]; p++)
    {
      if(p->state != RUNNABLE)
        continue;
      if(chosen == NULL || p->ctime < chosen->ctime)
        chosen = p;
    }

    if(chosen != NULL && chosen != 0 && chosen->state == RUNNABLE)
    {
      // From original scheduler, change to user space
      c->proc = chosen;
      switchuvm(chosen);
      chosen->state = RUNNING;

      // Increment number of CPU rounds
      chosen->rounds++;
      
      cprintf("Running process %d\n", chosen->pid);
      // Switch back to kernel memory
      swtch(&(c->scheduler), chosen->context);

	  cprintf("Returning to kernel\n");

      switchkvm();

      // Reset to init
      c->proc = 0;
    }
    release(&ptable.lock);
  }
}