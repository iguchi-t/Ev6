#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "log.h"
#include "buf.h"
#include "mlist.h"
#include "nmi.h"
#include "recovery_locking.h"
#include "proc.h"
#include "after-treatment.h"

extern struct bcache bcache;
extern struct ftable *ftable;
extern struct icache *icache;
extern struct nmi_info *nmi_queue;
extern struct proc proc[];
extern struct spinlock nmi_queue_lock;

struct recovery_lock recovery_locks[RL_NFLAGS];
struct recovery_lock_idx buf_idx[NBUF];  // Index of struct buf nodes to flag, Read-Only.
struct recovery_lock_idx file_idx[NFILE];  // Index of struct file nodes to flag, Read-Only.
struct recovery_lock_idx inode_idx[NINODE];  // Index of struct inode nodes to flag, Read-Only.
struct proc_rcs_info rcs_infos[NPROC];   // R.C.S informations of each procs.


int
locking(struct spinlock *lk)
{
  int res;

  push_off();
  res = lk->locked;
  pop_off();

  return res;
}

void
init_rcs_infos(int pid)
{
  for(int i = 0; i < NPROC; i++){
    if(rcs_infos[i].pid == 0){
      rcs_infos[i].pid = pid;
      break;
    }
  }
}

void
init_recovery_lock_idx(void)
{
  int i;  

  for(i = 0; i < NBUF; i++){
    buf_idx[i].addr = (void*)&bcache.buf[i];
    buf_idx[i].flag = LAST_FLAG_WO_IDX + i + 1;
  }

  for(i = 0; i < NFILE; i++){
    file_idx[i].addr = (void*)&ftable->file[i];
    file_idx[i].flag = LAST_FLAG_WO_IDX + NBUF + i;
  }

  for(i = 0; i < NINODE; i++){
    inode_idx[i].addr = (void*)&icache->inode[i];
    inode_idx[i].flag = LAST_FLAG_WO_IDX + NBUF + NFILE + i;
  }
}

void
init_recovery_locks(void)
{
  for(int i = 0; i < RL_NFLAGS; i++){
    recovery_locks[i].num = 0;
    recovery_locks[i].exception = 0;
    initlock(&recovery_locks[i].lock, "Recovery Lock");
    initlock(&recovery_locks[i].lk, "Recovery Lock's lock");
  }
}

static int
search_recovery_idx(int flag, void *addr)
{
  switch(flag){
    case RL_FLAG_BUF:
      for(int i = 0; i < NBUF; i++){
        if(buf_idx[i].addr == addr){
          return buf_idx[i].flag;
        }
      }
      break;
    case RL_FLAG_FILE:
      for(int i = 0; i < NFILE; i++){
        if(file_idx[i].addr == addr){
          return file_idx[i].flag;
        }
      }
      break;
    case RL_FLAG_INODE:
      for(int i = 0; i < NINODE; i++){
        if(inode_idx[i].addr == addr){
          return inode_idx[i].flag;
        }
      }
      break;
    default:
      printf("search_recovery_idx: Passed invalid Recovery-Locking flag (%d).\n", flag);
  }

  return -1;  // Not found.
}


/*
 * Recovery Critical Section history
 */
static void
add_rcs_history(int pid, int flag)
{
  int i, idx = -1;

  for(i = 0; i < NPROC; i++){
    if(rcs_infos[i].pid == pid){
      idx = i;
      break; 
    }
  }

  if(idx < 0){
    panic_without_pr("add_rcs_history: No corresponding R.C.S info history");
  }

  for(i = 0; i < RCS_INFO_HISTORY_SIZE; i++){
    if(rcs_infos[idx].history_idx[i] == 0){
      rcs_infos[idx].history_idx[i] = flag;
      break;
    }
  }
}

static void
del_rcs_history(int pid, int flag)
{
  int i, idx = -1;

  for(i = 0; i < NPROC; i++){
    if(rcs_infos[i].pid == pid){
      idx = i;
      break; 
    }
  }

  if(idx < 0)
    panic_without_pr("del_rcs_history: No corresponding R.C.S info history");

  for(i = RCS_INFO_HISTORY_SIZE-1; i >= 0; i--){
    if(rcs_infos[idx].history_idx[i] == flag){
      rcs_infos[idx].history_idx[i] = 0;
      break;
    }
  }
  
  for(i = 0; i < RCS_INFO_HISTORY_SIZE-1; i++){
    if(rcs_infos[idx].history_idx[i] == 0){
       if(rcs_infos[idx].history_idx[i+1] == 0)
          break;
       rcs_infos[idx].history_idx[i] = rcs_infos[idx].history_idx[i+1];
       rcs_infos[idx].history_idx[i+1] = 0;
    }
  }
  rcs_infos[idx].history_idx[i] = 0;
}

