#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "kernel/syscall.h"
#include "kernel/memlayout.h"
#include "kernel/riscv.h"

//
// Tests xv6 system calls.  usertests without arguments runs them all
// and usertests <name> runs <name> test. The test runner creates for
// each test a process and based on the exit status of the process,
// the test runner reports "OK" or "FAILED".  Some tests result in
// kernel printing usertrap messages, which can be ignored if test
// prints "OK".
//

#define BUFSZ  (MAXOPBLOCKS+2)*BSIZE

char buf[BUFSZ];
char name[3];


// does chdir() call iput(p->cwd) in a transaction?
void
iputtest(char *s)
{
  if(ev6_mkdir("iputdir") < 0){
    printf("%s: mkdir failed\n", s);
    ev6_exit(1);
  }
  if(ev6_chdir("iputdir") < 0){
    printf("%s: chdir iputdir failed\n", s);
    ev6_exit(1);
  }
  if(ev6_unlink("../iputdir") < 0){
    printf("%s: unlink ../iputdir failed\n", s);
    ev6_exit(1);
  }
  if(ev6_chdir("/") < 0){
    printf("%s: chdir / failed\n", s);
    ev6_exit(1);
  }
}

// does exit() call iput(p->cwd) in a transaction?
void
exitiputtest(char *s)
{
  int pid, xstatus;

  pid = ev6_fork();
  if(pid < 0){
    printf("%s: fork failed\n", s);
    ev6_exit(1);
  }
  if(pid == 0){
    if(ev6_mkdir("iputdir") < 0){
      printf("%s: mkdir failed\n", s);
      ev6_exit(1);
    }
    if(ev6_chdir("iputdir") < 0){
      printf("%s: child chdir failed\n", s);
      ev6_exit(1);
    }
    if(ev6_unlink("../iputdir") < 0){
      printf("%s: unlink ../iputdir failed\n", s);
      ev6_exit(1);
    }
    ev6_exit(0);
  }
  ev6_wait(&xstatus);
  ev6_exit(xstatus);
}

// does the error path in open() for attempt to write a
// directory call iput() in a transaction?
// needs a hacked kernel that pauses just after the namei()
// call in sys_open():
//    if((ip = namei(path)) == 0)
//      return -1;
//    {
//      int i;
//      for(i = 0; i < 10000; i++)
//        yield();
//    }
void
openiputtest(char *s)
{
  int pid, xstatus;

  if(ev6_mkdir("oidir") < 0){
    printf("%s: mkdir oidir failed\n", s);
    ev6_exit(1);
  }
  pid = ev6_fork();
  if(pid < 0){
    printf("%s: fork failed\n", s);
    ev6_exit(1);
  }
  if(pid == 0){
    int fd = ev6_open("oidir", O_RDWR);
    if(fd >= 0){
      printf("%s: open directory for write succeeded\n", s);
      ev6_exit(1);
    }
    ev6_exit(0);
  }
  ev6_sleep(1);
  if(ev6_unlink("oidir") != 0){
    printf("%s: unlink failed\n", s);
    ev6_exit(1);
  }
  ev6_wait(&xstatus);
  ev6_exit(xstatus);
}

// simple file system tests

void
opentest(char *s)
{
  int fd;

  fd = ev6_open("echo", 0);
  if(fd < 0){
    printf("%s: open echo failed!\n", s);
    ev6_exit(1);
  }
  ev6_close(fd);
  fd = ev6_open("doesnotexist", 0);
  if(fd >= 0){
    printf("%s: open doesnotexist succeeded!\n", s);
    ev6_exit(1);
  }
}

void
writetest(char *s)
{
  int fd;
  int i;
  enum { N=100, SZ=10 };
  
  fd = ev6_open("small", O_CREATE|O_RDWR);
  if(fd < 0){
    printf("%s: error: creat small failed!\n", s);
    ev6_exit(1);
  }
  for(i = 0; i < N; i++){
    if(ev6_write(fd, "aaaaaaaaaa", SZ) != SZ){
      printf("%s: error: write aa %d new file failed\n", i);
      ev6_exit(1);
    }
    if(ev6_write(fd, "bbbbbbbbbb", SZ) != SZ){
      printf("%s: error: write bb %d new file failed\n", i);
      ev6_exit(1);
    }
  }
  ev6_close(fd);
  fd = ev6_open("small", O_RDONLY);
  if(fd < 0){
    printf("%s: error: open small failed!\n", s);
    ev6_exit(1);
  }
  i = ev6_read(fd, buf, N*SZ*2);
  if(i != N*SZ*2){
    printf("%s: read failed\n", s);
    ev6_exit(1);
  }
  ev6_close(fd);

  if(ev6_unlink("small") < 0){
    printf("%s: unlink small failed\n", s);
    ev6_exit(1);
  }
}

void
writebig(char *s)
{
  int i, fd, n;

  fd = ev6_open("big", O_CREATE|O_RDWR);
  if(fd < 0){
    printf("%s: error: creat big failed!\n", s);
    ev6_exit(1);
  }

  for(i = 0; i < MAXFILE; i++){
    ((int*)buf)[0] = i;
    if(ev6_write(fd, buf, BSIZE) != BSIZE){
      printf("%s: error: write big file failed\n", i);
      ev6_exit(1);
    }
  }

  ev6_close(fd);

  fd = ev6_open("big", O_RDONLY);
  if(fd < 0){
    printf("%s: error: open big failed!\n", s);
    ev6_exit(1);
  }

  n = 0;
  for(;;){
    i = ev6_read(fd, buf, BSIZE);
    if(i == 0){
      if(n == MAXFILE - 1){
        printf("%s: read only %d blocks from big", n);
        ev6_exit(1);
      }
      break;
    } else if(i != BSIZE){
      printf("%s: read failed %d\n", i);
      ev6_exit(1);
    }
    if(((int*)buf)[0] != n){
      printf("%s: read content of block %d is %d\n",
             n, ((int*)buf)[0]);
      ev6_exit(1);
    }
    n++;
  }
  ev6_close(fd);
  if(ev6_unlink("big") < 0){
    printf("%s: unlink big failed\n", s);
    ev6_exit(1);
  }
}

// many creates, followed by unlink test
void
createtest(char *s)
{
  int i, fd;
  enum { N=52 };

  name[0] = 'a';
  name[2] = '\0';
  for(i = 0; i < N; i++){
    name[1] = '0' + i;
    fd = ev6_open(name, O_CREATE|O_RDWR);
    ev6_close(fd);
  }
  name[0] = 'a';
  name[2] = '\0';
  for(i = 0; i < N; i++){
    name[1] = '0' + i;
    ev6_unlink(name);
  }
}

void dirtest(char *s)
{
  printf("mkdir test\n");

  if(ev6_mkdir("dir0") < 0){
    printf("%s: mkdir failed\n", s);
    ev6_exit(1);
  }

  if(ev6_chdir("dir0") < 0){
    printf("%s: chdir dir0 failed\n", s);
    ev6_exit(1);
  }

  if(ev6_chdir("..") < 0){
    printf("%s: chdir .. failed\n", s);
    ev6_exit(1);
  }

  if(ev6_unlink("dir0") < 0){
    printf("%s: unlink dir0 failed\n", s);
    ev6_exit(1);
  }
  printf("%s: mkdir test ok\n");
}

