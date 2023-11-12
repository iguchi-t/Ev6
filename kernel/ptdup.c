#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "memlayout.h"
#include "riscv.h"
#include "proc.h"
#include "defs.h"
#include "ptdup.h"

#define PTDUP_SIZE NPROC+2  // NPROC + extra space
                            // The extra space is in the case of run recovery when kfree() in exec().
                            // The case, it is difficult to identify and delete only the old pagetable in exec() in current implementation.
                            // There may be some sophisticated way, but currently handle the case by this way.

struct ptdup_head ptdup_head[PTDUP_SIZE];  // headers to manage duplications of all pagetables.
pagetable_t idx_ptdup[PTDUP_SIZE];  // Correspondence L2_pagetable to index of ptdup_head.
                                    // Array's contents is L2_pagetable address of ptdup_head[index] manages.
struct spinlock idx_lock;  // spinlock for idx_ptdup.


// Create new user's pagetable duplications and initialize them.
// When this ptdup_init() is called, L2 pagetable must not hold any page directory.
void ptdup_init(pagetable_t L2_pagetable){
  int i, idx = -1;
  uint64 *ptds_page, *indv_page; 

  acquire(&idx_lock);
  for(i = 0; i < PTDUP_SIZE; i++){
    if(idx_ptdup[i] == 0){
      idx = i;
      idx_ptdup[i] = L2_pagetable;
      break;
    }  
  }
  release(&idx_lock);

  if(idx < 0)
    panic("ptdup_init: No empty index for new process");
  if(L2_pagetable == 0x0)
    panic("ptdup_init: NULL pagetable");

  initlock(&ptdup_head[idx].lock, "ptdup_lock");
  acquire(&ptdup_head[idx].lock);

  // Allocate new entry to ptdup_headers, it must correspond to L2_pagetable.
  ptdup_head[idx].l2 = (pagetable_t)kalloc();
  ptdup_head[idx].l2 = memset(ptdup_head[idx].l2, 0, PGSIZE);  
  ptdup_head[idx].l1 = (pagetable_t*)kalloc();
  ptdup_head[idx].l1 = memset(ptdup_head[idx].l1, 0, PGSIZE); 
  
  ptds_page = ptdup_head[idx].l0_ptds = (pagetable_t)kalloc();
  indv_page = ptdup_head[idx].l0_pted = (pagetable_t)kalloc(); 

  // Get ready for L0_pagetables pagetable data segment and other pages.
  // Set pointer to header to each page's last entries.
  ptds_page[ENTRY_SIZE-1] = (uint64)ptdup_head[idx].l0_ptds;
  indv_page[ENTRY_SIZE-1] = (uint64)ptdup_head[idx].l0_pted;
  release(&ptdup_head[idx].lock);

  return;
}


// Create a new pagetable duplication for L1.
// L2_pagetable duplication need not be updated before calling this function.
void ptdup_create_l1(pagetable_t L2_pagetable, pagetable_t L1_pagetable, uint64 pde_content){
  int i, idx = -1;

  acquire(&idx_lock);
  for(i = 0; i < PTDUP_SIZE; i++){
    if(idx_ptdup[i] == L2_pagetable){
      idx = i;
      break; 
    }
  }
  release(&idx_lock);

  if(idx < 0)
    panic("ptdup_create_l1: No index corresponting pagetable");
  else if(L1_pagetable == 0x0)
    panic("ptdup_create_l1: NULL pagetable");

  // Calc L1_pagetable's index in L2_pagetable.
  for(i = 0; i < ENTRY_SIZE; i++){
    if((uint64)L1_pagetable == PTE2PA(L2_pagetable[i])){
      acquire(&ptdup_head[idx].lock);
      ptdup_head[idx].l1[i] = (pagetable_t)kalloc();
      if(ptdup_head[idx].l1[i] == 0)
        panic("ptdup_create_l1: kalloc failed");
      
      ptdup_head[idx].l1[i] = memset(ptdup_head[idx].l1[i], 0, PGSIZE);
      ptdup_head[idx].l2[i] = pde_content;
      release(&ptdup_head[idx].lock);
      break;
    }
  }
  return;
}


