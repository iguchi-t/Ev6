// NMI Information to operate the NMI handler and the M-List Tracker.

// The contents of NMI Queue.
struct nmi_info{
  char* addr;  // Broken memobj address or terminate status.
  int pid;     // Process ID which noticed memory error.
  uint64 sp;   // Contents of $sp register of pid process.
  uint64 s0;   // Contents of $s0 register of pid process.
};

#define ISINSIDE(addr, start, end) ((start) <= (addr) && (addr) <= (end))