void
exectest(char *s)
{
  int fd, xstatus, pid;
  char *echoargv[] = { "echo", "OK", 0 };
  char buf[3];

  ev6_unlink("echo-ok");
  pid = ev6_fork();
  if(pid < 0) {
     printf("%s: fork failed\n", s);
     ev6_exit(1);
  }
  if(pid == 0) {
    ev6_close(1);
    fd = ev6_open("echo-ok", O_CREATE|O_WRONLY);
    if(fd < 0) {
      printf("%s: create failed\n", s);
      ev6_exit(1);
    }
    if(fd != 1) {
      printf("%s: wrong fd\n", s);
      ev6_exit(1);
    }
    if(ev6_exec("echo", echoargv) < 0){
      printf("%s: exec echo failed\n", s);
      ev6_exit(1);
    }
    // won't get to here
  }
  if (ev6_wait(&xstatus) != pid) {
    printf("%s: wait failed!\n", s);
  }
  if(xstatus != 0)
    ev6_exit(xstatus);

  fd = ev6_open("echo-ok", O_RDONLY);
  if(fd < 0) {
    printf("%s: open failed\n", s);
    ev6_exit(1);
  }
  if (ev6_read(fd, buf, 2) != 2) {
    printf("%s: read failed\n", s);
    ev6_exit(1);
  }
  ev6_unlink("echo-ok");
  if(buf[0] == 'O' && buf[1] == 'K')
    ev6_exit(0);
  else {
    printf("%s: wrong output\n", s);
    ev6_exit(1);
  }

}

// simple fork and pipe read/write

void
pipe1(char *s)
{
  int fds[2], pid, xstatus;
  int seq, i, n, cc, total;
  enum { N=5, SZ=1033 };
  
  if(ev6_pipe(fds) != 0){
    printf("%s: pipe() failed\n", s);
    ev6_exit(1);
  }
  pid = ev6_fork();
  seq = 0;
  if(pid == 0){
    ev6_close(fds[0]);
    for(n = 0; n < N; n++){
      for(i = 0; i < SZ; i++)
        buf[i] = seq++;
      if(ev6_write(fds[1], buf, SZ) != SZ){
        printf("%s: pipe1 oops 1\n", s);
        ev6_exit(1);
      }
    }
    ev6_exit(0);
  } else if(pid > 0){
    ev6_close(fds[1]);
    total = 0;
    cc = 1;
    while((n = ev6_read(fds[0], buf, cc)) > 0){
      for(i = 0; i < n; i++){
        if((buf[i] & 0xff) != (seq++ & 0xff)){
          printf("%s: pipe1 oops 2\n", s);
          return;
        }
      }
      total += n;
      cc = cc * 2;
      if(cc > sizeof(buf))
        cc = sizeof(buf);
    }
    if(total != N * SZ){
      printf("%s: pipe1 oops 3 total %d\n", s, total);
      ev6_exit(1);
    }
    ev6_close(fds[0]);
    ev6_wait(&xstatus);
    ev6_exit(xstatus);
  } else {
    printf("%s: fork() failed\n", s);
    ev6_exit(1);
  }
}

// meant to be run w/ at most two CPUs
void
preempt(char *s)
{
  int pid1, pid2, pid3;
  int pfds[2];

  pid1 = ev6_fork();
  if(pid1 < 0) {
    printf("%s: fork failed");
    ev6_exit(1);
  }
  if(pid1 == 0)
    for(;;)
      ;

  pid2 = ev6_fork();
  if(pid2 < 0) {
    printf("%s: fork failed\n", s);
    ev6_exit(1);
  }
  if(pid2 == 0)
    for(;;)
      ;

  ev6_pipe(pfds);
  pid3 = ev6_fork();
  if(pid3 < 0) {
     printf("%s: fork failed\n", s);
     ev6_exit(1);
  }
  if(pid3 == 0){
    ev6_close(pfds[0]);
    if(ev6_write(pfds[1], "x", 1) != 1)
      printf("%s: preempt write error");
    ev6_close(pfds[1]);
    for(;;)
      ;
  }

  ev6_close(pfds[1]);
  if(ev6_read(pfds[0], buf, sizeof(buf)) != 1){
    printf("%s: preempt read error");
    return;
  }
  ev6_close(pfds[0]);
  printf("kill... ");
  ev6_kill(pid1);
  ev6_kill(pid2);
  ev6_kill(pid3);
  printf("wait... ");
  ev6_wait(0);
  ev6_wait(0);
  ev6_wait(0);
}

// try to find any races between exit and wait
void
exitwait(char *s)
{
  int i, pid;

  for(i = 0; i < 100; i++){
    pid = ev6_fork();
    if(pid < 0){
      printf("%s: fork failed\n", s);
      ev6_exit(1);
    }
    if(pid){
      int xstate;
      if(ev6_wait(&xstate) != pid){
        printf("%s: wait wrong pid\n", s);
        ev6_exit(1);
      }
      if(i != xstate) {
        printf("%s: wait wrong exit status\n", s);
        ev6_exit(1);
      }
    } else {
      ev6_exit(i);
    }
  }
}

// try to find races in the reparenting
// code that handles a parent exiting
// when it still has live children.
void
reparent(char *s)
{
  int master_pid = ev6_getpid();
  for(int i = 0; i < 200; i++){
    int pid = ev6_fork();
    if(pid < 0){
      printf("%s: fork failed\n", s);
      ev6_exit(1);
    }
    if(pid){
      if(ev6_wait(0) != pid){
        printf("%s: wait wrong pid\n", s);
        ev6_exit(1);
      }
    } else {
      int pid2 = ev6_fork();
      if(pid2 < 0){
        ev6_kill(master_pid);
        ev6_exit(1);
      }
      ev6_exit(0);
    }
  }
  ev6_exit(0);
}

// what if two children exit() at the same time?
void
twochildren(char *s)
{
  for(int i = 0; i < 1000; i++){
    int pid1 = ev6_fork();
    if(pid1 < 0){
      printf("%s: fork failed\n", s);
      ev6_exit(1);
    }
    if(pid1 == 0){
      ev6_exit(0);
    } else {
      int pid2 = ev6_fork();
      if(pid2 < 0){
        printf("%s: fork failed\n", s);
        ev6_exit(1);
      }
      if(pid2 == 0){
        ev6_exit(0);
      } else {
        ev6_wait(0);
        ev6_wait(0);
      }
    }
  }
}

// concurrent ev6_forks to try to expose locking bugs.
void
forkfork(char *s)
{
  enum { N=2 };
  
  for(int i = 0; i < N; i++){
    int pid = ev6_fork();
    if(pid < 0){
      printf("%s: ev6_fork failed", s);
      ev6_exit(1);
    }
    if(pid == 0){
      for(int j = 0; j < 200; j++){
        int pid1 = ev6_fork();
        if(pid1 < 0){
          ev6_exit(1);
        }
        if(pid1 == 0){
          ev6_exit(0);
        }
        ev6_wait(0);
      }
      ev6_exit(0);
    }
  }

  int xstatus;
  for(int i = 0; i < N; i++){
    ev6_wait(&xstatus);
    if(xstatus != 0) {
      printf("%s: ev6_fork in child failed", s);
      ev6_exit(1);
    }
  }
}

void
forkforkfork(char *s)
{
  ev6_unlink("stopforking");

  int pid = ev6_fork();
  if(pid < 0){
    printf("%s: fork failed", s);
    ev6_exit(1);
  }

  if(pid == 0){
    while(1){
      int fd = ev6_open("stopforking", 0);
      if(fd >= 0){
        ev6_exit(0);
      }
      if(ev6_fork() < 0){
        ev6_close(ev6_open("stopforking", O_CREATE|O_RDWR));
      }
    }

    ev6_exit(0);
  }

  ev6_sleep(20); // two seconds
  ev6_close(ev6_open("stopforking", O_CREATE|O_RDWR));
  ev6_wait(0);
  ev6_sleep(10); // one second
}