// Delete all of old pagetable duplications.
void ptdup_delete_all(pagetable_t L2_pagetable, pagetable_t pagetable){
  int i, idx = -1;
  pagetable_t target = 0x0;
  uint64 *next;
 
  acquire(&idx_lock);
  for(i = 0; i < PTDUP_SIZE; i++){
    if(idx_ptdup[i] == L2_pagetable){
      idx = i;
      break;
    }
  }
  release(&idx_lock);

  if(idx < 0){
    printf("ptdup_delete_all: No corresponding PTDUP.");
    return;
  }

  // Search and Destroy target pagetable duplications.
  acquire(&ptdup_head[idx].lock);
  if(ptdup_head[idx].l1 != 0x0){
    for(i = 0; i < ENTRY_SIZE; i++){
      if(ptdup_head[idx].l1[i] != 0x0 && ptdup_head[idx].l1[i] != (pagetable_t)0x0505050505050505){
        kfree(ptdup_head[idx].l1[i]);
      }
    }
    kfree(ptdup_head[idx].l1);
  }

  if(ptdup_head[idx].l2 != 0x0){
    kfree(ptdup_head[idx].l2);
  }

  ptdup_head[idx].l2 = 0x0;
  ptdup_head[idx].l1 = 0x0;

  // Delete all PTDS pages.
  target = ptdup_head[idx].l0_ptds;
  if(target != 0x0){
    next = (uint64*)target[ENTRY_SIZE-1];
    do {
      kfree(target);
      target = next;
      next = (uint64*)target[ENTRY_SIZE-1];
    } while(target != ptdup_head[idx].l0_ptds);
  }
  ptdup_head[idx].l0_ptds = 0x0;

  // Delete all individual pte pages.
  target = ptdup_head[idx].l0_pted;
  if(target != 0x0){
    next = (uint64*)target[ENTRY_SIZE-1];
    do {
      kfree(target);
      target = next;
      next = (uint64*)target[ENTRY_SIZE-1];
    } while(target != ptdup_head[idx].l0_pted);
  }

  ptdup_head[idx].l0_pted = 0x0;
  acquire(&idx_lock);
  idx_ptdup[idx] = 0;
  release(&idx_lock);
  release(&ptdup_head[idx].lock);

  return;
}


// Update existing L1 pagetable duplication's single entry.
// L2 pagetable entry updating is written in create() and delete().
void ptdup_update(pagetable_t L2_pagetable, pagetable_t L1_pagetable, uint64 pde_content, int index){
  int i, idx = -1;
  pagetable_t target = 0x0;

  acquire(&idx_lock);
  for(i = 0; i < PTDUP_SIZE; i++){
    if(idx_ptdup[i] == L2_pagetable){
      idx = i;
      break;
    }
  }
  release(&idx_lock);

  if(idx < 0){
    panic("ptdup_update: No index corresponding L2_pagetable");
  }

  if(L1_pagetable == 0x0)
    panic("ptdup_update: NULL pagetable");
  
  // Search corresponding L1 duplication address.
  for(i = 0; i < ENTRY_SIZE; i++){
    if((uint64)L1_pagetable == PTE2PA(ptdup_head[idx].l2[i])){
      target = ptdup_head[idx].l1[i];
      break;
    }
  }
  if(target == 0x0)
    panic("ptdup_update: No pagetable in PTDUP");

  acquire(&ptdup_head[idx].lock);
  target[index] = pde_content;
  release(&ptdup_head[idx].lock);
  return;
}


// Add a new PTDS/PTED entry.
static void PTDS_PTED_add(int idx, uint64* header, uint64 content, int is_locked){
  for(uint64 *p = header; ; p = (uint64*)p[ENTRY_SIZE-1]){
    for(int i = 0; i < ENTRY_SIZE; i++){
      if(p[i] == 0x0 || p[i] == 0x0505050505050505){
        p[i] = content;
        if(!is_locked)
          release(&ptdup_head[idx].lock);
        return;
      }
      if(p[i] == (uint64)header){
        uint64* next = (uint64*)kalloc();
        if(next == 0x0)
          panic("PTDS_PTED_add: kalloc failed");
        next[i] = (uint64)header;
        p[i] = (uint64)next;
        p[0] = content;
        if(!is_locked)
          release(&ptdup_head[idx].lock);
        return;
      }
    }
  }
}


// Duplicate L0_pagetable's PTDS or PTED.
void L0_ptes_add(pagetable_t L2_pagetable, uint64 content, int flag){
  int i, idx = -1, is_locked = 1;
  uint64 *header;

  acquire(&idx_lock);
  for(i = 0; i < PTDUP_SIZE; i++){
    if(idx_ptdup[i] == L2_pagetable){
      idx = i;
      break;
    }
  }
  release(&idx_lock);

  if(idx < 0){
    panic("L0_ptes_add: No index corresponding L2_pagetable");
  }
  if(!holding(&ptdup_head[idx].lock)){
    acquire(&ptdup_head[idx].lock);
    is_locked = 0;
  }

  if(flag){  // Add a new PTDS entry.
    header = ptdup_head[idx].l0_ptds;
  } else {
    header = ptdup_head[idx].l0_pted;
  }
  if(header != 0x0)
    PTDS_PTED_add(idx, header, content, is_locked);
}


