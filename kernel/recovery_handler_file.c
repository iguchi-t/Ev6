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


extern struct mlist_header mlist;
extern struct bcache bcache;
extern struct ftable *ftable;
extern struct icache *icache;
extern struct log *log;
extern struct log _log;
extern struct logheader dup_lhdr;
extern struct proc proc[];
extern void (*handler_for_mem_fault)(char*);  // Function which is called from NMI handler.
extern int recovery_mode;
extern int dup_outstanding;

extern uint64 exit_start, exit_end;
extern uint64 fork_start, fork_end;
extern uint64 filealloc_start, filealloc_end;
extern uint64 filewrite_start, filewrite_end;
extern uint64 pipealloc_start, pipealloc_end;
extern uint64 sys_close_start, sys_close_end;
extern uint64 sys_open_start, sys_open_end;
extern uint64 sys_write_start, sys_write_end;


// Check the conditions for Fail-Stop from the kernel stack.
static int
check_fail_stop(uint64 *pcs)
{
  for (int i = 0; i < DEPTH; i++) {
    if (ISINSIDE(pcs[i], sys_open_start, sys_open_end))
      return 1;
    if (recovery_mode == CONSERVATIVE) {
      if (ISINSIDE(pcs[i], fork_start, fork_end) || ISINSIDE(pcs[i], pipealloc_start, pipealloc_end)
      || ISINSIDE(pcs[i], sys_write_start, sys_write_end))
        return 1;
    }
  }
  return 0;
}

// After-Treatment of struct file's recovery.
static int
after_treatment(uint64 *pcs, int num, struct proc *bp, struct inode *b_ip, uint64 pipe)
{
  int uc_status = is_enable_user_coop(bp->pid);
  int ret = (uc_status == ENABLE) ? REOPEN_SYSCALL_REDO : SYSCALL_FAIL;

  if (pipe != 0x0)  // FD_PIPE file node is broken.
    ret = SYSCALL_FAIL;

  for (int i = 0; i < DEPTH; i++) {
    if (ISINSIDE(pcs[i], sys_close_start, sys_close_end)) {
      ret = SYSCALL_SUCCESS;
    } else if (ISINSIDE(pcs[i], exit_start, exit_end)) {
      ret = PROCESS_KILL;
      break;
    } else if (uc_status == ENABLE && ISINSIDE(pcs[i], sys_write_start, sys_write_end)) {
      ret = REOPEN_SYSCALL_FAIL;
    } 
  }

  if (!pipe && log->outstanding > 0)
    end_op();

  if (((ret != REOPEN_SYSCALL_FAIL && ret != REOPEN_SYSCALL_REDO) || num == 1) && !pipe && b_ip) {
    if (holding(&icache->lock))
      release(&icache->lock);   // avoid deadlock
    iput(b_ip);
  }

  return ret;
}

