#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

char buf[512];

void
wc(int fd, char *name)
{
  int i, n;
  int l, w, c, inword;

  l = w = c = 0;
  inword = 0;
  while((n = ev6_read(fd, buf, sizeof(buf))) > 0){
    for(i=0; i<n; i++){
      c++;
      if(buf[i] == '\n')
        l++;
      if(strchr(" \r\t\n\v", buf[i]))
        inword = 0;
      else if(!inword){
        w++;
        inword = 1;
      }
    }
  }
  if(n < 0){
    printf("wc: read error\n");
    ev6_exit(1);
  }
  printf("%d %d %d %s\n", l, w, c, name);
}

int
main(int argc, char *argv[])
{
  int fd, i;

  if(argc <= 1){
    wc(0, "");
    ev6_exit(0);
  }

  for(i = 1; i < argc; i++){
    if((fd = ev6_open(argv[i], 0)) < 0){
      printf("wc: cannot open %s\n", argv[i]);
      ev6_exit(1);
    }
    wc(fd, argv[i]);
    ev6_close(fd);
  }
  ev6_exit(0);
  exit(1);  // Can't reach here.
}
