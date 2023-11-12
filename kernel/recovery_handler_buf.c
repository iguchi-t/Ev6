#include "param.h"
#include "types.h"
#include "stat.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "file.h"
#include "mlist.h"
#include "log.h"
#include "memlayout.h"
#include "virtio.h"
#include "nmi.h"
#include "proc.h"
#include "recovery_locking.h"
#include "after-treatment.h"


extern struct mlist_header mlist;
extern struct bcache bcache;
extern struct disk disk;
extern struct ftable *ftable;
extern struct icache *icache;
extern struct log *log;
extern struct logheader dup_lhdr;
extern int dup_outstanding;
extern void (*handler_for_mem_fault)(char*);  // Function which is called from NMI handler.
extern int recovery_mode;
extern int dup_outstanding;

extern uint64 bfree_start, bfree_end;
extern uint64 brelse_start, brelse_end;
extern uint64 commit_start, commit_end;
extern uint64 dirlink_start, dirlink_end;
extern uint64 end_op_start, end_op_end;
extern uint64 exit_start, exit_end;
extern uint64 fsinit_start, fsinit_end;
extern uint64 install_trans_start, install_trans_end;
extern uint64 iput_start, iput_end;
extern uint64 log_write_start, log_write_end;
extern uint64 readsb_start, readsb_end;
extern uint64 sys_chdir_start, sys_chdir_end;
extern uint64 sys_close_start, sys_close_end;
extern uint64 sys_link_start, sys_link_end;
extern uint64 sys_write_start, sys_write_end;
extern uint64 virtio_disk_intr_start, virtio_disk_intr_end;
extern uint64 write_head_start, write_head_end;
extern uint64 write_log_start, write_log_end;

extern void recover_from_log(void);

// Decide struct buf's After-Treatment policy.
static int
after_treatment_buf(uint64 *pcs, int pid, uint64 sp, uint64 s0)
{
  int ret = is_enable_user_coop(pid) ? SYSCALL_REDO : SYSCALL_FAIL;

  for(int i = 0; i < DEPTH; i++){
    if(ISINSIDE(pcs[i], exit_start, exit_end)){
      printf("end struct buf recovery: %d\n", get_ticks());
      return PROCESS_KILL;
    } else if(ISINSIDE(pcs[i], bfree_start, bfree_end) || ISINSIDE(pcs[i], dirlink_start, dirlink_end) ||
              ISINSIDE(pcs[i], iput_start, iput_end) || ISINSIDE(pcs[i], log_write_start, log_write_end) ||
              ISINSIDE(pcs[i], sys_link_start, sys_link_end) || ISINSIDE(pcs[i], sys_write_start, sys_write_end) ||
              ISINSIDE(pcs[i], write_log_start, write_log_end)){
      ret = SYSCALL_FAIL;
    } else if(ISINSIDE(pcs[i], virtio_disk_intr_start, virtio_disk_intr_end)){
      int trap = identify_nmi_occurred_trap(pid, sp, s0);

      if(trap == USERTRAP){
        printf("end struct buf recovery: %d\n", get_ticks());
        return RETURN_TO_USER;
      } else if(trap == KERNELTRAP){
        printf("end struct buf recovery: %d\n", get_ticks());
        return RETURN_TO_KERNEL;
      }
    } else if((recovery_mode == CONSERVATIVE && ISINSIDE(pcs[i], sys_close_start, sys_close_end))){ 
      printf("end struct buf recovery: %d\n", get_ticks());
      return SYSCALL_SUCCESS;
    } else if(ISINSIDE(pcs[i], install_trans_start, install_trans_end) || ISINSIDE(pcs[i], write_head_start, write_head_end)){
      if(is_enable_user_coop(pid)){
        ret = SYSCALL_SUCCESS;
      } else {
        printf("CAUTION: the specified file is opened, but the fd can't be returned due to ECC-uncorrectable Error occurrence.\n");
        ret = SYSCALL_FAIL;
      }
    }
  }

  printf("end struct buf recovery: %d, ret = %d\n", get_ticks(), ret);
  return ret;
}

