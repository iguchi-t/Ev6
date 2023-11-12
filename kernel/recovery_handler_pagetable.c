#include "param.h"
#include "types.h"
#include "stat.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "mlist.h"
#include "memlayout.h"
#include "ptdup.h"
#include "log.h"
#include "nmi.h"
#include "after-treatment.h"

#define ENTRY_SIZE 512

extern struct cons cons;
extern struct log *log;
extern struct ptdup_head ptdup_head[];
extern pagetable_t idx_ptdup[];
extern struct proc *proc;
extern int recovery_mode;
extern int dup_outstanding;

extern uint64 begin_op_start, begin_op_end;
extern uint64 clockintr_start, clockintr_end;
extern uint64 consoleintr_start, consoleintr_end;
extern uint64 freeproc_start, freeproc_end;
extern uint64 kerneltrap_start, kerneltrap_end;
extern uint64 kernelvec_start, kernelvec_end;
extern uint64 kvminit_start, kvminit_end;
extern uint64 nmivec_start, nmivec_end;
extern uint64 pipewrite_start, pipewrite_end;
extern uint64 sys_chdir_start, sys_chdir_end;
extern uint64 sys_write_start, sys_write_end;
extern uint64 uvmalloc_start, uvmalloc_end;
extern uint64 uvmcopy_start, uvmcopy_end;
extern uint64 uvmunmap_start, uvmunmap_end;
extern uint64 virtio_disk_intr_start, virtio_disk_intr_end;
extern uint64 walk_start, walk_end;
extern uint64 writei_start, writei_end;

// Search call stack & decide termination of this recovery handler
static int
check_all_fail_stop_cases(uint64 *pcs, int ret)
{
  for(int i = 0; i < DEPTH; i++){
    if(ISINSIDE(pcs[i], freeproc_start, freeproc_end) || ISINSIDE(pcs[i], uvmunmap_start, uvmunmap_end)){
      return FAIL_STOP;
    } else if(recovery_mode == CONSERVATIVE && (ISINSIDE(pcs[i], uvmalloc_start, uvmalloc_end) ||
              ISINSIDE(pcs[i], uvmcopy_start, uvmcopy_end) || ISINSIDE(pcs[i], sys_write_start, sys_write_end))){
      return FAIL_STOP;
    } else if(ISINSIDE(pcs[i], uvmalloc_start, uvmalloc_end)){
      return PROCESS_KILL;
    }
  }

  return ret;
}

static int
after_treatment_user_pagetable(uint64* pcs, int pid, uint64 sp, uint64 s0, int ret)
{ 
  if(ret == FAIL_STOP)
    return ret;

  int trap = identify_nmi_occurred_trap(pid, sp, s0);

  for(int i = 0; i < DEPTH; i++){
    if(ISINSIDE(pcs[i], consoleintr_start, consoleintr_end) || ISINSIDE(pcs[i], clockintr_start, clockintr_end)){
      if(trap == USERTRAP){
        ret = PROCESS_KILL;
        break;
      } else if(trap == KERNELTRAP && ret >= SYSCALL_FAIL){
        ret = RETURN_TO_KERNEL;
        break;
      }
    } else if(ISINSIDE(pcs[i], virtio_disk_intr_start, virtio_disk_intr_end)){
      if(trap == USERTRAP){
        ret = PROCESS_KILL;
        break;
      } else if(trap == KERNELTRAP){
        ret = FAIL_STOP;
        break;
      }
    } else if(ISINSIDE(pcs[i], kernelvec_start, kernelvec_end) || ISINSIDE(pcs[i], nmivec_start, nmivec_end)){  // In the case of which the NMI is occurred in kerneltrap().
      if(ISINSIDE(pcs[i+1], kerneltrap_start, kerneltrap_end)){
        ret = FAIL_STOP;
        break;
      }
    }
  }

  printf("end pagetable recovery: %d\n", get_ticks());
  return ret;
}



// Recovery L2_pagetable by using PTDUP.
static int
recovery_L2(int idx, void *address, pagetable_t new, struct proc *bp)
{
  int res;

  memmove(new, ptdup_head[idx].l2, PGSIZE);  // Copy L2_pagetable duplication's contents.

  if(*new == 0x0){
    printf("recovery_handler_pagetable: memmove() failed in recovering L2_pagetable(%d:%p)\n", bp->pid, address);
    res = -1;
  }

  bp->pagetable = new;  // Switching old pointer to new one.
  register_ptb_mlist(bp->pid, (uint64)new, 2);  // Register new pagetable to M-List.

  for(int i = 0; i <= NPROC; i++){
    if(idx_ptdup[i] == (pagetable_t)address){
      idx_ptdup[i] = new;  // Update pagetable duplication index.
    }
  }

  return res;
}


