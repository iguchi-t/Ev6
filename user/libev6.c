#include "kernel/types.h"
#include "kernel/after-treatment.h"
#include "user/user.h"

#define GET_REQUEST(x) (-x)


/* System administrator can choose user cooperation validation granularity
 * from userland-level or syscall-level.
 * 
 * userland-level: call setup_ev6_cooperation() in booting user phase.
 * Ev6 think that whole of system calls are supported by userland cooperation.
 *
 * syscall-level: activate the cooperation before calling syscall and deactivate after calling.
 * This policy allows that some unsupported syscalls exist.
 */
/*int
setup_ev6_cooperation(void)
{
  // Tell cooperation with userland is active to Ev6 kernel.
  enable_user_coop();
}*/

/*
 * Ev6 Syscall Wrappers.
 *
 * Syscall Wrapper template
 * return -2 means that issued syscall is failed due to ECC-uncorrectable error.
int
ev6_syscall(void)
{
  int res;

  if(check_reserved_fd_all() < 0)
    return -1;

  disable_user_coop();
     
  if((res = syscall()) >= -1)
    return res;  // Normal results.

  // Recovery handler requests additional operation.
  switch(GET_REQUEST(res)){
    case REOPEN_SYSCALL_FAIL:
      if(reopen() < 0){
        printf("Fail to Re-Open some files which was cleared in recovery.\n");
        disable_user_coop();
        return -2;
      }
    case SYSCALL_FAIL:
      disable_user_coop();
      return -2;
    case REOPEN_SYSCALL_REDO:
      if(reopen() < 0){
        printf("Fail to Re-Open some files which was cleared in recovery.\n");
        disable_user_coop();
        return -2;
      }
    case SYSCALL_REDO:
      disable_user_coop();
      return ev6_syscall();
    default:
      printf("Invalid After-Treatment policy is returned.\n");
      ev6_exit(-1);
  }
}
*/

int
ev6_fork(void)
{
  int res;  

  if(check_reserved_fd_all() < 0)
    return -1;

  enable_user_coop();

  if((res = fork()) >= -2){
    disable_user_coop();
    return res;
  }

  switch(GET_REQUEST(res)){
    case SYSCALL_FAIL:
      disable_user_coop();
      printf("ev6_fork: fork() failed due to ECC-uncorrectable Error.\n");
      return -2;
    case REOPEN_SYSCALL_REDO:
      if(reopen() < 0){
        printf("Fail to Re-Open some files which was cleared in recovery.\n");
        disable_user_coop();
        return -2;
      }
    case SYSCALL_REDO:
      printf("ev6_fork: Redo fork().\n");
      return ev6_fork();
    default:
      printf("ev6_fork: Invalid After-Treatment policy is returned.\n");
      ev6_exit(-1);
  }
  
  return -1;  // Can't reach here.
}

int
ev6_exit(int status)
{
  if(check_reserved_fd_all() < 0)
    return -1;

  return exit(status);  // Recovery handler select Process Kill or Fail-Stop even if under user app cooperation.
}

int
ev6_wait(int *xstatus)
{
  int res;  

  if(check_reserved_fd_all() < 0)
    return -1;

  enable_user_coop();

  if((res = wait(xstatus)) >= -2){
    disable_user_coop();
    return res;
  }

  switch(GET_REQUEST(res)){
    case SYSCALL_FAIL:
      disable_user_coop();
      printf("ev6_wait: wait() failed due to ECC-uncorrectable Error.\n");
      return -2;
    case SYSCALL_REDO:
      printf("ev6_wait: Redo wait().\n");
      return ev6_wait(xstatus);
    default:
      printf("ev6_wait: Invalid After-Treatment policy is returned.\n");
      ev6_exit(-1);
  }

  return -1;  // Can't reach here.
}