// regression test. does reparent() violate the parent-then-child
// locking order when giving away a child to init, so that exit()
// deadlocks against init's wait()? also used to trigger a "panic:
// release" due to exit() releasing a different p->parent->lock than
// it acquired.
void
reparent2(char *s)
{
  for(int i = 0; i < 800; i++){
    int pid1 = ev6_fork();
    if(pid1 < 0){
      printf("fork failed\n");
      ev6_exit(1);
    }
    if(pid1 == 0){
      ev6_fork();
      ev6_fork();
      ev6_exit(0);
    }
    ev6_wait(0);
  }

  ev6_exit(0);
}

// allocate all mem, free it, and allocate again
void
mem(char *s)
{
  void *m1, *m2;
  int pid;

  if((pid = ev6_fork()) == 0){
    m1 = 0;
    while((m2 = malloc(10001)) != 0){
      *(char**)m2 = m1;
      m1 = m2;
    }
    while(m1){
      m2 = *(char**)m1;
      free(m1);
      m1 = m2;
    }
    m1 = malloc(1024*20);
    if(m1 == 0){
      printf("couldn't allocate mem?!!\n", s);
      ev6_exit(1);
    }
    free(m1);
    ev6_exit(0);
  } else {
    int xstatus;
    ev6_wait(&xstatus);
    ev6_exit(xstatus);
  }
}

// More file system tests

// two processes write to the same file descriptor
// is the offset shared? does inode locking work?
void
sharedfd(char *s)
{
  int fd, pid, i, n, nc, np;
  enum { N = 1000, SZ=10};
  char buf[SZ];

  ev6_unlink("sharedfd");
  fd = ev6_open("sharedfd", O_CREATE|O_RDWR);
  if(fd < 0){
    printf("%s: cannot open sharedfd for writing", s);
    ev6_exit(1);
  }
  pid = ev6_fork();
  memset(buf, pid==0?'c':'p', sizeof(buf));
  for(i = 0; i < N; i++){
    if(ev6_write(fd, buf, sizeof(buf)) != sizeof(buf)){
      printf("%s: write sharedfd failed\n", s);
      ev6_exit(1);
    }
  }
  if(pid == 0) {
    ev6_exit(0);
  } else {
    int xstatus;
    ev6_wait(&xstatus);
    if(xstatus != 0)
      ev6_exit(xstatus);
  }
  
  ev6_close(fd);
  fd = ev6_open("sharedfd", 0);
  if(fd < 0){
    printf("%s: cannot open sharedfd for reading\n", s);
    ev6_exit(1);
  }
  nc = np = 0;
  while((n = ev6_read(fd, buf, sizeof(buf))) > 0){
    for(i = 0; i < sizeof(buf); i++){
      if(buf[i] == 'c')
        nc++;
      if(buf[i] == 'p')
        np++;
    }
  }
  ev6_close(fd);
  ev6_unlink("sharedfd");
  if(nc == N*SZ && np == N*SZ){
    ev6_exit(0);
  } else {
    printf("%s: nc/np test fails\n", s);
    ev6_exit(1);
  }
}

// four processes write different files at the same
// time, to test block allocation.
void
fourfiles(char *s)
{
  int fd, pid, i, j, n, total, pi;
  char *names[] = { "f0", "f1", "f2", "f3" };
  char *fname;
  enum { N=12, NCHILD=4, SZ=500 };
  
  for(pi = 0; pi < NCHILD; pi++){
    fname = names[pi];
    ev6_unlink(fname);

    pid = ev6_fork();
    if(pid < 0){
      printf("fork failed\n", s);
      ev6_exit(1);
    }

    if(pid == 0){
      fd = ev6_open(fname, O_CREATE | O_RDWR);
      if(fd < 0){
        printf("create failed\n", s);
        ev6_exit(1);
      }

      memset(buf, '0'+pi, SZ);
      for(i = 0; i < N; i++){
        if((n = ev6_write(fd, buf, SZ)) != SZ){
          printf("write failed %d\n", n);
          ev6_exit(1);
        }
      }
      ev6_exit(0);
    }
  }

  int xstatus;
  for(pi = 0; pi < NCHILD; pi++){
    ev6_wait(&xstatus);
    if(xstatus != 0)
      ev6_exit(xstatus);
  }

  for(i = 0; i < NCHILD; i++){
    fname = names[i];
    fd = ev6_open(fname, 0);
    total = 0;
    while((n = ev6_read(fd, buf, sizeof(buf))) > 0){
      for(j = 0; j < n; j++){
        if(buf[j] != '0'+i){
          printf("wrong char\n", s);
          ev6_exit(1);
        }
      }
      total += n;
    }
    ev6_close(fd);
    if(total != N*SZ){
      printf("wrong length %d\n", total);
      ev6_exit(1);
    }
    ev6_unlink(fname);
  }
}

// four processes create and delete different files in same directory
void
createdelete(char *s)
{
  enum { N = 20, NCHILD=4 };
  int pid, i, fd, pi;
  char name[32];

  for(pi = 0; pi < NCHILD; pi++){
    pid = ev6_fork();
    if(pid < 0){
      printf("fork failed\n", s);
      ev6_exit(1);
    }

    if(pid == 0){
      name[0] = 'p' + pi;
      name[2] = '\0';
      for(i = 0; i < N; i++){
        name[1] = '0' + i;
        fd = ev6_open(name, O_CREATE | O_RDWR);
        if(fd < 0){
          printf("%s: create failed\n", s);
          ev6_exit(1);
        }
        ev6_close(fd);
        if(i > 0 && (i % 2 ) == 0){
          name[1] = '0' + (i / 2);
          if(ev6_unlink(name) < 0){
            printf("%s: unlink failed\n", s);
            ev6_exit(1);
          }
        }
      }
      ev6_exit(0);
    }
  }

  int xstatus;
  for(pi = 0; pi < NCHILD; pi++){
    ev6_wait(&xstatus);
    if(xstatus != 0)
      ev6_exit(1);
  }

  name[0] = name[1] = name[2] = 0;
  for(i = 0; i < N; i++){
    for(pi = 0; pi < NCHILD; pi++){
      name[0] = 'p' + pi;
      name[1] = '0' + i;
      fd = ev6_open(name, 0);
      if((i == 0 || i >= N/2) && fd < 0){
        printf("%s: oops createdelete %s didn't exist\n", s, name);
        ev6_exit(1);
      } else if((i >= 1 && i < N/2) && fd >= 0){
        printf("%s: oops createdelete %s did exist\n", s, name);
        ev6_exit(1);
      }
      if(fd >= 0)
        ev6_close(fd);
    }
  }

  for(i = 0; i < N; i++){
    for(pi = 0; pi < NCHILD; pi++){
      name[0] = 'p' + i;
      name[1] = '0' + i;
      ev6_unlink(name);
    }
  }
}

// can I unlink a file and still read it?
void
unlinkread(char *s)
{
  enum { SZ = 5 };
  int fd, fd1;

  fd = ev6_open("unlinkread", O_CREATE | O_RDWR);
  if(fd < 0){
    printf("%s: create unlinkread failed\n", s);
    ev6_exit(1);
  }
  ev6_write(fd, "hello", SZ);
  ev6_close(fd);

  fd = ev6_open("unlinkread", O_RDWR);
  if(fd < 0){
    printf("%s: open unlinkread failed\n", s);
    ev6_exit(1);
  }
  if(ev6_unlink("unlinkread") != 0){
    printf("%s: unlink unlinkread failed\n", s);
    ev6_exit(1);
  }

  fd1 = ev6_open("unlinkread", O_CREATE | O_RDWR);
  ev6_write(fd1, "yyy", 3);
  ev6_close(fd1);

  if(ev6_read(fd, buf, sizeof(buf)) != SZ){
    printf("%s: unlinkread read failed", s);
    ev6_exit(1);
  }
  if(buf[0] != 'h'){
    printf("%s: unlinkread wrong data\n", s);
    ev6_exit(1);
  }
  if(ev6_write(fd, buf, 10) != 10){
    printf("%s: unlinkread write failed\n", s);
    ev6_exit(1);
  }
  ev6_close(fd);
  ev6_unlink("unlinkread");
}

