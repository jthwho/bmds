#include <sched.h>
#include <errno.h>
#include "syscall.h"

int sched_setparam(pid_t pid, const struct sched_param *param)
{
	/* BMDS: the thread's, as glibc's (devplan/processes.md §3.12). */
	return syscall(SYS_sched_setparam, pid, param);
}