// Solve inconsistencies between buf and log, inode,  or disk.
static void
solve_inconsistency_buf(uint64 pcs[], int pid, struct buf *broken)
{
  int nonum = 0, is_installing = 0, is_committing = 0;

  // Check virtio had already start processing with broken buf.
  if(!holding(&disk.vdisk_lock))
		acquire(&disk.vdisk_lock);

  for(int i = 0; i < NUM; i++){
    if(disk.info[i].b == broken){
      while((disk.used_idx % NUM) != (disk.used->id % NUM)){
        int id = disk.used->elems[disk.used_idx].id;

        if(disk.info[id].status != 0)
					panic("recovery_handler_buf: disk.info status");

        wakeup(disk.info[id].b);
        disk.used_idx = (disk.used_idx + 1) % NUM;
      }
    }
  }

  if(holding(&disk.vdisk_lock))
		release(&disk.vdisk_lock);

  if(log->committing){
    for(int i = 0; i < DEPTH; i++){
      if(ISINSIDE(pcs[i], install_trans_start, install_trans_end)){
        is_installing = 1;
        break;
      } else if (ISINSIDE(pcs[i], commit_start, commit_end)){  // this process was committing
        is_committing = 1;
        break;
      }
    }
    
    if(is_installing){
      printf("recovery_handler_buf: because of install_trans() is interrupted due to UE in the buf node, redo it.\n");
      recover_from_log();
    } else if (is_committing){
      printf("recovery_handler_buf: because of buf is broken, redo log commit.\n");
      commit();
    }
    if(!holding(&log->lock))
      acquire(&log->lock);
    log->committing = 0;
    wakeup(&log);
  } 
  
  // check losing buffered data to be written
  if(log->lh.n != 0){
		for(int i = 0; i < log->lh.n; i++){
			for(struct buf *b = bcache.head.next; b != &bcache.head; b = b->next){
				if(b->blockno == log->lh.block[i]){
					nonum = -1;
					break;
				} else {
          nonum = i;  // keep the index where blockno != block[i]
          break;
        }
      }

      // If lh.block[i] has no match with blockno in bcache,
      // consider the block[i] is broken buf's blockno.
      if(nonum != -1){
        // Evict broken blockno from log and slide others.
        for(int j = nonum; j <= log->lh.n-1; j++){
          log->lh.block[j] = log->lh.block[j+1];
          dup_lhdr.block[j] = dup_lhdr.block[j+1];
        }
        log->lh.block[log->lh.n] = 0;
        dup_lhdr.block[log->lh.n] = 0;
        nonum = -1;
        log->lh.n--;
        dup_lhdr.n--;

        wakeup(&log);
      }
    }
  }
  
  if(log->outstanding > 0){
    log->outstanding--;
    dup_outstanding--;
  }
  if(holding(&log->lock))
    release(&log->lock);
  wakeup(&log);

  for(struct inode *ip = &icache->inode[0]; ip < &icache->inode[NINODE]; ip++){
    if(ip->lock.pid == pid){
      releasesleep(&ip->lock);
    }
  }
  
  // Check the log transaction's status.
  check_and_handle_trans_log(pid);
  while(mycpu()->noff > 2)
    pop_off();  // Decrement the noff of broken buf's sleeplock's spinlock.
}

static int
check_fail_stop_cases(uint64 *pcs)
{
  int is_sys_chdir = 0, is_end_op = 0;

  for(int i = 0; i < DEPTH; i++){
    if(ISINSIDE(pcs[i], brelse_start, brelse_end) || ISINSIDE(pcs[i], readsb_start, readsb_end) ||
       ISINSIDE(pcs[i], fsinit_start, fsinit_end))
      return 1;
    else if(ISINSIDE(pcs[i], sys_chdir_start, sys_chdir_end))
      is_sys_chdir = 1;
    else if(ISINSIDE(pcs[i], end_op_start, end_op_end))
      is_end_op = 1;

    if(recovery_mode == CONSERVATIVE){
      if(ISINSIDE(pcs[i], bfree_start, bfree_end) || ISINSIDE(pcs[i], dirlink_start, dirlink_end) ||
         ISINSIDE(pcs[i], iput_start, iput_end) || ISINSIDE(pcs[i], log_write_start, log_write_end) ||
         ISINSIDE(pcs[i], sys_link_start, sys_link_end) || ISINSIDE(pcs[i], sys_write_start, sys_write_end) ||
         ISINSIDE(pcs[i], write_log_start, write_log_end)){
        return 1;
      }
    }
  }

  if(is_sys_chdir && is_end_op)
    return 1;

  return 0;
}