void
linktest(char *s)
{
  enum { SZ = 5 };
  int fd;

  ev6_unlink("lf1");
  ev6_unlink("lf2");

  fd = ev6_open("lf1", O_CREATE|O_RDWR);
  if(fd < 0){
    printf("%s: create lf1 failed\n", s);
    ev6_exit(1);
  }
  if(ev6_write(fd, "hello", SZ) != SZ){
    printf("%s: write lf1 failed\n", s);
    ev6_exit(1);
  }
  ev6_close(fd);

  if(link("lf1", "lf2") < 0){
    printf("%s: link lf1 lf2 failed\n", s);
    ev6_exit(1);
  }
  ev6_unlink("lf1");

  if(ev6_open("lf1", 0) >= 0){
    printf("%s: unlinked lf1 but it is still there!\n", s);
    ev6_exit(1);
  }

  fd = ev6_open("lf2", 0);
  if(fd < 0){
    printf("%s: open lf2 failed\n", s);
    ev6_exit(1);
  }
  if(ev6_read(fd, buf, sizeof(buf)) != SZ){
    printf("%s: read lf2 failed\n", s);
    ev6_exit(1);
  }
  ev6_close(fd);

  if(link("lf2", "lf2") >= 0){
    printf("%s: link lf2 lf2 succeeded! oops\n", s);
    ev6_exit(1);
  }

  ev6_unlink("lf2");
  if(link("lf2", "lf1") >= 0){
    printf("%s: link non-existant succeeded! oops\n", s);
    ev6_exit(1);
  }

  if(link(".", "lf1") >= 0){
    printf("%s: link . lf1 succeeded! oops\n", s);
    ev6_exit(1);
  }
}

// test concurrent create/link/unlink of the same file
void
concreate(char *s)
{
  enum { N = 40 };
  char file[3];
  int i, pid, n, fd;
  char fa[N];
  struct {
    ushort inum;
    char name[DIRSIZ];
  } de;

  file[0] = 'C';
  file[2] = '\0';
  for(i = 0; i < N; i++){
    file[1] = '0' + i;
    ev6_unlink(file);
    pid = ev6_fork();
    if(pid && (i % 3) == 1){
      ev6_link("C0", file);
    } else if(pid == 0 && (i % 5) == 1){
      ev6_link("C0", file);
    } else {
      fd = ev6_open(file, O_CREATE | O_RDWR);
      if(fd < 0){
        printf("concreate create %s failed\n", file);
        ev6_exit(1);
      }
      ev6_close(fd);
    }
    if(pid == 0) {
      ev6_exit(0);
    } else {
      int xstatus;
      ev6_wait(&xstatus);
      if(xstatus != 0)
        ev6_exit(1);
    }
  }

  memset(fa, 0, sizeof(fa));
  fd = ev6_open(".", 0);
  n = 0;
  while(ev6_read(fd, &de, sizeof(de)) > 0){
    if(de.inum == 0)
      continue;
    if(de.name[0] == 'C' && de.name[2] == '\0'){
      i = de.name[1] - '0';
      if(i < 0 || i >= sizeof(fa)){
        printf("%s: concreate weird file %s\n", s, de.name);
        ev6_exit(1);
      }
      if(fa[i]){
        printf("%s: concreate duplicate file %s\n", s, de.name);
        ev6_exit(1);
      }
      fa[i] = 1;
      n++;
    }
  }
  ev6_close(fd);

  if(n != N){
    printf("%s: concreate not enough files in directory listing\n", s);
    ev6_exit(1);
  }

  for(i = 0; i < N; i++){
    file[1] = '0' + i;
    pid = ev6_fork();
    if(pid < 0){
      printf("%s: fork failed\n", s);
      ev6_exit(1);
    }
    if(((i % 3) == 0 && pid == 0) ||
       ((i % 3) == 1 && pid != 0)){
      ev6_close(ev6_open(file, 0));
      ev6_close(ev6_open(file, 0));
      ev6_close(ev6_open(file, 0));
      ev6_close(ev6_open(file, 0));
    } else {
      ev6_unlink(file);
      ev6_unlink(file);
      ev6_unlink(file);
      ev6_unlink(file);
    }
    if(pid == 0)
      ev6_exit(0);
    else
      ev6_wait(0);
  }
}

// another concurrent link/unlink/create test,
// to look for deadlocks.
void
linkunlink(char *s)
{
  int pid, i;

  ev6_unlink("x");
  pid = ev6_fork();
  if(pid < 0){
    printf("%s: fork failed\n", s);
    ev6_exit(1);
  }

  unsigned int x = (pid ? 1 : 97);
  for(i = 0; i < 100; i++){
    x = x * 1103515245 + 12345;
    if((x % 3) == 0){
      ev6_close(ev6_open("x", O_RDWR | O_CREATE));
    } else if((x % 3) == 1){
      ev6_link("cat", "x");
    } else {
      ev6_unlink("x");
    }
  }

  if(pid)
    ev6_wait(0);
  else
    ev6_exit(0);
}

// directory that uses indirect blocks
void
bigdir(char *s)
{
  enum { N = 500 };
  int i, fd;
  char name[10];

  ev6_unlink("bd");

  fd = ev6_open("bd", O_CREATE);
  if(fd < 0){
    printf("%s: bigdir create failed\n", s);
    ev6_exit(1);
  }
  ev6_close(fd);

  for(i = 0; i < N; i++){
    name[0] = 'x';
    name[1] = '0' + (i / 64);
    name[2] = '0' + (i % 64);
    name[3] = '\0';
    if(link("bd", name) != 0){
      printf("%s: bigdir link failed\n", s);
      ev6_exit(1);
    }
  }

  ev6_unlink("bd");
  for(i = 0; i < N; i++){
    name[0] = 'x';
    name[1] = '0' + (i / 64);
    name[2] = '0' + (i % 64);
    name[3] = '\0';
    if(ev6_unlink(name) != 0){
      printf("%s: bigdir unlink failed", s);
      ev6_exit(1);
    }
  }
}