int
ev6_pipe(int *fds)
{
  int res;

  if(check_reserved_fd_all() < 0)
    return -1;
     
  enable_user_coop();
  
  if((res = pipe(fds) )>= -2){
    disable_user_coop();
    return res;
  }

  switch(GET_REQUEST(res)){
    case SYSCALL_FAIL:
      disable_user_coop();
      printf("ev6_pipe: pipe() failed due to ECC-uncorrectable Error.\n");
      return -2;
    case REOPEN_SYSCALL_REDO:
      if(reopen() < 0){
        printf("Fail to Re-Open some files which was cleared in recovery.\n");
        disable_user_coop();
        return -2;
      }
    case SYSCALL_REDO:
      printf("ev6_pipe: Redo pipe().\n");
      return ev6_pipe(fds);
    default:
      printf("ev6_pipe: Invalid After-Treatment policy is returned.\n");
      ev6_exit(-1);
  }

  return -1;  // Can't reach here.
}

int ev6_write(int fd, const void *buf, int size)
{
  int res;

  if(check_reserved_fd(fd) < 0)
    return -1;

  enable_user_coop();

  if((res = write(fd, buf, size)) >= -2){
    disable_user_coop();
    return res;
  }

  switch(GET_REQUEST(res)){
    case REOPEN_SYSCALL_FAIL:
      if(reopen() < 0){
        printf("Fail to Re-Open some files which was cleared in recovery.\n");
        disable_user_coop();
        return -2;
      }
    case SYSCALL_FAIL:
      disable_user_coop();
      printf("ev6_write: write() failed due to ECC-uncorrectable Error.\n");
      return -2;
    case SYSCALL_REDO:
      printf("ev6_write: Redo write().\n");
      return ev6_write(fd, buf, size);
    default:
      printf("ev6_write: Invalid After-Treatment policy is returned.\n");
      ev6_exit(-1);
  }

  return -1;  // Can't reach here.
}

int
ev6_read(int fd, void *buf, int size)
{
  int res;

  enable_user_coop();

  if(check_reserved_fd(fd) < 0)
    return -1;

  if((res = read(fd, buf, size)) >= -2){
    disable_user_coop();
    return res;
  }

  switch(GET_REQUEST(res)){
    case SYSCALL_FAIL:
      disable_user_coop();
      printf("ev6_read: read() failed due to ECC-uncorrectable Error.\n");
      return -2;
    case REOPEN_SYSCALL_REDO:
      if(reopen() < 0){
        printf("fail to re-open some files which was cleared in recovery.\n");
        disable_user_coop();
        return -2;
      }
    case SYSCALL_REDO:
      printf("ev6_read: Redo read().\n");
      return ev6_read(fd, buf, size);
    default:
      printf("ev6_read: Invalid After-Treatment policy is returned.\n");
      ev6_exit(-1);
  }

  return -1;  // Can't reach here.
}

int
ev6_close(int fd)
{
  if(check_reserved_fd(fd) < 0)
    return -1;

  return close(fd);  // Recovery handler only returns Syscall Fail or Syscall Success as a syscall result.
}

int
ev6_kill(int pid)
{
  return kill(pid);  // kill() only uses out-of-recovery-target objects.
}

int
ev6_exec(char *path, char **args)
{
  int res;

  enable_user_coop();
     
  if((res = exec(path, args)) >= -2){
    disable_user_coop();
    return res;
  }

  switch(GET_REQUEST(res)){
    case SYSCALL_FAIL:
      disable_user_coop();
      printf("ev6_exec: exec() failed due to ECC-uncorrectable Error.\n");
      return -2;
    case SYSCALL_REDO:
      printf("ev6_exec: Redo exec().\n");
      return ev6_exec(path, args);
    default:
      printf("ev6_exec: Invalid After-Treatment policy is returned.\n");
      ev6_exit(-1);
  }

  return -1;  // Can't reach here.
}

