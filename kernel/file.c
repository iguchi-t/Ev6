//
// Support functions for system calls that involve file descriptors.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "stat.h"
#include "proc.h"
#include "mlist.h"
#include "recovery_locking.h"

struct devsw _devsw[NDEV];
struct devsw *devsw = _devsw;
struct ftable _ftable;
struct ftable *ftable = &_ftable;

extern struct mlist_header mlist;

void
fileinit(void)
{
  initlock(&ftable->lock, "ftable");

  struct file *fp;
  for(fp = ftable->file; fp < ftable->file + NFILE; fp++){
    register_memobj(fp, mlist.fil_list);
  }
}

// Allocate a file structure.
struct file*
filealloc(void)
{
  struct file *f;

  acquire(&ftable->lock);
  for(f = ftable->file; f < ftable->file + NFILE; f++){
    enter_recovery_critical_section_nodes(RL_FLAG_FILE, f);
    if(f->ref == 0){

      f->ref = 1;
      release(&ftable->lock);
      return f;
    }
    exit_recovery_critical_section_nodes(RL_FLAG_FILE, f);
  }
  release(&ftable->lock);
  return 0;
}

// Increment ref count for file f.
struct file*
filedup(struct file *f)
{
  acquire(&ftable->lock);

  if(f->ref < 1)
    panic("filedup");
  f->ref++;
  release(&ftable->lock);
  return f;
}

// Close file f.  (Decrement ref count, close when reaches 0.)
void
fileclose(struct file *f)
{
  struct file ff;

  acquire(&ftable->lock);

  if(f->ref < 1){
    panic("fileclose");
  }
  if(--f->ref > 0){
    release(&ftable->lock);
    exit_recovery_critical_section_nodes(RL_FLAG_FILE, f);
    return;
  }
  ff = *f;
  f->ref = 0;
  f->type = FD_NONE;
  release(&ftable->lock);

  if(ff.type == FD_PIPE){
    enter_recovery_critical_section(RL_FLAG_PIPE, 0);
    pipeclose(ff.pipe, ff.writable);
    exit_recovery_critical_section(RL_FLAG_PIPE, 0);
  } else if(ff.type == FD_INODE || ff.type == FD_DEVICE){
    begin_op();
    enter_recovery_critical_section(RL_FLAG_ICACHE, 0);
    enter_recovery_critical_section_nodes(RL_FLAG_INODE, ff.ip);
    iput(ff.ip);
    exit_recovery_critical_section(RL_FLAG_ICACHE, 0);
    end_op();
  }
  exit_recovery_critical_section_nodes(RL_FLAG_FILE, f);
}

// Get metadata about file f.
// addr is a user virtual address, pointing to a struct stat.
int
filestat(struct file *f, uint64 addr)
{
  struct proc *p = myproc();
  struct stat st;
  
  if(f->type == FD_INODE || f->type == FD_DEVICE){
    enter_recovery_critical_section(RL_FLAG_ICACHE, 0);
    enter_recovery_critical_section_nodes(RL_FLAG_INODE, f->ip);
    ilock(f->ip);
    stati(f->ip, &st);
    iunlock(f->ip);
    exit_recovery_critical_section_nodes(RL_FLAG_INODE, f->ip);
    exit_recovery_critical_section(RL_FLAG_ICACHE, 0);

    if(copyout(p->pagetable, addr, (char *)&st, sizeof(st)) < 0){
      exit_recovery_critical_section_nodes(RL_FLAG_FILE, f);
      exit_recovery_critical_section(RL_FLAG_FTABLE, 0);
      return -1;
    }
    exit_recovery_critical_section_nodes(RL_FLAG_FILE, f);
    exit_recovery_critical_section(RL_FLAG_FTABLE, 0);
    return 0;
  }
  exit_recovery_critical_section_nodes(RL_FLAG_FILE, f);
  exit_recovery_critical_section(RL_FLAG_FTABLE, 0);
  return -1;
}

