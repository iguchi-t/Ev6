/*
 * Ev6 related system calls.
 */

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "fcntl.h"
#include "stat.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "fs.h"
#include "file.h"
#include "mlist.h"
#include "recovery_locking.h"
#include "after-treatment.h"
#include "usercoop.h"


extern struct ftable *ftable;
extern int recovery_mode;  // Recovery mode in recovery handlers and mlist_tracker.

struct open_arg_table oa_table[NPROC];
int dup_offset[NFILE];  // Duplication of struct file's offset.
struct spinlock oa_table_lock;


/* Switch recovery mode.
 * - Aggressive Mode  : 0. recovery handler tries to manage to survive the system(default).
 * - Conservative Mode: 1. recovery handler avoids inconsistent state in the system in recovery.
 */
int sys_change_recovery_mode(void){
  int i;

  if(argint(0, &i) < 0)
    return -1;

  if(i == 0){
    printf("Set recovery mode to 'Aggressive' mode.\n");
    recovery_mode = AGGRESSIVE;
  } else {
    printf("Set recovery mode to 'Conservative' mode.\n");
    recovery_mode = CONSERVATIVE;
  }

  return 0;
}


/*
 * Recording some values (path, omode, offset) to perform Re-Open.
 */
// Record arguments.
void
rec_open_args(int fd, char *path, int omode)
{
  int pid = myproc()->pid;
  struct open_arg_table *oat = 0x0;

  acquire(&oa_table_lock);

  for (int i = 0; i < NPROC; i++) {
    if (oat == 0x0 && oa_table[i].pid == 0) {
      oat = &oa_table[i];  // Memorize empty slot.
    } else if (oa_table[i].pid == pid) {
      oat = &oa_table[i];
      break;
    }
  }

  release(&oa_table_lock);
  
  if (oat->pid == 0) {
    oat->pid = pid;
  } else if (oat == 0x0) {
    panic("No empty open argument table");
  }
 
  strncpy(oat->args[fd].path, path, strlen(path));
  oat->args[fd].omode = omode;
}

// Delete recorded arguments of the fd.
void
del_open_args(int fd)
{
  int pid = myproc()->pid;
  struct open_arg_table *oat = 0x0;

  acquire(&oa_table_lock);
  for (int i = 0; i < NPROC; i++) {
    if (oa_table[i].pid == pid) {
      oat = &oa_table[i];
      break;
    }
  }
  release(&oa_table_lock);

  if (oat == 0x0) {
    panic("Invalid file descriptor was passed.");
  }
  
  memset(&oat->args[fd], 0, sizeof(struct open_args));
}

// Copy the open_arg_table entry for dup system call.
void
copy_open_args(int to, struct file *f)
{
  struct proc *p = myproc();
  struct open_arg_table *oat = 0x0;
  int from = -1;

  acquire(&oa_table_lock);
  for (int i = 0; i < NPROC; i++) {
    if (oa_table[i].pid == p->pid) {
      oat = &oa_table[i];
      break;
    }
  }
  release(&oa_table_lock);
 
  for (from = 0; from < NOFILE; from++) {
    if (p->ofile[from] == f) {
      break;
    }
  }

  if (oat == 0x0 || from < 0) {
    panic("Invalid file descriptor or file pointer were passed.");
  }

  strncpy(oat->args[to].path, oat->args[from].path, strlen(oat->args[from].path));
  oat->args[to].omode = oat->args[from].omode;
}

// Free the proc's open_arg_table.
void
free_open_arg_table(void)
{
  int pid = myproc()->pid;

  acquire(&oa_table_lock);

  for(int i = 0; i < NPROC; i++){
    if(oa_table[i].pid == pid){
      memset(&oa_table[i], 0, sizeof(struct open_arg_table));
      release(&oa_table_lock);
      return;
    }
  }

  release(&oa_table_lock);

  // Note that this function can be called in duplicate 
  // when Ev6 recovers in the context of exit() due to After-Treatment choice (i.e. Process Kill).
  printf("\nCAUTION: No corresponding open_arg_table to free due to recovery or inappropriate open_arg_table creation. It is recommended to confirm the cause.\n\n");
}

// Copy open_arg_table contents from fp to tp.
void
copy_open_arg_table(struct proc *fp, struct proc *tp)
{
  struct open_arg_table *from = 0x0, *to = 0x0;

  acquire(&oa_table_lock);

  for (int i = 0; i < NPROC; i++) {
    if (to == 0x0 && oa_table[i].pid == 0) {
      to = &oa_table[i];  // Memorize empty slot for the fork destination proc.
    } else if(oa_table[i].pid == fp->pid) {
      from = &oa_table[i];
    }

    if (to != 0x0 && from != 0x0) {  // Both are found.
      break;
    }
  }

  if (to == 0x0 || from == 0x0) {
    panic("copy_open_arg_table: copy destination/source table was not found.");
  }

  memmove(to, from, sizeof(struct open_arg_table));
  to->pid = tp->pid;
  release(&oa_table_lock);
}

