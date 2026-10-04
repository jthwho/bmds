/* BMDS: programs run on a function table, not ecall. Every system call
 * goes to __bmds_syscall() (sdk/crt/linux.c in the program), which carries
 * out the Linux call on the BMDS system call table. */

#define __SYSCALL_LL_E(x) \
((union { long long ll; long l[2]; }){ .ll = x }).l[0], \
((union { long long ll; long l[2]; }){ .ll = x }).l[1]
#define __SYSCALL_LL_O(x) __SYSCALL_LL_E((x))

hidden long __bmds_syscall(long, long, long, long, long, long, long);

static inline long __syscall0(long n)
{
	return __bmds_syscall(n, 0, 0, 0, 0, 0, 0);
}

static inline long __syscall1(long n, long a)
{
	return __bmds_syscall(n, a, 0, 0, 0, 0, 0);
}

static inline long __syscall2(long n, long a, long b)
{
	return __bmds_syscall(n, a, b, 0, 0, 0, 0);
}

static inline long __syscall3(long n, long a, long b, long c)
{
	return __bmds_syscall(n, a, b, c, 0, 0, 0);
}

static inline long __syscall4(long n, long a, long b, long c, long d)
{
	return __bmds_syscall(n, a, b, c, d, 0, 0);
}

static inline long __syscall5(long n, long a, long b, long c, long d, long e)
{
	return __bmds_syscall(n, a, b, c, d, e, 0);
}

static inline long __syscall6(long n, long a, long b, long c, long d, long e, long f)
{
	return __bmds_syscall(n, a, b, c, d, e, f);
}

#define IPC_64 0
