#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "proc.h"
#include "after-treatment.h"

char *syscall_res[7] = {"", "", "Syscall Success", "Syscall Fail", "Syscall Redo", "Reopen & Syscall Fail", "Reopen & Syscall Redo"};

// For Syscall Fail, Syscall Success, Syscall Redo, Re-Open, and their combinations.
void
af_return_syscall_result(int res)
{
  acquire(&myproc()->lock);
  myproc()->tf->a0 = -res;  // Return After-Treatment policy as syscall result.
  release(&myproc()->lock);
  printf_without_pr("Terminate by %s\n", syscall_res[res]);
  printf_without_pr("end all recovery operations: %d\n", get_ticks());  // For measuring recovery time.
  usertrapret();
}

void
af_process_kill(void)
{
  printf_without_pr("Terminate by Process Kill\n");
  printf_without_pr("end all recovery operations: %d\n", get_ticks());
  exit(0);
}

void
af_return_to_user(int pid, int irq)
{
  struct proc *p = search_proc_from_pid(pid);

  if(p != 0x0 && p->killed)
    exit(-1);

  printf_without_pr("Terminate by Return-to-User\n");
  printf_without_pr("end all recovery operations: %d\n", get_ticks());
  plic_complete(irq);
  usertrapret();
}

void
af_return_to_kernel(uint64 sp, uint64 s0, int irq)
{
  printf_without_pr("Terminate by Return-to-Kernel\n");
  printf_without_pr("end all recovery operations: %d\n", get_ticks());
  kerneltrapret(sp, s0, irq);
}
