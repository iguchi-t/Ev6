struct buf;
struct context;
struct file;
struct inode;
struct pipe;
struct proc;
struct spinlock;
struct sleeplock;
struct stat;
struct superblock;
struct bcache;
struct ftable;
struct icache;
struct log;
struct pipe;
struct run;
struct kmem;
struct mlist_node;
struct mlist_header;
struct disk;

// bio.c
void            binit(void);
struct buf*     bread(uint, uint);
void            brelse(struct buf*);
void            bwrite(struct buf*);
void            bpin(struct buf*);
void            bunpin(struct buf*);

// console.c
void            consoleinit(void);
void            consoleintr(int);
void            consputc(int);
int				consolewrite(int, uint64, int);
int				consoleread(int, uint64, int);

// exec.c
int             exec(char*, char**);

// file.c
struct file*    filealloc(void);
void            fileclose(struct file*);
struct file*    filedup(struct file*);
void            fileinit(void);
int             fileread(struct file*, uint64, int n);
int             filestat(struct file*, uint64 addr);
int             filewrite(struct file*, uint64, int n);

// fs.c
void            fsinit(int);
int             dirlink(struct inode*, char*, uint);
struct inode*   dirlookup(struct inode*, char*, uint*);
struct inode*   ialloc(uint, short);
struct inode*   idup(struct inode*);
void            iinit();
void            ilock(struct inode*);
void            iput(struct inode*);
void            iunlock(struct inode*);
void            iunlockput(struct inode*);
void            iupdate(struct inode*);
int             namecmp(const char*, const char*);
struct inode*   namei(char*);
struct inode*   nameiparent(char*, char*);
int             readi(struct inode*, int, uint64, uint, uint);
void            stati(struct inode*, struct stat*);
int             writei(struct inode*, int, uint64, uint, uint);
void			readsb(int, struct superblock*);

// ramdisk.c
void            ramdiskinit(void);
void            ramdiskintr(void);
void            ramdiskrw(struct buf*);

// kalloc.c
void*           kalloc(void);
void            kfree(void *);
void            kinit();
void            freerange(void*, void*);

// log.c
void            initlog(int, struct superblock*);
void            log_write(struct buf*);
void            begin_op();
void            end_op();
void			commit();
void            write_log();


// pipe.c
int             pipealloc(struct file**, struct file**);
void            pipeclose(struct pipe*, int);
int             piperead(struct pipe*, uint64, int);
int             pipewrite(struct pipe*, uint64, int);

// printf.c
void            printf(char*, ...);
void            panic(char*) __attribute__((noreturn));
void            printfinit(void);
void            printf_without_pr(char*, ...);
void            panic_without_pr(char*) __attribute__((noreturn));

// proc.c
int             cpuid(void);
void            exit(int);
int             fork(void);
int             growproc(int);
pagetable_t     proc_pagetable(struct proc *);
void            proc_freepagetable(pagetable_t, uint64);
int             kill(int);
struct cpu*     mycpu(void);
struct cpu*     getmycpu(void);
struct proc*    myproc();
void            procinit(void);
void            scheduler(void) __attribute__((noreturn));
void            sched(void);
void            setproc(struct proc*);
void            sleep(void*, struct spinlock*);
void            userinit(void);
int             wait(uint64);
void            wakeup(void*);
void            wakeup1(struct proc*);
void            yield(void);
int             either_copyout(int user_dst, uint64 dst, void *src, uint64 len);
int             either_copyin(void *dst, int user_src, uint64 src, uint64 len);
void            procdump(void);
struct proc*    allocproc(void);
void            freeproc(struct proc*);

// swtch.S
void            swtch(struct context*, struct context*);

// spinlock.c
void            acquire(struct spinlock*);
int             holding(struct spinlock*);
void            initlock(struct spinlock*, char*);
void            release(struct spinlock*);
void            push_off(void);
void            pop_off(void);

