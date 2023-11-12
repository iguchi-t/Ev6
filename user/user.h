struct stat;
struct rtcdate;

// system calls
int fork(void);
int exit(int) __attribute__((noreturn));
int wait(int*);
int pipe(int*);
int write(int, const void*, int);
int read(int, void*, int);
int close(int);
int kill(int);
int exec(char*, char**);
int open(const char*, int);
int mknod(const char*, short, short);
int unlink(const char*);
int fstat(int fd, struct stat*);
int link(const char*, const char*);
int mkdir(const char*);
int chdir(const char*);
int dup(int);
int getpid(void);
char* sbrk(int);
int sleep(int);
int uptime(void);

// ulib.c
int stat(const char*, struct stat*);
char* strcpy(char*, const char*);
void *memmove(void*, const void*, int);
char* strchr(const char*, char c);
int strcmp(const char*, const char*);
void fprintf(int, const char*, ...);
void printf(const char*, ...);
char* gets(char*, int max);
uint strlen(const char*);
void* memset(void*, int, uint);
void* malloc(uint);
void free(void*);
int atoi(const char*);

// Ev6 system call
void change_recovery_mode(int);

// Userland Cooperation
int enable_user_coop(void);
int disable_user_coop(void);
int check_reserved_fd(int);
int check_reserved_fd_all(void);
int pick_fd(const char*, int);
int reopen(void);

// libev6.c
int ev6_fork(void);
int ev6_exit(int);
int ev6_wait(int*);
int ev6_pipe(int*);
int ev6_write(int, const void*, int);
int ev6_read(int, void*, int);
int ev6_close(int);
int ev6_kill(int);
int ev6_exec(char*, char**);
int ev6_open(const char*, int);
int ev6_mknod(const char*, short, short);
int ev6_unlink(const char*);
int ev6_fstat(int fd, struct stat*);
int ev6_link(const char*, const char*);
int ev6_mkdir(const char*);
int ev6_chdir(const char*);
int ev6_dup(int);
int ev6_getpid(void);
char* ev6_sbrk(int);
int ev6_sleep(int);
int ev6_uptime(void);