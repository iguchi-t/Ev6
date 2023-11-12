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
#include "kalloc.h"
#include "pipe.h"
#include "fs.h"
#include "file.h"
#include "nmi.h"
#include "log.h"
#include "recovery_locking.h"
#include "after-treatment.h"

extern struct mlist_header mlist;
extern char data[];
extern pagetable_t *idx_ptdup;
extern struct kmem *kmem;
extern struct log  *log;
extern struct proc proc[];
extern struct proc *initproc;
extern struct ftable *ftable;
extern struct spinlock idx_lock;
extern int recovery_mode;

extern uint64 allocproc_start, allocproc_end;
extern uint64 exit_start, exit_end;
extern uint64 freeproc_start, freeproc_end;
extern uint64 kvminit_start, kvminit_end;
extern uint64 procinit_start, procinit_end;
extern uint64 pipealloc_start, pipealloc_end;
extern uint64 sys_close_start, sys_close_end;
extern uint64 sys_exec_start, sys_exec_end;
extern uint64 sys_sbrk_start, sys_sbrk_end;
extern uint64 uvmalloc_start, uvmalloc_end;
extern uint64 uvmcopy_start, uvmcopy_end;
extern uint64 uvmunmap_start, uvmunmap_end;


// struct kmem & run's After-Treatments.
static int after_treatment(int pid, uint64 sp, uint64 s0){
  int i, ret = (is_enable_user_coop(pid)) ? SYSCALL_REDO : SYSCALL_FAIL;
  uint64 pcs[DEPTH];

  if(sp == 0x0 || s0 - sp > PGSIZE){
    struct proc *p = search_proc_from_pid(pid);
    getcallerpcs_bottom(pcs, sp, p->kstack, DEPTH);
  } else {
    getcallerpcs_top(pcs, sp, s0, DEPTH);
  }

  // Check After-Treatment conditions.
  for(i = 0; i < DEPTH; i++){
    if(pipealloc_start < pcs[i] && pcs[i] < pipealloc_end){
      // If pipealloc() exists, cancel pipe operation & close the files.
      for(struct file *fp = ftable->file; fp < ftable->file + NFILE; fp++){
        if(fp->type == FD_PIPE)
          fileclose(fp);
      }
    }

    if(ISINSIDE(pcs[i], freeproc_start, freeproc_end) || ISINSIDE(pcs[i], kvminit_start, kvminit_end)
    || ISINSIDE(pcs[i], procinit_start, procinit_end) || ISINSIDE(pcs[i], uvmunmap_start, uvmunmap_end))
      return FAIL_STOP;

    if(recovery_mode == CONSERVATIVE)
      if(ISINSIDE(pcs[i], sys_exec_start, sys_exec_end) || ISINSIDE(pcs[i], uvmalloc_start, uvmalloc_end)
      || ISINSIDE(pcs[i], uvmcopy_start, uvmcopy_end))
        return FAIL_STOP;

    if(ISINSIDE(pcs[i], exit_start, exit_end) || ISINSIDE(pcs[i], uvmalloc_start, uvmalloc_end))
      ret = PROCESS_KILL;

    if(ISINSIDE(pcs[i], sys_close_start, sys_close_end))
      ret = SYSCALL_SUCCESS;

    if(ISINSIDE(pcs[i], sys_sbrk_start, sys_sbrk_end)){
      if(log->outstanding){
        acquire(&log->lock);
        if(log->outstanding > 1)
          log->outstanding--;
        release(&log->lock);
        exit_recovery_critical_section(RL_FLAG_LOG, 0);
      }
    }
  }

  // Check transaction conditions.
  if(check_and_handle_trans_pagetable(pid) > 0 && ret == SYSCALL_FAIL){
    ret = PROCESS_KILL;
  }

  for(struct proc *p = proc; p < &proc[NPROC]; p++){
    if(holding(&p->lock)){
      release(&p->lock);  // Release np->lock in fork().
    }
  }
  while(mycpu()->noff > 1) pop_off();  // Decrement noff of broken kmem's spinlock.
  exit_rcs_after_recovery(pid, 0);
  release_recovery_lock(RL_FLAG_KMEM);
  printf("end struct kmem or run recovery: %d\n", get_ticks());

  return ret;
}


int recovery_handler_kmem(void* broken, int pid, uint64 sp, uint64 s0){
  printf("start struct kmem recovery: %d\n", get_ticks());
  acquire_recovery_lock(RL_FLAG_KMEM);

  struct kmem *new = (struct kmem*)mlist.run_list->next->addr;

  // Internal Surgery
  // In recovery_handler_kmem, we can't use kalloc() and my_kalloc().
  // So we have to allocate new page with our own hands.
  if(!new)
    panic("recovery_handler_kmem: memory page allocation failed.");
  delete_memobj((void*)new, mlist.run_list, 0x0);

  recovery_handler_spinlock("kmem", &new->lock, broken);
  acquire(&new->lock);
  new->freelist = (struct run*)mlist.run_list->next->next->addr;  // Assign address of run_list's node instead of broken kmem.

  delete_memobj(broken, mlist.kmm_list, 0x0);
  if(__sync_lock_test_and_set(&kmem, new));  // Assign new kmem pointer to kmem in atomic.
  register_memobj(new, mlist.kmm_list);
  printf("struct kmem recovery completes (%p).\n", kmem);

  // After-Treatments
  release(&kmem->lock);
  check_and_handle_trans_run(pid);
  return after_treatment(pid, sp, s0);
}


int recovery_handler_run(void *broken, int pid, uint64 sp, uint64 s0){
  printf("start struct run recovery: %d\n", get_ticks());
  struct mlist_node *bdnode, *dnode = 0;
  struct run *r = 0x0;
  acquire_recovery_lock(RL_FLAG_KMEM);

  // Internal Surgery
  // Identify the previous run node of the broken node.
  for(bdnode = mlist.run_list; bdnode->next != mlist.run_list; bdnode = bdnode->next){
    if(bdnode->next->addr == broken){
      dnode = bdnode->next;
      bdnode->next = dnode->next;

      if(bdnode != mlist.run_list){
        r = (struct run*)bdnode->addr;
        r->next = (struct run*)dnode->next->addr;
      } else {
        r = (struct run*)bdnode->next->addr;
        r->next = (struct run*)bdnode->next->next->addr;
      }

      dnode->addr = (void*)0x1010101;
      dnode->next = (void*)0x1010101;
      break;
    }
  }

  // kmem->lock is already acquired because when memory error in run is discovered,
  // the process wants to alloc a page and has to acquire kmem->lock.
  kmem->freelist = r;
  if(holding(&kmem->lock))
    release(&kmem->lock);
  printf("strcut run recovery completes.\n");

  // After-Treatment
  return after_treatment(pid, sp, s0);
}
