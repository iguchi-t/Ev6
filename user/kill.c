#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char **argv)
{
  int i;

  if(argc < 2){
    fprintf(2, "usage: kill pid...\n");
    ev6_exit(1);
  }
  for(i=1; i<argc; i++)
    ev6_kill(atoi(argv[i]));
  ev6_exit(0);
  exit(1);  // Can't reach here.
}
