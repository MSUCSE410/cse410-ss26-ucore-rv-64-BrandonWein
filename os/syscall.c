#include "syscall.h"
#include "console.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"

uint64 console_write(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
	tracef("write size = %d", size);
	for (int i = 0; i < size; ++i) {
		console_putchar(str[i]);
	}
	return len;
}

uint64 console_read(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	tracef("read size = %d", len);
	for (int i = 0; i < len; ++i) {
		int c = consgetc();
		str[i] = c;
	}
	copyout(p->pagetable, va, str, len);
	return len;
}

uint64 sys_write(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_write(va, len);
	case FD_INODE:
		return inodewrite(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
}

uint64 sys_read(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_read(va, len);
	case FD_INODE:
		return inoderead(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
}

__attribute__((noreturn)) void sys_exit(int code)
{
	exit(code);
	__builtin_unreachable();
}

uint64 sys_sched_yield()
{
	yield();
	return 0;
}

uint64 sys_gettimeofday(uint64 val, int _tz)
{
	struct proc *p = curr_proc();
	uint64 cycle = get_cycle();
	TimeVal t;
	t.sec = cycle / CPU_FREQ;
	t.usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
	copyout(p->pagetable, val, (char *)&t, sizeof(TimeVal));
	return 0;
}

uint64 sys_getpid()
{
	return curr_proc()->pid;
}

uint64 sys_getppid()
{
	struct proc *p = curr_proc();
	return p->parent == NULL ? IDLE_PID : p->parent->pid;
}

uint64 sys_clone()
{
	debugf("fork!");
	return fork();
}

static inline uint64 fetchaddr(pagetable_t pagetable, uint64 va)
{
	uint64 *addr = (uint64 *)useraddr(pagetable, va);
	return *addr;
}

uint64 sys_exec(uint64 path, uint64 uargv)
{
	struct proc *p = curr_proc();
	char name[MAX_STR_LEN];
	copyinstr(p->pagetable, name, path, MAX_STR_LEN);
	uint64 arg;
	static char strpool[MAX_ARG_NUM][MAX_STR_LEN];
	char *argv[MAX_ARG_NUM];
	int i;
	for (i = 0; uargv && (arg = fetchaddr(p->pagetable, uargv));
	     uargv += sizeof(char *), i++) {
		copyinstr(p->pagetable, (char *)strpool[i], arg, MAX_STR_LEN);
		argv[i] = (char *)strpool[i];
	}
	argv[i] = NULL;
	return exec(name, (char **)argv);
}

uint64 sys_wait(int pid, uint64 va)
{
	struct proc *p = curr_proc();
	int *code = (int *)useraddr(p->pagetable, va);
	return wait(pid, code);
}

uint64 sys_spawn(uint64 va)
{
	// Get the current process structure
	struct proc *p = curr_proc();
	// Buffer to store the executable name copied from user space
	char name[MAX_STR_LEN];
	// Copy the executable name from user space into kernel space
	copyinstr(p->pagetable, name, va, MAX_STR_LEN);

	// Look up the inode for the executable file by name
	struct inode *ip = namei(name);
	// Return error if the file doesn't exist
	if (ip == NULL)
		return -1;

	// Allocate a new process structure for the spawned process
	struct proc *np = allocproc();
	// Release the inode and return error if process allocation fails
	if (np == NULL) {
		iput(ip);
		return -1;
	}

	// Initialize standard I/O streams (stdin, stdout, stderr) for the new process
	init_stdio(np);
	// Load the executable binary into the new process's memory space from disk
	bin_loader(ip, np);
	// Decrement reference count and free the inode (we no longer need it)
	iput(ip);

	// Set up argument vector: argv[0] is the program name, argv[1] is NULL terminator
	char *argv[2];
	argv[0] = name;
	argv[1] = NULL;
	// Push the argument vector onto the new process's stack and store return value in a0 register
	np->trapframe->a0 = push_argv(np, argv);

	// Set the current process as the parent of the new process
	np->parent = p;
	// Add the new process to the task queue so it can be scheduled
	add_task(np);
	// Return the process ID of the newly spawned process
	return np->pid;
}

uint64 sys_set_priority(long long prio)
{
	// Validate that the priority is at least 2 (minimum allowed priority for stride scheduling)
	if (prio < 2)
		// Return error if priority is too low
		return -1;
	// Return the valid priority value
	return prio;
}

uint64 sys_openat(uint64 va, uint64 omode, uint64 _flags)
{
	struct proc *p = curr_proc();
	char path[200];
	copyinstr(p->pagetable, path, va, 200);
	return fileopen(path, omode);
}

uint64 sys_close(int fd)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d", fd);
		return -1;
	}
	fileclose(f);
	p->files[fd] = 0;
	return 0;
}
// sys_fstat retrieves file statistics for a given file descriptor.
// It validates the fd, accesses the inode, and copies relevant metadata
// (device, inode number, mode, link count) to a user-provided buffer.
int sys_fstat(int fd, uint64 stat)
{
	// Validate that the file descriptor is within the valid range
	if (fd < 0 || fd >= FD_BUFFER_SIZE)
		// Return error if fd is out of bounds
		return -1;
	// Get the current process structure
	struct proc *p = curr_proc();
	// Get the file structure associated with this file descriptor
	struct file *f = p->files[fd];
	// Check if the file descriptor is valid and points to an inode (not console/pipe)
	if (f == NULL || f->type != FD_INODE)
		// Return error if fd is invalid or not an inode
		return -1;

	// Get the inode associated with this file
	struct inode *ip = f->ip;
	// Read the inode from disk into memory if not already cached
	ivalid(ip);

	// Local structure to hold file statistics matching Linux stat structure layout
	struct {
		uint64 dev;
		uint64 ino;
		uint32 mode;
		uint32 nlink;
		uint64 pad[7];
	} st;

	// Copy device number from inode to stat structure
	st.dev = ip->dev;
	// Copy inode number from inode to stat structure
	st.ino = ip->inum;
	// Set mode field: 0x040000 for directories, 0x100000 for regular files
	st.mode = (ip->type == T_DIR) ? 0x040000 : 0x100000;
	// Copy hard link count from inode to stat structure
	st.nlink = ip->nlink;
	// Zero out the padding fields for the user
	memset(st.pad, 0, sizeof(st.pad));

	// Copy the stat structure to user space memory
	if (copyout(p->pagetable, stat, (char *)&st, sizeof(st)) < 0)
		// Return error if the copyout to user space fails
		return -1;
	// Return success
	return 0;
}

