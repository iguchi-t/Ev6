// For identifying position of function by using getcallerpcs().
#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"

extern struct inode* create();
extern uint64 sys_close();
extern uint64 sys_chdir();
extern uint64 sys_exec();
extern uint64 sys_fstat();
extern uint64 sys_open();
extern uint64 sys_pipe();
extern uint64 sys_read();
extern uint64 sys_sbrk();
extern uint64 sys_link();
extern uint64 sys_unlink();
extern uint64 sys_write();
extern void   bfree();
extern void   clockintr();
extern void   devintr();
extern void   kerneltrap();
extern void   usertrap();
extern void   install_trans();
extern void   write_head();

// bio.c
const uint64 bfree_start  = (uint64)bfree,  bfree_end  = (uint64)bfree  + 120;
const uint64 brelse_start = (uint64)brelse, brelse_end = (uint64)brelse + 276;

// console.c
const uint64 consoleintr_start  = (uint64)consoleintr,  consoleintr_end  = (uint64)consoleintr  + 412;
const uint64 consoleread_start  = (uint64)consoleread,  consoleread_end  = (uint64)consoleread  + 262;
const uint64 consolewrite_start = (uint64)consolewrite, consolewrite_end = (uint64)consolewrite + 158;

// exec.c
const uint64 exec_start = (uint64)exec, exec_end = (uint64)exec +  898;

// file.c
const uint64 filealloc_start = (uint64)filealloc,  filealloc_end = (uint64)filealloc + 140;
const uint64 fileclose_start = (uint64)fileclose,  fileclose_end = (uint64)fileclose + 314;

// fs.c
const uint64 dirlink_start = (uint64)dirlink, dirlink_end = (uint64)dirlink + 190;
const uint64 idup_start    = (uint64)idup,    idup_end    = (uint64)idup    +  64;
const uint64 iput_start    = (uint64)iput,    iput_end    = (uint64)iput    + 182;
const uint64 iupdate_start = (uint64)iupdate, iupdate_end = (uint64)iupdate + 138;
const uint64 readsb_start  = (uint64)readsb,  readsb_end  = (uint64)readsb  +  64;
const uint64 writei_start  = (uint64)writei,  writei_end  = (uint64)writei  + 280;
const uint64 fsinit_start  = (uint64)fsinit,  fsinit_end  = (uint64)fsinit  +  90;

// kalloc.c
const uint64 kalloc_start = (uint64)kalloc, kalloc_end = (uint64)kalloc + 286;
const uint64 kfree_start  = (uint64)kfree,  kfree_end  = (uint64)kfree  + 238;

// log.c
const uint64 begin_op_start   = (uint64)begin_op,   begin_op_end   = (uint64)begin_op   + 322;
const uint64 end_op_start     = (uint64)end_op,     end_op_end     = (uint64)end_op     + 282;
const uint64 log_write_start  = (uint64)log_write,  log_write_end  = (uint64)log_write  + 372;
const uint64 write_head_start = (uint64)write_head, write_head_end = (uint64)write_head + 386;
const uint64 write_log_start  = (uint64)write_log,  write_log_end  = (uint64)write_log  + 204;
const uint64 commit_start     = (uint64)commit,     commit_end     = (uint64)commit     + 138;
const uint64 install_trans_start = (uint64)install_trans, install_trans_end = (uint64)install_trans + 410; 

// pipe.c
const uint64 pipealloc_start = (uint64)pipealloc,  pipealloc_end = (uint64)pipealloc + 312;
const uint64 pipeclose_start = (uint64)pipeclose,  pipeclose_end = (uint64)pipeclose + 146;
const uint64 pipewrite_start = (uint64)pipewrite,  pipewrite_end = (uint64)pipewrite + 268;
const uint64 piperead_start  = (uint64)piperead,   piperead_end  = (uint64)piperead  + 234;

// printf.c
const uint64 printf_start = (uint64)printf, printf_end = (uint64)printf + 472;
const uint64 panic_start  = (uint64)panic,  panic_end  = (uint64)panic  +  76;