// struct buf's recovery handler.
int
recovery_handler_buf(void *address, int pid, uint64 sp, uint64 s0)
{
  printf("start struct buf recovery: %d, broken = %p\n", get_ticks(), address);
  acquire_recovery_lock_buf(address);

  // Prepare a new clean buf and some items.
  struct buf *new_buf = (struct buf*)my_kalloc(0, 0);
  struct buf *broken = (struct buf*)address;
  struct buf *prev = 0, *next = 0;
  uint64 pcs[DEPTH];

  // Search call stack & check Fail-Stop situation.
  if(s0 - sp > PGSIZE){
    struct proc *p = search_proc_from_pid(pid);
    getcallerpcs_bottom(pcs, sp, p->kstack, DEPTH);
  } else {
    getcallerpcs_top(pcs, sp, s0, DEPTH);
  }
  
  if(check_fail_stop_cases(pcs) || new_buf == 0){
    printf_without_pr("The context before the NMI meets Fail-Stop condition.\n");
    return FAIL_STOP;
  } else if(check_and_count_procs_in_rcs(RL_FLAG_BUF, broken) > 1){
    printf("recovery_handler_buf: Fail-Stop due to %d processes are in R.C.S. already.\n", check_and_count_procs_in_rcs(RL_FLAG_BUF, address));
    return FAIL_STOP;
  }

  memset(new_buf, 0, sizeof(struct buf));

  delete_memobj(broken, mlist.buf_list, 0x0);
  register_memobj(new_buf, mlist.buf_list);

  /*
   * Internal-Surgery
   */
  // Search the broken buf node's place including bcache.head.
  if(!holding(&bcache.lock))
  	acquire(&bcache.lock);
  if(bcache.head.next == broken)
		prev = &bcache.head;
  if(bcache.head.prev == broken)
		next = &bcache.head;
  for(int i = 0; i < NBUF; i++){
    if(bcache.buf[i].next == broken)
	  	prev = &bcache.buf[i];
    if(bcache.buf[i].prev == broken)
	  	next = &bcache.buf[i];
  }
  
  // Modify prev->next and next->prev to delete the broken from list.
  prev->next = next;
  next->prev = prev;
  release(&bcache.lock);

  // Initialize the new buf node.
  new_buf->valid = 0;
  new_buf->disk = 0;
  new_buf->dev = 0;
  new_buf->refcnt = 0;
  new_buf->qnext = 0;
  new_buf->blockno = 0;
  recovery_handler_sleeplock("bcache", &new_buf->lock, broken, sizeof(struct buf));

  // Insert new buf node to bcache list.
	acquire(&bcache.lock);
  new_buf->next = &bcache.head;
  new_buf->prev = bcache.head.prev;
  bcache.head.prev->next = new_buf;
  bcache.head.prev = new_buf;
	release(&bcache.lock);

  // Release related buf node's locks.
  acquire(&bcache.lock);
  for(struct buf *b = bcache.head.next; b != &bcache.head; b = b->next){
    if(b->lock.pid == pid){
      if(b->lock.lk.locked){
        release(&b->lock.lk);
      }
      releasesleep(&b->lock);
    }
  }
  release(&bcache.lock);
  
  /*
   * Solve-Inconsistency
   */
  update_recovery_lock_idx(RL_FLAG_BUF, broken, new_buf);
  release_recovery_lock_buf(new_buf);
  solve_inconsistency_buf(pcs, pid, broken);

  exit_rcs_after_recovery(pid, 0);

  /*
   * After-Treatment
   */
  return after_treatment_buf(pcs, pid, sp, s0);
}