void
subdir(char *s)
{
  int fd, cc;

  ev6_unlink("ff");
  if(ev6_mkdir("dd") != 0){
    printf("%s: mkdir dd failed\n", s);
    ev6_exit(1);
  }

  fd = ev6_open("dd/ff", O_CREATE | O_RDWR);
  if(fd < 0){
    printf("%s: create dd/ff failed\n", s);
    ev6_exit(1);
  }
  ev6_write(fd, "ff", 2);
  ev6_close(fd);

  if(ev6_unlink("dd") >= 0){
    printf("%s: unlink dd (non-empty dir) succeeded!\n", s);
    ev6_exit(1);
  }

  if(ev6_mkdir("/dd/dd") != 0){
    printf("subdir mkdir dd/dd failed\n", s);
    ev6_exit(1);
  }

  fd = ev6_open("dd/dd/ff", O_CREATE | O_RDWR);
  if(fd < 0){
    printf("%s: create dd/dd/ff failed\n", s);
    ev6_exit(1);
  }
  ev6_write(fd, "FF", 2);
  ev6_close(fd);

  fd = ev6_open("dd/dd/../ff", 0);
  if(fd < 0){
    printf("%s: open dd/dd/../ff failed\n", s);
    ev6_exit(1);
  }
  cc = ev6_read(fd, buf, sizeof(buf));
  if(cc != 2 || buf[0] != 'f'){
    printf("%s: dd/dd/../ff wrong content\n", s);
    ev6_exit(1);
  }
  ev6_close(fd);

  if(link("dd/dd/ff", "dd/dd/ffff") != 0){
    printf("link dd/dd/ff dd/dd/ffff failed\n", s);
    ev6_exit(1);
  }

  if(ev6_unlink("dd/dd/ff") != 0){
    printf("%s: unlink dd/dd/ff failed\n", s);
    ev6_exit(1);
  }
  if(ev6_open("dd/dd/ff", O_RDONLY) >= 0){
    printf("%s: open (unlinked) dd/dd/ff succeeded\n", s);
    ev6_exit(1);
  }

  if(ev6_chdir("dd") != 0){
    printf("%s: chdir dd failed\n", s);
    ev6_exit(1);
  }
  if(ev6_chdir("dd/../../dd") != 0){
    printf("%s: chdir dd/../../dd failed\n", s);
    ev6_exit(1);
  }
  if(ev6_chdir("dd/../../../dd") != 0){
    printf("chdir dd/../../dd failed\n", s);
    ev6_exit(1);
  }
  if(ev6_chdir("./..") != 0){
    printf("%s: chdir ./.. failed\n", s);
    ev6_exit(1);
  }

  fd = ev6_open("dd/dd/ffff", 0);
  if(fd < 0){
    printf("%s: open dd/dd/ffff failed\n", s);
    ev6_exit(1);
  }
  if(ev6_read(fd, buf, sizeof(buf)) != 2){
    printf("%s: read dd/dd/ffff wrong len\n", s);
    ev6_exit(1);
  }
  ev6_close(fd);

  if(ev6_open("dd/dd/ff", O_RDONLY) >= 0){
    printf("%s: open (unlinked) dd/dd/ff succeeded!\n", s);
    ev6_exit(1);
  }

  if(ev6_open("dd/ff/ff", O_CREATE|O_RDWR) >= 0){
    printf("%s: create dd/ff/ff succeeded!\n", s);
    ev6_exit(1);
  }
  if(ev6_open("dd/xx/ff", O_CREATE|O_RDWR) >= 0){
    printf("%s: create dd/xx/ff succeeded!\n", s);
    ev6_exit(1);
  }
  if(ev6_open("dd", O_CREATE) >= 0){
    printf("%s: create dd succeeded!\n", s);
    ev6_exit(1);
  }
  if(ev6_open("dd", O_RDWR) >= 0){
    printf("%s: open dd rdwr succeeded!\n", s);
    ev6_exit(1);
  }
  if(ev6_open("dd", O_WRONLY) >= 0){
    printf("%s: open dd wronly succeeded!\n", s);
    ev6_exit(1);
  }
  if(link("dd/ff/ff", "dd/dd/xx") == 0){
    printf("%s: link dd/ff/ff dd/dd/xx succeeded!\n", s);
    ev6_exit(1);
  }
  if(link("dd/xx/ff", "dd/dd/xx") == 0){
    printf("%s: link dd/xx/ff dd/dd/xx succeeded!\n", s);
    ev6_exit(1);
  }
  if(link("dd/ff", "dd/dd/ffff") == 0){
    printf("%s: link dd/ff dd/dd/ffff succeeded!\n", s);
    ev6_exit(1);
  }
  if(ev6_mkdir("dd/ff/ff") == 0){
    printf("%s: mkdir dd/ff/ff succeeded!\n", s);
    ev6_exit(1);
  }
  if(ev6_mkdir("dd/xx/ff") == 0){
    printf("%s: mkdir dd/xx/ff succeeded!\n", s);
    ev6_exit(1);
  }
  if(ev6_mkdir("dd/dd/ffff") == 0){
    printf("%s: mkdir dd/dd/ffff succeeded!\n", s);
    ev6_exit(1);
  }
  if(ev6_unlink("dd/xx/ff") == 0){
    printf("%s: unlink dd/xx/ff succeeded!\n", s);
    ev6_exit(1);
  }
  if(ev6_unlink("dd/ff/ff") == 0){
    printf("%s: unlink dd/ff/ff succeeded!\n", s);
    ev6_exit(1);
  }
  if(ev6_chdir("dd/ff") == 0){
    printf("%s: chdir dd/ff succeeded!\n", s);
    ev6_exit(1);
  }
  if(ev6_chdir("dd/xx") == 0){
    printf("%s: chdir dd/xx succeeded!\n", s);
    ev6_exit(1);
  }

  if(ev6_unlink("dd/dd/ffff") != 0){
    printf("%s: unlink dd/dd/ff failed\n", s);
    ev6_exit(1);
  }
  if(ev6_unlink("dd/ff") != 0){
    printf("%s: unlink dd/ff failed\n", s);
    ev6_exit(1);
  }
  if(ev6_unlink("dd") == 0){
    printf("%s: unlink non-empty dd succeeded!\n", s);
    ev6_exit(1);
  }
  if(ev6_unlink("dd/dd") < 0){
    printf("%s: unlink dd/dd failed\n", s);
    ev6_exit(1);
  }
  if(ev6_unlink("dd") < 0){
    printf("%s: unlink dd failed\n", s);
    ev6_exit(1);
  }
}

// test writes that are larger than the log.
void
bigwrite(char *s)
{
  int fd, sz;

  ev6_unlink("bigwrite");
  for(sz = 499; sz < (MAXOPBLOCKS+2)*BSIZE; sz += 471){
    fd = ev6_open("bigwrite", O_CREATE | O_RDWR);
    if(fd < 0){
      printf("%s: cannot create bigwrite\n", s);
      ev6_exit(1);
    }
    int i;
    for(i = 0; i < 2; i++){
      int cc = ev6_write(fd, buf, sz);
      if(cc != sz){
        printf("%s: ev6_write(%d) ret %d\n", s, sz, cc);
        ev6_exit(1);
      }
    }
    ev6_close(fd);
    ev6_unlink("bigwrite");
  }
}

void
bigfile(char *s)
{
  enum { N = 20, SZ=600 };
  int fd, i, total, cc;

  ev6_unlink("bigfile");
  fd = ev6_open("bigfile", O_CREATE | O_RDWR);
  if(fd < 0){
    printf("%s: cannot create bigfile", s);
    ev6_exit(1);
  }
  for(i = 0; i < N; i++){
    memset(buf, i, SZ);
    if(ev6_write(fd, buf, SZ) != SZ){
      printf("%s: write bigfile failed\n", s);
      ev6_exit(1);
    }
  }
  ev6_close(fd);

  fd = ev6_open("bigfile", 0);
  if(fd < 0){
    printf("%s: cannot open bigfile\n", s);
    ev6_exit(1);
  }
  total = 0;
  for(i = 0; ; i++){
    cc = ev6_read(fd, buf, SZ/2);
    if(cc < 0){
      printf("%s: read bigfile failed\n", s);
      ev6_exit(1);
    }
    if(cc == 0)
      break;
    if(cc != SZ/2){
      printf("%s: short read bigfile\n", s);
      ev6_exit(1);
    }
    if(buf[0] != i/2 || buf[SZ/2-1] != i/2){
      printf("%s: read bigfile wrong data\n", s);
      ev6_exit(1);
    }
    total += cc;
  }
  ev6_close(fd);
  if(total != N*SZ){
    printf("%s: read bigfile wrong total\n", s);
    ev6_exit(1);
  }
  ev6_unlink("bigfile");
}

