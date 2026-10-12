#include <sched.h>
#include <errno.h>
#include "syscall.h"

int sched_getscheduler(pid_t pid)
{
	/* BMDS: the thread's, as glibc's (devplan/processes.md §3.12). */
	return syscall(SYS_sched_getscheduler, pid);
}
