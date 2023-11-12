#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "buf.h"
#include "mlist.h"
#include "kalloc.h"
#include "memlayout.h"
#include "log.h"
#include "proc.h"
#include "ptdup.h"
#include "virtio.h"
#include "nmi.h"
#include "printf.h"
#include "console.h"
#include "pipe.h"
#include "recovery_locking.h"
#include "after-treatment.h"


extern int dup_outstanding;
extern pagetable_t idx_ptdup[];  // Correspondence L2_pagetable to index of ptdup_head (from pt_dup.c)
extern pagetable_t idx_mlist[];  // Correspondence L2_pagetable to index of ptdup_head.
extern pagetable_t kernel_pagetable;
extern struct mlist_header mlist;
extern struct proc proc[];
extern struct kmem *kmem;
extern struct bcache bcache;
extern struct icache *icache;
extern struct ftable *ftable;
extern struct log *log;
extern struct devsw *devsw;
extern struct pr *pr;
extern struct proc *initproc;
extern struct spinlock *pid_lock;
extern struct spinlock *tickslock;
extern struct nmi_info *nmi_queue;
extern struct spinlock nmi_queue_lock;
extern struct spinlock nmi_lock;
extern struct spinlock idx_lock;
extern void   (*handler_for_mem_fault)(char*);  // Function which is called from NMI handler.

extern uint64 brelse_start, brelse_end;
extern uint64 end_op_start, end_op_end;
extern uint64 exit_start, exit_end;
extern uint64 fileclose_start, fileclose_end;
extern uint64 freeproc_start, freeproc_end;
extern uint64 kvminit_start, kvminit_end;
extern uint64 procinit_start, procinit_end;
extern uint64 sys_chdir_start, sys_chdir_end;
extern uint64 sys_open_start, sys_open_end;
extern uint64 uvmunmap_start, uvmunmap_end;

struct recovered_addr_node recovered_list[DEPTH];
int recovery_mode = CONSERVATIVE;  // AGGRESSIVE or CONSERVATIVE

void
check_and_acquire(struct spinlock *lk)
{
  if (!holding(lk)) {
    acquire(lk);
  }
}

void
check_and_release(struct spinlock *lk)
{
  if (holding(lk)) {
    release(lk);
  }
}

// Find a struct proc from given pid.
struct proc*
search_proc_from_pid(int target_pid)
{
  struct proc *p;
  for (p = proc; p < &proc[NPROC]; p++) {
    if (p->pid == target_pid) {
      return p;
    }
  }
  return 0x0;
}

// Function to judge that the M-List has target address or not.
static uint64
search_mlist(void *target, struct mlist_node* header, uint64 size)
{
  struct mlist_node *node;
  uint64 ret = 0x0;

  acquire(&mlist.giant_lock);

  for(node = header->next; node != header; node = node->next){
    if(node->addr <= target && target < (void*)((uint64)node->addr + size)){
      ret = (uint64)node->addr;
      break;
    }
  }

  release(&mlist.giant_lock);
  return ret;
}

// M-List search function for pagetable list.
static uint64
search_ptb_mlist(void *target)
{
  uint64 *page = mlist.ptb_list, ret = 0x0;

  acquire(&mlist.giant_lock);

  for(int i = 0; i < ENTRY_SIZE; i++){
    if(MLNODE2PA(page[i]) <= (uint64)target && (uint64)target < MLNODE2PA(page[i])+PGSIZE){
      ret = page[i];  // found
      break;
    }
    else if(i == ENTRY_SIZE-1){
      if(page[i] == (uint64)mlist.ptb_list){
        break;   // not found
      }
      page = (uint64*)page[i];  // Move to the next M-List page.
      i = 0;
    }
  }

  release(&mlist.giant_lock);
  return ret;  // If target entry isn't found, return 0x0.
}

