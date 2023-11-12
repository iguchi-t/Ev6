// NMI handling functions.

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "nmi.h"
#include "fs.h"
#include "file.h"
#include "after-treatment.h"

void nmivec(void);        // My NMI handler in assembly
int  nmi_handle(char*);   // My NMI handler
void mem_nmi_handle_first(char*);   // NMI handler because of memory error (at the first time)
void mem_nmi_handle_follow(char*);  // NMI handler because of memory error (after the second time)
void nmi_imitate(char*);  // Original NMI imitator

void (*mem_nmi_handle_next)(char*) = mem_nmi_handle_first;  // Next calling NMI handler because of memory error
void (*handler_for_mem_fault)(char*) = mlist_tracker;  // Next calling function pointer
struct nmi_info *nmi_queue = 0x0;  // NMI Queue's top
struct spinlock nmi_queue_lock;  // Spinlock to protect NMI Queue
struct spinlock nmi_lock;  // Spinlock to protect nmi handling function pointers

extern struct proc proc[];
extern int panicked;
extern struct ftable *ftable;


/* Shadow NMI handler
 * When NMI occurs, this function is called at first,
 * setting function pointer to panic(), and start searching M-List.
 */
int nmi_handle(char *argument){
  if(mem_nmi_handle_next == panic)
    panic_without_pr("Fail-Stop: NMI handler can't handle this NMI (NMI on NMI, NMIs occur with no interval, or NMI Looping)");
  if(__sync_lock_test_and_set(&mem_nmi_handle_next, panic));

  struct proc *p = myproc();
  if(p == 0x0)
    panic("Fail-Stop: NMI is occurred in the scheduler or boot scheme.");
  if(p->state == RECOVERING)
    panic("Fail-Stop: NMI on recovering process or recovery waiting process");

  for(struct proc *p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
	    continue;

    if(!holding(&p->lock))
      acquire(&p->lock);
    if(p->state == RECOVERING || nmi_queue != 0x0){
      acquire(&nmi_lock);     
      if(__sync_lock_test_and_set(&mem_nmi_handle_next, mem_nmi_handle_follow));
      mem_nmi_handle_next = mem_nmi_handle_follow;
      release(&nmi_lock);
      release(&p->lock);
      break;
    }
    release(&p->lock);
  }

  if(mem_nmi_handle_next != mem_nmi_handle_follow){
    acquire(&nmi_lock);
    mem_nmi_handle_next = mem_nmi_handle_first;
    release(&nmi_lock);
  }

  if(mem_nmi_handle_next == panic)
    argument = "Fail-Stop: NMI handler can't handle these NMIs";
  (*mem_nmi_handle_next)(argument);  // Call next NMI handler.
  return 1;
}


// NMI handler because of memory error (at the first time)
void mem_nmi_handle_first(char *broken_addr){
  acquire(&nmi_lock);
  mem_nmi_handle_next = panic;
  release(&nmi_lock);

  char *argument = broken_addr;
  struct nmi_info queue[NMI_QUEUE_SIZE];  // Get local area in kstack for NMI Queue.
  struct proc *p = myproc();

  acquire(&nmi_queue_lock);
  if(nmi_queue != 0x0){  // In the case of multiple process enter this function.
    release(&nmi_queue_lock);
    mem_nmi_handle_follow(broken_addr);
  }

  for(int i = 0; i < NMI_QUEUE_SIZE; i++){
    queue[i].addr = (char*)0x0;
    queue[i].pid  = 0;
    queue[i].sp   = 0;
    queue[i].s0   = 0;
  }

  nmi_queue = queue;
  nmi_queue[0].addr = broken_addr;
  nmi_queue[0].pid  = p->pid;
  nmi_queue[0].sp   = r_sp();
  nmi_queue[0].s0   = r_s0();
  release(&nmi_queue_lock);

  acquire(&p->lock);
  p->state = RECOVERING;
  release(&p->lock);
  acquire(&nmi_lock);
  mem_nmi_handle_next = mem_nmi_handle_first;
  release(&nmi_lock);

  if(handler_for_mem_fault == panic)
    argument = "Fail-Stop: Multiple processes try to enter recovery handlers in parallel.";
  (*handler_for_mem_fault)(argument);
  return;
}


