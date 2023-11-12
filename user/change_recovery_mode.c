#include "kernel/types.h"
#include "user/user.h"

int main(int argc, char *argv[]){
  int mode = 0;

  if(argc > 1 && 0 <= atoi(argv[1]) && atoi(argv[1]) <= 12)
    mode = atoi(argv[1]);
  change_recovery_mode(mode);

  exit(0);
}

