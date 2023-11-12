/*
 * Ev6 Userland Cooperation functionalities.
 */

#include "types.h"
#include "param.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"

extern struct proc proc[];
extern struct spinlock oa_table_lock;

uint64 user_coop_status;  // Status bit for per process (1: Enable, 0: Disable).
                          // These bits have better to be included in process memory object in the future.

void
usercoopinit(void)
{
  initlock(&oa_table_lock, "open argment table");
}

int
sys_enable_user_coop(void)
{
  struct proc *p = myproc();

  for(int i = 0; i < NPROC; i++){
    if(p->pid == proc[i].pid){
      user_coop_status |= (1L << i);  // Must be long type to cover all struct proc nodes.
      return 0;
    }
  }

  return -1;
}

int
sys_disable_user_coop(void)
{
  struct proc *p = myproc();

  for(int i = 0; i < NPROC; i++){
    if(p->pid == proc[i].pid){
      user_coop_status &= ~(1L << i);
      return 0;
    }
  }

  return -1;
}

// Check user cooperation is valid or not.
int
is_enable_user_coop(int pid)
{
  for(int i = 0; i < NPROC; i++){
    if(pid == proc[i].pid){
      return (user_coop_status & (1L << i)) >> i;
    }
  }

  return -1;
}