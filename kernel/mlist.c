/* Initialize all header nodes of "struct mlist_header".
 * To initialize them, use headerinit() function.
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
#include "nmi.h"
#include "console.h"
#include "printf.h"
#include "recovery_locking.h"
#include "trans.h"
#include "usercoop.h"

#define AL_ARRAY_SIZE (PGSIZE / sizeof(struct mlist_node))
#define EMPTY 0x101010101010101  // Indicate empty addrlist's page entry.

struct mlist_header mlist;
struct logheader dup_lhdr;  // For duplicate logheader for recovering struct log/logheader.
int dup_outstanding = 0;
int dup_committing = 0;

extern char end[];     // first address after kernel loaded from ELF file
                       // defined by the kernel linker script in kernel.ld
extern int recovery_mode;
extern struct cons *cons;
extern struct devsw *devsw;
extern struct kmem *kmem;
extern struct pr *pr;
extern struct recovered_addr_node recovered_list[];
extern struct spinlock idx_lock;
extern struct spinlock nmi_queue_lock;
extern struct spinlock *tickslock;


void init_run_list(void);  // Set up run's address list.
void register_memobj(void*, struct mlist_node*);
void delete_memobj(void*, struct mlist_node*, uint64);
char* my_kalloc(void*, int);
void getcallerpcs_top(uint64*, uint64, uint64, int);
void getcallerpcs_bottom(uint64*, uint64, uint64, int);


void mlistinit(void){
  char *message;

  // FS
  struct mlist_node *buf_head = (struct mlist_node*)kalloc();
  struct mlist_node *fil_head = (struct mlist_node*)kalloc();
  struct mlist_node *ino_head = (struct mlist_node*)kalloc();
  struct mlist_node *log_head = (struct mlist_node*)kalloc();
  struct mlist_node *lhd_head = (struct mlist_node*)kalloc();
  struct mlist_node *pip_head = (struct mlist_node*)kalloc();
  struct mlist_node *slp_head = (struct mlist_node*)kalloc();
  struct mlist_node *spn_head = (struct mlist_node*)kalloc();
  // Console
  struct mlist_node *con_head = (struct mlist_node*)kalloc();
  struct mlist_node *dev_head = (struct mlist_node*)kalloc();
  struct mlist_node *pr_head  = (struct mlist_node*)kalloc();
  // Memory Allocation
  struct mlist_node *kmm_head = (struct mlist_node*)kalloc();
  struct mlist_node *run_head = (struct mlist_node*)kalloc();
  pagetable_t ptb_head = (pagetable_t)kalloc();

  // FS
  buf_head->next = mlist.buf_list = buf_head;
  fil_head->next = mlist.fil_list = fil_head;
  ino_head->next = mlist.ino_list = ino_head;
  log_head->next = mlist.log_list = log_head;
  lhd_head->next = mlist.lhd_list = lhd_head;
  pip_head->next = mlist.pip_list = pip_head;
  slp_head->next = mlist.slp_list = slp_head;
  spn_head->next = mlist.spn_list = spn_head;
  // Console
  con_head->next = mlist.con_list = con_head;
  dev_head->next = mlist.dev_list = dev_head;
  pr_head->next  = mlist.pr_list  = pr_head;
  // Memory Allocation
  kmm_head->next = mlist.kmm_list = kmm_head;
  run_head->next = mlist.run_list = run_head;
  mlist.ptb_list = ptb_head;
  ptb_head[511]  = (uint64)mlist.ptb_list;

  initlock(&mlist.giant_lock, "M-List");
  initlock(&mlist.ptb_lock, "M-List(page table)");
  initlock(&idx_lock, "idx_ptdup");
  initlock(&nmi_queue_lock, "NMI_Queue");

  // Register memobj which is defined before allheadersinit().
  register_memobj(kmem, mlist.kmm_list);
  register_memobj(&kmem->lock, mlist.spn_list);
  init_run_list();
  register_memobj(devsw, mlist.dev_list);
  register_memobj(cons, mlist.con_list);
  register_memobj(&cons->lock, mlist.spn_list);
  register_memobj(pr, mlist.pr_list);
  register_memobj(&pr->lock, mlist.spn_list);

  for(int i = 0; i < 5; i++){
    recovered_list[i].start = (void*)0x0;
    recovered_list[i].end   = (void*)0x0;
  }
  init_recovery_lock_idx();

  if(recovery_mode == AGGRESSIVE)
    message = "Current recovery mode is 'Aggressive'.\n";
  else
    message = "Current recovery mode is 'Conservative'.\n";
  printf(message);

  return;
}


// Set up run's address list.
void init_run_list(void){
  struct mlist_node *rnode, *brnode = mlist.run_list, *nrnode;
  struct run *r;
  int i;
  
  rnode = brnode + 1;  // "++" can't use here.
  for(i = 1, r = kmem->freelist; r->next != 0; r = r->next, rnode++, i++, brnode = brnode->next){
    // When we reach last node of this page, 
    // use the node as next page's address keeper for registering.
    if(i == AL_ARRAY_SIZE-1){
      nrnode = (struct mlist_node*)kalloc();
      nrnode->addr = (void*)0x1010101;
      rnode->addr = (void*)0;  // addr == 0 means that this node is the last node.
      rnode->next = nrnode;
      rnode = nrnode;
      i = 0;  // Reset address list node's counter.
    }
    rnode->addr = (void*)r;
    rnode->next = brnode->next;
    brnode->next = rnode;
  }
  return;
}


void register_memobj(void *address, struct mlist_node *header){
  // Note that header must not be NULL (0x0).
  struct mlist_node *rnode, *next_page;
  // Register address page at first.
  // Check for empty entries.
  
  acquire(&mlist.giant_lock);
  for(rnode = header + 1;; rnode++){
    if(rnode->addr == (void*)0x0)
      rnode = rnode->next;
    // In the case of reaching the last entry of this page.
    if(((uint64)rnode & 0xfff) == 0xff0){
      next_page = (struct mlist_node*)my_kalloc(address, 0);
      next_page->addr = (void*)EMPTY;
      rnode->addr = (void*)0x0;  // addr == 0x0 means this node is the last entry.
      rnode->next = next_page;
      rnode = next_page;
      break;
    }
    else if(rnode->addr == (void*)EMPTY || rnode->addr == (void*)0x0505050505050505)
      break;
    else if(rnode->addr == address){  // Already exists.
      release(&mlist.giant_lock);
      return;
    }
  }

  // Next, register the address to M-List.
  if(rnode->addr == (void*)EMPTY || rnode->addr == (void*)0x0505050505050505){
    rnode->addr  = address;
    rnode->next  = header->next;
    header->next = rnode;
    release(&mlist.giant_lock);
    return;
  }
  release(&mlist.giant_lock);
  printf("register_memobj: fail to register %p\n", address);
  return;
}


/* Just delete the address from mlist and address page.
 * We don't free all empty address page in this function.
 * If mlist.giant_lock is already locked by register_memobj(),
 * we don't lock & release mlist.giant_lock in this function.
 */