// sleeplock.c
void            acquiresleep(struct sleeplock*);
void            releasesleep(struct sleeplock*);
int             holdingsleep(struct sleeplock*);
void            initsleeplock(struct sleeplock*, char*);

// string.c
int             memcmp(const void*, const void*, uint);
void*           memmove(void*, const void*, uint);
void*           memset(void*, int, uint);
char*           safestrcpy(char*, const char*, int);
int             strlen(const char*);
int             strncmp(const char*, const char*, uint);
char*           strncpy(char*, const char*, int);

// syscall.c
int             argint(int, int*);
int             argstr(int, char*, int);
int             argaddr(int, uint64 *);
int             fetchstr(uint64, char*, int);
int             fetchaddr(uint64, uint64*);
void            syscall();

// trap.c
extern uint     ticks;
void            trapinit(void);
void            trapinithart(void);
extern struct spinlock *tickslock;
void            usertrapret(void);
void			kernelvec(void);

// uart.c
void            uartinit(void);
void            uartintr(void);
void            uartputc(int);
int             uartgetc(void);

// vm.c
void            kvminit(void);
void            kvminithart(void);
uint64          kvmpa(uint64);
void            kvmmap(uint64, uint64, uint64, int);
int             mappages(pagetable_t, uint64, uint64, uint64, int);
pagetable_t     uvmcreate(void);
void            uvminit(pagetable_t, uchar *, uint);
uint64          uvmalloc(pagetable_t, uint64, uint64);
uint64          uvmdealloc(pagetable_t, uint64, uint64);
int             uvmcopy(pagetable_t, pagetable_t, uint64);
void            uvmfree(pagetable_t, uint64);
void            uvmunmap(pagetable_t, uint64, uint64, int);
void            uvmclear(pagetable_t, uint64);
uint64          walkaddr(pagetable_t, uint64);
int             copyout(pagetable_t, uint64, char *, uint64);
int             copyin(pagetable_t, char *, uint64, uint64);
int             copyinstr(pagetable_t, char *, uint64, uint64);

// plic.c
void            plicinit(void);
void            plicinithart(void);
uint64          plic_pending(void);
int             plic_claim(void);
void            plic_complete(int);

// virtio_disk.c
void            virtio_disk_init(void);
void            virtio_disk_rw(struct buf *, int);
void            virtio_disk_intr();
void            free_chain(int);

// after-treatment.c
void            af_return_syscall_result(int);
void            af_process_kill(void);
void            af_return_to_user(int, int);
void            af_return_to_kernel(uint64, uint64, int);

// kerneltrapret.c
int             identify_nmi_occurred_trap(int, uint64, uint64);
void            kerneltrapret(uint64, uint64, int);

// mlist.c
void            mlistinit(void);
void            init_ptb_list(pagetable_t, int);
void            register_memobj(void*, struct mlist_node*);
void            delete_memobj(void*, struct mlist_node*, uint64);
void            delete_locks_from_mlist(uint64, uint64, struct mlist_node*);
char*           my_kalloc(void*, int);
void            getcallerpcs_top(uint64*, uint64, uint64, int);
void            getcallerpcs_bottom(uint64*, uint64, uint64, int);
uint64          get_ticks(void);
uint64          sys_check_memory_space_overhead(void);

// mlist_pagetable.c
void            register_ptb_mlist(int, uint64, int);
void            delete_ptb_mlist(uint64);
void            delete_ptb_mlist_all(int);

// mlist_tracker.c
void            check_and_acquire(struct spinlock*);
void            check_and_release(struct spinlock*);
void            mlist_tracker(char*);
void            cleanup_unused_procs(int);
void            check_all_locks(int);
struct cpu*     search_cpu_from_pid(int);
struct proc*    search_proc_from_pid(int);

// nmi_handle.c
int             nmi_handle(char*);
void	        nmi_imitate(char*);
void	        nmivec(void);

