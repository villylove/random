/*	$NetBSD: emul.c,v 1.185 2017/11/21 15:22:06 ozaki-r Exp $	*/

/*
 * Copyright (c) 2007-2011 Antti Kantee.  All Rights Reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: emul.c,v 1.185 2017/11/21 15:22:06 ozaki-r Exp $");

#include <sys/param.h>
#include <sys/cprng.h>
#include <sys/filedesc.h>
#include <sys/fstrans.h>
#include <sys/kauth.h>
#include <sys/module.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <sys/pserialize.h>
#ifdef LOCKDEBUG
#include <sys/sleepq.h>
#endif

#include <dev/cons.h>

#include <rump-sys/kern.h>

#include <rump/rumpuser.h>

void (*rump_vfs_fini)(void) = (void *)nullop;

/*
 * physmem is largely unused (except for nmbcluster calculations),
 * so pick a default value which suits ZFS.  if an application wants
 * a very small memory footprint, it can still adjust this before
 * calling rump_init()
 */
#define PHYSMEM 512*256
psize_t physmem = PHYSMEM;
int nkmempages = PHYSMEM/2; /* from le chapeau */
#undef PHYSMEM

struct vnode *rootvp;
dev_t rootdev = NODEV;

const int schedppq = 1;
bool mp_online = false;
struct timespec boottime;
int cold = 1;
int boothowto = AB_SILENT;
struct tty *constty;

const struct bdevsw *bdevsw0[255];
const struct bdevsw **bdevsw = bdevsw0;
const int sys_cdevsws = 255;
int max_cdevsws = 255;

const struct cdevsw *cdevsw0[255];
const struct cdevsw **cdevsw = cdevsw0;
const int sys_bdevsws = 255;
int max_bdevsws = 255;

int mem_no = 2;

device_t booted_device;
device_t booted_wedge;
int booted_partition;
const char *booted_method;

/* XXX: unused */
kmutex_t tty_lock;
krwlock_t exec_lock;

/* sparc doesn't sport constant page size, pretend we have 4k pages */
#ifdef __sparc__
int nbpg = 4096;
int pgofset = 4096-1;
int pgshift = 12;
#endif

/* on sun3 VM_MAX_ADDRESS is a const variable */
/* XXX: should be moved into rump.c and initialize for sun3 and sun3x? */
#ifdef sun3
const vaddr_t kernbase = KERNBASE3;
#endif

struct loadavg averunnable = {
	{ 0 * FSCALE,
	  1 * FSCALE,
	  11 * FSCALE, },
	FSCALE,
};

/*
 * Include the autogenerated list of auto-loadable syscalls
 */
#include <kern/syscalls_autoload.c>

struct emul emul_netbsd = {
	.e_name = "netbsd-rump",
	.e_sysent = rump_sysent,
#ifndef __HAVE_MINIMAL_EMUL
	.e_nsysent = SYS_NSYSENT,
#endif
	.e_vm_default_addr = uvm_default_mapaddr,
#ifdef __HAVE_SYSCALL_INTERN
	.e_syscall_intern = syscall_intern,
#endif
	.e_sc_autoload = netbsd_syscalls_autoload,
};

cprng_strong_t *kern_cprng;

/* not used, but need the symbols for pointer comparisons */
syncobj_t mutex_syncobj, rw_syncobj;

int
kpause(const char *wmesg, bool intr, int timeo, kmutex_t *mtx)
{
	extern int hz;
	int rv __diagused;
	uint64_t sec, nsec;

	if (mtx)
		mutex_exit(mtx);

	sec = timeo / hz;
	nsec = (timeo % hz) * (1000000000 / hz);
	rv = rumpuser_clock_sleep(RUMPUSER_CLOCK_RELWALL, sec, nsec);
	KASSERT(rv == 0);

	if (mtx)
		mutex_enter(mtx);

	return 0;
}

vaddr_t
calc_cache_size(vsize_t vasz, int pct, int va_pct)
{
	paddr_t t;

	t = (paddr_t)physmem * pct / 100 * PAGE_SIZE;
	if ((vaddr_t)t != t) {
		panic("%s: needs tweak", __func__);
	}
	return t;
}

#define	RETURN_ADDRESS	(uintptr_t)__builtin_return_address(0)

void
assert_sleepable(void)
{
	const char *reason = NULL;

	/* always sleepable, although we should improve this */

	if (!pserialize_not_in_read_section()) {
		reason = "pserialize";
	}

	if (reason) {
		panic("%s: %s caller=%p", __func__, reason,
		    (void *)RETURN_ADDRESS);
	}
}

void
module_init_md(void)
{

	/*
	 * Nothing for now.  However, we should load the librump
	 * symbol table.
	 */
}

