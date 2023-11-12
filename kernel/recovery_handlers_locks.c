#include "param.h"
#include "types.h"
#include "stat.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "mlist.h"
#include "memlayout.h"
#include "nmi.h"
#include "after-treatment.h"

extern int recovery_mode;
extern struct mlist_header mlist;
extern struct spinlock *pid_lock;
extern struct spinlock *tickslock;

extern uint64 clockintr_start, clockintr_end;

extern void (*handler_for_mem_fault)(char*);  // Function which is called from NMI handler.

/*
 * Recovery handlers raw struct spinlock / struct sleeplock.
 */
void
recovery_handler_spinlock(char *name, struct spinlock *lk, void *broken)
{
  if(!lk)
    panic_without_pr("recovery_handler_spinlock: NULL lk pointer passed");

  initlock(lk, name);  // Recovery spinlock by re-initialization.
  delete_memobj(broken, mlist.spn_list, 0x0);
}

void
recovery_handler_sleeplock(char *name, struct sleeplock *lk, void *broken, uint64 size)
{
  if(!lk)
    panic("recovery_handler_sleeplock: NULL lk pointer passed");

  initsleeplock(lk, name);  // Recovery spinlock by re-initialization.
  delete_memobj(broken, mlist.slp_list, size);
  delete_memobj(broken, mlist.spn_list, size);  // Delete lk->lk.
}

void
acquiresleep_wo_sleep(struct sleeplock *lk)
{
  acquire(&lk->lk);
  while(lk->locked)
    ;
  lk->locked = 1;
  lk->pid = myproc()->pid;
  release(&lk->lk);
}

int
recovery_handler_pid_lock(void* broken, int pid)
{
  struct spinlock *new = (struct spinlock*)my_kalloc(0, 0);
  
  if(!new){
    printf("recovery_handler_pid_lock: Allocating new region is fail.\n");
    return FAIL_STOP;
  }

  // Internal-Surgery
  recovery_handler_spinlock("nextpid", new, broken);  // Recovery spinlock by re-initialization.
  pid_lock = new;
  delete_memobj(broken, mlist.spn_list, 0x0);
  register_memobj((void*)new, mlist.spn_list);
  while(mycpu()->noff > 1) pop_off();

  return (is_enable_user_coop(pid) == ENABLE) ? SYSCALL_REDO : SYSCALL_FAIL;
}

/*
 * Timer Interrupt
 */
static int
after_treatment_tickslock(int pid, uint64 sp, uint64 s0)
{
  struct proc *p;
  uint64 pcs[DEPTH];

  // Search call stack.
  if(sp == 0x0 || s0 - sp > PGSIZE){
    p = search_proc_from_pid(pid);
    getcallerpcs_bottom(pcs, sp, p->kstack, DEPTH);
  } else {
    getcallerpcs_top(pcs, sp, s0, DEPTH);
  }

  for(int i = 0; i < DEPTH; i++){
    if(ISINSIDE(pcs[i], clockintr_start, clockintr_end)){
      int trap = identify_nmi_occurred_trap(pid, sp, s0);

      if(trap == USERTRAP){
        printf("end recovery_handler_ticklock: %d\n", get_ticks());
        return RETURN_TO_USER;
      } else if(trap == KERNELTRAP){
        printf("end recovery_handler_ticklock: %d\n", get_ticks());
        return RETURN_TO_KERNEL;
      }
    }
  }

  printf("end recovery_handler_ticklock: %d\n", get_ticks());
  return (is_enable_user_coop(pid) == ENABLE) ? SYSCALL_REDO : SYSCALL_FAIL;
}

int
recovery_handler_tickslock(void *broken, int pid, uint64 sp, uint64 s0)
{
  struct spinlock *new  = (struct spinlock*)my_kalloc(0, 0);

  if(!new){
    printf("recovery_handler_tickslock: my_kalloc() is fail.\n");
    return FAIL_STOP;
  }

  recovery_handler_spinlock("time", new, broken);  // Recovery spinlock by re-initialization.
  tickslock = new;
  register_memobj((void*)new, mlist.spn_list);
  while(mycpu()->noff > 0) pop_off();

  return after_treatment_tickslock(pid, sp, s0);
}