void delete_memobj(void *address, struct mlist_node *header, uint64 size){
  // Note that header must not be 0x0.
  struct mlist_node *dnode, *bdnode = header;
  int is_need_release = 0;

  if(!holding(&mlist.giant_lock)){
    acquire(&mlist.giant_lock);
    is_need_release = 1;
  }

  // Given address equals to the broken address.
  if(size == 0){
    for(; bdnode->next != header; bdnode = bdnode->next){
      if(bdnode->next->addr == address){
        dnode = bdnode->next;
        bdnode->next = dnode->next;
        dnode->addr = (void*)EMPTY;
        dnode->next = (void*)EMPTY;
        if(holding(&mlist.giant_lock) && is_need_release)
          release(&mlist.giant_lock);
        return;
      }
    }
  }

  // Given address includes the broken address.
  else if(size > 0){
    for(; bdnode->next != header; bdnode = bdnode->next){
      if(address <= bdnode->next->addr && bdnode->next->addr <= (void*)((uint64)address + size)){
        dnode = bdnode->next;
        bdnode->next = dnode->next;
        dnode->addr = (void*)EMPTY;
        dnode->next = (void*)EMPTY;
        if(holding(&mlist.giant_lock) && is_need_release)
          release(&mlist.giant_lock);
        return;
      }
    }
  }
  if(holding(&mlist.giant_lock) && is_need_release)
    release(&mlist.giant_lock);
  return;
}


