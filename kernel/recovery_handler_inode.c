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
extern struct ftable *ftable;
extern struct icache *icache;
extern struct log *log;
extern struct log _log;
extern struct logheader dup_lhdr;
extern struct proc proc[];
extern void (*handler_for_mem_fault)(char*);  // Function which is called from NMI handler.
extern int recovery_mode;
extern int dup_outstanding;

extern uint64 create_start, create_end;
extern uint64 exit_start, exit_end;
extern uint64 idup_start, idup_end;
extern uint64 iput_start, iput_end;
extern uint64 iupdate_start, iupdate_end;
extern uint64 sys_chdir_start, sys_chdir_end;
extern uint64 sys_close_start, sys_close_end;
extern uint64 sys_fstat_start, sys_fstat_end;
extern uint64 sys_link_start, sys_link_end;
extern uint64 sys_open_start, sys_open_end;
extern uint64 sys_read_start, sys_read_end;
extern uint64 sys_unlink_start, sys_unlink_end;
extern uint64 sys_write_start, sys_write_end;
extern uint64 writei_start, writei_end;

static int
lockingsleep(struct sleeplock *lk)
{
  int res;
  
  acquire(&lk->lk);
  res = lk->locked;
  release(&lk->lk);
  return res;
}

// Check the conditions for Fail-Stop from the kernel stack.
static int
check_fail_stop(struct inode *broken, struct icache *old_icache, uint64 *pcs)
{
  int i, is_tdev = 0, is_root = 0;
  struct inode *ip;

  if(recovery_mode == CONSERVATIVE){
    for(int i = 0; i < DEPTH; i++){
      if(ISINSIDE(pcs[i], create_start, create_end) || ISINSIDE(pcs[i], idup_start, idup_end) ||
         ISINSIDE(pcs[i], iput_start, iput_end) || ISINSIDE(pcs[i], iupdate_start, iupdate_end) ||
         ISINSIDE(pcs[i], sys_link_start, sys_link_end) || ISINSIDE(pcs[i], sys_open_start, sys_open_end) ||
         ISINSIDE(pcs[i], sys_unlink_start, sys_unlink_end) || ISINSIDE(pcs[i], writei_start, writei_end)){
        return 1;
      }
    }
  }

  // If there are no T_DEVICE in icache, conclude T_DEVICE's inode is broken,
  // or if there are no ROOTINO in icache, conclude ROOTINO is broken.
  for(i = 0, ip = &old_icache->inode[0]; ip < &old_icache->inode[0] + NINODE; i++, ip++){
    if(ip == broken)
      continue;
    else if(ip->type == T_DEVICE)
      is_tdev = 1;
    else if(ip->inum == ROOTINO)
      is_root = 1;
  }

  if(ip == &old_icache->inode[0] + NINODE && is_tdev == 0){
    // If there is no inodes of T_DEVICE, conclude T_DEVICE's inode was broken and do system-down.
    printf("recovery_handler_inode: T_DEVICE is broken, so do system-down.\n");
    return 1;
  } else if(ip == &old_icache->inode[0] + NINODE && is_root == 0){
    printf("recovery_handler_inode: root directory was broken, go panic().\n");
    return 1;
  }

  return 0;  // Not Fail-Stop case.
}

static struct file*
solve_inconsistency_inode(void *broken, int b_idx, int pid, struct icache *old, struct icache *new)
{
  int i;
  struct file *b_fp = 0x0, *fp;
  struct proc *p;

  // Between struct file nodes.
  // Fix all struct file's ip value & clear fp which points the broken inode.
  if (!holding(&ftable->lock))
    acquire(&ftable->lock);

  for (fp = ftable->file; fp < ftable->file + NFILE; fp++) {
    if ((void*)fp->ip == broken) {
      b_fp = fp;
      // Find the file which uses broken inode's pointer.
      // Clear this file's member.
      fp->type = FD_NONE;
      fp->ref = 0;
      fp->ip = 0;
      fp->off = 0;
      fp->major = 0;
      for (p = proc; p < &proc[NPROC]; p++) {  // Evict from cleared fp from the ofile[].
        for (i = 0; i < NOFILE; i++) {
          if (p->ofile[i] == fp) {
            p->ofile[i] = (is_enable_user_coop(pid) == ENABLE) ? (struct file*)RESERVED : 0x0;
          }
        }
      }
    } else {
      for (i = 0; i < NINODE; i++) {
        if (i != b_idx) {
          if (fp->ip == &old->inode[i]) {
            fp->ip = &new->inode[i];
          }
        }
      }
    }
  }

  // Between struct log.
  if (!holding(&log->lock))
    acquire(&log->lock);
  if (log->outstanding > 0)
    log->outstanding--;
  if (holding(&log->lock))
    release(&log->lock);

  // Between struct procs.
  // Fix all running proc's cwd.
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->pid == 0)
      continue;

    for(i = 0; i < NINODE; i++){
      if(p->cwd == &old->inode[i]){
        if(!holding(&p->lock))
          acquire(&p->lock);
        p->cwd = &new->inode[i];
        release(&p->lock);
      }
    }
  }

  return b_fp;
}