// sys_linkat creates a hard link to an existing file.
// It copies the old path and new path from user space, looks up the source inode,
// increments its link count, and adds a new directory entry pointing to the same inode.
int sys_linkat(int olddirfd, uint64 oldpath, int newdirfd, uint64 newpath, uint64 flags)
{
	// Get the current process structure
	struct proc *p = curr_proc();
	// Buffers to store the paths copied from user space
	char old[MAXPATH], new[MAXPATH];
	// Copy the old file path from user space into kernel space
	copyinstr(p->pagetable, old, oldpath, MAXPATH);
	// Copy the new link path from user space into kernel space
	copyinstr(p->pagetable, new, newpath, MAXPATH);

	// Check if the old and new paths are the same (hard link to self is not allowed)
	// "Link a file with the same name" → error
	if (strncmp(old, new, MAXPATH) == 0)
		// Return error if trying to create a hard link with the same name
		return -1;

	// Look up the inode for the file to be hard-linked
	struct inode *ip = namei(old);
	// Return error if the source file doesn't exist
	if (ip == NULL)
		return -1;

	// Read the inode from disk into memory if not already cached
	ivalid(ip);
	// Increment the hard link count since we're adding another directory entry pointing to this inode
	ip->nlink++;
	// Write the updated inode (with new link count) back to disk
	iupdate(ip);

	// Get the root directory inode to add the new directory entry
	struct inode *dp = root_dir();
	// Create the hard link by adding a new directory entry pointing to the same inode number
	if (dirlink(dp, new, ip->inum) < 0) {
		// If dirlink fails, rollback: decrement the link count we just incremented
		ip->nlink--;
		// Write the rolled-back inode back to disk
		iupdate(ip);
		// Release references to both inodes
		iput(dp);
		iput(ip);
		// Return error indicating the operation failed
		return -1;
	}
	// Release reference to the directory inode
	iput(dp);
	// Release reference to the linked inode
	iput(ip);
	// Return success
	return 0;
}