// A function to allocate kernel page without using normal kalloc() 
// to avoid allocating deleting page in kfree() by calling register_memobj().
// I'll add the function of multiple (more than 3 pages) page allocation soon.
char* my_kalloc(void *avoid_addr, int multi){
  struct run *r, *r1, *br;
  int i;
  
  acquire(&kmem->lock);
  // When we need to allocate continuous more than one page.
  if(multi > 1){
    struct run* pages[multi];
    for(br = kmem->freelist, r = br->next, r1 = r; r != kmem->freelist || r != (struct run*)0x0; br = br->next, r = br->next, r1 = r){
      for(i = multi; i > 0; i--){
        if((uint64)r1 - PGSIZE == (uint64)r1->next){
          pages[i-1] = r1;
          r1 = r1->next;
        }
        else
          break;
      }
      if(i == 0){  // If found all pages
        br->next = pages[0]->next;
        r = pages[0];
        break;
      } else {  // If NOT found all pages
        br = r1->next;
        i = multi-1;
      }
      if(r == (struct run*)0x0)
        return 0;
    }
  }
  // Or only one page is needed.
  else {
    r = kmem->freelist;
    if((void*)r == avoid_addr){  // If r shouldn't allocate, reallocate.
      r = kmem->freelist->next;
      if(r)
        kmem->freelist->next = r->next;
    } else if((void*)r != avoid_addr || avoid_addr == 0)
    if(r)
      kmem->freelist = r->next;
  }

  if(holding(&kmem->lock))
    release(&kmem->lock);
  if(r && mlist.run_list != 0)
    delete_memobj((void*)r, mlist.run_list, 0x0);
  return (char*)r;
}


#define FILTER  0xFFFFFFFFFFFFF000  // filter for extracting upper 13 bits.
#define NOVALUE 0x0505050505050505  // Not meaningful value.

// To check call stack(kstack) from top of stack and extract return addresses
// to identify caller functions.
void getcallerpcs_top(uint64 *pcs, uint64 sp, uint64 s0, int size){
  uint64 m_sp = sp, m_s0 = s0, depth, *stack, endline;
  int i = 0;

  if(sp == 0x0 || s0 - sp > PGSIZE){
    panic("getcallerpcs_top: Invalid sp/s0 was passed");
  }
  endline = (sp & FILTER) + 0x1000;

  stack = (uint64*)sp;
  while(m_s0 < endline && i < size){
    depth  = (m_s0 - m_sp) / 8;
    m_sp   = m_s0;            // Save next stack's top address.
    m_s0   = stack[depth-2];  // Pick up s0 from stack's second from the bottom.
    pcs[i] = stack[depth-1];  // Pick up ra from stack's first from the bottom.
    i++;

    if(m_sp == endline || m_s0 == endline)
      break;
    stack = (uint64*)m_sp;  // Update stack to see next stacked contents.
  }

  for(; i < size; i++)
    pcs[i] = 0;

  // To confirm the contents of pcs[] (for debug).
  for(i = 0; i < size && pcs[i] != 0x0; i++)
    printf("getcallerpcs_bottom: pcs[%d] = %p\n", i, pcs[i]);
  return;
}


#define TEXT_TOP 0x80000000  // The top of section .text.
#define TEXT_END 0x8000a10e  // The end of section .text.

/* To check call stack (kstack) from bottom of stack and extract return addresses
 * to identify caller functions.
 * When xv6 calls kernelvec(), getcallerpcs_top() can't track under a chunk of contents because of trap,
 * so search stack from bottom.
 */ 
void getcallerpcs_bottom(uint64 *pcs, uint64 endline, uint64 bottom, int size){
  int i = 0;
  uint64 tmp[DEPTH];
  uint64 *stack = (uint64*)(bottom + 0xff8);

  if(endline == 0x0)
    panic("getcallerpcs_bottom: Invalid sp was passed");
  for(; stack > (uint64*)endline; stack -= 0x1){  // 0x1 means 1 byte
    if(TEXT_TOP < *stack && *stack < TEXT_END){
      tmp[i] = *stack;
      i++;
    }
    if(i > size)
      break;
  }
  for(; i < size; i++)
    tmp[i] = 0;

  // Reorder pcs from "bottom to top" to "top to bottom".
  for(i = 0; i < size; i++){
    pcs[i] = tmp[DEPTH-1-i];
  }

  // To confirm the contents of pcs[] (for debug).
  for(i = 0; i < size && pcs[i] != 0x0; i++)
    printf("getcallerpcs_bottom: pcs[%d] = %p\n", i, pcs[i]);
  return;
}

// Get the ticks now.
uint64 get_ticks(void){
  uint64 nticks = -1;
  
  if(!holding(tickslock)) {
    acquire(tickslock);
  }

  nticks = ticks;

  if(holding(tickslock)) {
    release(tickslock);
  }

  return nticks;
}