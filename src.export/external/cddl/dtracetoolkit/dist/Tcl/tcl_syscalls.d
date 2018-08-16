#!/usr/sbin/dtrace -Zs
/*
 * tcl_syscalls.d - count Tcl calls and syscalls using DTrace.
 *                  Written for the Tcl DTrace provider.
 *
 * $Id: tcl_syscalls.d,v 1.1.1.1 2015/09/30 22:01:09 christos Exp $
 *
 * This traces activity from all Tcl processes on the system with DTrace
 * provider support (tcl8.4.16).
 *
 * USAGE: tcl_syscalls.d { -p PID | -c cmd }	# hit Ctrl-C to end
 *
 * FIELDS:
 *		PID		Process ID
 *		TYPE		Type of call (method/syscall)
 *		NAME		Name of call
 *		COUNT		Number of calls during sample
 *
 * COPYRIGHT: Copyright (c) 2007 Brendan Gregg.
 *
 * CDDL HEADER START
 *
 *  The contents of this file are subject to the terms of the
 *  Common Development and Distribution License, Version 1.0 only
 *  (the "License").  You may not use this file except in compliance
 *  with the License.
 *
 *  You can obtain a copy of the license at Docs/cddl1.txt
 *  or http://www.opensolaris.org/os/licensing.
 *  See the License for the specific language governing permissions
 *  and limitations under the License.
 *
 * CDDL HEADER END
 *
 * 09-Sep-2007	Brendan Gregg	Created this.
 */

#pragma D option quiet

dtrace:::BEGIN
{
	printf("Tracing... Hit Ctrl-C to end.\n");
}

tcl$target:::proc-entry
{
	@calls[pid, "proc", copyinstr(arg0)] = count();
}

tcl$target:::cmd-entry
{
	@calls[pid, "cmd", copyinstr(arg0)] = count();
}

syscall:::entry
/pid == $target/
{
	@calls[pid, "syscall", probefunc] = count();
}


dtrace:::END
{
	printf(" %6s %-8s %-52s %8s\n", "PID", "TYPE", "NAME", "COUNT");
	printa(" %6d %-8s %-52s %@8d\n", @calls);
}