// Delete a PTDS area. Not necessary deleting hole PTDS, paritial is OK.
int PTDS_delete(pagetable_t L2_pagetable, uint64 *header, uint64 va_start, uint64 va_end){
  uint64 *dnode, *next, target_va_start, target_va_end, size, new = 0x0;
  int i, dsize = 0;

  for(dnode = header, next = (uint64*)dnode[ENTRY_SIZE-1]; ; dnode = next, next = (uint64*)dnode[ENTRY_SIZE-1]){
    for(i = 0; i < ENTRY_SIZE-1; i++){
      target_va_start = PTDS2VA(dnode[i]);
      size = PTDS2SIZE(dnode[i]);
      target_va_end = target_va_start + size * PGSIZE - 1;
      
      // The case of deleting area and PTDS area matched.
      if(va_start <= target_va_start && target_va_end <= va_end){
        dsize += PTDS2SIZE(dnode[i]);
        dnode[i] = 0x0;
      }

      // The case of deleting area back is shorter than PTDS area.
      else if(va_start == target_va_start && va_end < target_va_end){
        // Change start VA and size to truncate the beginning.
        new = VA2PTDS((va_end + 1)) | PPN2PTDS((PTDS2PPN(dnode[i]) - size * PGSIZE)) | UB2PTDS(PTDS2UB(dnode[i])) | (PTDS2SIZE(dnode[i]) - size);
        dnode[i] = new;
        dsize += PTDS2SIZE(dnode[i]);
      }

      // The case of deleting area's beginning is shorter than PTDS area.
      else if(target_va_start < va_start && target_va_end == va_end){
        // Update size to truncate the back.
        new = dnode[i] | ~(0x1ff);
        new |= (dnode[i] & 0x1ff) - size;
        dnode[i] = new;
        dsize += PTDS2SIZE(size);
      }

      // The case of deleting area is midde of PTDS area.
      else if(target_va_start < va_start && va_end < target_va_end){
        uint64 upper, lower;
        // Change start VA and size to truncate the beginning of "lower" PTDS.
        lower = VA2PTDS((va_end + 1)) | PPN2PTDS((PTDS2PPN(dnode[i]) - size * PGSIZE)) | UB2PTDS(PTDS2UB(dnode[i])) | (PTDS2SIZE(dnode[i]) - size);
        L0_ptes_add(L2_pagetable, lower, 1);
        // Change size to truncate the back of "upper" PTDS.
        upper = dnode[i] | ~(0x1ff);
        upper |= target_va_start - va_start;  // Update size.
        dnode[i] += size;
      }
    }
    if(next == header)
      break;
  }

  return dsize;
}


// Delete a PTE duplication.
int PTED_delete(uint64 *header, uint64 va_start, uint64 va_end){
  uint64 *dnode, *next;
  int i, dsize = 0;
 
  for(dnode = header, next = (uint64*)dnode[ENTRY_SIZE-1]; ; dnode = next, next = (uint64*)dnode[ENTRY_SIZE-1]){
    for(i = 0; i < ENTRY_SIZE-1; i++){
      if(va_start <= PTED2VA(dnode[i]) && PTED2VA(dnode[i]) < va_end){
        dnode[i] = 0x0505050505050505;
        dsize++;
      }
    }
    if(next == header)
      break;
  }
  return dsize;
}


// Delete L0_pagetable entries duplications.
void L0_ptes_delete(pagetable_t L2_pagetable, uint64 va_start, int sz){
  int i, idx = -1, size = sz + 1;
  uint64 va_end = va_start + size * PGSIZE;

  acquire(&idx_lock);
  for(i = 0; i < PTDUP_SIZE; i++){
    if(idx_ptdup[i] == L2_pagetable){
      idx = i;
      break;
    }
  }
  release(&idx_lock);

  // There is a case this function is called after PTDUP deletion.
  if(idx > 0){
    acquire(&ptdup_head[idx].lock);
    if(ptdup_head[idx].l0_pted != 0x0)
      size -= PTED_delete(ptdup_head[idx].l0_pted, va_start, va_end);
    if(ptdup_head[idx].l0_ptds != 0x0)
      size -= PTDS_delete(L2_pagetable, ptdup_head[idx].l0_ptds, va_start, va_end);
    release(&ptdup_head[idx].lock);
  }

  return;
}


