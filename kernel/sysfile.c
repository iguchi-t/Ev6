//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
#include "recovery_locking.h"


// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  if(argint(n, &fd) < 0)
    return -1;
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  enter_recovery_critical_section_nodes(RL_FLAG_FILE, f);
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void)
{
  struct file *f = 0x0;
  int fd;

  enter_recovery_critical_section(RL_FLAG_FTABLE, 0);
  if(argfd(0, 0, &f) < 0){
    exit_recovery_critical_section(RL_FLAG_FTABLE, 0);
    return -1;
  }
  if((fd=fdalloc(f)) < 0){
    exit_recovery_critical_section_nodes(RL_FLAG_FILE, f);
    exit_recovery_critical_section(RL_FLAG_FTABLE, 0);
    return -1;
  }
  filedup(f);
  exit_recovery_critical_section_nodes(RL_FLAG_FILE, f);
  exit_recovery_critical_section(RL_FLAG_FTABLE, 0);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f = 0x0;
  int n;
  uint64 p;

  enter_recovery_critical_section(RL_FLAG_FTABLE, 0);
  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0){
    if(f != 0x0)
      exit_recovery_critical_section_nodes(RL_FLAG_FILE, f);
    exit_recovery_critical_section(RL_FLAG_FTABLE, 0);
    return -1;
  }
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f = 0x0;
  int n;
  uint64 p;

  enter_recovery_critical_section(RL_FLAG_FTABLE, 0);
  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0){
    if(f != 0x0)
      exit_recovery_critical_section_nodes(RL_FLAG_FILE, f);
    exit_recovery_critical_section(RL_FLAG_FTABLE, 0);
    return -1;
  }

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  enter_recovery_critical_section(RL_FLAG_FTABLE, 0);
  if(argfd(0, &fd, &f) < 0){
    exit_recovery_critical_section(RL_FLAG_FTABLE, 0);
    return -1;
  }
  
  myproc()->ofile[fd] = 0;
  fileclose(f);
  exit_recovery_critical_section(RL_FLAG_FTABLE, 0);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  enter_recovery_critical_section(RL_FLAG_FTABLE, 0);
  if(argfd(0, 0, &f) < 0 || argaddr(1, &st) < 0){
    if(f != 0x0)
      exit_recovery_critical_section_nodes(RL_FLAG_FILE, f);
    exit_recovery_critical_section(RL_FLAG_FTABLE, 0);
    return -1;
  }
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  enter_recovery_critical_section(RL_FLAG_ICACHE, 0);
  if((ip = namei(old)) == 0){
    exit_recovery_critical_section(RL_FLAG_ICACHE, 0);
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    exit_recovery_critical_section(RL_FLAG_ICACHE, 0);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);
  exit_recovery_critical_section(RL_FLAG_ICACHE, 0);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  exit_recovery_critical_section(RL_FLAG_ICACHE, 0);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0){
      return 0;
    }

  }
  return 1;
}

uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  enter_recovery_critical_section(RL_FLAG_ICACHE, 0);
  if((dp = nameiparent(path, name)) == 0){
    exit_recovery_critical_section(RL_FLAG_ICACHE, 0);
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  exit_recovery_critical_section(RL_FLAG_ICACHE, 0);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  exit_recovery_critical_section(RL_FLAG_ICACHE, 0);
  end_op();
  return -1;
}

struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE)){
      return ip;
    }
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0)
    panic("create: ialloc");

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    dp->nlink++;  // for ".."
    iupdate(dp);
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      panic("create dots");
  }

  if(dirlink(dp, name, ip->inum) < 0)
    panic("create: dirlink");

  iunlockput(dp);

  return ip;
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  if((n = argstr(0, path, MAXPATH)) < 0 || argint(1, &omode) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE){
    enter_recovery_critical_section(RL_FLAG_ICACHE, 0);
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      exit_recovery_critical_section(RL_FLAG_ICACHE, 0);
      end_op();
      return -1;
    }
  } else {
    enter_recovery_critical_section(RL_FLAG_ICACHE, 0);
    if((ip = namei(path)) == 0){
      end_op();
      exit_recovery_critical_section(RL_FLAG_ICACHE, 0);
      return -1;
    }
    ilock(ip);

    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      exit_recovery_critical_section(RL_FLAG_ICACHE, 0);
      end_op();
      return -1;
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
    iunlockput(ip);
    exit_recovery_critical_section(RL_FLAG_ICACHE, 0);
    end_op();
    return -1;
  }

  enter_recovery_critical_section(RL_FLAG_FTABLE, 0);
  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    exit_recovery_critical_section(RL_FLAG_FTABLE, 0);
    iunlockput(ip);
    exit_recovery_critical_section(RL_FLAG_ICACHE, 0);
    end_op();
    return -1;
  }

  if(ip->type == T_DEVICE){
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
  exit_recovery_critical_section_nodes(RL_FLAG_FILE, f);
  exit_recovery_critical_section(RL_FLAG_FTABLE, 0);

  iunlock(ip);
  exit_recovery_critical_section_nodes(RL_FLAG_INODE, ip);
  exit_recovery_critical_section(RL_FLAG_ICACHE, 0);
  end_op();

  return fd;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  enter_recovery_critical_section(RL_FLAG_ICACHE, 0);
  if(argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    exit_recovery_critical_section(RL_FLAG_ICACHE, 0);
    end_op();
    return -1;
  }
  iunlockput(ip);
  exit_recovery_critical_section(RL_FLAG_ICACHE, 0);
  end_op();
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  enter_recovery_critical_section(RL_FLAG_ICACHE, 0);
  if((argstr(0, path, MAXPATH)) < 0 ||
     argint(1, &major) < 0 ||
     argint(2, &minor) < 0 ||
     (ip = create(path, T_DEVICE, major, minor)) == 0){
    exit_recovery_critical_section(RL_FLAG_ICACHE, 0);
    end_op();
    return -1;
  }
  iunlockput(ip);
  exit_recovery_critical_section(RL_FLAG_ICACHE, 0);
  end_op();
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();
  
  begin_op();
  enter_recovery_critical_section(RL_FLAG_ICACHE, 0);
  if(argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0){
    exit_recovery_critical_section(RL_FLAG_ICACHE, 0);
    end_op();
    return -1;
  }
  ilock(ip);

  if(ip->type != T_DIR){
    iunlockput(ip);
    exit_recovery_critical_section(RL_FLAG_ICACHE, 0);
    end_op();
    return -1;
  }
  iunlock(ip);
  exit_recovery_critical_section_nodes(RL_FLAG_INODE, ip);
  iput(p->cwd);
  exit_recovery_critical_section(RL_FLAG_ICACHE, 0);
  
  end_op();
  p->cwd = ip;
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  if(argstr(0, path, MAXPATH) < 0 || argaddr(1, &uargv) < 0){
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){
      goto bad;
    }
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if(argv[i] == 0)
      panic("sys_exec kalloc");
    if(fetchstr(uarg, argv[i], PGSIZE) < 0){
      goto bad;
    }
  }

  int ret = exec(path, argv);

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

 bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  enter_recovery_critical_section(RL_FLAG_FTABLE, 0);
  if(argaddr(0, &fdarray) < 0){
    exit_recovery_critical_section(RL_FLAG_FTABLE, 0);
    return -1;
  }
  if(pipealloc(&rf, &wf) < 0){
    exit_recovery_critical_section(RL_FLAG_FTABLE, 0);
    return -1;
  }
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0){
      p->ofile[fd0] = 0;
    }
    fileclose(rf);
    fileclose(wf);
    exit_recovery_critical_section(RL_FLAG_FTABLE, 0);
    return -1;
  }
  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    exit_recovery_critical_section(RL_FLAG_FTABLE, 0);
    return -1;
  }
  exit_recovery_critical_section_nodes(RL_FLAG_FILE, rf);
  exit_recovery_critical_section_nodes(RL_FLAG_FILE, wf);
  exit_recovery_critical_section(RL_FLAG_FTABLE, 0);
  return 0;
}