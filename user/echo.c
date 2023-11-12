#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int i;

  for(i = 1; i < argc; i++){
    ev6_write(1, argv[i], strlen(argv[i]));
    if(i + 1 < argc){
      ev6_write(1, " ", 1);
    } else {
      ev6_write(1, "\n", 1);
    }
  }
  ev6_exit(0);
  exit(1);  // Can't reach here.
}