// Delete recovered broken address from NMI Queue and return next broken address.
void*
ret_and_search_nmiqueue(int terminate_status, int irq)
{
  int i;

  acquire(&nmi_queue_lock);
  nmi_queue[0].sp = terminate_status;
  nmi_queue[0].s0 = irq;
  release(&nmi_queue_lock);

  // Wait other process which is waiting for this process finish recovery.
  if(nmi_queue[0].pid != myproc()->pid){
    while(1){
      acquire(&nmi_queue_lock);
      if(nmi_queue[0].addr == (char*)0x0){  // Following process finished.
        release(&nmi_queue_lock);
        break;
      }
      release(&nmi_queue_lock);
    }
  }

  acquire(&nmi_queue_lock);
  for(i = 0; i < NMI_QUEUE_SIZE-1; i++){
    nmi_queue[i].addr = nmi_queue[i+1].addr;
    nmi_queue[i].pid  = nmi_queue[i+1].pid;
    nmi_queue[i].sp   = nmi_queue[i+1].sp;
    nmi_queue[i].s0   = nmi_queue[i+1].s0;

    if(nmi_queue[i+1].addr == 0x0 && nmi_queue[i+1].pid == 0x0){
      break;
    }
  }
  release(&nmi_queue_lock);

  return nmi_queue[0].addr;
}


// Record address of finish recoverying memory object.
static void
record_recovered_memobj(char *start, char *end, int res, int pid, int rcs_flag)
{
  for(int i = 0; i < DEPTH; i++){
    if(recovered_list[i].start == (void*)0x0 && recovered_list[i].end == (void*)0x0){
      recovered_list[i].start = start;
      recovered_list[i].end = end;
      recovered_list[i].terminate_flag = res;
      recovered_list[i].pid = pid;
      recovered_list[i].rcs_flag = rcs_flag;
      break;
    }
  }
}


// Search recorded recovered addresses.
static int
search_recovered_addr(char *broken)
{
  int ret = -1;
  char* target = (char*)((uint64)broken & 0xFFFFFFFF);

  for(int i = 0; i < DEPTH; i++){
    if(recovered_list[i].start <= target && target <= recovered_list[i].end){
      ret = recovered_list[i].terminate_flag;  // Found
      break;
    }
  }

  return ret;  // Not Found
}

int
is_in_old_rcs(int r_pid, int m_pid)
{
  int flag = 0;

  for(int i = 0; i < DEPTH; i++){
    if(recovered_list[i].pid == r_pid){
      flag = recovered_list[i].rcs_flag;
    }
  }

  // If RL_FLAG is valid, search the first recovery proc's R.C.S history.
  if(flag != 0 && search_rcs_history(r_pid, flag) && search_rcs_history(m_pid, flag)){
    return 1;  // The hardware intr came from recovered object's R.C.S.
  }
  return 0;  // not related
}

// Search isolated pagetable and delete related PTDUP.
static void
search_and_delete_isolated_ptdup(int pid)
{
  struct proc *p;
  uint64 entry;

  if(pid == 0){
    for(int i = 1; i <= NPROC; i++){
      if(idx_ptdup[i] == 0x0){
        continue;
      }

      acquire(&idx_lock);
      for(p = proc; p < &proc[NPROC]; p++){
        if(idx_ptdup[i] == (uint64*)p->pagetable){
          break;
        }
      }

      if(p == &proc[NPROC] && idx_ptdup[i] != p->pagetable){
        release(&idx_lock);
        ptdup_delete_all(idx_ptdup[i], 0x0);
        acquire(&idx_lock);
      }
      release(&idx_lock);
    }
  } else {  // For memory errors in exec().
    p = search_proc_from_pid(pid);

    for(int i = 1; i <= NPROC; i++){
      if(idx_ptdup[i] == 0x0){
        continue;
      }

      acquire(&idx_lock);
      entry = search_ptb_mlist(idx_ptdup[i]);
      if(pid == MLNODE2PID(entry) && (uint64)p->pagetable != MLNODE2PA(entry)){
        release(&idx_lock);
        ptdup_delete_all(idx_ptdup[i], 0x0);
        acquire(&idx_lock);
      }
      release(&idx_lock);
    }
  }
  return;
}


// Decrement file reference count.
static void
fileundup(struct file *fp)
{
  check_and_acquire(&ftable->lock);
  if(fp->ref > 0)
    fp->ref--;
  check_and_release(&ftable->lock);
}


// Decrement file reference count.
static void
iundup(struct inode *ip)
{
  acquiresleep(&ip->lock);
  if(ip->ref > 1)
    ip->ref--;
  releasesleep(&ip->lock);
}