// Recovery L1_pagetable by using PTDUP.
static int
recovery_L1(int idx, void *address, pagetable_t new, struct proc *bp)
{
  int i;
  int ret = (is_enable_user_coop(bp->pid)) ? SYSCALL_REDO : SYSCALL_FAIL;
  pagetable_t target = 0x0;
  pte_t *pte = 0x0;

  for(i = 0; i < ENTRY_SIZE; i++){
    // Identify broken L1_pagetable's entry in L2.
    if((uint64)address == PTE2PA(ptdup_head[idx].l2[i])){
      target = ptdup_head[idx].l1[i];
      if(ptdup_head[idx].l1[i] == 0x0){
        return PROCESS_KILL;
      }
      break;
    }
  }

  if(target == 0x0){
    printf("recovery_handler_pagetable: L1_pagetable duplication does not setup or can't find corresponding PTDUP.\n");
    if(recovery_mode == CONSERVATIVE)
      ret = FAIL_STOP;  // Avoid memory leak;
    else
      ret = PROCESS_KILL;  // terminate this proc by exit();
    return ret;
  }

  memmove(new, target, PGSIZE);  // Copy L1_pagetable duplication's contents.

  if(*new == 0x0 && *target != 0x0){
    printf("recovery_handler_pagetable: Failed in recovering L1_pagetable(%d:%p)\n", bp->pid, address);

    if(recovery_mode == CONSERVATIVE)
      ret = FAIL_STOP;  // Avoid memory leak;
    else
      ret = PROCESS_KILL;  // terminate this proc by exit();
    return ret;
  }

  // Find corresponding L2_pagetable entry to old L1_pagetable.
  pte = &bp->pagetable[i];
  *pte = PA2PTE(new) | PTE_V;
  pte = &ptdup_head[idx].l2[i];
  *pte = PA2PTE(new) | PTE_V;
  register_ptb_mlist(bp->pid, (uint64)new, 1);

  return ret;
}


// Reconstruct L0 pagetable from PageTable Direct Segment(PTDS)
static int
reconst_from_PTDS(int idx, pagetable_t new, uint64 sva)
{
  int num, res = 0, size;
  uint64 start_va, eva = sva + 0x200000;
  uint64 *p, *header = ptdup_head[idx].l0_ptds, ppn;

  for(p = header; ; p = (uint64*)p[ENTRY_SIZE-1]){
    for(int i = 0; i < ENTRY_SIZE-1; i++){
      if(p[i] == 0x0 || p[i] == 0x0505050505050505)
        continue;

      start_va = PTDS2VA(p[i]);

      if(sva <= start_va && start_va <= eva){
        // Reconstruct L0_pagetable's entries.
        ppn = PTDS2PPN(p[i]);
        num = PX(0, PTDS2VA(p[i]));
        size = PTDS2SIZE(p[i]);
        
        if(PTDS2ORDER(p[i])){
          for(int j = 0; j < size; j++, num++, ppn += 0x400){
            if(new[num]){
              printf("reconst_from_PTDS: Not an empty entry (initializing is failed): %d, %p\n", num, new[num]);
              res = -1;
            } else {
              // Restore PPN, User, eXecutable, Writable, Readable and Valid flags, but other flags don't.
              // Because these four flags is common among User pages.
              new[num] = ppn | PTDS2UB(p[i]) | PTE_D | PTE_X | PTE_W | PTE_R | PTE_V;
            }
          }
        } else if(!PTDS2ORDER(p[i])){
          for(int j = 0; j < size; j++, num++, ppn -= 0x400){
            if(new[num]){
              printf("reconst_from_PTDS: Not an empty entry (initializing is failed): %d, %p\n", num, new[num]);
              res = -1;
            } else {
              new[num] = ppn | PTDS2UB(p[i]) | PTE_D | PTE_X | PTE_W | PTE_R | PTE_V;
            }
          }
        }
      }
    }

    if((uint64*)p[ENTRY_SIZE-1] == header)
      break;
  }

  return res;
}


// Reconstruct L0 pagetable from PageTable Entry Duplication(PTED).
static int
reconst_from_PTED(int idx, pagetable_t new, uint64 sva)
{
  uint64 va, eva = sva + 0x200000;
  uint64 *p, *header = ptdup_head[idx].l0_pted;
  int res = 0;

  for(p = header; ; p = (uint64*)p[ENTRY_SIZE-1]){
    for(int i = 0; i < ENTRY_SIZE-1; i++){
      if(p[i] == 0x0 || p[i] == 0x0505050505050505)
        continue;

      va = PTED2VA(p[i]);

      if(sva <= va && va < eva){
        if(new[PX(0, va)]){
          printf("reconst_from_PTED: Not Empty entry (Overwriting): %d, %p, %p, %d\n", PX(0, va), new[PX(0, va)], p[i], i);
          res = -1;
        } else { // Reconstruct L0_pagetable's entries.
          new[PX(0, va)] = PTED2PPN(p[i]) | PTE_D | PTED2FLAGS(p[i]) | PTE_V;
        }
      }
    }

    if((uint64*)p[ENTRY_SIZE-1] == header)
      break;
  }

  return res;
}


