#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

char buf[512];

void
cat(int fd)
{
  int n;

  while((n = ev6_read(fd, buf, sizeof(buf))) > 0) {
    if (ev6_write(1, buf, n) != n) {
      printf("cat: write error\n");
      ev6_exit(1);
    }
  }
  if(n < 0){
    printf("cat: read error\n");
    ev6_exit(1);
  }
}

int
main(int argc, char *argv[])
{
  int fd, i;

  if(argc <= 1){
    cat(0);
    ev6_exit(1);
  }

  for(i = 1; i < argc; i++){
    if((fd = ev6_open(argv[i], 0)) < 0){
      printf("cat: cannot open %s\n", argv[i]);
      ev6_exit(1);
    }
    cat(fd);
    ev6_close(fd);
  }
  ev6_exit(0);
  exit(1);  // Can't reach here. 
}
