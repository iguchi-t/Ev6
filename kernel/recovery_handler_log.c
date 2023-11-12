#include "param.h"
#include "types.h"
#include "stat.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "fs.h"
#include "buf.h"
#include "pipe.h"
#include "file.h"
#include "mlist.h"
#include "log.h"
#include "memlayout.h"
#include "virtio.h"
#include "nmi.h"
#include "recovery_locking.h"
#include "after-treatment.h"


extern int recovery_mode;
extern int dup_outstanding;
extern struct mlist_header mlist;
extern struct bcache bcache;
extern struct ftable *ftable;
extern struct icache *icache;
extern struct log *log;
extern struct log _log;
extern struct logheader dup_lhdr;
extern struct proc proc[];
extern void (*handler_for_mem_fault)(char*);  // Function which is called from NMI handler.

extern uint64 create_start, create_end;
extern uint64 begin_op_start, begin_op_end;
extern uint64 end_op_start, end_op_end;
extern uint64 exit_start, exit_end;
extern uint64 fileclose_start, fileclose_end;
extern uint64 log_write_start, log_write_end;
extern uint64 sys_chdir_start, sys_chdir_end;
extern uint64 sys_write_start, sys_write_end;


// After-Treatment of struct log's recovery.
static int
after_treatment_log(uint64 *pcs, struct proc *p)
{
  int uc_status = is_enable_user_coop(p->pid);
  int ret = (uc_status == ENABLE) ? SYSCALL_REDO : SYSCALL_FAIL;

  for(int i = 0; i < DEPTH; i++){
    if(ISINSIDE(pcs[i], exit_start, exit_end)){
      p->cwd->ref++;  // Prevent inode->ref from being decremented one more time.
      ret = PROCESS_KILL;
    }
  }
  release_recovery_lock(RL_FLAG_LOG);

  printf("end struct log recovery: %d\n", get_ticks());
  return ret;
}

// Struct log's recovery handler. Struct logheader is included.
int
recovery_handler_log(void* address, int pid, uint64 sp, uint64 s0)
{
  printf("start struct log recovery: %d, broken = %p\n", get_ticks(), address);
  acquire_recovery_lock(RL_FLAG_LOG);  // Validate recovery-locking critical section.

  int i, is_chdir = 0, is_begin_op = 0, is_end_op = 0, is_log_write = 0;
  struct log *new_log = (struct log*)my_kalloc(0, 0);
  struct proc *p = search_proc_from_pid(pid);
  struct superblock sb;
  uint64 pcs[DEPTH];

  if (sp == 0x0 || s0 - sp > PGSIZE) {
    getcallerpcs_bottom(pcs, sp, p->kstack, DEPTH);
  } else {
    getcallerpcs_top(pcs, sp, s0, DEPTH);
  }

  for (i = 0; i < DEPTH; i++) {
    if(ISINSIDE(pcs[i], log_write_start, log_write_end)){
      if(recovery_mode == CONSERVATIVE)
        return FAIL_STOP;
      is_log_write = 1;
    } else if(ISINSIDE(pcs[i], begin_op_start, begin_op_end)){
      is_begin_op = 1;
    } else if(ISINSIDE(pcs[i], end_op_start, end_op_end)){
      is_end_op = 1;
    } else if(ISINSIDE(pcs[i], sys_chdir_start, sys_chdir_end)){
      is_chdir = 1;
    } else if(ISINSIDE(pcs[i], end_op_start, end_op_end) && is_chdir){
      printf("end struct log recovery: %d\n", get_ticks());
      return FAIL_STOP;
    }

    if(recovery_mode == CONSERVATIVE){
      if(ISINSIDE(pcs[i], create_start, create_end) || (ISINSIDE(pcs[i], fileclose_start, fileclose_end) && is_begin_op) ||
         ISINSIDE(pcs[i], sys_write_start, sys_write_end)){
        return FAIL_STOP;
      }
    }
  }

  while(check_proc_in_log_commit(pid))  // Wait completing commit().
    ;

  if(check_and_count_procs_in_rcs(RL_FLAG_LOG, 0x0) > 1){
    printf("recovery_handler_log: Goto Fail-Stop due to %d processes are in Recovery-locking Critical Section already.\n", check_and_count_procs_in_rcs(RL_FLAG_LOG, 0));
    return FAIL_STOP;
  }

  /*
   * Internal Surgery
   */
  // Pick up log's basic information.
  readsb(ROOTDEV, &sb);
  recovery_handler_spinlock("log", &new_log->lock, (void*)address);
  new_log->start = sb.logstart;
  new_log->size  = sb.nlog;
  new_log->dev   = ROOTDEV;
  new_log->committing = 0;

  // Recover log.lh (logheader).
  check_and_handle_trans_logheader(pid);
  new_log->lh.n = dup_lhdr.n;
  for(i = 0; i < new_log->lh.n; i++)
    new_log->lh.block[i] = dup_lhdr.block[i];
  new_log->outstanding = dup_outstanding;
 
  // Replace old to new log pointer.
  new_log->lock.locked = 0;
  delete_memobj((void*)address, mlist.log_list, 0x0);
  register_memobj((void*)new_log, mlist.log_list);

  acquire(&new_log->lock);
  if(__sync_lock_test_and_set(&log, new_log));  // Assign new log pointer to log in atomic.
  printf_without_pr("struct log recovery is completed. (%p)\n", log);

  /*
   * Solving Inconsistencies
   */
  // Reflect the interrupted syscall procedure aborting to the outstanding value.
  if(log->outstanding && (is_begin_op || is_end_op || is_log_write)){
    log->outstanding--;
    dup_outstanding--;
  }

  // Check struct buf nodes which recovery process has their sleeplock.
  for(struct buf *b = bcache.head.next; b != &bcache.head; b = b->next){
    if(b->lock.locked && b->lock.pid == pid){
      // Do the same things as brelse() except holdingsleep()
      // because the recovery process can differ from the acquiring process.
      releasesleep(&b->lock);
      acquire(&bcache.lock);
      b->refcnt--;
      if(b->refcnt == 0){
        b->next->prev = b->prev;
        b->prev->next = b->next;
        b->next = bcache.head.next;
        b->prev = &bcache.head;
        bcache.head.next->prev = b;
        bcache.head.next = b;
      }
      release(&bcache.lock);
    }
  }

  // Commit after checking struct bufs sleeplock to avoid re-acquiring sleeplock.
  if(log->outstanding == 0){
    log->committing = 1;
    commit();
    log->committing = 0;
    wakeup(&log);
  }

  // Release this proc holding inodes' sleeplocks and decrement their reference counts.
  for(struct inode *ip = &icache->inode[0]; ip < &icache->inode[NINODE]; ip++){
    if(ip->lock.locked && ip->lock.pid == pid){     
      iunlockput(ip);
    }
  }
  release(&log->lock);

  // pop_off for the old log's lock, but leave 1 noff for recovery_lock.
  while(mycpu()->noff > 1)
    pop_off();

  exit_rcs_after_recovery(pid, 0);

  /*
   * After-Treatment
   */
  return after_treatment_log(pcs, p);
}

