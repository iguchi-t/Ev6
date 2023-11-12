/* Transactions to protect the gap between real data structure updates and meta-data updates.
 * Transactioning target objects:
 *   - page tables: between page table mappings and PTDUP, and M-List
 *   - struct log: between logheader and outstanding, and their replica
 *   - struct run: between Free-List and M-List (to recover struct kmem) 
 */

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "mlist.h"
#include "kalloc.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"
#include "log.h"
#include "ptdup.h"
#include "trans.h"

int log_outstanding;             // Logging outstanding value.
struct logheader log_logheader;  // Logging logheader.
struct trans_info_array trans_info_array;
struct run *log_run;             // Logging run pointer.

extern int nextpid;
extern int dup_outstanding;
extern struct log *log;
extern struct kmem *kmem;
extern struct logheader dup_lhdr;
extern struct mlist_header mlist;


void init_trans_info(void){
  initlock(&trans_info_array.lock, "transaction");
  trans_info_array.recovering = 0;

  for(int i = 0; i <= NPROC; i++){
    trans_info_array.array[i].pid = 0;
    trans_info_array.array[i].pagetable_ntrans = 0;
    trans_info_array.array[i].log_ntrans = 0;
    trans_info_array.array[i].run_ntrans = 0;
  }
}

void register_trans_info(int pid){
  acquire(&trans_info_array.lock);
  for(int i = 0; i <= NPROC; i++){
    if(trans_info_array.array[i].pid == 0){
      trans_info_array.array[i].pid = pid;
      break;
    }
  }
  release(&trans_info_array.lock);
}

void delete_trans_info(int pid){
  acquire(&trans_info_array.lock);
  for(int i = 0; i <= NPROC; i++){
    if(trans_info_array.array[i].pid == pid)
      trans_info_array.array[i].pid = 0;  // Mark unused.
  }
  release(&trans_info_array.lock);
}

// Search corresponding trans_info with pid and return corresponding index of the array.
static int search_trans_info(int pid){
  int ret = -1;  // -1 means fail to find.

  acquire(&trans_info_array.lock);
  for(int i = 0; i <= NPROC; i++){
    if(trans_info_array.array[i].pid == pid)
      ret = i;
  }
  release(&trans_info_array.lock);
  return ret;
}

// Transaction entering functions are below.
// Entrance function of log transaction. log->lock must be hold.
void enter_trans_log(void){
  int idx = 0;
  if(nextpid > 2){
    idx = search_trans_info(myproc()->pid);
  }
  struct trans_info *ti = &trans_info_array.array[idx];

  if(ti->log_ntrans < 0)
    panic("enter_trans_log: invalid ntrans value");

  log_logheader = log->lh;
  log_outstanding = log->outstanding;
  ti->log_ntrans++;  // Enter transaction.
}

// Exit function of log transaction. log->lock must be hold.
void exit_trans_log(void){
  int idx = 0;
  if(nextpid > 2){
    idx = search_trans_info(myproc()->pid);
  }
  struct trans_info *ti = &trans_info_array.array[idx];

  if(ti->log_ntrans < 1)
    panic("exit_trans_log: invalid ntrans value");

  ti->log_ntrans--;  // Exit transaction.
  log_outstanding = -1;
}

/* We think that complete/sophisticated transaction for page tables will be challenging,
 * so as the first step, we target leaving no inconsistencies among page tables, PTDUP, and M-List.
 * I think that Ev6 can choose Process Kill on Aggressive mode and Fail-Stop on Conservative mode
 * when any NMIs occur during the transaction (in 2022.07.15).
 */
void enter_trans_pagetable(void){
  int idx = -1;
  if(nextpid > 2){
    idx = search_trans_info(myproc()->pid);
  } else
    idx = 0;

  if(idx < 0){
    printf("enter_trans_info: No corresponding trans_info.\n");
    return;
  }
  if(trans_info_array.array[idx].pagetable_ntrans < 0)
    panic("enter_trans_pagetable: invalid ntrans value");

  trans_info_array.array[idx].pagetable_ntrans++;  // Enter transaction.
}

void exit_trans_pagetable(void){
  int idx;
  if(nextpid > 2){
    idx = search_trans_info(myproc()->pid);
  } else
    idx = 0;

  if(idx < 0){
    printf("enter_trans_info: No corresponding trans_info.\n");
    return;
  }
  if(trans_info_array.array[idx].pagetable_ntrans < 1)
    panic("exit_trans_pagetable: invalid ntrans value");

  trans_info_array.array[idx].pagetable_ntrans--;  // Exit transaction.
}

