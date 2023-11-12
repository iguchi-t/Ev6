#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int i;

  if(argc < 2){
    fprintf(2, "Usage: rm files...\n");
    ev6_exit(1);
  }

  for(i = 1; i < argc; i++){
    if(ev6_unlink(argv[i]) < 0){
      fprintf(2, "rm: %s failed to delete\n", argv[i]);
      break;
    }
  }

  ev6_exit(0);
  exit(1);  // Can't reach here.
}
