// Create a zombie process that
// must be reparented at exit.

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(void)
{
  if(ev6_fork() > 0)
    ev6_sleep(5);  // Let child exit before parent.
  ev6_exit(0);
  exit(1);  // Can't reach here.
}