// ptdup.c
void            ptdup_init(pagetable_t);
void            ptdup_create_l1(pagetable_t, pagetable_t, uint64);
void            ptdup_delete_all(pagetable_t, pagetable_t);
void            ptdup_update(pagetable_t, pagetable_t, uint64, int);
void            L0_ptes_add(pagetable_t, uint64, int);
void            L0_ptes_delete(pagetable_t, uint64, int);
void            L0_ptes_clear_user(pagetable_t, uint64);

// recovery_locking.c
void            init_recovery_lock_idx(void);
void            init_recovery_locks(void);
void            init_rcs_infos(int);
int             check_and_count_procs_in_rcs(int, void*);
int             check_and_wait_procs_in_rcs_file(void*, void*);
int             check_and_wait_procs_in_rcs_inode(void*, void*, int);
int             check_proc_in_log_commit(int);
void            enter_recovery_critical_section(int, int);
void            enter_recovery_critical_section_nodes(int, void*);
void            exit_recovery_critical_section(int, int);
void            exit_recovery_critical_section_nodes(int, void*);
void            exit_rcs_after_recovery(int, int);
void            acquire_recovery_lock(int);
void            acquire_recovery_lock_buf(void*);
void            acquire_recovery_lock_file(void*);
void            acquire_recovery_lock_inode(void*);
void            release_recovery_lock(int);
void            release_recovery_lock_buf(void*);
void            release_recovery_lock_file(void*);
void            release_recovery_lock_inode(void*);
void            free_rcs_history(int);
int             search_rcs_history(int, int);
void            update_recovery_lock_idx(int, void*, void*);

// Recovery Handlers (FS)
int             recovery_handler_buf(void*, int, uint64, uint64);
int             recovery_handler_file(void*, int, uint64, uint64);
int             recovery_handler_inode(void*, int, uint64, uint64);
int             recovery_handler_log(void*, int, uint64, uint64);
int             recovery_handler_pipe(void*, int, uint64, uint64);

// recovery_handlers_locks.c
void            recovery_handler_spinlock(char*, struct spinlock*, void*);
void            recovery_handler_sleeplock(char*, struct sleeplock*, void*, uint64);
int             recovery_handler_tickslock(void*, int, uint64, uint64);
int             recovery_handler_pid_lock(void*, int);
void            acquiresleep_wo_sleep(struct sleeplock*);

// recovery_handlers_MM.c
int             recovery_handler_kmem(void*, int, uint64, uint64);
int             recovery_handler_run(void*, int, uint64, uint64);

// recovery_handler_pagetable.c
int             recovery_handler_pagetable(int, int, struct proc*, void*, uint64, uint64);

// recovery_handlers_console.c
int             recovery_handler_devsw(void*, int);
int             recovery_handler_cons(void*, int, uint64, uint64);
int             recovery_handler_pr(void*, int, uint64, uint64);

// sysproc.c
void		    check_pcs(struct proc*);

// sysev6.c
void            rec_open_args(int, char*, int);
void            del_open_args(int);
void            copy_open_args(int, struct file*);
void            free_open_arg_table(void);
void            copy_open_arg_table(struct proc*, struct proc*);
void            pick_fd_from_oatable(char*, int);
void            update_dup_off(struct file*, int);
uint64          check_reserved_fd(struct proc*, int);

// trans.c
void            init_trans_info(void);
void            register_trans_info(int);
void            delete_trans_info(int);
void            enter_trans_log(void);
void            exit_trans_log(void);
void            enter_trans_pagetable(void);
void            exit_trans_pagetable(void);
void            enter_trans_run(struct run*);
void            exit_trans_run(void);
int             check_and_handle_trans_log(int);
int             check_and_handle_trans_logheader(int);
int             check_and_handle_trans_pagetable(int);
void            check_and_handle_trans_run(int);

// user_coop.c
void            usercoopinit(void);
int             is_enable_user_coop(int);

// number of elements in fixed-size array
#define NELEM(x) (sizeof(x)/sizeof((x)[0]))
