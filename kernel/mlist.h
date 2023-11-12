// The definition of memory objects list(M-List)'s content and header structure.

// Recovery Modes
#define AGGRESSIVE 0
#define CONSERVATIVE 1

// Flags to identify whether register or delete the memory object's address.
#define REG 0  // Register to the M-List
#define DEL 1  // Delete from the M-List
#define DEPTH 30  // The depth of kernel stack searching.

// Content of M-List for keeping memory objects addresses.
struct mlist_node {
  void  *addr;
  struct mlist_node *next;
};

// The all address lists header node
struct mlist_header {
  // File System's memory objects.
  struct mlist_node *buf_list;  // buf
  struct mlist_node *fil_list;  // file
  struct mlist_node *ino_list;  // inode
  struct mlist_node *log_list;  // log
  struct mlist_node *lhd_list;  // logheader
  struct mlist_node *pip_list;  // pipe
  struct mlist_node *slp_list;  // sleeplock
  struct mlist_node *spn_list;  // spinlock

  // Console's memory objects.
  struct mlist_node *con_list;  // cons
  struct mlist_node *dev_list;  // devsw
  struct mlist_node *pr_list;   // pr

  // Memory Allocator's memory objects.
  struct mlist_node *kmm_list;  // kmem
  struct mlist_node *pgd_list;  // page directory (not struct, includes kpgdir)
  struct mlist_node *run_list;  // run
  uint64           *ptb_list;  // page table / page directory (not struct)

  // M-List locks.
  struct spinlock  giant_lock;  // Giant lock on the whole M-List
  struct spinlock  ptb_lock;    // Fine-grained M-List lock for page table
};

// Record recovered memobj's address
struct recovered_addr_node {
  char *start;
  char *end;
  int terminate_flag;

  // To identify R.C.S in the case of that an NMI is occurred in the interrupt handlers from old R.C.S.
  // In Ev6, this case may happen in file and inode.
  int pid;
  int rcs_flag;
};

// Process content to add it to M-List node (for pagetable)
#define PID2MLNODE(pid) (pid << 2)
// Extract content from M-List node (for pagetable)
#define MLNODE2PA(node) (node & ~0xFFF)
#define MLNODE2PID(node) ((node & 0xFFD) >> 2)
#define MLNODE2LEVEL(node) (node & 0x3)