// For struct run.
/* During transaction of run, we log the address of manipulating run
 * for recovering struct kmem failure due to ECC-uncorrectable errors
 * by adding the logging run to the Free-List.
 * In kalloc(), the run (empty page) doesn't allocate yet due to interrupted by the error.
 * In kfree(), the run was already unused but wasn't added to the Free-List due to the error.
 */
void enter_trans_run(struct run *addr){
  int idx = 0;
  struct trans_info *ti;
  if(nextpid > 2){
    idx = search_trans_info(myproc()->pid);
  }
  ti = &trans_info_array.array[idx];

  if(ti->run_ntrans < 0)
    panic("enter_trans_run: invalid ntrans value");

  log_run = addr;
  ti->run_ntrans++;  // Enter transaction.
}

void exit_trans_run(void){
  int idx = 0;
  struct trans_info *ti;
  if (nextpid > 2){
    idx = search_trans_info(myproc()->pid);
  }
  ti = &trans_info_array.array[idx];

  if (ti->run_ntrans < 1) {
    panic("exit_trans_run: invalid ntrans value");
  }

  ti->run_ntrans--;  // Exit transaction.
  log_run = (struct run*)0x0;
}


// Transaction handlers when the transaction is interrupted by NMI due to ECC-uncorrectable errors 
// and related functions.
static int check_inside_trans(int target, int pid){
  int res = 0, idx = search_trans_info(pid);
  struct trans_info *ti = &trans_info_array.array[idx];

  switch(target){
    case TRANS_LOG:
      res = ti->log_ntrans;
      break;
    case TRANS_PAGETABLE:
      res = ti->pagetable_ntrans;
      break;
    case TRANS_RUN:
      res = ti->run_ntrans;
      break;
    default:
      printf("check_inside_trans: Invalid flag is passed.\n");
      res = -1;  // -1 means failing in checking.
  }
  
  return res;
}

// In struct log, copy real logheader & outstanding to their duplications for struct buf.
int check_and_handle_trans_log(int pid){
  int ntrans = check_inside_trans(TRANS_LOG, pid);

  if(ntrans > 0){  // Inside of transaction.
    log->lh = log_logheader;
    dup_lhdr = log->lh;
    dup_outstanding = log_outstanding;
    return 0;
  }
  else if(ntrans < 0){  // Fail in checking.
    printf("check_and_handle_trans_log: Fail to identify whether inside or outside of trans.");
    return -1;
  }
  return 0;
}

// For log's recovery handler to roll-back log contents.
int check_and_handle_trans_logheader(int pid){
  int ntrans = check_inside_trans(TRANS_LOG, pid);

  if(ntrans > 0){  // Inside of transaction.
    dup_lhdr = log_logheader;
    if(log_outstanding >= 0)
      dup_outstanding = log_outstanding;
    return 0;
  }
  else if(ntrans < 0){  // Fail in checking.
    printf("check_and_handle_trans_log: Fail to identify whether inside or outside of trans.");
    return -1;
  }
  return 0;
}

// In pagetables, do Process Kill to leave no inconsistencies between pagetables and PTDUP.
// For pagetables, struct kmem, and run.
int check_and_handle_trans_pagetable(int pid){
  int ntrans = check_inside_trans(TRANS_PAGETABLE, pid);

  if (ntrans > 0) {  // Inside of transaction.
    return 1;  // Choose Process Kill.
  } else if (ntrans < 0) {  // Fail in checking.
    printf("check_and_handle_trans_pagetable: Fail to identify whether inside or outside of trans.");
    return -1;
  }
  return 0;
}

// In struct run, add logging run to the Free-List.
// This is for struct kmem, so kmem must be already recovered.
void check_and_handle_trans_run(int pid){
  int ntrans = check_inside_trans(TRANS_RUN, pid);

  if (ntrans > 0 && (void*)kmem != (void*)log_run) {  // inside of transaction
    acquire(&kmem->lock);
    log_run->next = kmem->freelist;
    kmem->freelist = log_run;
    register_memobj(log_run, mlist.run_list);
    release(&kmem->lock);
    exit_trans_run();
  } else if (ntrans < 0) {  // invalid ntrans value
    panic("check_and_handle_trans_run: Invalid ntrans value.");
  }
}