// Decompose PTDS to a PTDS and some PTEDs, or some PTEDs (no PTDS).
static int PTDS_decompose_clear(pagetable_t L2_pagetable, int idx, uint64* page, int eidx, uint64 target_va){
  uint64 content, target_entry = page[eidx];
  uint64 start_va = PTDS2VA(target_entry), start_ppn = PTDS2PPN(target_entry), start_pa = PTE2PA(start_ppn);
  int ptds_size = PTDS2SIZE(target_entry);

  if(ptds_size == 2){  // Not combined.
    content = VA2PTED(start_va) | PPN2PTED(start_ppn) | PTE_W|PTE_X|PTE_R;
    PTDS_PTED_add(idx, ptdup_head[idx].l0_pted, content, 1);  // User bit cleared PTE.
    if(PTDS2ORDER(target_entry)){  // Continuous regions are ascending order.
      content = VA2PTED((start_va + PGSIZE)) | PPN2PTED(PA2PTE((start_pa + PGSIZE))) | PTE_W|PTE_X|PTE_R|PTDS2UB(target_entry);
    } else {
      content = VA2PTED((start_va + PGSIZE)) | PPN2PTED(PA2PTE((start_pa - PGSIZE))) | PTE_W|PTE_X|PTE_R|PTDS2UB(target_entry);
    }
    PTDS_PTED_add(idx, ptdup_head[idx].l0_pted, content, 1);  // User bit cleared PTE.
    PTDS_delete(L2_pagetable, ptdup_head[idx].l0_ptds, start_va, start_va + 2 * PGSIZE);
  }

  else if(ptds_size > 2){  // Combined with other upper entry.
    start_va = start_va + (ptds_size - 2) * PGSIZE;
    start_pa = start_pa + (ptds_size - 2) * PGSIZE;
    content = VA2PTED(start_va) | PPN2PTED(PA2PTE(start_pa)) | PTE_W|PTE_X|PTE_R;
    PTDS_PTED_add(idx, ptdup_head[idx].l0_pted, content, 1);  // User bit cleared PTE.
    if(PTDS2ORDER(target_entry)){
      content = VA2PTED((start_va + PGSIZE)) | PPN2PTED(PA2PTE((start_pa + PGSIZE))) | PTE_W|PTE_X|PTE_R|PTDS2UB(target_entry);
    } else {
      content = VA2PTED((start_va + PGSIZE)) | PPN2PTED(PA2PTE((start_pa - PGSIZE))) | PTE_W|PTE_X|PTE_R|PTDS2UB(target_entry);
    }
    PTDS_PTED_add(idx, ptdup_head[idx].l0_pted, content, 1);  // User bit cleared PTE.
    target_entry = target_entry & ~0xFF;  // Clear size.
    target_entry = target_entry | (ptds_size - 2);
  } else {
    printf("PTDS_decompose_clear: Invalid PTDS entry is passed.\n");
    return 1;
  }

  return 0;
}


// Clear PTE_U of PTDS or PTED flags to deal with uvmclear().
// If necessary, take a PTDS to two PTEDs.
void L0_ptes_clear_user(pagetable_t L2_pagetable, uint64 target_va){
  int i, idx = -1;
  uint64 va_start, va_end;
  uint64 target_end_va = target_va + PGSIZE;
  uint64 *header, *next, *p;

  acquire(&idx_lock);
  for(i = 0; i < PTDUP_SIZE; i++){
    if(idx_ptdup[i] == L2_pagetable){
      idx = i;
      break;
    }
  }
  release(&idx_lock);
  if(idx < 0){
    printf("L0_ptes_clear_user: No corresponding PTDUP.\n");
    return;
  }

  acquire(&ptdup_head[idx].lock);
  // Search PTDS list.
  if(ptdup_head[idx].l0_ptds != 0x0){
    header = ptdup_head[idx].l0_ptds;
    for(p = header, next = (uint64*)p[ENTRY_SIZE-1]; ; p = next, next = (uint64*)p[ENTRY_SIZE-1]){
      for(i = 0; i < ENTRY_SIZE-1; i++){
        va_start = PTDS2VA(p[i]);
        va_end = target_va + PGSIZE - 1;    
        if(va_start <= target_va && target_va <= va_end){
          if(PTDS_decompose_clear(L2_pagetable, idx, p, i, target_va) != 0)
            goto bad;
          release(&ptdup_head[idx].lock);
          return;
        }
      }
      if(next == header)
        break;
    }
  }
  
  // Search PTED list.
  if(ptdup_head[idx].l0_pted != 0x0){
    header = ptdup_head[idx].l0_pted;
    for(p = header, next = (uint64*)p[ENTRY_SIZE-1]; ; p = next, next = (uint64*)p[ENTRY_SIZE-1]){
      for(i = 0; i < ENTRY_SIZE-1; i++){
        if(target_va <= PTED2VA(p[i]) && PTED2VA(p[i]) <= target_end_va){
          p[i] = p[i] & ~PTE_U;  // Clear user bit.
          release(&ptdup_head[idx].lock);
          return;
        }
      }
      if(next == header)
        break;
    }
  }

bad:
  release(&ptdup_head[idx].lock);
  printf("L0_ptes_clear_user: Fail to clear User bit in PTDUP.\n");
}