// After-Treatment in inode recovery.
static int
after_treatment_inode(uint64 *pcs, int pid, struct file *fp)
{
  int uc_status = is_enable_user_coop(pid);
  int ret = (uc_status == ENABLE) ? SYSCALL_REDO : SYSCALL_FAIL;

  for (int i = 0; i < DEPTH; i++) {
    if (ISINSIDE(pcs[i], sys_close_start, sys_close_end)) {
      ret = SYSCALL_SUCCESS;
    } else if (ISINSIDE(pcs[i], sys_unlink_start, sys_unlink_end)) {
      if(uc_status){
        ret = (fp != 0) ? REOPEN_SYSCALL_REDO : SYSCALL_REDO;
      }
    } else if (ISINSIDE(pcs[i], sys_chdir_start, sys_chdir_end) || ISINSIDE(pcs[i], sys_fstat_start, sys_fstat_end) ||
               ISINSIDE(pcs[i], sys_link_start, sys_link_end) || ISINSIDE(pcs[i], sys_read_start, sys_read_end)) {
      ret = (uc_status == ENABLE) ? REOPEN_SYSCALL_REDO : SYSCALL_FAIL;
    } else if (ISINSIDE(pcs[i], idup_start, idup_end)) {
      ret = PROCESS_KILL;
      break;
    }
  }

  printf("end struct inode recovery: %d\n", get_ticks());
  return ret;
}


// struct inode's recovery handler.
int
recovery_handler_inode(void* address, int pid, uint64 sp, uint64 s0)
{
  printf("start struct inode recovery: %d, broken = %p\n", get_ticks(), address);
  acquire_recovery_lock_inode(address);  // Validate the recovery-locking critical section (R.C.S).
 
  int i, b_idx = 0;
  struct file *b_fp = 0x0;
  struct icache *old_icache = icache;
  struct inode *ip, *broken = (struct inode*)address, *old_ip, *new_ip;
  struct proc *p;
  uint64 pcs[DEPTH];

  // Search call stack and check Fail-Stop cases.
  if(sp == 0x0 || s0 - sp > PGSIZE){  // In the case of invalid s0 or sp is passed.
    p = search_proc_from_pid(pid);
    getcallerpcs_bottom(pcs, sp, p->kstack, DEPTH);
  } else {
    getcallerpcs_top(pcs, sp, s0, DEPTH);
  }

  if(check_fail_stop(broken, old_icache, pcs))
    return FAIL_STOP;

  for(i = 0, ip = &old_icache->inode[0]; ip < &old_icache->inode[0] + NINODE; i++, ip++){
    if(ip == broken){
      b_idx = i;
      break;
    }
  }

  /*
   * Internal-Surgery
   */
  // Allocate new icache and move data from the old to the new.
  struct icache *new_icache = (struct icache*)my_kalloc(0, 3);

  if(new_icache == 0x0)
    panic("recovery_handler_inode: my_kalloc() failed");

  memset(new_icache, 0, sizeof(struct icache));
  recovery_handler_spinlock("icache", &new_icache->lock, (void*)&old_icache->lock);

  // Clear broken struct inode node.
  new_icache->inode[b_idx].valid = new_icache->inode[b_idx].inum = 0;
  new_icache->inode[b_idx].type  = new_icache->inode[b_idx].major = new_icache->inode[b_idx].minor = new_icache->inode[b_idx].nlink = 0;
  new_icache->inode[b_idx].size  = 0;

  // Delete old M-List nodes & Register new inodes to M-List.
  for(i = 0; i < NINODE; i++){
    old_ip = &old_icache->inode[i];
    new_ip = &new_icache->inode[i];
    delete_memobj((void*)old_ip, mlist.ino_list, 0x0);
    register_memobj((void*)new_ip, mlist.ino_list);

    if(old_ip == broken){
      recovery_handler_sleeplock("inode", &new_ip->lock, (void*)old_ip, sizeof(struct inode));
    } else {
      if(lockingsleep(&old_ip->lock) && old_ip->lock.pid == pid){
        if(holding(&icache->lock))
          release(&icache->lock);
        iunlock(old_ip);  // Release inodes which are held by the UE occurred proc.
      }
      recovery_handler_sleeplock("inode", &new_ip->lock, &old_ip->lock, 0x0);
    }
  }

  if(check_and_wait_procs_in_rcs_inode(broken, b_fp, pid)){
    printf_without_pr("recovery_handler_inode: Fail-Stop because other procs are in the same Recovery-Critical Section.\n");
    return FAIL_STOP;
  }

  // Copy old inodes' data to new inodes (except broken node).
  if(!holding(&icache->lock))
    acquire(&icache->lock);
  for(i = 0; i < NINODE; i++){
    if(i != b_idx){
      new_icache->inode[i] = old_icache->inode[i];
    }
  }

  acquire(&new_icache->lock);
  if(__sync_lock_test_and_set(&icache, new_icache));  // Switch the icache pointer from the old to the new.
  if(!__sync_lock_test_and_set(&old_icache->lock.locked, 1)){
    release(&icache->lock);
  } else if(!holding(&old_icache->lock)){
    icache->lock = old_icache->lock;
  }
  if(holding(&old_icache->lock))
    release(&old_icache->lock);

  /*
   * Solve Inconsistency
   */
  b_fp = solve_inconsistency_inode(broken, b_idx, pid, old_icache, new_icache);

  printf("struct inode recovery is completed: %d\n", get_ticks());
  exit_rcs_after_recovery(pid, 0);
  release_recovery_lock_inode(address);  // Invalidate R.C.S.

  /*
   * After-Treatment
   */
  return after_treatment_inode(pcs, pid, b_fp);
}
