// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "mlist.h"
#include "kalloc.h"
#include "proc.h"
#include "recovery_locking.h"


void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.
extern struct mlist_header mlist;

struct kmem _kmem;
struct kmem *kmem = &_kmem;

void
kinit()
{
  initlock(&kmem->lock, "kmem");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;
  
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP){
    panic("kfree");
  }

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  enter_recovery_critical_section(RL_FLAG_KMEM, 0);
  acquire(&kmem->lock);
  enter_trans_run(r);
  if(mlist.run_list != 0)
    register_memobj(r, mlist.run_list);

  r->next = kmem->freelist;
  kmem->freelist = r;

  exit_trans_run();
  release(&kmem->lock);
  exit_recovery_critical_section(RL_FLAG_KMEM, 0);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  enter_recovery_critical_section(RL_FLAG_KMEM, 0);

  acquire(&kmem->lock);

  r = kmem->freelist;
  if(r){
    kmem->freelist = r->next;
  }

  if(r && mlist.run_list != 0)
    delete_memobj(r, mlist.run_list, 0x0);
  release(&kmem->lock);
  exit_recovery_critical_section(RL_FLAG_KMEM, 0);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
