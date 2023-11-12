#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "mlist.h"
#include "memlayout.h"
#include "proc.h"
#include "nmi.h"
#include "after-treatment.h"

extern int recovery_mode;
extern struct mlist_header mlist;

extern uint64 clockintr_start, clockintr_end;
extern uint64 consoleintr_start, consoleintr_end;
extern uint64 devintr_start, devintr_end;
extern uint64 exit_start, exit_end;
extern uint64 kerneltrap_start, kerneltrap_end;
extern uint64 kernelvec_start, kernelvec_end;
extern uint64 nmivec_start, nmivec_end;
extern uint64 usertrap_start, usertrap_end;

void kernelret();

// Extract the contents of some general registers which are stored in recovery process's stack.
static void
getregs_from_stack(uint64 *regs, uint64 sp, uint64 s0)
{
  int is_devintr = 0, is_kerneltrap = 0;
  int i = 0;
  uint64 m_sp = sp, m_s0 = s0, ra;
  uint64 depth, *stack, endline;
  uint64 tmp_regs[DEPTH][4];

  if(sp == 0x0 || s0 - sp > PGSIZE)
    panic("get_regs_in_kerneltrap: Invalid sp/s0 was passed");

  endline = (sp & 0xFFFFFFFFFFFFF000) + 0x1000;
  stack = (uint64*)sp;
 
  // Search kernel stack from top to bottom and store some values to temporary buffer.
  while(m_s0 < endline){
    depth = (m_s0 - m_sp) / 8;
    m_sp = m_s0;            // Save next stack's top address.
    m_s0 = stack[depth-2];  // Pick up s0 from stack's second from the bottom.
    ra = stack[depth-1];

    tmp_regs[i][0] = m_s0;  // s0
    tmp_regs[i][1] = stack[depth-3];  // s1
    tmp_regs[i][2] = stack[depth-4];  // s2
    tmp_regs[i][3] = stack[depth-1];  // ra
    i++;

    if(m_sp == endline || m_s0 == endline)
      break;

    stack = (uint64*)m_sp;  // Update stack to see next stacked contents.
  }

  // Search temporary buffer and find appropriate values.
  for(int j = 0; j < i-1; j++){
    ra = tmp_regs[j][3];

    if(ISINSIDE(ra, consoleintr_start, consoleintr_end)){  // In the kernelvec's stack contents.
      regs[2] = tmp_regs[j+1][2];  // sepc (from s2 register).
    }
    else if(ISINSIDE(ra, devintr_start, devintr_end)){  // In the uartintr()'s stack content.
      if(is_devintr == 1){  // Hardware intr's devintr().
        regs[1] = tmp_regs[j+1][1];  // sstatus (from s1 register).
      }
      is_devintr++;
    }
    else if(ISINSIDE(ra, kerneltrap_start, kerneltrap_end)){  // In the devintr()'s stack content.
      if(is_kerneltrap == 1){  // Hardware intr's kerneltrap().
        regs[0] = tmp_regs[j][0];  // sp (from s0 register)
      }
      is_kerneltrap++;
    }
    else if(ISINSIDE(ra, clockintr_start, clockintr_end)){  // In the acquire()/release() of tickslock.
      regs[2] = tmp_regs[j][2];  // sepc (from s2 register).
    }
  }
}


// Return to the kernel context which had a hardware interrupt before recovery operations.
// Return function of timer intrrupt isn't needed because after sret, the procedure will move returning procedures in timervec().
void
kerneltrapret(uint64 sp, uint64 s0, int irq)
{
  uint64 regs[3] = {0, 0, 0};  // {sp, sstatus, sepc}.

  if(irq == UART0_IRQ || irq == VIRTIO0_IRQ){
    plic_complete(irq);
  } else {  // For timer intrrupt.
    w_sip(r_sip() & ~2);
  }

  getregs_from_stack(regs, sp, s0);
  w_sstatus(regs[1]);
  w_sp(regs[0]);
  w_sepc(regs[2]);
  kernelret();
}

// Identify the place which NMI is occurred is whether in user area or kernel area.
int
identify_nmi_occurred_trap(int pid, uint64 sp, uint64 s0)
{
  int is_kernelvec = 0;
  uint64 pcs[DEPTH];

  if(sp == 0x0 || s0 - sp > PGSIZE){  // In the case of invalid s0 or sp is passed.
    struct proc *p = search_proc_from_pid(pid);
    getcallerpcs_bottom(pcs, sp, p->kstack, DEPTH);
  } else {
    getcallerpcs_top(pcs, sp, s0, DEPTH);
  }

  for(int i = 0; i < DEPTH; i++){
    if(ISINSIDE(pcs[i], kernelvec_start, kernelvec_end) || ISINSIDE(pcs[i], nmivec_start, nmivec_end)){
      is_kernelvec = 1;
    }
    else if(is_kernelvec == 1){
      if(ISINSIDE(pcs[i], usertrap_start, usertrap_end)){
        return USERTRAP;  // usertrap (console intr) -> kerneltrap (NMI)
      }
      else if(ISINSIDE(pcs[i], kerneltrap_start, kerneltrap_end)){
        return KERNELTRAP;  // usertrap (other intrs) -> kerneltrap (console intr) -> kerneltrap (NMI)
      }
    }
  }

  return 0;
}
