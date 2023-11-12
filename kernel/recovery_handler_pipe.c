#include "param.h"
#include "types.h"
#include "stat.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "pipe.h"
#include "file.h"
#include "proc.h"
#include "mlist.h"
#include "memlayout.h"
#include "nmi.h"
#include "recovery_locking.h"
#include "after-treatment.h"


extern int recovery_mode;
extern struct mlist_header mlist;
extern struct bcache bcache;
extern struct ftable *ftable;
extern struct proc proc[];
extern void (*handler_for_mem_fault)(char*);  // Function which is called from NMI handler.

extern uint64 exit_start, exit_end;
extern uint64 sys_close_start, sys_close_end;
extern uint64 sys_pipe_start, sys_pipe_end;


// Determine the way of termination of recovery handler.
static int
after_treatment_pipe(uint64 sp, uint64 s0, int pid, struct pipe *new, struct file *file)
{
  int ret = SYSCALL_FAIL;
  uint64 pcs[DEPTH];

  // Search call stack & check Fail-Stop situation.
  if(sp == 0x0 || s0 - sp > PGSIZE){  // In the case of invalid s0 or sp is passed.
    getcallerpcs_bottom(pcs, sp, search_proc_from_pid(pid)->kstack, DEPTH);
  } else {
    getcallerpcs_top(pcs, sp, s0, DEPTH);
  }

  for(int i = 0; i < DEPTH; i++){
    if(ISINSIDE(pcs[i], exit_start, exit_end)){
      ret = PROCESS_KILL;
      if (file != 0x0 && file->ref == 0) {
        acquire(&ftable->lock);
        file->ref++;
        release(&ftable->lock);
      }
    } else if(ISINSIDE(pcs[i], sys_close_start, sys_close_end)){
      ret = SYSCALL_SUCCESS;
    } else if(is_enable_user_coop(pid) && ISINSIDE(pcs[i], sys_pipe_start, sys_pipe_end)){
      ret = SYSCALL_REDO;
    }
  }
  if(holding(&new->lock))
    release(&new->lock);
  else
    pop_off();
  return ret;
}


// struct pipe's recovery handler.
int recovery_handler_pipe(void* address, int pid, uint64 sp, uint64 s0){
  printf_without_pr("start struct pipe recovery: %d, pid = %d\n", get_ticks(), pid);
  acquire_recovery_lock(RL_FLAG_PIPE);  // Validate R.C.S.

  struct file *f;
  struct pipe *new = (struct pipe*)my_kalloc(0, 0);
  struct proc *p;
 
  if(check_and_count_procs_in_rcs(RL_FLAG_PIPE, 0x0) > 1){
    printf_without_pr("recovery_handler_pipe: Goto Fail-Stop due to %d processes are in Recovery-locking Critical Section already.\n", check_and_count_procs_in_rcs(RL_FLAG_PIPE, 0x0));
    return FAIL_STOP;
  }

  /*
   * Internal-Surgery
   */
  recovery_handler_spinlock("pipe", &new->lock, address);
  new->nread = new->nwrite = 0;
  new->readopen = new->writeopen = 0;

  delete_memobj((void*)address, mlist.pip_list, 0x0);
  register_memobj((void*)new, mlist.pip_list);

  /*
   * Solve-Inconsistency
   */
  acquire(&new->lock);
  acquire(&ftable->lock);

  // Check ftable & set correct values to readopen/writeopen and kill FD_PIPE process.
  for(p = proc; p < &proc[NPROC]; p++){
    for(int i = 0; i < NOFILE; i++){
      f = p->ofile[i];
      if(f == 0x0)
        continue;

      if(f->pipe == address){
        if(f->writable){
          new->writeopen = 1;
          f->pipe = new;
        } else if(f->readable){
          new->readopen = 1;
          f->pipe = new;
        }

        if(p->state == SLEEPING){
          acquire(&p->lock);
          p->state = RUNNABLE;
          p->chan = 0x0;
          release(&p->lock);
        }

        acquire(&p->lock);
        p->killed = 1;
        release(&p->lock);
        goto end;
      }
    }
  }

end:
  release(&ftable->lock);

  while(mycpu()->noff > 2)
    pop_off();

  release_recovery_lock(RL_FLAG_PIPE);  // Invalidate R.C.S.

  exit_rcs_after_recovery(pid, 0);
  printf_without_pr("end struct pipe recovery: %d, %p\n", get_ticks(), new);
  /*
   * After-Treatment
   */
  return after_treatment_pipe(sp, s0, pid, new, f);
}