void
fourteen(char *s)
{
  int fd;

  // DIRSIZ is 14.

  if(ev6_mkdir("12345678901234") != 0){
    printf("%s: mkdir 12345678901234 failed\n", s);
    ev6_exit(1);
  }
  if(ev6_mkdir("12345678901234/123456789012345") != 0){
    printf("%s: mkdir 12345678901234/123456789012345 failed\n", s);
    ev6_exit(1);
  }
  fd = ev6_open("123456789012345/123456789012345/123456789012345", O_CREATE);
  if(fd < 0){
    printf("%s: create 123456789012345/123456789012345/123456789012345 failed\n", s);
    ev6_exit(1);
  }
  ev6_close(fd);
  fd = ev6_open("12345678901234/12345678901234/12345678901234", 0);
  if(fd < 0){
    printf("%s: open 12345678901234/12345678901234/12345678901234 failed\n", s);
    ev6_exit(1);
  }
  ev6_close(fd);

  if(ev6_mkdir("12345678901234/12345678901234") == 0){
    printf("%s: mkdir 12345678901234/12345678901234 succeeded!\n", s);
    ev6_exit(1);
  }
  if(ev6_mkdir("123456789012345/12345678901234") == 0){
    printf("%s: mkdir 12345678901234/123456789012345 succeeded!\n", s);
    ev6_exit(1);
  }
}

void
rmdot(char *s)
{
  if(ev6_mkdir("dots") != 0){
    printf("%s: mkdir dots failed\n", s);
    ev6_exit(1);
  }
  if(ev6_chdir("dots") != 0){
    printf("%s: chdir dots failed\n", s);
    ev6_exit(1);
  }
  if(ev6_unlink(".") == 0){
    printf("%s: rm . worked!\n", s);
    ev6_exit(1);
  }
  if(ev6_unlink("..") == 0){
    printf("%s: rm .. worked!\n", s);
    ev6_exit(1);
  }
  if(ev6_chdir("/") != 0){
    printf("%s: chdir / failed\n", s);
    ev6_exit(1);
  }
  if(ev6_unlink("dots/.") == 0){
    printf("%s: unlink dots/. worked!\n", s);
    ev6_exit(1);
  }
  if(ev6_unlink("dots/..") == 0){
    printf("%s: unlink dots/.. worked!\n", s);
    ev6_exit(1);
  }
  if(ev6_unlink("dots") != 0){
    printf("%s: unlink dots failed!\n", s);
    ev6_exit(1);
  }
}

void
dirfile(char *s)
{
  int fd;

  fd = ev6_open("dirfile", O_CREATE);
  if(fd < 0){
    printf("%s: create dirfile failed\n", s);
    ev6_exit(1);
  }
  ev6_close(fd);
  if(ev6_chdir("dirfile") == 0){
    printf("%s: chdir dirfile succeeded!\n", s);
    ev6_exit(1);
  }
  fd = ev6_open("dirfile/xx", 0);
  if(fd >= 0){
    printf("%s: create dirfile/xx succeeded!\n", s);
    ev6_exit(1);
  }
  fd = ev6_open("dirfile/xx", O_CREATE);
  if(fd >= 0){
    printf("%s: create dirfile/xx succeeded!\n", s);
    ev6_exit(1);
  }
  if(ev6_mkdir("dirfile/xx") == 0){
    printf("%s: mkdir dirfile/xx succeeded!\n", s);
    ev6_exit(1);
  }
  if(ev6_unlink("dirfile/xx") == 0){
    printf("%s: unlink dirfile/xx succeeded!\n", s);
    ev6_exit(1);
  }
  if(link("README", "dirfile/xx") == 0){
    printf("%s: link to dirfile/xx succeeded!\n", s);
    ev6_exit(1);
  }
  if(ev6_unlink("dirfile") != 0){
    printf("%s: unlink dirfile failed!\n", s);
    ev6_exit(1);
  }

  fd = ev6_open(".", O_RDWR);
  if(fd >= 0){
    printf("%s: open . for writing succeeded!\n", s);
    ev6_exit(1);
  }
  fd = ev6_open(".", 0);
  if(ev6_write(fd, "x", 1) > 0){
    printf("%s: write . succeeded!\n", s);
    ev6_exit(1);
  }
  ev6_close(fd);
}

// test that iput() is called at the end of _namei()
void
iref(char *s)
{
  int i, fd;

  for(i = 0; i < NINODE + 1; i++){
    if(ev6_mkdir("irefd") != 0){
      printf("%s: mkdir irefd failed\n", s);
      ev6_exit(1);
    }
    if(ev6_chdir("irefd") != 0){
      printf("%s: chdir irefd failed\n", s);
      ev6_exit(1);
    }

    ev6_mkdir("");
    ev6_link("README", "");
    fd = ev6_open("", O_CREATE);
    if(fd >= 0)
      ev6_close(fd);
    fd = ev6_open("xx", O_CREATE);
    if(fd >= 0)
      ev6_close(fd);
    ev6_unlink("xx");
  }

  ev6_chdir("/");
}

// test that fork fails gracefully
// the forktest binary also does this, but it runs out of proc entries first.
// inside the bigger usertests binary, we run out of memory first.
void
forktest(char *s)
{
  enum{ N = 1000 };
  int n, pid;

  for(n=0; n<N; n++){
    pid = ev6_fork();
    if(pid < 0)
      break;
    if(pid == 0)
      ev6_exit(0);
  }

  if (n == 0) {
    printf("%s: no fork at all!\n", s);
    ev6_exit(1);
  }

  if(n == N){
    printf("%s: fork claimed to work 1000 times!\n", s);
    ev6_exit(1);
  }

  for(; n > 0; n--){
    if(ev6_wait(0) < 0){
      printf("%s: wait stopped early\n", s);
      ev6_exit(1);
    }
  }

  if(ev6_wait(0) != -1){
    printf("%s: wait got too many\n", s);
    ev6_exit(1);
  }
}

void
sbrkbasic(char *s)
{
  enum { TOOMUCH=1024*1024*1024};
  int i, pid, xstatus;
  char *c, *a, *b;
  
  // does sbrk() return the expected failure value?
  a = ev6_sbrk(TOOMUCH);
  if(a != (char*)0xffffffffffffffffL){
    printf("%s: sbrk(<toomuch>) returned %p\n", a);
    ev6_exit(1);
  }

  // can one sbrk() less than a page?
  a = ev6_sbrk(0);
  for(i = 0; i < 5000; i++){
    b = ev6_sbrk(1);
    if(b != a){
      printf("%s: sbrk test failed %d %x %x\n", i, a, b);
      ev6_exit(1);
    }
    *b = 1;
    a = b + 1;
  }
  pid = ev6_fork();
  if(pid < 0){
    printf("%s: sbrk test fork failed\n", s);
    ev6_exit(1);
  }
  c = ev6_sbrk(1);
  c = ev6_sbrk(1);
  if(c != a + 1){
    printf("%s: sbrk test failed post-fork\n", s);
    ev6_exit(1);
  }
  if(pid == 0)
    ev6_exit(0);
  ev6_wait(&xstatus);
  ev6_exit(xstatus);
}

void
sbrkmuch(char *s)
{
  enum { BIG=100*1024*1024 };
  char *c, *oldbrk, *a, *lastaddr, *p;
  uint64 amt;

  oldbrk = ev6_sbrk(0);

  // can one grow address space to something big?
  a = ev6_sbrk(0);
  amt = BIG - (uint64)a;
  p = ev6_sbrk(amt);
  if (p != a) {
    printf("%s: sbrk test failed to grow big address space; enough phys mem?\n", s);
    ev6_exit(1);
  }
  lastaddr = (char*) (BIG-1);
  *lastaddr = 99;

  // can one de-allocate?
  a = ev6_sbrk(0);
  c = ev6_sbrk(-PGSIZE);
  if(c == (char*)0xffffffffffffffffL){
    printf("%s: sbrk could not deallocate\n", s);
    ev6_exit(1);
  }
  c = ev6_sbrk(0);
  if(c != a - PGSIZE){
    printf("%s: sbrk deallocation produced wrong address, a %x c %x\n", a, c);
    ev6_exit(1);
  }

  // can one re-allocate that page?
  a = ev6_sbrk(0);
  c = ev6_sbrk(PGSIZE);
  if(c != a || ev6_sbrk(0) != a + PGSIZE){
    printf("%s: sbrk re-allocation failed, a %x c %x\n", a, c);
    ev6_exit(1);
  }
  if(*lastaddr == 99){
    // should be zero
    printf("%s: sbrk de-allocation didn't really deallocate\n", s);
    ev6_exit(1);
  }

  a = ev6_sbrk(0);
  c = ev6_sbrk(-(ev6_sbrk(0) - oldbrk));
  if(c != a){
    printf("%s: sbrk downsize failed, a %x c %x\n", a, c);
    ev6_exit(1);
  }
}

