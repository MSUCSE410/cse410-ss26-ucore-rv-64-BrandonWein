#include "syscall.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"

uint64 sys_write(int fd, uint64 va, uint len)
{
	debugf("sys_write fd = %d va = %x, len = %d", fd, va, len);
	if (fd != STDOUT)
			return -1;
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
	debugf("size = %d", size);
	for (int i = 0; i < size; ++i) {
			console_putchar(str[i]);
	}
	return size;
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

uint64 sys_gettimeofday(uint64 va, int _tz)
{
	struct proc *p = curr_proc();
	uint64 cycle = get_cycle();
	TimeVal val;
	val.sec = cycle / CPU_FREQ;
	val.usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
	if (copyout(p->pagetable, va, (char *)&val, sizeof(TimeVal)) < 0)
			return -1;
	return 0;
}

uint64 sys_mmap(uint64 start, uint64 len, int port, int flag, int fd)
{
	if (len == 0)
			return 0;
	if (len > (1ULL << 30))
			return -1;
	if (port & ~0x7)
			return -1;
	if (!(port & 0x7))
			return -1;
	if (start % PGSIZE != 0)
			return -1;

	struct proc *p = curr_proc();
	uint64 end = PGROUNDUP(start + len);

	for (uint64 a = start; a < end; a += PGSIZE) {
			if (walkaddr(p->pagetable, a) != 0)
					return -1;
	}

	int pte_flags = PTE_U;
	if (port & 1) pte_flags |= PTE_R;
	if (port & 2) pte_flags |= PTE_W;
	if (port & 4) pte_flags |= PTE_X;

	for (uint64 a = start; a < end; a += PGSIZE) {
			void *pa = kalloc();
			if (pa == 0) {
					for (uint64 b = start; b < a; b += PGSIZE)
							uvmunmap(p->pagetable, b, 1, 1);
					return -1;
			}
			memset(pa, 0, PGSIZE);
			if (mappages(p->pagetable, a, PGSIZE, (uint64)pa, pte_flags) != 0) {
					kfree(pa);
					for (uint64 b = start; b < a; b += PGSIZE)
							uvmunmap(p->pagetable, b, 1, 1);
					return -1;
			}
	}
	return 0;
}

uint64 sys_munmap(uint64 start, uint64 len)
{
	if (len == 0)
			return 0;
	if (start % PGSIZE != 0)
			return -1;

	struct proc *p = curr_proc();
	uint64 end = PGROUNDUP(start + len);

	for (uint64 a = start; a < end; a += PGSIZE) {
			if (walkaddr(p->pagetable, a) == 0)
					return -1;
	}

	uvmunmap(p->pagetable, start, (end - start) / PGSIZE, 1);
	return 0;
}

/*
* LAB1: you may need to define sys_task_info here
*/
uint64 sys_task_info(uint64 ti)
{
	struct proc *p = curr_proc();
	TaskInfo info;
	info.status = Running;
	memmove(info.syscall_times, p->syscall_times, sizeof(info.syscall_times));
	uint64 current_ms = get_cycle() / (CPU_FREQ / 1000);
	info.time = (int)(current_ms - p->first_time);
	if (copyout(p->pagetable, ti, (char *)&info, sizeof(TaskInfo)) < 0)
			return -1;
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
	/*
	* LAB1: you may need to update syscall counter for task info here
	*/
	if (id >= 0 && id < MAX_SYSCALL_NUM)
			curr_proc()->syscall_times[id]++;
	switch (id) {
	case SYS_write:
			ret = sys_write(args[0], args[1], args[2]);
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
	case SYS_task_info:
			ret = sys_task_info(args[0]);
			break;
	case SYS_mmap:
			ret = sys_mmap(args[0], args[1], args[2], args[3], args[4]);
			break;
	case SYS_munmap:
			ret = sys_munmap(args[0], args[1]);
			break;
	default:
			ret = -1;
			errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}