// Check process which allocated but not used (state == UNUSED).
void
cleanup_unused_procs(int epid)
{
  struct proc *p, *exit_proc = 0x0;

  for(p = proc; p < &proc[NPROC]; p++){
    if(p->pid == epid){
      exit_proc = p;
      break;
    }
  }

  for(p = proc; p < &proc[NPROC]; p++){
    if(p->pid == 0)
      continue;

    // Search processes which allocated pagetables but p->pagetable is not valid.
    // If valid child processes remain, then they can be passed to the initproc by reparent().
    if(p->state == UNUSED && (p->parent == 0x0 || p->pid != 0)){
      if(!holding(&p->lock)){
        acquire(&p->lock);
      }

      if(p->pagetable == 0x0){
        search_and_delete_isolated_ptdup(0);
      }

      for(int i = 0; i < NOFILE; i++){  // Decrement unused file reference counts in the case of fork().
        if(p->ofile[i]){
          fileundup(p->ofile[i]);
        }
      }

      if(p->cwd){
        release(&p->lock);
        iundup(p->cwd);  // Decrement unused file reference counts in the case of fork().
      }

      freeproc(p);

      if (holding(&p->lock)) {
        if (mycpu()->noff == 0)
          push_off();
        release(&p->lock);
      }
    }
  }

  // Free recovery process's proc PTDUP for exit() and freeproc().
  // We don't use freeproc() because parent process will call freeproc() in wait().
  if(!holding(&exit_proc->lock))
    acquire(&exit_proc->lock);

  search_and_delete_isolated_ptdup(epid);

  if(holding(&exit_proc->lock)){
    release(&exit_proc->lock);
  }
}

// Cleanup NMI queue and process state etc after recovery, in SYSCALL_FAIL, PROCESS_KILL.
static void
cleanup_after_recovery(void)
{
  struct proc *p = myproc();

  handler_for_mem_fault = mlist_tracker;

  if(!holding(&p->lock))
    acquire(&p->lock);

  p->state = RUNNING;

  if(holding(&p->lock))
    release(&p->lock);

  acquire(&nmi_queue_lock);
  nmi_queue = 0x0;  // Avoid NULL unvalid reference & NMI mechanism goes wrong.
  release(&nmi_queue_lock);
}

struct cpu*
search_cpu_from_pid(int pid)
{
  struct cpu *c;

  for(c = cpus; c < &cpus[NCPU]; c++){
    if(c != 0x0 && c->proc != 0 && c->proc->pid == pid){
      return c;
    }
  }

  return 0;
}

// Check all spinlock & sleeplock and release if necessary to avoid kernel panic related to locks.
void
check_all_locks(int pid)
{
  struct mlist_node *node;
  struct cpu* c = search_cpu_from_pid(pid);
  struct spinlock *splk;
  struct sleeplock *sllk;

  // Check sleeplocks.
  acquire(&mlist.giant_lock);
  for(node = mlist.slp_list->next; node != mlist.slp_list; node = node->next){
    sllk = (struct sleeplock*)node->addr;
    if(sllk->locked && sllk->pid == pid){
      releasesleep(sllk);
    }
  }

  // Check spinlocks.
  for(node = mlist.spn_list->next; node != mlist.spn_list; node = node->next){
    splk = (struct spinlock*)node->addr;

    if(splk == &nmi_queue_lock || splk == &mlist.giant_lock || splk == &nmi_lock)
      continue;

    if(splk->locked && splk->cpu == c){
      if(c == 0x0){
        push_off();
      }
      else if(c != mycpu()){
        // Push noff of this process's CPU.
        push_off();
        // Pop noff of recovery process's CPU.
        c->noff -= 1;
        if(c->noff < 0)
          panic("check_all_locks: pop_off");
        if(c->noff == 0 && c->intena)
          intr_on();
        splk->cpu = mycpu();
      }
      release(splk);
    }
  }
  release(&mlist.giant_lock);
}

