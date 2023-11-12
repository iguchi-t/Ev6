#define RL_FLAG_BCACHE 0x1  // Giant section
#define RL_FLAG_CONS   0x2
#define RL_FLAG_FTABLE 0x3  // Giant section
#define RL_FLAG_ICACHE 0x4  // Giant section
#define RL_FLAG_KMEM   0x5
#define RL_FLAG_LOG    0x6
#define RL_FLAG_PIPE   0x7
#define RL_FLAG_PR     0x8
#define RL_FLAG_BUF    0x9
#define RL_FLAG_FILE   0xa
#define RL_FLAG_INODE  0xb

#define LAST_FLAG_WO_IDX 8
#define RL_NFLAGS        8 + NBUF + NFILE + NINODE + 1  // The number of flags for the recovery-locking mechanism.
#define RL_FLAG_MAX      RL_NFLAGS

// Exception Flags
#define LOG_COMMIT  1  // In the commit() in log.c
#define ICACHE_FORK 2  // In the fork() in proc.c (deadlock can occur, more sophisticated way is needed)

// R.C.S info
#define RCS_INFO_HISTORY_SIZE 5

// R.C.S exit flag of hardware interrupt cases.
#define CONSINTR_CONS 1
#define CONSINTR_PR   2
#define CLCKINTR_TICKSLOCK 3


// Indexing some types of struct which has some nodes to their flags.
struct recovery_lock_idx{
  int flag;
  void *addr;
};

// Recovery-locking infomation.
struct recovery_lock{
  int num;         // The number of processes which in the recovery-locking critical section.
  int exception;   // Indicate some process are in excepting section (ex. begin_op() in log.c)
  struct spinlock lock;  // Recovery Lock
  struct spinlock lk;    // Recovery Lock's lock
};

// Infomation about Recovery Critical Section nesting of each process.
struct proc_rcs_info{
  int pid;
  int history_idx[RCS_INFO_HISTORY_SIZE];  // R.C.S nesting history recording RL_FLAG.
};