// can we read the kernel's memory?
void
kernmem(char *s)
{
  char *a;
  int pid;

  for(a = (char*)(KERNBASE); a < (char*) (KERNBASE+2000000); a += 50000){
    pid = ev6_fork();
    if(pid < 0){
      printf("%s: fork failed\n", s);
      ev6_exit(1);
    }
    if(pid == 0){
      printf("%s: oops could read %x = %x\n", a, *a);
      ev6_exit(1);
    }
    int xstatus;
    ev6_wait(&xstatus);
    if(xstatus != -1)  // did kernel kill child?
      ev6_exit(1);
  }
}

// if we run the system out of memory, does it clean up the last
// failed allocation?
void
sbrkfail(char *s)
{
  enum { BIG=100*1024*1024 };
  int i, xstatus;
  int fds[2];
  char scratch;
  char *c, *a;
  int pids[10];
  int pid;
 
  if(ev6_pipe(fds) != 0){
    printf("%s: ev6_pipe() failed\n", s);
    ev6_exit(1);
  }
  for(i = 0; i < sizeof(pids)/sizeof(pids[0]); i++){
    if((pids[i] = ev6_fork()) == 0){
      // allocate a lot of memory
      ev6_sbrk(BIG - (uint64)ev6_sbrk(0));
      ev6_write(fds[1], "x", 1);
      // sit around until killed
      for(;;) ev6_sleep(1000);
    }
    if(pids[i] != -1){
      ev6_read(fds[0], &scratch, 1);
    }
  }

  // if those failed allocations freed up the pages they did allocate,
  // we'll be able to allocate here
  c = ev6_sbrk(PGSIZE);
  for(i = 0; i < sizeof(pids)/sizeof(pids[0]); i++){
    if(pids[i] == -1)
      continue;
    ev6_kill(pids[i]);
    ev6_wait(0);
  }
  if(c == (char*)0xffffffffffffffffL){
    printf("%s: failed sbrk leaked memory\n", s);
    ev6_exit(1);
  }

  // test running fork with the above allocated page 
  pid = ev6_fork();
  if(pid < 0){
    printf("%s: fork failed\n", s);
    ev6_exit(1);
  }
  if(pid == 0){
    // allocate a lot of memory
    a = ev6_sbrk(0);
    ev6_sbrk(10*BIG);
    int n = 0;
    for (i = 0; i < 10*BIG; i += PGSIZE) {
      n += *(a+i);
    }
    printf("%s: allocate a lot of memory succeeded %d\n", n);
    ev6_exit(1);
  }
  ev6_wait(&xstatus);
  if(xstatus != -1)
    ev6_exit(1);
}

  
// test reads/writes from/to allocated memory
void
sbrkarg(char *s)
{
  char *a;
  int fd, n;

  a = ev6_sbrk(PGSIZE);
  fd = ev6_open("sbrk", O_CREATE|O_WRONLY);
  ev6_unlink("sbrk");
  if(fd < 0)  {
    printf("%s: open sbrk failed\n", s);
    ev6_exit(1);
  }
  if ((n = ev6_write(fd, a, PGSIZE)) < 0) {
    printf("%s: write sbrk failed\n", s);
    ev6_exit(1);
  }
  ev6_close(fd);

  // test writes to allocated memory
  a = ev6_sbrk(PGSIZE);
  if(ev6_pipe((int *) a) != 0){
    printf("%s: pipe() failed\n", s);
    ev6_exit(1);
  } 
}

void
validatetest(char *s)
{
  int hi;
  uint64 p;

  hi = 1100*1024;
  for(p = 0; p <= (uint)hi; p += PGSIZE){
    // try to crash the kernel by passing in a bad string pointer
    if(link("nosuchfile", (char*)p) != -1){
      printf("%s: link should not succeed\n", s);
      ev6_exit(1);
    }
  }
}

// does unintialized data start out zero?
char uninit[10000];
void
bsstest(char *s)
{
  int i;

  for(i = 0; i < sizeof(uninit); i++){
    if(uninit[i] != '\0'){
      printf("%s: bss test failed\n", s);
      ev6_exit(1);
    }
  }
}

// does exec return an error if the arguments
// are larger than a page? or does it write
// below the stack and wreck the instructions/data?
void
bigargtest(char *s)
{
  int pid, fd, xstatus;

  ev6_unlink("bigarg-ok");
  pid = ev6_fork();
  if(pid == 0){
    static char *args[MAXARG];
    int i;
    for(i = 0; i < MAXARG-1; i++)
      args[i] = "bigargs test: failed\n                                                                                                                                                                                                       ";
    args[MAXARG-1] = 0;
    ev6_exec("echo", args);
    fd = ev6_open("bigarg-ok", O_CREATE);
    ev6_close(fd);
    ev6_exit(0);
  } else if(pid < 0){
    printf("%s: bigargtest: fork failed\n", s);
    ev6_exit(1);
  }
  
  ev6_wait(&xstatus);
  if(xstatus != 0)
    ev6_exit(xstatus);
  fd = ev6_open("bigarg-ok", 0);
  if(fd < 0){
    printf("%s: bigarg test failed!\n", s);
    ev6_exit(1);
  }
  ev6_close(fd);
}

// what happens when the file system runs out of blocks?
// answer: balloc panics, so this test is not useful.
void
fsfull()
{
  int nfiles;
  int fsblocks = 0;

  printf("fsfull test\n");

  for(nfiles = 0; ; nfiles++){
    char name[64];
    name[0] = 'f';
    name[1] = '0' + nfiles / 1000;
    name[2] = '0' + (nfiles % 1000) / 100;
    name[3] = '0' + (nfiles % 100) / 10;
    name[4] = '0' + (nfiles % 10);
    name[5] = '\0';
    printf("%s: writing %s\n", name);
    int fd = ev6_open(name, O_CREATE|O_RDWR);
    if(fd < 0){
      printf("%s: open %s failed\n", name);
      break;
    }
    int total = 0;
    while(1){
      int cc = ev6_write(fd, buf, BSIZE);
      if(cc < BSIZE)
        break;
      total += cc;
      fsblocks++;
    }
    printf("%s: wrote %d bytes\n", total);
    ev6_close(fd);
    if(total == 0)
      break;
  }

  while(nfiles >= 0){
    char name[64];
    name[0] = 'f';
    name[1] = '0' + nfiles / 1000;
    name[2] = '0' + (nfiles % 1000) / 100;
    name[3] = '0' + (nfiles % 100) / 10;
    name[4] = '0' + (nfiles % 10);
    name[5] = '\0';
    ev6_unlink(name);
    nfiles--;
  }

  printf("fsfull test finished\n");
}

void argptest(char *s)
{
  int fd;
  fd = ev6_open("init", O_RDONLY);
  if (fd < 0) {
    printf("%s: open failed\n", s);
    ev6_exit(1);
  }
  ev6_read(fd, ev6_sbrk(0) - 1, -1);
  ev6_close(fd);
}