// Recovery L0_pagetable by using PTDS & PTED.
static int
recovery_L0(int idx, void *address, pagetable_t new, struct proc *bp)
{
  int i, j = 0;
  int ret = (is_enable_user_coop(bp->pid)) ? SYSCALL_REDO : SYSCALL_FAIL;
  pagetable_t L1_pagetable;
  pte_t *pte = 0x0;
  uint64 va = 0x0;

  for(i = 0; i < ENTRY_SIZE; i++){
    if(ptdup_head[idx].l1[i] == 0x0)
      continue;

    for(j = 0; j < ENTRY_SIZE; j++){
      // Identify broken L0_pagetable's entry in L1.
      if((uint64)address == PTE2PA(ptdup_head[idx].l1[i][j])){
        break;
      }
    }

    if(j < ENTRY_SIZE){
      break;
    }
  }
  if(i == ENTRY_SIZE && j == ENTRY_SIZE)
    panic("Fail-Stop due to ECC-uncorrectable Error occurred on unregistered L0_pagetable to PTDUP");

  // Reconstruct the broken L0_pagetable from PTDS and PTED.
  va = (uint64)((i << 30) | (j << 21));

  if(reconst_from_PTDS(idx, new, va) || reconst_from_PTED(idx, new, va)){
    printf("recovery_L0: reconstruction L0_pagetable is failed (pid: %d, addr: %p).\n", bp->pid, address);
    return PROCESS_KILL;
  }

  // Find corresponding L1_pagetable entry to old L0_pagetable.
  L1_pagetable = (pagetable_t)PTE2PA(bp->pagetable[i]);
  pte = (pte_t*)&L1_pagetable[j];
  *pte = PA2PTE(new) | PTE_V;  // Update L1_pagetable's entry.

  // Update L1_pagetable duplication's entry.
  L1_pagetable = ptdup_head[idx].l1[i];
  pte = &L1_pagetable[j];
  *pte = PA2PTE(new) | PTE_V;
  register_ptb_mlist(bp->pid, (uint64)new, 0);  // Register new pagetable to M-List.

  return ret;
}


// Recovery handler of all layers page tables.
// When this function is called, already identified which process's pagetable was broken.
int
recovery_handler_pagetable(int level, int idx, struct proc *bp, void *address, uint64 sp, uint64 s0)
{
  printf("start pagetable recovery: %d, p = %p, broken = %p\n", get_ticks(), bp, address);

  int ret = (is_enable_user_coop(bp->pid)) ? SYSCALL_REDO : SYSCALL_FAIL;
  int pid = bp->pid;
  pagetable_t new = (pagetable_t)kalloc();
  uint64 pcs[DEPTH];

  // Search call stack & check Fail-Stop situation.
  if(sp == 0x0 || s0 - sp > PGSIZE)  // In the case of invalid s0 or sp is passed.
    getcallerpcs_bottom(pcs, sp, bp->kstack, DEPTH);
  else
    getcallerpcs_top(pcs, sp, s0, DEPTH);

  if((ret = check_all_fail_stop_cases(pcs, ret)) == FAIL_STOP)
    goto ret;

  if(ptdup_head[idx].l2 == 0x0){  // PTDTP is not ready.
    kfree(new);
    new = 0x0;

    if(recovery_mode == CONSERVATIVE)
      ret = FAIL_STOP;  // Avoid memory leak.
    else
      ret = PROCESS_KILL;  // terminate this proc by exit();
    printf("recovery_handler_pagetable: page table duplication was not ready\n");
    goto ret;
  }

  if(new == 0x0)
    panic("recovery_handler_pagetable: kalloc failed");

  new = memset(new, 0, PGSIZE);  // Fill allocated page with 0.

  // Reconstruct & switching broken pagetable to new.
  switch(level){
    case 2:
      recovery_L2(idx, address, new, bp);
      break;
    case 1:
      ret = recovery_L1(idx, address, new, bp);
      if(ret != SYSCALL_FAIL)
        goto ret;
      break;
    case 0:
      ret = recovery_L0(idx, address, new, bp);
      if(ret != SYSCALL_FAIL)
        goto ret;
      break;
    default:
      printf("recovery_handler_pagetable: Invalid pagetable level(%d) was passed (pid: %d, addr :%p).\n", level, bp->pid, address);
      ret = -1;
      break;
  }


ret:
  // Check transactions status.
  if(check_and_handle_trans_pagetable(pid) && ret == SYSCALL_FAIL)
    ret = PROCESS_KILL;

  delete_ptb_mlist((uint64)address);
  printf("pagetable recovery is completed (new = %p), ret = %d\n", new, ret);

  if(log->committing && !holding(&log->lock)){  // Check commit().
    commit();
    acquire(&log->lock);
    log->committing = 0;
    wakeup(&log);
    release(&log->lock);
  } else if(holding(&log->lock) && log->outstanding > 0){
    log->outstanding--;
    dup_outstanding--; 
  }

  if(holding(&bp->lock))
    release(&bp->lock);  // In the case of recovering process is locked

  check_all_locks(myproc()->pid);

  exit_rcs_after_recovery(pid, 0);
  return after_treatment_user_pagetable(pcs, pid, sp, s0, ret);
}
