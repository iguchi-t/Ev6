#include "param.h"
#include "types.h"
#include "stat.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "fs.h"
#include "file.h"
#include "mlist.h"
#include "memlayout.h"
#include "nmi.h"
#include "buf.h"
#include "printf.h"
#include "console.h"
#include "recovery_locking.h"
#include "after-treatment.h"


extern struct mlist_header mlist;
extern struct bcache bcache;
extern struct cons *cons;
extern struct devsw *devsw;
extern struct icache *icache;
extern struct pr *pr;
extern int recovery_mode;

extern uint64 consoleintr_start, consoleintr_end;
extern uint64 kerneltrap_start, kerneltrap_end;
extern uint64 panic_start, panic_end;

/*
 * struct cons's recovery handling.
 */
static int
after_treatment_cons(uint64 *pcs, int pid, uint64 sp, uint64 s0)
{
  int ret = (is_enable_user_coop(pid) == ENABLE) ? SYSCALL_REDO : SYSCALL_FAIL;

  for(int i = 0; i < DEPTH; i++){
    if(ISINSIDE(pcs[i], consoleintr_start, consoleintr_end)){
      int trap = identify_nmi_occurred_trap(pid, sp, s0);
      if(trap == USERTRAP)
        ret = RETURN_TO_USER;
      else if(trap == KERNELTRAP)
        ret = RETURN_TO_KERNEL;
    }
  }
  release_recovery_lock(RL_FLAG_CONS);  // Invalidate R.C.S.

  if(ret == RETURN_TO_KERNEL)
    exit_rcs_after_recovery(pid, CONSINTR_CONS);
  else
    exit_rcs_after_recovery(pid, 0);
 
  printf("end struct cons recovery: %d\n", get_ticks());
  return ret;
}

// Recovery Handler of struct cons.
int
recovery_handler_cons(void* address, int pid, uint64 sp, uint64 s0)
{
  printf("start struct cons recovery: %d\n", get_ticks());
  acquire_recovery_lock(RL_FLAG_CONS);  // Validate R.C.S.

  uint64 pcs[DEPTH];
  struct cons *new = (struct cons*)my_kalloc(0, 0);

  if(check_and_count_procs_in_rcs(RL_FLAG_CONS, 0x0) > 1){
    printf("recovery_handler_cons: Goto Fail-Stop due to %d processes are in Recovery-locking Critical Section already.\n", check_and_count_procs_in_rcs(RL_FLAG_CONS, 0x0));
    return FAIL_STOP;  // Avoid process hanging due to old lock waiting & invading critical section with old lock.
  }
  if(new == 0)
    return FAIL_STOP;

  // Search call stack & check Fail-Stop situation.
  if(sp == 0x0 || s0 - sp > PGSIZE){  // In the case of invalid s0 or sp is passed.
    struct proc *p = search_proc_from_pid(pid);
    getcallerpcs_bottom(pcs, sp, p->kstack, DEPTH);
  } else {
    getcallerpcs_top(pcs, sp, s0, DEPTH);
  }

  delete_memobj(address, mlist.con_list, 0x0);  // Delete broken node from address list.
  register_memobj(new, mlist.con_list);  // Register new node to address list.

  // Initialize all members.
  recovery_handler_spinlock("cons", &new->lock, address);
  new->r = new->w = new->e = 0;
  cons = new;

  while(mycpu()->noff > 1)
    pop_off();  // Decrement noff of broken spinlock in old cons.
  printf("struct cons recovery is completed.(%p)\n", cons);
  return after_treatment_cons(pcs, pid, sp, s0);
}

/*
 * struct devsw's recovery handling.
 */
int
recovery_handler_devsw(void* address, int pid)
{
  printf("start strcut devsw recovery: %d\n", get_ticks());
  struct devsw *new = (struct devsw*)my_kalloc(0, 0);

  if(new == 0)
    return -1;  // my_kalloc() failed.

  delete_memobj(address, mlist.dev_list, 0x0);  // Delete broken node from address list.
  register_memobj(new, mlist.dev_list);  // Register new node to address list.

  new[CONSOLE].write = consolewrite;
  new[CONSOLE].read = consoleread;
  if(__sync_lock_test_and_set(&devsw, new));  // Replace entity of devsw by replacing pointer from _devsw to new.

  // Release related locks and do syscall fai.
  for(struct inode *ip = &icache->inode[0]; ip < &icache->inode[NINODE]; ip++){
    if(ip->lock.locked && ip->lock.pid == pid)
      releasesleep(&ip->lock);
  }
  for(struct buf *b = bcache.head.next; b->next != &bcache.head; b = b->next){
    if(b->lock.locked && b->lock.pid == pid)
      releasesleep(&b->lock);
  }

  push_off();
  while(mycpu()->noff > 1) pop_off(); 
  pop_off();

  exit_rcs_after_recovery(pid, 0);
  printf("end struct devsw recovery: %d\n", get_ticks());
  return SYSCALL_FAIL;
}

/*
 * struct pr's recovery handling.
 */
static int
after_treatment_pr(uint64 *pcs, int pid, uint64 sp, uint64 s0)
{
  int ret = PROCESS_KILL;

  for(int i = 0; i < DEPTH; i++){
    if(ISINSIDE(pcs[i], panic_start, panic_end)){
      pr->locking = 0;
      ret = FAIL_STOP;
    }
    else if(ISINSIDE(pcs[i], consoleintr_start, consoleintr_end)){
      int trap = identify_nmi_occurred_trap(pid, sp, s0);
      if(trap == USERTRAP)
        ret =  RETURN_TO_USER;
      else if(trap == KERNELTRAP)
        ret = RETURN_TO_KERNEL;
    }
  }

  if(ret == RETURN_TO_KERNEL)
    exit_rcs_after_recovery(pid, CONSINTR_CONS);
  else
    exit_rcs_after_recovery(pid, 0);

  printf("end struct pr recovery: %d\n", get_ticks());
  return ret;
}


int
recovery_handler_pr(void* address, int pid, uint64 sp, uint64 s0)
{
  acquire_recovery_lock(RL_FLAG_PR);  // Validate R.C.S.

  uint64 pcs[DEPTH];
  struct pr *new_pr = (struct pr*)my_kalloc(0, 0);

  if(check_and_count_procs_in_rcs(RL_FLAG_PR, 0x0) > 1){
    printf_without_pr("recovery_handler_pr: Goto Fail-Stop due to %d processes are in Recovery-locking Critical Section already.\n", check_and_count_procs_in_rcs(RL_FLAG_PR, 0x0));
    return FAIL_STOP;
  }
  if(new_pr == 0x0)
    return -1;

  // Search call stack & check Fail-Stop situation.
  if(sp == 0x0 || s0 - sp > PGSIZE){  // In the case of invalid s0 or sp is passed.
    struct proc *p = search_proc_from_pid(pid);
    getcallerpcs_bottom(pcs, sp, p->kstack, DEPTH);
  } else {
    getcallerpcs_top(pcs, sp, s0, DEPTH);
  }

  delete_memobj(address, mlist.pr_list, 0x0);  // Delete broken node from address list.
  register_memobj(new_pr, mlist.pr_list);  // Register new node to address list.

  // Recovery pr's members.
  recovery_handler_spinlock("pr", &new_pr->lock, address);
  new_pr->locking = 1;
  pr = new_pr;

  while(mycpu()->noff > 1) pop_off(); 

  release_recovery_lock(RL_FLAG_PR);  // Invalidate R.C.S.
  return after_treatment_pr(pcs, pid, sp, s0);
}