// proc.c
const uint64 allocproc_start = (uint64)allocproc,  allocproc_end = (uint64)allocproc + 222;
const uint64 exit_start      = (uint64)exit,       exit_end      = (uint64)exit      + 262;
const uint64 fork_start      = (uint64)fork,       fork_end      = (uint64)fork      + 268;
const uint64 freeproc_start  = (uint64)freeproc,   freeproc_end  = (uint64)freeproc  +  96;
const uint64 procinit_start  = (uint64)procinit,   procinit_end  = (uint64)procinit  + 204;

// sleeplock.c
const uint64 acquiresleep_start = (uint64)acquiresleep, acquiresleep_end = (uint64)acquiresleep + 84;

// spinlock.c
const uint64 acquire_start = (uint64)acquire, acquire_end = (uint64)acquire + 136;
const uint64 holding_start = (uint64)holding, holding_end = (uint64)holding +  47;
const uint64 release_start = (uint64)release, release_end = (uint64)release +  65;

// sysfile.c
const uint64 sys_read_start   = (uint64)sys_read,   sys_read_end   = (uint64)sys_read   + 134;
const uint64 sys_write_start  = (uint64)sys_write,  sys_write_end  = (uint64)sys_write  + 152;
const uint64 sys_close_start  = (uint64)sys_close,  sys_close_end  = (uint64)sys_close  + 126;
const uint64 sys_fstat_start  = (uint64)sys_fstat,  sys_fstat_end  = (uint64)sys_fstat  + 112;
const uint64 sys_link_start   = (uint64)sys_link,   sys_link_end   = (uint64)sys_link   + 390;
const uint64 sys_unlink_start = (uint64)sys_unlink, sys_unlink_end = (uint64)sys_unlink + 464;
const uint64 create_start     = (uint64)create,     create_end     = (uint64)create     + 356;
const uint64 sys_open_start   = (uint64)sys_open,   sys_open_end   = (uint64)sys_open   + 376;
const uint64 sys_chdir_start  = (uint64)sys_chdir,  sys_chdir_end  = (uint64)sys_chdir  + 168;
const uint64 sys_exec_start   = (uint64)sys_exec,   sys_exec_end   = (uint64)sys_exec   + 296;
const uint64 sys_pipe_start   = (uint64)sys_pipe,   sys_pipe_end   = (uint64)sys_pipe   + 430;

// sysproc.c
const uint64 sys_sbrk_start = (uint64)sys_sbrk, sys_sbrk_end = (uint64)sys_sbrk + 70;

// trap.c
const uint64 clockintr_start  = (uint64)clockintr,  clockintr_end  = (uint64)clockintr  +  68;
const uint64 kerneltrap_start = (uint64)kerneltrap, kerneltrap_end = (uint64)kerneltrap + 310;
const uint64 usertrap_start   = (uint64)usertrap,   usertrap_end   = (uint64)usertrap   + 254;
const uint64 devintr_start    = (uint64)devintr,    devintr_end    = (uint64)devintr    + 162;
const uint64 kernelvec_start  = (uint64)kernelvec,  kernelvec_end  = (uint64)kernelvec  + 142;
const uint64 nmivec_start     = (uint64)nmivec,     nmivec_end     = (uint64)nmivec     + 142;

// vm.c
const uint64 kvminit_start  = (uint64)kvminit,  kvminit_end  = (uint64)kvminit  + 208;
const uint64 uvmunmap_start = (uint64)uvmunmap, uvmunmap_end = (uint64)uvmunmap + 206;
const uint64 uvmalloc_start = (uint64)uvmalloc, uvmalloc_end = (uint64)uvmalloc + 422;
const uint64 uvmcopy_start  = (uint64)uvmcopy,  uvmcopy_end  = (uint64)uvmcopy  + 430;

// vritio_disk.c
const uint64 virtio_disk_intr_start = (uint64)virtio_disk_intr, virtio_disk_intr_end = (uint64)virtio_disk_intr + 208;