/*
 * Try to emulate all the MD definitions of DELAY() / delay().
 * Would be nice to fix the #defines in MD headers, but this quicker.
 *
 * XXX: we'd need a rumpuser_clock_sleep_nowrap() here.  Since we
 * don't have it in the current hypercall revision, busyloop.
 * Note that rather than calibrate a loop delay and work with that,
 * get call gettime (which does not block) in a loop to make sure
 * we didn't get virtual ghosttime.  That might be slightly inaccurate
 * for very small delays ...
 *
 * The other option would be to run a thread in the hypervisor which
 * sleeps for us and we can wait for it using rumpuser_cv_wait_nowrap()
 * Probably too fussy.  Better just wait for hypercall rev 18 ;)
 */
static void
rump_delay(unsigned int us)
{
	struct timespec target, tmp;
	uint64_t sec, sec_ini, sec_now;
	long nsec, nsec_ini, nsec_now;
	int loops;

	rumpuser_clock_gettime(RUMPUSER_CLOCK_ABSMONO, &sec_ini, &nsec_ini);

#ifdef __mac68k__
	sec = us / 1000;
	nsec = (us % 1000) * 1000000;
#else
	sec = us / 1000000;
	nsec = (us % 1000000) * 1000;
#endif

	target.tv_sec = sec_ini;
	tmp.tv_sec = sec;
	target.tv_nsec = nsec_ini;
	tmp.tv_nsec = nsec;
	timespecadd(&target, &tmp, &target);

	if (__predict_false(sec != 0))
		printf("WARNING: over 1s delay\n");

	for (loops = 0; loops < 1000*1000*100; loops++) {
		struct timespec cur;

		rumpuser_clock_gettime(RUMPUSER_CLOCK_ABSMONO,
		    &sec_now, &nsec_now);
		cur.tv_sec = sec_now;
		cur.tv_nsec = nsec_now;
		if (timespeccmp(&cur, &target, >=)) {
			return;
		}
	}
	printf("WARNING: DELAY ESCAPED\n");
}
void (*delay_func)(unsigned int) = rump_delay;
__strong_alias(delay,rump_delay);
__strong_alias(_delay,rump_delay);

/* Weak aliases for fstrans to be used unless librumpvfs is present. */

void rump_fstrans_start(struct mount *);
void
rump_fstrans_start(struct mount *mp)
{

}
__weak_alias(fstrans_start,rump_fstrans_start);

int rump_fstrans_start_nowait(struct mount *);
int
rump_fstrans_start_nowait(struct mount *mp)
{

	return 0;
}
__weak_alias(fstrans_start_nowait,rump_fstrans_start_nowait);

void rump_fstrans_done(struct mount *);
void
rump_fstrans_done(struct mount *mp)
{

}
__weak_alias(fstrans_done,rump_fstrans_done);

/*
 * Provide weak aliases for tty routines used by printf.
 * They will be used unless the rumpkern_tty component is present.
 */

int rump_ttycheckoutq(struct tty *, int);
int
rump_ttycheckoutq(struct tty *tp, int wait)
{

	return 1;
}
__weak_alias(ttycheckoutq,rump_ttycheckoutq);

int rump_tputchar(int, int, struct tty *);
int
rump_tputchar(int c, int flags, struct tty *tp)
{

	cnputc(c);
	return 0;
}
__weak_alias(tputchar,rump_tputchar);

void
cnputc(int c)
{

	rumpuser_putchar(c);
}

void
cnflush(void)
{

	/* done */
}

void
resettodr(void)
{

	/* setting clocks is not in the jurisdiction of rump kernels */
}

#ifdef __HAVE_SYSCALL_INTERN
void
syscall_intern(struct proc *p)
{

	p->p_emuldata = NULL;
}
#endif

#ifdef LOCKDEBUG
void
turnstile_print(volatile void *obj, void (*pr)(const char *, ...))
{

	/* nada */
}
#endif

void
cpu_reboot(int howto, char *bootstr)
{
	int ruhow = 0;
	void *finiarg;

	printf("rump kernel halting...\n");

	if (!RUMP_LOCALPROC_P(curproc))
		finiarg = RUMP_SPVM2CTL(curproc->p_vmspace);
	else
		finiarg = NULL;

	/* dump means we really take the dive here */
	if ((howto & RB_DUMP) || panicstr) {
		ruhow = RUMPUSER_PANIC;
		goto out;
	}

	/* try to sync */
	if (!((howto & RB_NOSYNC) || panicstr)) {
		rump_vfs_fini();
	}

	doshutdownhooks();

	/* your wish is my command */
	if (howto & RB_HALT) {
		printf("rump kernel halted (with RB_HALT, not exiting)\n");
		rump_sysproxy_fini(finiarg);
		for (;;) {
			rumpuser_clock_sleep(RUMPUSER_CLOCK_RELWALL, 10, 0);
		}
	}

	/* this function is __dead, we must exit */
 out:
	rump_sysproxy_fini(finiarg);
	rumpuser_exit(ruhow);
}

const char *
cpu_getmodel(void)
{

	return "rumpcore (virtual)";
}