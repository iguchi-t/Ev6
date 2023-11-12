/* 
 * After-Treatment policies and related flags to execute termination of recovery operations.
 */

// The signs of finishing recovery.
#define SYSCALL_SUCCESS  0x2  // As succeeded the system call.
#define SYSCALL_FAIL     0x3  // As failing the system call.
#define SYSCALL_REDO     0x4  // Request the user to redo the system call.
#define REOPEN_SYSCALL_FAIL 0x5  // Combination of Re-Open & Syscall Fail.
#define REOPEN_SYSCALL_REDO 0x6  // Combination of Re-Open & Syscall Redo.
#define FAIL_STOP        0x7  // Stop system to avoid inconsistency.
#define PROCESS_KILL     0x8  // Terminate the process.
#define RETURN_TO_USER   0x9  // By returning to interrupted user's procedure for handing NMI during hardware interrupts.
#define RETURN_TO_KERNEL 0xa  // By returning to interrupted kernel procedure.
#define PIPE             0xb  // Recovered pipe and determine the way of termination in each proc.

// The signs of the place of the NMI occurred.
#define USERTRAP 0x1
#define KERNELTRAP 0x2

// Flag to indicate the entry is reserved for re-open.
#define RESERVED -1

// Status of Ev6 User Cooperation.
#define ENABLE  1
#define DISABLE 0
