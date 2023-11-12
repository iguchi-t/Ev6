// Test that fork fails gracefully.
// Tiny executable so that the limit can be filling the proc table.

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define N  1000

void
print(const char *s)
{
  ev6_write(1, s, strlen(s));
}

void
forktest(void)
{
  int n, pid;

  print("fork test\n");

  for(n=0; n<N; n++){
    pid = ev6_fork();
    if(pid < 0)
      break;
    if(pid == 0)
      ev6_exit(0);
  }

  if(n == N){
    print("fork claimed to work N times!\n");
    ev6_exit(1);
  }

  for(; n > 0; n--){
    if(ev6_wait(0) < 0){
      print("wait stopped early\n");
      ev6_exit(1);
    }
  }

  if(ev6_wait(0) != -1){
    print("wait got too many\n");
    ev6_exit(1);
  }

  print("fork test OK\n");
}

int
main(void)
{
  forktest();
  ev6_exit(0);
  exit(1);  // Can't reach here.
}
