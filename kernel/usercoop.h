// Manage open syscall's arguments for Re-Open under cooperation with user land.
// Under multi-thread environment, this object needs exclusive control.
struct open_args{
  char path[MAXPATH];
  int omode;
};

// Arguments table for Re-Open.
// Each process has its own table. 
struct open_arg_table{
  struct open_args args[NOFILE];
  int pid;
  struct spinlock lock;
};