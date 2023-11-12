// init: The initial user-level program

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fcntl.h"

char *argv[] = { "sh", 0 };

int
main(void)
{
  int pid, wpid;

  if(ev6_open("console", O_RDWR) < 0){
    ev6_mknod("console", 1, 1);
    ev6_open("console", O_RDWR);
  }
  ev6_dup(0);  // stdout
  ev6_dup(0);  // stderr

  for(;;){
    printf("init: starting sh\n");
    pid = ev6_fork();
    if(pid < 0){
      printf("init: fork failed\n");
      ev6_exit(1);
    }
    if(pid == 0){
      ev6_exec("sh", argv);
      printf("init: exec sh failed\n");
      ev6_exit(1);
    }
    while((wpid=ev6_wait(0)) >= 0 && wpid != pid){
      //printf("zombie!\n");
    }
  }
}