// struct file's recovery handler.
int
recovery_handler_file(void* address, int pid, uint64 sp, uint64 s0)
{
  printf("start struct file recovery: %d, pid = %d\n", get_ticks(), pid);
  acquire_recovery_lock_file(address);  // Validate the recovery-locking critical section (R.C.S).

  int i, fd, match = 0;  // matched file node count
  int bfd_num = 0;  // number of fd which points broken file node.
  struct file *fp, *b_fp = 0, *broken = (struct file*)address, *r_fp = 0, *w_fp = 0;
  struct ftable *old_ftable = ftable;
  struct ftable *new_ftable = (struct ftable*)my_kalloc(0, 1);
  struct inode *ip, *b_ip = 0x0;
  struct proc *p, *bp = search_proc_from_pid(pid);
  uint64 pcs[DEPTH];

  // Search call stack & check Fail-Stop situation.
  if (s0 - sp > PGSIZE) {  // In the case of invalid s0 or sp is passed.
    getcallerpcs_bottom(pcs, sp, bp->kstack, DEPTH);
  } else {
    getcallerpcs_top(pcs, sp, s0, DEPTH);
  }
 
  if (check_fail_stop(pcs))
    return FAIL_STOP;
    
  push_off();

  /*
   * Internal-Surgery
   */
  // Allocate new ftable and find the place of broken node.
  // In this point, not to copy old ftable's contents to new ftable yet.
  for (i = 0, fp = old_ftable->file; fp < old_ftable->file + NFILE; fp++, i++) {
    if (fp == broken) {
      b_fp = &new_ftable->file[i];
      break;
    }
  }

  if (b_fp == 0) {
    printf("recovery_handler_file: Error. broken node can't be found.\n");
    return -1;
  }
 
  // Reset data where broken file exists in old ftable.
  b_fp->type	 = FD_NONE;
  b_fp->ip		 = 0;
  b_fp->pipe	 = 0;
  b_fp->readable = b_fp->writable = 0;
  b_fp->ref		 = b_fp->off = 0;
  b_fp->major	 = 0;

  // Check ftable.file[] to find FD_PIPE.
  for (fp = old_ftable->file; fp < old_ftable->file + NFILE; fp++) {
    if (fp == broken)
      continue;
    if (fp->type == FD_PIPE && fp->readable) {
      r_fp = fp;
    } else if (fp->type == FD_PIPE && fp->writable) {
      w_fp = fp;
    }
  }

  if (r_fp != 0x0 && w_fp == 0x0) {  // Lost writable pipe file.
    r_fp->pipe->readopen = 0;
    if (holding(&r_fp->pipe->lock))
      release(&r_fp->pipe->lock);

    for (p = proc; p < &proc[NPROC]; p++) {
      for (i = 0; i < NOFILE; i++) {
        if (p->ofile[i] == r_fp) {
          p->killed = 1;
          break;
        }
      }
    }
  } else if (r_fp == 0x0 && w_fp != 0x0) {  // Lost readable pipe file.
    w_fp->pipe->writeopen = 0;
    if (holding(&w_fp->pipe->lock))
      release(&w_fp->pipe->lock);

    for (p = proc; p < &proc[NPROC]; p++) {
      for (i = 0; i < NOFILE; i++) {
        if (p->ofile[i] == w_fp) {
          p->killed = 1;
          break;
        }
      }
    }
  }

  // Search the broken file's inode.
  if (!holding(&icache->lock))
    acquire(&icache->lock);
  for (ip = icache->inode; ip < icache->inode + NINODE; ip++) {
    if (ip->ref < 1)
      continue;
    for (fp = ftable->file; fp < ftable->file + NFILE; fp++) {
      if (fp == broken || fp->type == FD_NONE)
        continue;
      if (fp->ip == ip) {
        match++;
      }
    }
    for (p = proc; p < &proc[NPROC]; p++) {
      if (p->cwd == ip) {
        match++;
      }
    }

    // To find broken file's inode, compare ip->ref and matching count.
    // If match & ref don't match, it means the ip is inode of broken file.
    if (ip->ref != match && (ip->type == T_FILE || ip->type == T_DEVICE)) {
      b_ip = ip;
      break;
    }
    match = 0;  // Reset counter.
  }

  // Check Recovery-Critical Section.
  if (check_and_wait_procs_in_rcs_file(b_fp, b_ip)) {
    printf_without_pr("recovery_handler_file: Fail-Stop because other procs are in the same Recovery-Critical Section.\n");
    return FAIL_STOP;
  }

  if (!holding(&old_ftable->lock))
    acquire(&old_ftable->lock);

  // Move remain data to new ftable.
  for (i = 0, fp = old_ftable->file; fp < old_ftable->file + NFILE; fp++, i++) {
    if (fp == broken)
      continue;
    new_ftable->file[i] = old_ftable->file[i];
  }

  // Update each proc's every ofile[].
  for (p = proc; p < &proc[NPROC]; p++) {
    for (fd = 0; fd < NOFILE; fd++) {
      if (p->ofile[fd] == 0x0) {
        continue;
      } else if (p->ofile[fd] == broken) {
        p->ofile[fd] = (is_enable_user_coop(pid) == ENABLE && !((uint64)w_fp || (uint64)r_fp)) ? (struct file*)RESERVED : 0x0;
        bfd_num++;
        continue;
      }

      for (i = 0; i < NFILE; i++) {
        if (p->ofile[fd] && p->ofile[fd] == &old_ftable->file[i]) {
          p->ofile[fd] = &new_ftable->file[i];
        }
      }
    }
  }

  if (holding(&old_ftable->lock))
    release(&old_ftable->lock);
  
  // Delete old M-List nodes & Register new files to M-List.
  for (fp = old_ftable->file; fp < old_ftable->file + NFILE; fp++)
    delete_memobj((void*)fp, mlist.fil_list, 0x0);

  recovery_handler_spinlock("ftable", &new_ftable->lock, &old_ftable->lock);

  for (fp = new_ftable->file; fp < new_ftable->file + NFILE; fp++)
    register_memobj((void*)fp, mlist.fil_list);

  // Release related locks.
  for (ip = &icache->inode[0]; ip < &icache->inode[NINODE]; ip++) {
    if (ip->lock.locked && ip->lock.pid == pid) {
      releasesleep(&ip->lock);
    }
  }

  for (struct buf *b = bcache.head.next; b->next != &bcache.head; b = b->next) {
    if (b->lock.locked && b->lock.pid == pid) {
      releasesleep(&b->lock);
    }
  }

  // After finishing all of process, replace old pointer to new one.
  acquire(&new_ftable->lock);
  if (__sync_lock_test_and_set(&ftable, new_ftable));  // Assign new ftable pointer to ftable.
  if (holding(&old_ftable->lock))
    release(&old_ftable->lock);
  release(&ftable->lock);

  printf("struct file recovery: recovering completed. new ftable = %p\n", ftable);
  pop_off();

  exit_rcs_after_recovery(pid, 0);
  for(p = proc; p < &proc[NPROC]; p++){
    if(holding(&p->lock)){
      release(&p->lock);  // Release np->lock in fork().
    }
  }
  release_recovery_lock_file(address);  // Invalidate the R.C.S.
  printf("end struct file recovery: %d\n", get_ticks());

  /*
   * After-Treatment
   */
  return after_treatment(pcs, bfd_num, bp, b_ip, ((uint64)w_fp || (uint64)r_fp));
}