static int
scan_function_call_history(uint64 sp, uint64 s0, int pid)
{
  int is_chdir = 0, is_end_op = 0;
  struct proc *p;
  uint64 pcs[DEPTH];

  if(s0 - sp > PGSIZE){
    p = search_proc_from_pid(pid);
    getcallerpcs_bottom(pcs, sp, p->kstack, DEPTH);
  } else {
    getcallerpcs_top(pcs, sp, s0, DEPTH);
  }

  // Check Fail-Stop cases which Ev6 should do it even if in Aggressive mode.
  for(int i = 0; i < DEPTH; i++){
    if(ISINSIDE(pcs[i], end_op_start, end_op_end)){
      is_end_op = 1;
    }
    else if(ISINSIDE(pcs[i], sys_chdir_start, sys_chdir_end)){
      is_chdir = 1;
    }

    if(is_chdir && is_end_op){
      return 0;
    }
    else if(ISINSIDE(pcs[i], brelse_start, brelse_end)
         || ISINSIDE(pcs[i], freeproc_start, freeproc_end)
         || ISINSIDE(pcs[i], kvminit_start, kvminit_end)
         || ISINSIDE(pcs[i], procinit_start, procinit_end)
         || ISINSIDE(pcs[i], sys_open_start, sys_open_end)
         || ISINSIDE(pcs[i], uvmunmap_start, uvmunmap_end)){
      return 0;
    }
  }

  return 1;
}