// NMI handler because of memory error (follow the second time)
void mem_nmi_handle_follow(char *broken_addr){
  int i, prev_state, irq = 0, term_status = 0;
  struct proc *p = myproc();
  uint64 m_sp = 0x0, m_s0 = 0x0;

  // Store broken address to NMI Queue in NMI process's kstack.
  acquire(&nmi_queue_lock);
  if(nmi_queue == 0x0){  // In the case of first NMI process is about to finish recovery.
    release(&nmi_queue_lock);
    mem_nmi_handle_first(broken_addr);
  }

  for(i = 1; i < NMI_QUEUE_SIZE; i++){
    if(nmi_queue[i].addr == 0x0){
      nmi_queue[i].addr = broken_addr;
      nmi_queue[i].pid  = p->pid;
      nmi_queue[i].sp   = m_sp = r_sp();
      nmi_queue[i].s0   = m_s0 = r_s0();
      break;
    }
  }

  if(i == NMI_QUEUE_SIZE && nmi_queue[i-1].addr != broken_addr)
    panic_without_pr("Fail-Stop: Too many NMIs occuer");
  release(&nmi_queue_lock);

  acquire(&p->lock);
  prev_state = p->state;
  p->state = RECOVERING;
  release(&p->lock);

  // Then, this process waits NMI process to finish recovery by polling and receive recovery handler's termination status.
  while(1){
    acquire(&nmi_queue_lock);
    if(nmi_queue == 0x0)
      mem_nmi_handle_first(broken_addr);
    // Recovery result is put in sp.
    if(nmi_queue[0].pid == p->pid && SYSCALL_SUCCESS <= nmi_queue[0].sp && nmi_queue[0].sp <= RETURN_TO_KERNEL){
      term_status = nmi_queue[0].sp;
      irq = nmi_queue[0].s0;
      release(&nmi_queue_lock);
      break;
    }
    release(&nmi_queue_lock);
  }
  
  acquire(&nmi_queue_lock);
  nmi_queue[0].addr = (char*)0x0;
  nmi_queue[0].pid  = 0;
  nmi_queue[0].sp   = 0x0;
  nmi_queue[0].s0   = 0x0;
  release(&nmi_queue_lock);
  while(mycpu()->noff > 0)
    pop_off();

  switch(term_status){
    case SYSCALL_SUCCESS:
    case SYSCALL_FAIL:
    case SYSCALL_REDO:
    case REOPEN_SYSCALL_FAIL:
    case REOPEN_SYSCALL_REDO:
      p->state = prev_state;
      af_return_syscall_result(term_status);

    case PROCESS_KILL:
      cleanup_unused_procs(p->pid);
      af_process_kill();

    case RETURN_TO_USER:
      af_return_to_user(p->pid, irq);

    case RETURN_TO_KERNEL:
      af_return_to_kernel(m_sp, m_s0, irq);

    default:
      panic("Fail-Stop: Invalid recovery result is passed");
  }
  return;
}


/* NMI Imitator
 * The flow when NMI occurs is maybe as below.
 * "NMI➛ machine mode interrupt➛ NMI handler➛ supervisor interrupt(kernelvec➛ kerneltrap)➛ ..."
 * So in this function, do imitation from machine-mode interrupt to software interrupt.
 */ 
void nmi_imitate(char *broken){
  // Save the contents of registers to memory temporarily. 
  uint64 sstatus = r_sstatus();

  if((sstatus & SSTATUS_SPP) == 0 || intr_get()){
    // If you want to imitate NMI via kernelvec() & kerneltrap() from usermode,
    // you can do that through writing 1 on SSTATUS_SPP by w_sstatus().
    w_sstatus(sstatus | SSTATUS_SPP);
    intr_off();  // Disable other software interrupt (especially timer interrupt & context switch)
  }

  w_scause(16);  // The cause of trap is "Unknown(NMI)" (this scause value is not default).
  w_stval((uint64)broken);

  /* Trigger software interrupt (kernelvec and kerneltrap).
   * If recovery succeeded and returned from recovery handler, return original processing.
   * Otherwise, if recovery succeeded and do exit(), it don't return here.
   */
  nmivec();
  return;  // Can't reach here.
}