void
free_rcs_history(int pid)
{
  int i, idx = -1;

  for(i = 0; i < NPROC; i++){
    if(rcs_infos[i].pid == pid){
      idx = i;
      break; 
    }
  }
  if (idx < 0)
    return;   // panic is not appropriate in the case of allocproc() in fork().

  // Clear all R.C.S nestings, but any histories must not remain.
  for(i = 0; i < RCS_INFO_HISTORY_SIZE; i++){
    rcs_infos[idx].history_idx[i] = 0;
  }
  rcs_infos[idx].pid = 0;
}

int
search_rcs_history(int pid, int flag)
{
  int idx = -1;

  for(int i = 0; i < NPROC; i++){
    if(rcs_infos[i].pid == pid){
      idx = i;
      break;
    }
  }
  if(idx < 0)
    panic_without_pr("search_rcs_history: No corresponding R.C.S info history");

  for(int i = 0; i < RCS_INFO_HISTORY_SIZE; i++){
    if(rcs_infos[idx].history_idx[i] == flag){
      return 1;  // Found.
    }
  }
  
  return 0;
}

/*
 * Recovery Critical Section entrance/exit API
 */
// Entrance function of recovery-locking critical section.
void
enter_recovery_critical_section(int flag, int exception)
{
  struct recovery_lock *rlk;

  if (flag < 0 || RL_FLAG_MAX < flag) {
    panic_without_pr("enter_recovery_critical_section: Invalid recovery-locking flag.");
  }
  if (mycpu()->noff < 0) {
    panic_without_pr("enter_recovery_critical_section: Invalid push_off() nesting value (noff).");  // to avoid system hung-up due to panic() calling loop because of invalid noff.
  }

  rlk = &recovery_locks[flag];
  if(!holding(&rlk->lock)){  // All processes except recovery process have to go through the inspection.
    acquire(&rlk->lk);
    while(locking(&rlk->lock)){
      if(exception == ICACHE_FORK)
        panic("Avoids deadlock occurrence");
      sleep(rlk, &rlk->lk);
    }
    
    // Enter recovery-locking critical section.
    if(__sync_lock_test_and_set(&rlk->num, rlk->num + 1));  // Replace num atomically.
    
    if(exception)
      rlk->exception = exception;
    release(&rlk->lk);
  }

  if(myproc()){  // Avoid NULL pointer access during boot time.
    add_rcs_history(myproc()->pid, flag);  // Record as in the R.C.S.
  }
}

// For buf, file, inode.
void
enter_recovery_critical_section_nodes(int f, void *addr)
{
  int flag = search_recovery_idx(f, addr);

  if(flag < 0){
    panic("Invalid flag passed");
  }
  else if(locking(&recovery_locks[flag].lock) && !holding(&recovery_locks[flag].lock))
    panic("Try to enter broken node's Recovery Critical Section");  // Now I choose Fail-Stop, but Process Kill or Syscall Fail may be able to be applied.

  enter_recovery_critical_section(flag, 0);
}

// Exit function of recovery-locking critical section.
// Exit is free because there are no needs for waiting the recovery process.
void
exit_recovery_critical_section(int flag, int exception)
{
  struct recovery_lock *rlk;

  if(flag < 0 || RL_FLAG_MAX < flag){
    panic_without_pr("exit_recovery_critical_section: Invalid recovery-locking flag.");
  }
  if (mycpu()->noff < 0) {
    panic_without_pr("enter_recovery_critical_section: Invalid push_off() nesting value (noff).");  // to avoid system hung-up due to panic() calling loop because of invalid noff.
  }

  // Exit recovery-locking critical section.
  rlk = &recovery_locks[flag];
  acquire(&rlk->lk);

  if(__sync_lock_test_and_set(&rlk->num, rlk->num-1));  // Replace num atomically.
  if(rlk->exception == exception)
    rlk->exception = 0;
  release(&rlk->lk);

  if(myproc()){
    del_rcs_history(myproc()->pid, flag);  // Remove the record.
  }
}

void
exit_recovery_critical_section_nodes(int flag, void *addr)
{
  flag = search_recovery_idx(flag, addr);
  if(flag < 0 || RL_FLAG_MAX < flag){
    panic("exit_recovery_critical_section_nodes: Invalid recovery-locking flag.");  // panic() can't handle the case of normal operation failing.
    return;
  }
  exit_recovery_critical_section(flag, 0);
}