// sys_unlinkat removes a directory entry (unlinks a file).
// It copies the path from user space, looks up the inode, removes the directory entry,
// decrements the link count, and frees the inode if no links remain.
int sys_unlinkat(int dirfd, uint64 name, uint64 flags)
{
	// Get the current process structure
	struct proc *p = curr_proc();
	// Buffer to store the path copied from user space
	char path[MAXPATH];
	// Copy the file path from user space into kernel space
	copyinstr(p->pagetable, path, name, MAXPATH);

	// Get the root directory inode
	struct inode *dp = root_dir();
	// Variable to store the offset of the directory entry within the directory data
	uint off;
	// Look up the file to be unlinked and get its inode and the offset of its directory entry
	struct inode *ip = dirlookup(dp, path, &off);
	// Return error if the file to unlink doesn't exist
	if (ip == NULL) {
		iput(dp);
		return -1;
	}

	// Create a zeroed-out directory entry structure to overwrite the existing entry
	// Zero out the directory entry
	struct dirent de;
	memset(&de, 0, sizeof(de));
	// Overwrite the directory entry at offset 'off' with the zeroed entry to remove the file name
	if (writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
		// Panic if writing to directory fails (critical error, can't continue)
		panic("unlink: writei");
	// Release reference to the directory inode
	iput(dp);

	// Read the inode from disk into memory if not already cached
	ivalid(ip);
	// Decrement the hard link count since we removed one directory entry pointing to this inode
	ip->nlink--;
	// Write the updated inode (with decremented link count) back to disk
	iupdate(ip);
	// Release reference to the inode; if nlink == 0, the inode will be freed and blocks truncated
	iput(ip);  // will free the inode & blocks if nlink hit 0
	// Return success
	return 0;
}

extern char trap_page[];

void syscall()
{
	struct trapframe *trapframe = curr_proc()->trapframe;
	int id = trapframe->a7, ret;
	uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
			   trapframe->a3, trapframe->a4, trapframe->a5 };
	tracef("syscall %d args = [%x, %x, %x, %x, %x, %x]", id, args[0],
	       args[1], args[2], args[3], args[4], args[5]);
	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
		break;
	case SYS_read:
		ret = sys_read(args[0], args[1], args[2]);
		break;
	case SYS_openat:
		ret = sys_openat(args[0], args[1], args[2]);
		break;
	case SYS_close:
		ret = sys_close(args[0]);
		break;
	case SYS_exit:
		sys_exit(args[0]);
		// __builtin_unreachable();
	case SYS_sched_yield:
		ret = sys_sched_yield();
		break;
	case SYS_gettimeofday:
		ret = sys_gettimeofday(args[0], args[1]);
		break;
	case SYS_getpid:
		ret = sys_getpid();
		break;
	case SYS_getppid:
		ret = sys_getppid();
		break;
	case SYS_clone: // SYS_fork
		ret = sys_clone();
		break;
	case SYS_execve:
		ret = sys_exec(args[0], args[1]);
		break;
	case SYS_wait4:
		ret = sys_wait(args[0], args[1]);
		break;
	// Handle the fstat syscall: get file statistics
	case SYS_fstat:
	    // Call sys_fstat with file descriptor and pointer to stat buffer
	    ret = sys_fstat(args[0],args[1]);
		// Break to prevent falling through to the next case
		break;
	// Handle the linkat syscall: create a hard link to a file
	case SYS_linkat:
	    // Call sys_linkat with old path, new path, and flags
	    ret = sys_linkat(args[0],args[1],args[2],args[3],args[4]);
		// Break to prevent falling through to the next case
		break;
	// Handle the unlinkat syscall: remove a hard link to a file
	case SYS_unlinkat:
	    // Call sys_unlinkat with directory fd, path, and flags
	    ret = sys_unlinkat(args[0],args[1],args[2]);
		// Break to prevent falling through to the next case (important for correctness!)
		break;
	case SYS_spawn:
		ret = sys_spawn(args[0]);
		break;
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}