int
ev6_open(const char *path, int omode)
{
  int res, fd;
   
  enable_user_coop();
  
  if((res = open(path, omode)) >= -1){
    disable_user_coop();
    return res;
  }

  switch(GET_REQUEST(res)){
    case SYSCALL_SUCCESS:
      disable_user_coop();
      if((fd = pick_fd(path, omode)) >= 0){
        return fd;
      } else {
        printf("ev6_open: open() failed due to ECC-uncorrectable Error.\n");
        return -2;
      }
    case REOPEN_SYSCALL_FAIL:
      reopen();
    case SYSCALL_FAIL:
      printf("ev6_open: open() failed due to ECC-uncorrectable Error.\n");
      disable_user_coop();
      return -2;
    case SYSCALL_REDO:
      printf("ev6_open: Redo open().\n");
      return ev6_open(path, omode);
    default:
      printf("ev6_exec: Invalid After-Treatment policy is returned.\n");
      ev6_exit(-1);
  }

  return -1;  // Can't reach here.
}

int
ev6_mknod(const char *path, short major, short minor)
{
  int res;

  enable_user_coop();     

  if((res = mknod(path, major, minor)) >= -2){
    disable_user_coop();
    return res;
  }

  switch(GET_REQUEST(res)){
    case SYSCALL_FAIL:
      disable_user_coop();
      printf("ev6_mknod: mknod() failed due to ECC-uncorrectable Error.\n");
      return -2;
    case SYSCALL_REDO:
      printf("ev6_mknod: Redo mknod().\n");
      return ev6_mknod(path, major, minor);
    default:
      printf("ev6_mknod: Invalid After-Treatment policy is returned.\n");
      ev6_exit(-1);
  }

  return -1;  // Can't reach here.
}

int
ev6_unlink(const char *path)
{
  int res;
     
  enable_user_coop();

  if((res = unlink(path)) >= -2){
    disable_user_coop();
    return res;
  }

  switch(GET_REQUEST(res)){
    case SYSCALL_FAIL:
      disable_user_coop();
      printf("ev6_unlink: unlink() failed due to ECC-uncorrectable Error.\n");
      return -2;
    case REOPEN_SYSCALL_REDO:
      if(reopen() < 0){
        printf("fail to re-open some files which was cleared in recovery.\n");
        disable_user_coop();
        return -2;
      }
    case SYSCALL_REDO:
      printf("ev6_unlink: Redo unlink().\n");
      return ev6_unlink(path);
    default:
      printf("ev6_unlink: Invalid After-Treatment policy is returned.\n");
      ev6_exit(-1);
  }

  return -1;  // Can't reach here.
}

int
ev6_fstat(int fd, struct stat *st)
{
  int res;

  if(check_reserved_fd(fd) < 0)
    return -1;

  enable_user_coop();
     
  if((res = fstat(fd, st)) >= -2){
    disable_user_coop();
    return res;
  }

  switch(GET_REQUEST(res)){
    case SYSCALL_FAIL:
      disable_user_coop();
      printf("ev6_fstat: fstat() failed due to ECC-uncorrectable Error.\n");
      return -2;
    case REOPEN_SYSCALL_REDO:
      if(reopen() < 0){
        printf("fail to re-open some files which was cleared in recovery.\n");
        disable_user_coop();
        return -2;
      }
    case SYSCALL_REDO:
      printf("ev6_fstat: Redo fstat().\n");
      return ev6_fstat(fd, st);
    default:
      printf("ev6_fstat: Invalid After-Treatment policy is returned.\n");
      ev6_exit(-1);
  }

  return -1;  // Can't reach here.
}

int
ev6_link(const char *new, const char *old)
{
  int res;
     
  enable_user_coop();

  if((res = link(new, old)) >= -2){
    disable_user_coop();
    return res;
  }

  switch(GET_REQUEST(res)){
    case SYSCALL_FAIL:
      disable_user_coop();
      printf("ev6_link: link() failed due to ECC-uncorrectable Error.\n");
      return -2;
    case REOPEN_SYSCALL_REDO:
      if(reopen() < 0){
        printf("fail to re-open some files which was cleared in recovery.\n");
        disable_user_coop();
        return -2;
      }
    case SYSCALL_REDO:
      printf("ev6_link: Redo link().\n");
      return ev6_link(new, old);
    default:
      printf("ev6_link: Invalid After-Treatment policy is returned.\n");
      ev6_exit(-1);
  }

  return -1;  // Can't reach here.
}