static void
exit_recovery_critical_section_all(int pid)
{
  int i, idx = -1, flag = -1;
  struct recovery_lock *rlk;

  for(i = 0; i < NPROC; i++){
    if(rcs_infos[i].pid == pid){
      idx = i;
      break; 
    }
  }
  if(idx < 0)
    panic_without_pr("exit_recovery_critical_section_all: No corresponding R.C.S nesting history");

  // Exit from every R.C.S.
  for(i = 0; i < RCS_INFO_HISTORY_SIZE; i++){
    flag = rcs_infos[idx].history_idx[i];

    rlk = &recovery_locks[flag];
    acquire(&rlk->lk);
    if(__sync_lock_test_and_set(&rlk->num, rlk->num-1));
    release(&rlk->lk);
    rcs_infos[idx].history_idx[i] = 0;
  }
}

void
exit_rcs_after_recovery(int pid, int intr)
{
  switch(intr){
    case CONSINTR_CONS:
      exit_recovery_critical_section(RL_FLAG_CONS, 0);
      break;

    case CONSINTR_PR:
      exit_recovery_critical_section(RL_FLAG_PR, 0);
      exit_recovery_critical_section(RL_FLAG_CONS, 0);
      break;

    case CLCKINTR_TICKSLOCK:
      break;

    default:
      exit_recovery_critical_section_all(pid);
  } 
}

/*
 * Recovery-Locking mechanism.
 */
void
acquire_recovery_lock(int flag)
{
  if(flag < 0 || RL_FLAG_MAX < flag)
    panic_without_pr("Invalid recovery_locks flag");

  acquire(&recovery_locks[flag].lock);
}

void
acquire_recovery_lock_buf(void *b)
{
  int flag = search_recovery_idx(RL_FLAG_BUF, b);

  if(flag < 0 || RL_FLAG_MAX < flag)
    panic("Invalid recovery-locking flag");
  acquire_recovery_lock(RL_FLAG_BCACHE);
  acquire_recovery_lock(flag);
}

void
acquire_recovery_lock_file(void *f)
{
  int flag = search_recovery_idx(RL_FLAG_FILE, f);

  if(flag < 0 || RL_FLAG_MAX < flag)
    panic("Invalid recovery-locking flag");
  acquire_recovery_lock(RL_FLAG_FTABLE);
  acquire_recovery_lock(flag);
}

void
acquire_recovery_lock_inode(void *ip)
{
  int flag = search_recovery_idx(RL_FLAG_INODE, ip);

  if(flag < 0 || RL_FLAG_MAX < flag)
    panic("Invalid recovery-locking flag");
  acquire_recovery_lock(RL_FLAG_ICACHE);
  acquire_recovery_lock(flag);
}

void
release_recovery_lock(int flag)
{
  if(flag < 0 || RL_FLAG_MAX < flag)
    panic_without_pr("Invalid recovery_locks flag");
  release(&recovery_locks[flag].lock);
  wakeup(&recovery_locks[flag]);
}

// This update function is only called in the recovery handlers.
void
update_recovery_lock_idx(int flag, void *broken, void *new){
  switch(flag){
    case RL_FLAG_BUF:
      for(int i = 0; i < NBUF; i++){
        if(buf_idx[i].addr == broken){
          buf_idx[i].addr = new;
        }
      }
      break;

    case RL_FLAG_FILE:
      for(int i = 0; i < NFILE; i++){
        file_idx[i].addr = (void*)&ftable->file[i];
      }
      break;

    case RL_FLAG_INODE:
      for(int i = 0; i < NINODE; i++){
        inode_idx[i].addr = (void*)&icache->inode[i];
      }
      break;

    default:
      printf("update_recovery_lock_idx: Passed invalid RL_FLAG.\n");
  }
}

void
release_recovery_lock_buf(void *broken)
{
  int old_idx = search_recovery_idx(RL_FLAG_BUF, broken);

  if(old_idx < 0 || RL_FLAG_MAX < old_idx)
    panic("Invalid recovery-locking flag");

  release_recovery_lock(RL_FLAG_BCACHE);
  release_recovery_lock(old_idx);
}

void
release_recovery_lock_file(void *f)
{
  int old_idx = search_recovery_idx(RL_FLAG_FILE, f);

  if(old_idx < 0 || RL_FLAG_MAX < old_idx)
    panic("Invalid recovery-locking index");

  update_recovery_lock_idx(RL_FLAG_FILE, 0x0, 0x0);

  release_recovery_lock(RL_FLAG_FTABLE);
  release_recovery_lock(old_idx);
}

void
release_recovery_lock_inode(void *ip)
{
  int old_idx = search_recovery_idx(RL_FLAG_INODE, ip);

  if(old_idx < 0 || RL_FLAG_MAX < old_idx)
    panic("Invalid recovery-locking flag");

  update_recovery_lock_idx(RL_FLAG_INODE, 0x0, 0x0);

  release_recovery_lock(RL_FLAG_ICACHE);
  release_recovery_lock(old_idx);
}

/*
 * R.C.S. checker
 */