// Update duplicated offset.
void
update_dup_off(struct file *t_fp, int off)
{
  for(int i = 0; i < NFILE; i++){
    if(t_fp == &ftable->file[i])
      dup_offset[i] = off;
  }
}

/*
 * Syscall reopen.
 * reopen() is for reopen cleared files because of recovery handling.
 */
// Reopen a cleared file into the specified fd.
// reopen() doesn't call create() because recovery handling of struct file and struct inode 
// does not clear inode on the disk, so there is no need to create inode on the disk.
static struct file*
do_reopen(int fd, char *path, int omode, int n)
{
  struct file *f;
  struct inode *ip;
  struct proc *p = myproc();

  begin_op();

  if((ip = namei(path)) == 0)
    panic("Can't find the inode of reopening file");

  ilock(ip);
  if (ip->type == T_DIR && omode != O_RDONLY)
    goto bad;

  if (ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV))
    goto bad;

  if ((f = filealloc()) == 0) {
    fileclose(f);
    goto bad;
  }
  p->ofile[fd] = f;  

  if (ip->type == T_DEVICE) {
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    for (int i = 0; i < NFILE; i++) {
      if (f == &ftable->file[i]) {
        f->off = dup_offset[i];
      }
    }
  }

  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
  exit_recovery_critical_section_nodes(RL_FLAG_FILE, f);

  iunlock(ip);
  exit_recovery_critical_section_nodes(RL_FLAG_INODE, ip);
  end_op();

  return f;

bad:
  iunlockput(ip);
  end_op();
  p->ofile[fd] = 0;
  del_open_args(fd);
  return (struct file*)-1;
}

// Reopen all of Reserved entries in the fd table.
uint64
sys_reopen(void)
{
  char path[MAXPATH];
  int nextfd, ret = 0;
  struct file *f;
  struct open_arg_table *oat = 0x0;
  struct proc *p = myproc();

  for(int i = 0; i < NPROC; i++){
    if(p->pid == oa_table[i].pid)
      oat = &oa_table[i];
  }

  enter_recovery_critical_section(RL_FLAG_FTABLE, 0);
  enter_recovery_critical_section(RL_FLAG_ICACHE, 0);

  // In single iteration, it handle one struct file's node.
  do {
    nextfd = 0;
    f = 0x0;

    for (int fd = 0; fd < NOFILE; fd++) {
      if (p->ofile[fd] == (struct file*)RESERVED) {
        if (f == 0x0) {  // not reopen yet
          if ((f = do_reopen(fd, oat->args[fd].path, oat->args[fd].omode, strlen(oat->args[fd].path))) < 0) {
            ret = -1;
          } else {
            strncpy(path, oat->args[fd].path, MAXPATH);
          }
        } else if (f > 0x0) {
          if (strncmp(path, oat->args[fd].path, MAXPATH) == 0) {
            p->ofile[fd] = f;
            enter_recovery_critical_section_nodes(RL_FLAG_FILE, f);
            filedup(f);
            exit_recovery_critical_section_nodes(RL_FLAG_FILE, f);
          } else {
            nextfd = fd;  // Reopen in the next iteration.
          }
        }
      }
    }
  } while (nextfd != 0);

  exit_recovery_critical_section(RL_FLAG_ICACHE, 0);
  exit_recovery_critical_section(RL_FLAG_FTABLE, 0);

  return ret;
}

uint64
check_reserved_fd(struct proc *p, int fd)
{
  if (p->ofile[fd] == (struct file*)RESERVED)
    return sys_reopen();

  return 0;
}

uint64
sys_check_reserved_fd(void)
{
  int fd;

  if (argint(0, &fd) < 0)
    return -1;

  return check_reserved_fd(myproc(), fd);
}

uint64
sys_check_reserved_fd_all(void)
{
  struct proc *p = myproc();
  int res = 0;

  for (int fd = 0; fd < NOFILE; fd++) {
    res += check_reserved_fd(p, fd);
  }
  
  return res;
}

uint64
sys_pick_fd(void)
{
  char path[MAXPATH];
  int omode, n, pid = myproc()->pid;
  struct open_arg_table *oat = 0x0;

  if ((n = argstr(0, path, MAXPATH)) < 0 || argint(1, &omode) < 0)
    return -1;

  acquire(&oa_table_lock);
  for (int i = 0; i < NPROC; i++) {
    if (oa_table[i].pid == pid) {
      oat = &oa_table[i];
      break;
    }
  }
  release(&oa_table_lock);
  
  if (oat == 0x0) {
    panic("No empty open argument table");
  }
 
  for (int fd = 0; fd < NOFILE; fd++) {
    if (strncmp(oat->args[fd].path, path, n) == 0 && oat->args[fd].omode == omode) {
      return fd;
    }
  }

  return -1;  // not found
}