unsigned long randstate = 1;
unsigned int
rand()
{
  randstate = randstate * 1664525 + 1013904223;
  return randstate;
}

// check that there's an invalid page beneath
// the user stack, to catch stack overflow.
void
stacktest(char *s)
{
  int pid;
  int xstatus;
  
  pid = ev6_fork();
  if(pid == 0) {
    char *sp = (char *) r_sp();
    sp -= PGSIZE;
    // the *sp should cause a trap.
    printf("%s: stacktest: read below stack %p\n", *sp);
    ev6_exit(1);
  } else if(pid < 0){
    printf("%s: fork failed\n", s);
    ev6_exit(1);
  }
  ev6_wait(&xstatus);
  if(xstatus == -1)  // kernel killed child?
    ev6_exit(0);
  else
    ev6_exit(xstatus);
}

// regression test. copyin(), copyout(), and copyinstr() used to cast
// the virtual page address to uint, which (with certain wild system
// call arguments) resulted in a kernel page faults.
void
pgbug(char *s)
{
  char *argv[1];
  argv[0] = 0;
  ev6_exec((char*)0xeaeb0b5b00002f5e, argv);
  ev6_pipe((int*)0xeaeb0b5b00002f5e);

  ev6_exit(0);
}

// regression test. does the kernel panic if a process sbrk()s its
// size to be less than a page, or zero, or reduces the break by an
// amount too small to cause a page to be freed?
void
sbrkbugs(char *s)
{
  int pid = ev6_fork();
  if(pid < 0){
    printf("fork failed\n");
    ev6_exit(1);
  }
  if(pid == 0){
    int sz = (uint64) ev6_sbrk(0);
    // free all user memory; there used to be a bug that
    // would not adjust p->sz correctly in this case,
    // causing exit() to panic.
    ev6_sbrk(-sz);
    // user page fault here.
    ev6_exit(0);
  }
  ev6_wait(0);

  pid = ev6_fork();
  if(pid < 0){
    printf("fork failed\n");
    ev6_exit(1);
  }
  if(pid == 0){
    int sz = (uint64) ev6_sbrk(0);
    // set the break to somewhere in the very first
    // page; there used to be a bug that would incorrectly
    // free the first page.
    ev6_sbrk(-(sz - 3500));
    ev6_exit(0);
  }
  ev6_wait(0);

  pid = ev6_fork();
  if(pid < 0){
    printf("fork failed\n");
    ev6_exit(1);
  }
  if(pid == 0){
    // set the break in the middle of a page.
    ev6_sbrk((10*4096 + 2048) - (uint64)ev6_sbrk(0));

    // reduce the break a bit, but not enough to
    // cause a page to be freed. this used to cause
    // a panic.
    ev6_sbrk(-10);

    ev6_exit(0);
  }
  ev6_wait(0);

  ev6_exit(0);
}

// regression test. does write() with an invalid buffer pointer cause
// a block to be allocated for a file that is then not freed when the
// file is deleted? if the kernel has this bug, it will panic: balloc:
// out of blocks. assumed_free may need to be raised to be more than
// the number of free blocks. this test takes a long time.
void
badwrite(char *s)
{
  int assumed_free = 600;
  
  ev6_unlink("junk");
  for(int i = 0; i < assumed_free; i++){
    int fd = ev6_open("junk", O_CREATE|O_WRONLY);
    if(fd < 0){
      printf("open junk failed\n");
      ev6_exit(1);
    }
    ev6_write(fd, (char*)0xffffffffffL, 1);
    ev6_close(fd);
    ev6_unlink("junk");
  }

  int fd = ev6_open("junk", O_CREATE|O_WRONLY);
  if(fd < 0){
    printf("open junk failed\n");
    ev6_exit(1);
  }
  if(ev6_write(fd, "x", 1) != 1){
    printf("write failed\n");
    ev6_exit(1);
  }
  ev6_close(fd);
  ev6_unlink("junk");

  ev6_exit(0);
}

// regression test. test whether exec() leaks memory if one of the
// arguments is invalid. the test passes if the kernel doesn't panic.
void
badarg(char *s)
{
  for(int i = 0; i < 50000; i++){
    char *argv[2];
    argv[0] = (char*)0xffffffff;
    argv[1] = 0;
    ev6_exec("echo", argv);
  }
  
  ev6_exit(0);
}

// run each test in its own process. run returns 1 if child's exit()
// indicates success.
int
run(void f(char *), char *s, int count) {
  int pid;
  int xstatus;

  printf("test %s (%d/46): ", s, count);
  if((pid = fork()) < 0) {
    printf("runtest: fork error\n");
    ev6_exit(1);
  }

  if(pid == 0) {
    f(s);
    ev6_exit(0);
  } else {
    ev6_wait(&xstatus);
    if(xstatus != 0) 
      printf("FAILED\n", s);
    else printf("OK\n", s);
    return xstatus == 0;
  }

  exit(1);  // Can't reach here.
}

int
main(int argc, char *argv[])
{
  char *n = 0;
  if(argc > 1) {
    n = argv[1];
  }
  
  struct test {
    void (*f)(char *);
    char *s;
  } tests[] = {
    {reparent2, "reparent2"},
    {pgbug, "pgbug" },
    {sbrkbugs, "sbrkbugs" },
    // {badwrite, "badwrite" },
    {badarg, "badarg" },
    {reparent, "reparent" },
    {twochildren, "twochildren"},
    {forkfork, "forkfork"},
    {forkforkfork, "forkforkfork"},
    {argptest, "argptest"},
    {createdelete, "createdelete"},
    {linkunlink, "linkunlink"},
    {linktest, "linktest"},
    {unlinkread, "unlinkread"},
    {concreate, "concreate"},
    {subdir, "subdir"},
    {fourfiles, "fourfiles"},
    {sharedfd, "sharedfd"},
    {exectest, "exectest"},
    {bigargtest, "bigargtest"},
    {bigwrite, "bigwrite"},
    {bsstest, "bsstest"},
    {sbrkbasic, "sbrkbasic"},
    {sbrkmuch, "sbrkmuch"},
    {kernmem, "kernmem"},
    {sbrkfail, "sbrkfail"},
    {sbrkarg, "sbrkarg"},
    {validatetest, "validatetest"},
    {stacktest, "stacktest"},
    {opentest, "opentest"},
    {writetest, "writetest"},
    {writebig, "writebig"},
    {createtest, "createtest"},
    {openiputtest, "openiput"},
    {exitiputtest, "exitiput"},
    {iputtest, "iput"},
    {mem, "mem"},
    {pipe1, "pipe1"},
    {preempt, "preempt"},
    {exitwait, "exitwait"},
    {rmdot, "rmdot"},
    {fourteen, "fourteen"},
    {bigfile, "bigfile"},
    {dirfile, "dirfile"},
    {iref, "iref"},
    {forktest, "forktest"},
    {bigdir, "bigdir"}, // slow
    { 0, 0},
  };
    
  printf("usertests starting\n");

  if(ev6_open("usertests.ran", 0) >= 0){
    printf("already ran user tests -- rebuild fs.img (rm fs.img; make fs.img)\n");
    ev6_exit(1);
  }
  ev6_close(ev6_open("usertests.ran", O_CREATE));

  int fail = 0;
  int i = 0;  // To watch progress of usertests
  for (struct test *t = tests; t->s != 0; t++) {
	  i++;
    if((n == 0) || strcmp(t->s, n) == 0) {
      if(!run(t->f, t->s, i)){
        fail = 1;
        printf("main: counted as failed\n");
      }
    }
  }
  if(!fail)
    printf("ALL TESTS PASSED\n");
  else
    printf("SOME TESTS FAILED\n");
  ev6_exit(1);   // not reached.
  exit(1);  // Can't reach here.
}
