// Transactions to protect updating gap between real data structures and meta-data.
// In FS, for managing copy & switching from old icache/ftable to new one.

// The flags for judging in/out of transaction codes.
struct trans_info{
  int pid;
  int pagetable_ntrans;  // Depth of transaction nesting.
  int log_ntrans;
  int run_ntrans;
};

// For managing transaction flags of all of the processes.
struct trans_info_array{
  int recovering;
  struct spinlock lock;
  struct trans_info array[NPROC+1];
};

// Signs to identify updated member of struct log.
#define LOG_LOGHEADER   0x1
#define LOG_OUTSTANDING 0x2

// Flags to identify which transaction flag is wanted to check.
#define TRANS_LOG 0x1
#define TRANS_PAGETABLE 0x2
#define TRANS_RUN 0x3