// Read from file f.
// addr is a user virtual address.
int
fileread(struct file *f, uint64 addr, int n)
{
  int r = 0;

  if(f->readable == 0){
    exit_recovery_critical_section_nodes(RL_FLAG_FILE, f);
    exit_recovery_critical_section(RL_FLAG_FTABLE, 0);
    return -1;
  }

  if(f->type == FD_PIPE){
    enter_recovery_critical_section(RL_FLAG_PIPE, 0);
    r = piperead(f->pipe, addr, n);
    exit_recovery_critical_section(RL_FLAG_PIPE, 0);
  } else if(f->type == FD_DEVICE){
    if(f->major < 0 || f->major >= NDEV || !devsw[f->major].read){
      exit_recovery_critical_section_nodes(RL_FLAG_FILE, f);
      exit_recovery_critical_section(RL_FLAG_FTABLE, 0);
      return -1;
    }
    r = devsw[f->major].read(1, addr, n);
  } else if(f->type == FD_INODE){
    enter_recovery_critical_section(RL_FLAG_ICACHE, 0);
    enter_recovery_critical_section_nodes(RL_FLAG_INODE, f->ip);
    ilock(f->ip);
    if((r = readi(f->ip, 1, addr, f->off, n)) > 0)
      f->off += r;
    iunlock(f->ip);
    exit_recovery_critical_section_nodes(RL_FLAG_INODE, f->ip);
    exit_recovery_critical_section(RL_FLAG_ICACHE, 0);
  } else {
    panic("fileread");
  }

  exit_recovery_critical_section_nodes(RL_FLAG_FILE, f);
  exit_recovery_critical_section(RL_FLAG_FTABLE, 0);
  return r;
}

// Write to file f.
// addr is a user virtual address.
int
filewrite(struct file *f, uint64 addr, int n)
{
  int r, ret = 0;

  if(f->writable == 0){
    exit_recovery_critical_section_nodes(RL_FLAG_FILE, f);
    exit_recovery_critical_section(RL_FLAG_FTABLE, 0);
    return -1;
  }

  if(f->type == FD_PIPE){
    enter_recovery_critical_section(RL_FLAG_PIPE, 0);
    ret = pipewrite(f->pipe, addr, n);
    exit_recovery_critical_section(RL_FLAG_PIPE, 0);
  } else if(f->type == FD_DEVICE){
    if(f->major < 0 || f->major >= NDEV || !devsw[f->major].write){
      exit_recovery_critical_section_nodes(RL_FLAG_FILE, f);
      exit_recovery_critical_section(RL_FLAG_FTABLE, 0);
      exit_recovery_critical_section(RL_FLAG_ICACHE, 0);
      return -1;
    }
    ret = devsw[f->major].write(1, addr, n);
  } else if(f->type == FD_INODE){
    // write a few blocks at a time to avoid exceeding
    // the maximum log transaction size, including
    // i-node, indirect block, allocation blocks,
    // and 2 blocks of slop for non-aligned writes.
    // this really belongs lower down, since writei()
    // might be writing a device like the console.
    int max = ((MAXOPBLOCKS-1-1-2) / 2) * BSIZE;
    int i = 0;
    while(i < n){
      int n1 = n - i;
      if(n1 > max)
        n1 = max;

      begin_op();
      enter_recovery_critical_section(RL_FLAG_ICACHE, 0);
      enter_recovery_critical_section_nodes(RL_FLAG_INODE, f->ip);
      ilock(f->ip);
      if ((r = writei(f->ip, 1, addr + i, f->off, n1)) > 0)
        f->off += r;
      iunlock(f->ip);
      exit_recovery_critical_section_nodes(RL_FLAG_INODE, f->ip);
      exit_recovery_critical_section(RL_FLAG_ICACHE, 0);
      end_op();

      if(r < 0)
        break;
      if(r != n1)
        panic("short filewrite");
      i += r;
    }
    ret = (i == n ? n : -1);
  } else {
    panic("filewrite");
  }

  exit_recovery_critical_section_nodes(RL_FLAG_FILE, f);
  exit_recovery_critical_section(RL_FLAG_FTABLE, 0);
  return ret;
}