int
ev6_mkdir(const char *path)
{
  int res;
   
  enable_user_coop();
  
  if((res = mkdir(path)) >= -2){
    disable_user_coop();
    return res;
  }

  switch(GET_REQUEST(res)){
    case SYSCALL_FAIL:
      disable_user_coop();
      printf("ev6_mkdir: mkdir() failed due to ECC-uncorrectable Error.\n");
      return -2;
    case SYSCALL_REDO:
      printf("ev6_mkdir: Redo mkdir().\n");
      return ev6_mkdir(path);
    default:
      printf("ev6_mkdir: Invalid After-Treatment policy is returned.\n");
      ev6_exit(-1);
  }

  return -1;  // Can't reach here.
}

int
ev6_chdir(const char *path)
{
  int res;
     
  enable_user_coop();

  if((res = chdir(path)) >= -2){
    disable_user_coop();
    return res;
  }

  switch(GET_REQUEST(res)){
    case SYSCALL_FAIL:
      disable_user_coop();
      printf("ev6_chdir: chdir() failed due to ECC-uncorrectable Error.\n");
      return -2;
    case REOPEN_SYSCALL_REDO:
      if(reopen() < 0){
        printf("fail to re-open some files which was cleared in recovery.\n");
        disable_user_coop();
        return -2;
      }
    case SYSCALL_REDO:
      printf("ev6_chdir: Redo chdir().\n");
      return ev6_chdir(path);
    default:
      printf("ev6_chdir: Invalid After-Treatment policy is returned.\n");
      ev6_exit(-1);
  }

  return -1;  // Can't reach here.
}

int
ev6_dup(int fd)
{
  int res;

  if(check_reserved_fd(fd) < 0)
    return -1;

  enable_user_coop();

  if((res = dup(fd)) >= -2){
    disable_user_coop();
    return res;
  }

  switch(GET_REQUEST(res)){
    case SYSCALL_FAIL:
      disable_user_coop();
      printf("ev6_dup: dup() failed due to ECC-uncorrectable Error.\n");
      return -2;
    case REOPEN_SYSCALL_REDO:
      if(reopen() < 0){
        printf("fail to re-open some files which was cleared in recovery.\n");
        disable_user_coop();
        return -2;
      }
      printf("ev6_dup: Redo dup().\n");
      return ev6_dup(fd);
    default:
      printf("ev6_dup: Invalid After-Treatment policy is returned.\n");
      ev6_exit(-1);
  }

  return -2;  // Can't reach here.
}

int
ev6_getpid(void)
{
  return getpid();  // getpid() only uses out-of-recovery-target objects.
}

char*
ev6_sbrk(int n)
{
  return sbrk(n);  // Recovery handler never returns a syscall result.
}

int
ev6_sleep(int n)
{
  int res;
     
  enable_user_coop();

  if((res = sleep(n)) >= -2){
    disable_user_coop();
    return res;
  }

  switch(GET_REQUEST(res)){
    case SYSCALL_FAIL:
      disable_user_coop();
      printf("ev6_sleep: sleep() failed due to ECC-uncorrectable Error.\n");
      return -2;
    case SYSCALL_REDO:
      printf("ev6_sleep: Redo sleep().\n");
      return ev6_sleep(n);
    default:
      printf("ev6_sleep: Invalid After-Treatment policy is returned.\n");
      ev6_exit(-1);
  }

  return -1;  // Can't reach here.
}

int
ev6_uptime(void)
{
  int res;

  enable_user_coop();
     
  if((res = uptime()) >= -2){
    disable_user_coop();
    return res;
  }

  switch(GET_REQUEST(res)){
    case SYSCALL_FAIL:
      disable_user_coop();
      printf("ev6_uptime: uptime() failed due to ECC-uncorrectable Error.\n");
      return -2;
    case SYSCALL_REDO:
      printf("ev6_uptime: Redo uptime().\n");
      return ev6_uptime();
    default:
      printf("ev6_uptime: Invalid After-Treatment policy is returned.\n");
      ev6_exit(-1);
  }

  return -1;  // Can't reach here.
}