int
check_and_count_procs_in_rcs(int flag, void *addr)
{
  if(flag < 0 || RL_FLAG_MAX < flag)
    panic("Invalid recovery-locking flag");

  if(RL_FLAG_BUF <= flag && addr != 0x0)
    flag = search_recovery_idx(flag, addr);

  return recovery_locks[flag].num;
}

static int
check_proc_waiting_rcs_icache(void *broken, int pid)
{
  struct proc *p1, *p2;

  for(p1 = proc; p1 < &proc[NPROC]; p1++){
    if(p1->state == SLEEPING && p1->chan == &recovery_locks[RL_FLAG_ICACHE]){
      for(p2 = proc; p2 < &proc[NPROC]; p2++){
        if(p2->state == SLEEPING){
          struct sleeplock *lk = (struct sleeplock*)p2->chan;
          if(lk->pid == p1->pid){
            return 1;
          }
        }
      }
    } else if(p1->state == SLEEPING){
      struct sleeplock *lk = (struct sleeplock*)p1->chan;
      if(lk->pid == pid){
        return 1;
      }
    }
  }
  return 0;
}

static int
check_proc_in_target_rcs(int pid, int flag)
{
  int i, idx = -1;

  for(i = 0; i < NPROC; i++){
    if(rcs_infos[i].pid == pid){
      idx = i;
      break; 
    }
  }

  if(idx < 0)
    panic_without_pr("check_proc_in_target_rcs: No corresponding R.C.S info history");

  for(i = 0; i < RCS_INFO_HISTORY_SIZE; i++){
    if(rcs_infos[idx].history_idx[i] == flag){
      return 1;
    }
  }

  return 0;
}

static int
check_waiting_procs_in_nmi_queue(int flag)
{
  int np = 0;  // Number of waiting proc in NMI Shepherding.

  if(nmi_queue == 0x0)
    panic_without_pr("check_waiting_procs_in_nmi_queue: The NMI Queue is not valid");

  for(int i = 0; i < NMI_QUEUE_SIZE; i++){
    if(nmi_queue[i].addr == 0x0)
      continue;
    if(check_proc_in_target_rcs(nmi_queue[i].pid, flag)){
      np++;
    }
  }
  return np;
}

int
check_and_wait_procs_in_rcs_file(void *f, void *ip)
{
  if(holding(&ftable->lock))
    release(&ftable->lock);

  if(ip != 0x0){
    if(check_and_count_procs_in_rcs(RL_FLAG_FILE, f) > 1 ||
       check_and_count_procs_in_rcs(RL_FLAG_INODE, ip) > 1){
      return FAIL_STOP;
    }
  } else {
    if(check_and_count_procs_in_rcs(RL_FLAG_FILE, f) > 1){
      return FAIL_STOP;
    }
  }

  // Check the NMI Queue if following recovery procs are in the ftable's R.C.S.
  // If any other procs aren't in the ftable's R.C.S or all procs are waiting for preceding recovery proc,
  // the preceding recovery process stops to wait for other procs.
  while(1){
    if(check_and_count_procs_in_rcs(RL_FLAG_FTABLE, 0x0) - check_waiting_procs_in_nmi_queue(RL_FLAG_FTABLE) == 0){
      break;
    }
  }

  return 0;
}

int
check_and_wait_procs_in_rcs_inode(void *ip, void *f, int pid)
{
  if(holding(&icache->lock))
    release(&icache->lock);

  if(check_and_count_procs_in_rcs(RL_FLAG_INODE, ip) > 1 || check_and_count_procs_in_rcs(RL_FLAG_FILE, f) > 1)
    return FAIL_STOP;

  // Same as case of file/ftable.
  if((check_proc_waiting_rcs_icache(ip, pid)) > 0){
      panic("Fail-Stop: broken inode can't be replaced safely.\n");
  }

  while(1){
    if(check_and_count_procs_in_rcs(RL_FLAG_ICACHE, 0x0) - check_waiting_procs_in_nmi_queue(RL_FLAG_ICACHE) == 0){
      break;
    } else if((check_proc_waiting_rcs_icache(ip, pid)) > 0){
      panic("Fail-Stop: broken inode can't be replaced safely.\n");
    }
  }

  return 0;
}

int
check_proc_in_log_commit(int pid)
{
  struct recovery_lock *rlk = &recovery_locks[RL_FLAG_LOG];

  if(rlk->exception == pid){
    acquire(&rlk->lk);
    rlk->exception = 0;
    release(&rlk->lk);
    return 0;  // No other processes are in log R.C.S, so no needs for identify recovery process was committing or not.
  }
  else if(rlk->num >= 0){
    return rlk->exception && LOG_COMMIT;
  }
  else {
    panic("Invalid Recovery-Locking process count");
  }
}