// A function to receive broken address and specify the memobj which it's data on the address,
// then call moderate recovery handler.
void
mlist_tracker(char *address)
{
  char *message;
  char terminate_status = 0;  // Record terminate way of the first recovery process.
  int res = 0;  // recovery result
  int idx, pid, irq = 0;
  int r_pid;  // First recovery proc's pid.
  struct proc *p;
  uint64 sp, s0, baddr, pcs[DEPTH];
  void* broken;

next_nmi_continue:
  broken = nmi_queue[0].addr;
  pid    = nmi_queue[0].pid;
  sp     = nmi_queue[0].sp;
  s0     = nmi_queue[0].s0;
  idx = 0;

  handler_for_mem_fault = panic;  // In preparation for NMI on NMI.
  intr_off();  // Disable software interrupt(Supervisor).

  // At first, check memobjs related memory allocation(kmem, kpgdir, run, kmap).
  // struct kmem
  baddr = search_mlist(broken, mlist.kmm_list, sizeof(struct kmem));
  if(baddr != 0){
    res = recovery_handler_kmem((void*)baddr, pid, sp, s0);
    switch(res){
      case SYSCALL_FAIL:
      case SYSCALL_SUCCESS:
      case SYSCALL_REDO:
      case PROCESS_KILL:
        record_recovered_memobj((char*)baddr, (char*)(baddr + sizeof(struct kmem)), res, pid, 0);
        goto recovery_success;
      case FAIL_STOP:
        goto fail_stop;
      default:
        intr_on();  // Enable software interrupt(Supervisor).
        message = "mlist_tracker: recovery_handler_kmem failed";
        goto bad;
    }
  }

  // struct run
  baddr = search_mlist(broken, mlist.run_list, sizeof(struct run));
  if(baddr != 0){
    res = recovery_handler_run((void*)baddr, pid, sp, s0);
    switch(res){
      case SYSCALL_FAIL:
      case SYSCALL_SUCCESS:
      case SYSCALL_REDO:
      case PROCESS_KILL:
        record_recovered_memobj((char*)baddr, (char*)(baddr + sizeof(struct run)), res, pid, 0);
        goto recovery_success;
      case FAIL_STOP:
        goto fail_stop;
      default:
        message = "mlist_tracker: recovery_handler_run failed";
        goto bad;
    }
  }

  // Next, search large and complex memobj's M-List(pagetable, buf, inode, file).
  uint64 b_ptb = search_ptb_mlist(broken);
  if(b_ptb){
    pagetable_t L2_pagetable = 0x0;
    // If b_ptb != 0, one of pagetables is broken.
    for(p = proc; p < &proc[NPROC]; p++){  // Identify a process which have broken pagetable.
      if(p->pid == MLNODE2PID(b_ptb)){
        //printf_without_pr("mlist_tracker: matched. p->pid = %d\n", p->pid);
        L2_pagetable = p->pagetable;
        break;
      }
    }
    //if((uint64)kernel_pagetable == MLNODE2PA(b_ptb))
    if(p->pid == 0 || L2_pagetable == 0x0){
      goto fail_stop;  // Broken page table is included in kernel_pagetable and it can't recover.
    }

    for(idx = 0; idx <= NPROC; idx++){  // Identify pagetable duplication index.
      if(L2_pagetable == idx_ptdup[idx])
        break;
      else if(MLNODE2PA(b_ptb) == (uint64)idx_ptdup[idx])
        break;
    }
    res = recovery_handler_pagetable(MLNODE2LEVEL(b_ptb), idx, p, (void*)MLNODE2PA(b_ptb), sp, s0);
    switch(res){
      case SYSCALL_FAIL:
      case SYSCALL_SUCCESS:
      case SYSCALL_REDO:
      case PROCESS_KILL:
        record_recovered_memobj((char*)b_ptb, (char*)((uint64)b_ptb + PGSIZE), res, pid, 0);
        goto recovery_success;
      case RETURN_TO_KERNEL:
        record_recovered_memobj((char*)b_ptb, (char*)((uint64)b_ptb + PGSIZE), res, pid, 0);
        goto recovery_success_intr;
      case FAIL_STOP:
        goto fail_stop;
      default:
        message = "mlist_tracker: recovery_handler_pgtable failed";
        goto bad;
    }
  }

  // struct pr
  baddr = search_mlist(broken, mlist.pr_list, sizeof(struct pr));
  if(baddr != 0){
    // If baddr != 0, one of bcache.buf[] is broken.
    res = recovery_handler_pr((void*)pr, pid, sp, s0);
    switch(res){
      case PROCESS_KILL:
        record_recovered_memobj((char*)baddr, (char*)((uint64)baddr + sizeof(struct pr)), res, pid, 0);
        goto recovery_success;
      case RETURN_TO_USER:
      case RETURN_TO_KERNEL:
        record_recovered_memobj((char*)baddr, (char*)((uint64)baddr + sizeof(struct pr)), res, pid, 0);
        irq = UART0_IRQ;
        goto recovery_success_intr;
      case FAIL_STOP:
        goto fail_stop;
      default:
        message = "mlist_tracker: recovery_handler_pr failed";
        goto bad;
    }
  }

  // struct buf
  baddr = search_mlist(broken, mlist.buf_list, sizeof(struct buf));
  if(baddr != 0){
    // If baddr != 0, one of bcache.buf[] is broken.
    res = recovery_handler_buf((void*)baddr, pid, sp, s0);
    switch(res){
      case SYSCALL_SUCCESS:
      case SYSCALL_FAIL:
      case SYSCALL_REDO:
      case PROCESS_KILL:
        record_recovered_memobj((char*)baddr, (char*)((uint64)baddr + sizeof(struct buf)), res, pid, 0);
        goto recovery_success;
      case RETURN_TO_USER:
      case RETURN_TO_KERNEL:
        record_recovered_memobj((char*)baddr, (char*)((uint64)baddr + sizeof(struct buf)), res, pid, 0);
        irq = VIRTIO0_IRQ;
        goto recovery_success_intr;
      case FAIL_STOP:
        goto fail_stop;
      default:
        message = "mlist_tracker: recovery_handler_buf failed";
        goto bad;
    }
  }

  // ftable/file
  if((void*)ftable <= broken && broken < (void*)((uint64)ftable + sizeof(struct ftable))){
    baddr = search_mlist(broken, mlist.fil_list, sizeof(struct file));
    if(baddr != 0){
      // If baddr != 0, one of ftable.file[] is broken.
      char* ftable_addr = (char*)ftable;
      res = recovery_handler_file((void*)baddr, pid, sp, s0);
      switch(res){
        case SYSCALL_FAIL:
        case SYSCALL_SUCCESS:
        case REOPEN_SYSCALL_FAIL:
        case REOPEN_SYSCALL_REDO:
        case PROCESS_KILL:
          record_recovered_memobj(ftable_addr, (char*)((uint64)ftable_addr + sizeof(struct ftable)), res, pid, RL_FLAG_FTABLE);
          goto recovery_success;
        case FAIL_STOP:
          goto fail_stop;
        default:
          message = "mlist_tracker: recovery_handler_file failed";
          goto bad;
      }
    }
  }

  // icache/inode
  if((void*)icache <= broken && broken < (void*)((uint64)icache + sizeof(struct icache))){
    baddr = search_mlist(broken, mlist.ino_list, sizeof(struct inode));
    if(baddr != 0){
      char* icache_addr = (char*)icache;
      // If baddr != 0, one of ftable.file[] is broken.
      res = recovery_handler_inode((void*)baddr, pid, sp, s0);
      switch(res){
        case SYSCALL_FAIL:
        case SYSCALL_SUCCESS:
        case SYSCALL_REDO:
        case REOPEN_SYSCALL_FAIL:
        case REOPEN_SYSCALL_REDO:
        case PROCESS_KILL:
          record_recovered_memobj(icache_addr, (char*)((uint64)icache_addr + sizeof(struct icache)), res, pid, RL_FLAG_ICACHE);
          goto recovery_success;
        case FAIL_STOP:
          goto fail_stop;
        default:
          message = "mlist_tracker: recovery_handler_ino failed";
          goto bad;
      }
    }
  }

  // Finally check other memobj's address lists.
  // devsw
  baddr = search_mlist(broken, mlist.dev_list, sizeof(struct devsw) * NDEV);
  if(baddr != 0){
    res = recovery_handler_devsw((void*)baddr, pid);
    switch(res){
      case SYSCALL_FAIL:
      case SYSCALL_SUCCESS:
      case SYSCALL_REDO:
      case PROCESS_KILL:
        //cleanup_unused_procs(pid);
        record_recovered_memobj((char*)baddr, (char*)((uint64)baddr + sizeof(struct devsw)), res, pid, 0);
        goto recovery_success;
      case FAIL_STOP:
        goto fail_stop;
      default:
        message = "mlist_tracker: recovery_handler_devsw failed";
        goto bad;
    }
  }

  // struct log / logheader
  baddr = search_mlist(broken, mlist.log_list, sizeof(struct log));
  if(baddr != 0){
    res = recovery_handler_log((void*)baddr, pid, sp, s0);
    switch(res){
      case SYSCALL_FAIL:
      case SYSCALL_SUCCESS:
      case SYSCALL_REDO:
      case PROCESS_KILL:
        record_recovered_memobj((char*)baddr, (char*)((uint64)baddr + sizeof(struct log)), res, pid, 0);
        goto recovery_success;
      case FAIL_STOP:
        goto fail_stop;
      default:
        message = "mlist_tracker: recovery_handler_log failed";
        goto bad;
    }
  }

  // cons
  baddr = search_mlist(broken, mlist.con_list, sizeof(struct cons));
  if(baddr != 0){
    res = recovery_handler_cons((void*)baddr, pid, sp, s0);
    switch(res){
      case SYSCALL_FAIL:
      case SYSCALL_REDO:
        record_recovered_memobj((char*)baddr, (char*)((uint64)baddr + sizeof(struct cons)), res, pid, 0);
        goto recovery_success;
      case RETURN_TO_USER:
      case RETURN_TO_KERNEL:
        record_recovered_memobj((char*)baddr, (char*)((uint64)baddr + sizeof(struct cons)), res, pid, 0);
        irq = UART0_IRQ;
        goto recovery_success_intr;
      case FAIL_STOP:
        goto fail_stop;
      default:
        message = "mlist_tracker: recovery_handler_cons failed";
        goto bad;
    }
  }

  // pipe
  baddr = search_mlist(broken, mlist.pip_list, sizeof(struct pipe));
  if(baddr != 0){
    res = recovery_handler_pipe(broken, pid, sp, s0);
    switch(res){
      case SYSCALL_FAIL:
      case SYSCALL_SUCCESS:
      case SYSCALL_REDO:
      case PROCESS_KILL:
        record_recovered_memobj((char*)baddr, (char*)((uint64)baddr + sizeof(struct pipe)), PIPE, pid, 0);
        goto recovery_success;
      case FAIL_STOP:
        goto fail_stop;
      default:
        message = "mlist_tracker: recovery_handler_pipe failed";
        goto bad;
    }
  }

  // locks
  baddr = search_mlist(broken, mlist.spn_list, sizeof(struct spinlock));
  if(baddr != 0x0){
    if(baddr == (uint64)tickslock){
      res = recovery_handler_tickslock((void*)baddr, pid, sp, s0);
    }
    else if(baddr == (uint64)pid_lock){
      res = recovery_handler_pid_lock((void*)baddr, pid);
    }
    switch(res){
      case SYSCALL_FAIL:
      case PROCESS_KILL:
      case SYSCALL_REDO:
        record_recovered_memobj((char*)baddr, (char*)((uint64)baddr + sizeof(struct spinlock)), res, pid, 0);
        goto recovery_success;
      case RETURN_TO_USER:
      case RETURN_TO_KERNEL:
        record_recovered_memobj((char*)baddr, (char*)((uint64)baddr + sizeof(struct spinlock)), res, pid, 0);
        goto recovery_success_intr;
      case FAIL_STOP:
        goto fail_stop;
      default:
        message = "mlist_tracker: recovery_handler_locks failed or spinlock in the unrecoverable object was broken.";
        goto bad;
    }
  }

  // If reach here, we can't specify broken memobj or broken address is out of memobj's area.
  switch((res = search_recovered_addr(broken))){
    case PIPE:
      p = search_proc_from_pid(pid);
      // Search call stack & check Fail-Stop situation.
      if(sp == 0x0 || s0 - sp > PGSIZE){  // In the case of invalid s0 or sp is passed.
        getcallerpcs_bottom(pcs, sp, p->kstack, DEPTH);
      } else {
        getcallerpcs_top(pcs, sp, s0, DEPTH);
      }
      res = SYSCALL_FAIL;
      for(int i = 0; i < DEPTH; i++){
        if(exit_start < pcs[i] && pcs[i] < exit_end){
          res = PROCESS_KILL;
          for (int j = 0; j < NOFILE; j++) {
            if (p->ofile[j] != 0x0 && p->ofile[j]->pipe == broken) {
              acquire(&ftable->lock);
              p->ofile[j]->ref++;
              release(&ftable->lock);
            }
          }
        }
      }
    case SYSCALL_FAIL:
    case SYSCALL_SUCCESS:
    case SYSCALL_REDO:
    case PROCESS_KILL:
      printf_without_pr("mlist_tracker: the broken memobj(%p) is already recovered.\n", broken);
      if(log->outstanding)
        log->outstanding--;
      goto recovery_success;
    default:
      message = "mlist_tracker can't specify broken memobj & can't found in recovered list";
      goto bad;
  }

  // struct spinlock (out of other memobjs)
  baddr = search_mlist(broken, mlist.spn_list, sizeof(struct spinlock));
  // If baddr != NULL, one of spinlock (maybe idelock) is broken.
  if(baddr != 0){
    message = "mlist_tracker: unrecoverable spinlock is broken";
    goto bad;
  }


recovery_success:  // Recovery is succeeded and finish by SYSCALL_FAIL or PROCESS_KILL.
  cleanup_unused_procs(pid);
  check_all_locks(pid);  // Is this needed?

recovery_success_intr:  // Recovery is succeeded and finish by RETURN_TO_USER or RETURN_TO_KERNEL.
  if(nmi_queue[0].pid == myproc()->pid){
    terminate_status = res;  // Record the first NMI process will terminate by exit().
    r_pid = pid;
  }

  // In the case of the hardware intr came from already recovered object's R.C.S.
  if(terminate_status == RETURN_TO_KERNEL && is_in_old_rcs(r_pid, pid)){
    if(recovery_mode == AGGRESSIVE && scan_function_call_history(s0, sp, pid))
      terminate_status = PROCESS_KILL;
    else
      goto fail_stop;
  }

  if((broken = ret_and_search_nmiqueue(res, irq)) != 0x0){
    printf_without_pr("mlist_tracker: other NMI (%p) is pending.\n", broken);
    goto next_nmi_continue;
  }

  cleanup_after_recovery();

  switch(terminate_status){
    case SYSCALL_SUCCESS:
    case SYSCALL_FAIL:
    case SYSCALL_REDO:
    case REOPEN_SYSCALL_FAIL:
    case REOPEN_SYSCALL_REDO:
      af_return_syscall_result(terminate_status);

    case PROCESS_KILL:
      af_process_kill();

    case RETURN_TO_USER:
      af_return_to_user(pid, irq);

    case RETURN_TO_KERNEL:
      af_return_to_kernel(sp, s0, irq);

    default:
      message = "Unvalid recovery result is passed";
      goto bad;
  }

fail_stop:
  message = "Fail-Stop due to an unrecoverable memory error is detected.";

bad:
  panic_without_pr(message);
}
