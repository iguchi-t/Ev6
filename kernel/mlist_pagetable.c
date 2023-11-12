/* Manage Pagetable M-List(ptb_mlist).
 */
#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "buf.h"
#include "mlist.h"
#include "kalloc.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"
#include "log.h"
#include "ptdup.h"

extern struct mlist_header mlist;
extern pagetable_t idx_ptdup[];  // Correspondence L2_pagetable address to index of ptdup_head.


// Register a pagetable to M-List (for all level).
void register_ptb_mlist(int pid, uint64 addr, int level){
  uint64 *page = mlist.ptb_list;

  acquire(&mlist.ptb_lock);

  for(int i = 0; i < ENTRY_SIZE; i++){
    if(i == ENTRY_SIZE-1){  // Reaching the M-List page's last entry.
      if(page[i] == (uint64)mlist.ptb_list){  // In the case of last M-List page.
        uint64 *newpage = (uint64*)kalloc();
        if(newpage == 0x0)
          panic("register_ptb_mlist: kalloc failed\n");
        newpage[i] = (uint64)mlist.ptb_list;
        page[i] = (uint64)newpage;
      }
      page = (uint64*)page[i];  // move to the next M-List page
      i = 0;
    }

    if(page[i] == 0x0 || page[i] == 0x0505050505050505){  // Find empty entry.
      page[i] = addr | PID2MLNODE(pid) | (uint64)level;  // Register target address, pid and level.
      break;
    }
  }

  release(&mlist.ptb_lock);
  return;
}


// Delete a pagetable address from the M-List.
void delete_ptb_mlist(uint64 addr){
  uint64* page = mlist.ptb_list;

  acquire(&mlist.ptb_lock);

  for(int i = 0; i < ENTRY_SIZE; i++){
    if(i == ENTRY_SIZE-1){
      if(page[i] == (uint64)mlist.ptb_list){
        // Sometimes delete_ptb_mlist & delete_ptb_mlist_all are overlapped. Manage this problem later.
        break;
      }
      page = (uint64*)page[i];
      i = 0;
    }
    if((page[i] & ~0xFFF) == addr){
      page[i] = 0x0;  // Fill 0 as deleting the address from the M-List.
      break;
    }
    
  }
  release(&mlist.ptb_lock);
  return;
}


// Delete all pagetables of pid's process from the M-List.
void delete_ptb_mlist_all(int pid){
  uint64 *page = mlist.ptb_list;

  acquire(&mlist.ptb_lock);
  
  for(int i = 0; i < ENTRY_SIZE; i++){
    if(MLNODE2PID(page[i]) == pid)
      page[i] = 0x0;
    else if(i == ENTRY_SIZE-1){
      if(page[i] == (uint64)mlist.ptb_list)
		    break;
      page = (uint64*)page[i];
      i = 0;
    }
  }

  release(&mlist.ptb_lock);
  return;
}