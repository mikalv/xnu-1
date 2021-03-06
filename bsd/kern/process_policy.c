/*
 * Copyright (c) 2005, 2010 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 * 
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */

/*
 * process policy syscall implementation
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/proc_internal.h>
#include <sys/kauth.h>
#include <sys/unistd.h>
#include <sys/buf.h>
#include <sys/ioctl.h>
#include <sys/vm.h>
#include <sys/user.h>

#include <security/audit/audit.h>

#include <mach/machine.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <kern/task.h>
#include <kern/lock.h>
#include <kern/kalloc.h>
#include <kern/assert.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
#include <mach/host_info.h>
#include <mach/task_info.h>
#include <mach/thread_info.h>
#include <mach/vm_region.h>

#include <sys/process_policy.h>
#include <sys/proc_info.h>
#include <sys/bsdtask_info.h>
#include <sys/kdebug.h>
#include <sys/sysproto.h>
#include <sys/msgbuf.h>

#include <machine/machine_routines.h>

#include <kern/ipc_misc.h>
#include <vm/vm_protos.h>
#if CONFIG_EMBEDDED
#include <security/mac.h>
#include <sys/kern_memorystatus.h>
#endif /* CONFIG_EMBEDDED */

static int handle_background(int scope, int action, int policy, int policy_subtype, user_addr_t attrp, proc_t proc, uint64_t target_threadid);
static int handle_hwaccess(int scope, int action, int policy, int policy_subtype, user_addr_t attrp, proc_t proc, uint64_t target_threadid);
static int handle_lowresrouce(int scope, int action, int policy, int policy_subtype, user_addr_t attrp, proc_t proc, uint64_t target_threadid);
static int handle_resourceuse(int scope, int action, int policy, int policy_subtype, user_addr_t attrp, proc_t proc, uint64_t target_threadid);
static int handle_apptype(int scope, int action, int policy, int policy_subtype, user_addr_t attrp, proc_t proc, uint64_t target_threadid);

extern kern_return_t task_suspend(task_t);
extern kern_return_t task_resume(task_t);

#if CONFIG_EMBEDDED
static int handle_applifecycle(int scope, int action, int policy, int policy_subtype, user_addr_t attrp, proc_t proc, uint64_t target_threadid);
#endif /* CONFIG_EMBEDDED */


/***************************** process_policy ********************/

/*
 *int process_policy(int scope, int action, int policy, int policy_subtype, 
 *                   proc_policy_attribute_t * attrp, pid_t target_pid, 
 *                   uint64_t target_threadid)
 *{ int process_policy(int scope, int action, int policy, int policy_subtype, 
 * user_addr_t attrp, pid_t target_pid, uint64_t target_threadid); }
 */

/* system call implementaion */
int
process_policy(__unused struct proc *p, struct process_policy_args * uap, __unused int32_t *retval)
{
	int error = 0;
	int scope = uap->scope;
	int policy = uap->policy;
	int action = uap->action;
	int policy_subtype = uap->policy_subtype;
	user_addr_t attrp = uap->attrp;
	pid_t target_pid = uap->target_pid;
	uint64_t target_threadid = uap->target_threadid;
	proc_t target_proc = PROC_NULL;
	proc_t curp = current_proc();
	kauth_cred_t my_cred;
#if CONFIG_EMBEDDED
	kauth_cred_t target_cred;
#endif

	if ((scope != PROC_POLICY_SCOPE_PROCESS) && (scope != PROC_POLICY_SCOPE_THREAD)) {
		return(EINVAL);
	}
	target_proc = proc_find(target_pid);
	if (target_proc == PROC_NULL)  {
		return(ESRCH);
	}

	my_cred = kauth_cred_get();

#if CONFIG_EMBEDDED
	target_cred = kauth_cred_proc_ref(target_proc);

	if (!kauth_cred_issuser(my_cred) && kauth_cred_getruid(my_cred) &&
	    kauth_cred_getuid(my_cred) != kauth_cred_getuid(target_cred) &&
	    kauth_cred_getruid(my_cred) != kauth_cred_getuid(target_cred))
#else
	/* 
	 * Resoure starvation control can be used by unpriv resource owner but priv at the time of ownership claim. This is
	 * checked in low resource handle routine. So bypass the checks here.
	 */
	if ((policy != PROC_POLICY_RESOURCE_STARVATION) && 
		(policy != PROC_POLICY_APPTYPE) && 
		(!kauth_cred_issuser(my_cred) && curp != p))
#endif
	{
		error = EPERM;
		goto out;
	}

#if CONFIG_MACF
#if CONFIG_EMBEDDED
	/* Lifecycle management will invoke approp macf checks */
	if (policy != PROC_POLICY_APP_LIFECYCLE) {
#endif /* CONFIG_EMBEDDED */
		error = mac_proc_check_sched(curp, target_proc);
		if (error) 
			goto out;
#if CONFIG_EMBEDDED
	}
#endif /* CONFIG_EMBEDDED */
#endif /* CONFIG_MACF */


	switch(policy) {
		case PROC_POLICY_BACKGROUND:
			error = handle_background(scope, action, policy, policy_subtype, attrp, target_proc, target_threadid);
			break;
		case PROC_POLICY_HARDWARE_ACCESS:
			error = handle_hwaccess(scope, action, policy, policy_subtype, attrp, target_proc, target_threadid);
			break;
		case PROC_POLICY_RESOURCE_STARVATION:
			error = handle_lowresrouce(scope, action, policy, policy_subtype, attrp, target_proc, target_threadid);
			break;
		case PROC_POLICY_RESOURCE_USAGE:
			error = handle_resourceuse(scope, action, policy, policy_subtype, attrp, target_proc, target_threadid);
			break;
#if CONFIG_EMBEDDED
		case PROC_POLICY_APP_LIFECYCLE:
			error = handle_applifecycle(scope, action, policy, policy_subtype, attrp, target_proc, target_threadid);
			break;
#endif /* CONFIG_EMBEDDED */
		case PROC_POLICY_APPTYPE:
			error = handle_apptype(scope, action, policy, policy_subtype, attrp, target_proc, target_threadid);
			break;
		default:
			error = EINVAL;
			break;
	}

out:
	proc_rele(target_proc);
#if CONFIG_EMBEDDED
        kauth_cred_unref(&target_cred);
#endif
	return(error);
}


/* darwin background handling code */
static int 
handle_background(int scope, int action, __unused int policy, __unused int policy_subtype, user_addr_t attrp, proc_t proc, uint64_t target_threadid)
{
	int intval, error = 0;


	switch (action) {
		case PROC_POLICY_ACTION_GET: 
			if (scope == PROC_POLICY_SCOPE_PROCESS) {
				intval = proc_get_task_bg_policy(proc->task);
			} else {
				/* thread scope */
				intval = proc_get_thread_bg_policy(proc->task, target_threadid);
			}
			error = copyout((int *)&intval, (user_addr_t)attrp, sizeof(int));
			break;

		case PROC_POLICY_ACTION_SET: 
			error = copyin((user_addr_t)attrp, (int *)&intval, sizeof(int));
			if (error != 0)
				goto out;
			if (intval > PROC_POLICY_BG_ALL) {
				error = EINVAL;
				goto out;	
			}
			if (scope == PROC_POLICY_SCOPE_PROCESS) {
				error = proc_set_bgtaskpolicy(proc->task, intval);
			} else {
				/* thread scope */
				error = proc_set_bgthreadpolicy(proc->task, target_threadid, intval);
			}
			break;

		case PROC_POLICY_ACTION_ADD: 
			error = copyin((user_addr_t)attrp, (int *)&intval, sizeof(int));
			if (error != 0)
				goto out;
			if (intval > PROC_POLICY_BG_ALL) {
				error = EINVAL;
				goto out;	
			}
			if (scope == PROC_POLICY_SCOPE_PROCESS) {
				error = proc_add_bgtaskpolicy(proc->task, intval);
			} else {
				/* thread scope */
				error = proc_add_bgthreadpolicy(proc->task, target_threadid, intval);
			}
			break;

		case PROC_POLICY_ACTION_REMOVE: 
			error = copyin((user_addr_t)attrp, (int *)&intval, sizeof(int));
			if (error != 0)
				goto out;
			if (intval > PROC_POLICY_BG_ALL) {
				error = EINVAL;
				goto out;	
			}
			if (scope == PROC_POLICY_SCOPE_PROCESS) {
				error = proc_remove_bgtaskpolicy(proc->task, intval);
			} else {
				/* thread scope */
				error = proc_remove_bgthreadpolicy(proc->task, target_threadid, intval);
			}
			break;
		
		case PROC_POLICY_ACTION_APPLY:
			if (scope == PROC_POLICY_SCOPE_PROCESS) {
				error = proc_apply_bgtaskpolicy(proc->task);
			} else {
				/* thread scope */
				error = proc_apply_bgthreadpolicy(proc->task, target_threadid);
			}	
			break;
		
		case PROC_POLICY_ACTION_RESTORE:
			if (scope == PROC_POLICY_SCOPE_PROCESS) {
				error = proc_restore_bgtaskpolicy(proc->task);
			} else {
				/* thread scope */
				error = proc_restore_bgthreadpolicy(proc->task, target_threadid);
			}
			break;
		
		case PROC_POLICY_ACTION_DENYINHERIT:
			error = proc_denyinherit_policy(proc->task);
			break;
		
		case PROC_POLICY_ACTION_DENYSELFSET:
			error = proc_denyselfset_policy(proc->task);
			break;
		
		default:
			return(EINVAL);
	}

out:
	return(error);
}

static int 
handle_hwaccess(__unused int scope, __unused int action, __unused int policy, int policy_subtype, __unused user_addr_t attrp, __unused proc_t proc, __unused uint64_t target_threadid)
{
	switch(policy_subtype) {
		case PROC_POLICY_HWACCESS_NONE:
		case PROC_POLICY_HWACCESS_DISK:
		case PROC_POLICY_HWACCESS_GPU:
		case PROC_POLICY_HWACCESS_NETWORK:
		case PROC_POLICY_HWACCESS_CPU:
			break;
		default:
			return(EINVAL);	
	}
	return(0);
}

static int 
handle_lowresrouce(__unused int scope, int action, __unused int policy, int policy_subtype, __unused user_addr_t attrp, proc_t proc, __unused uint64_t target_threadid)
{
	int error = 0;

	switch(policy_subtype) {
		case PROC_POLICY_RS_NONE:
		case PROC_POLICY_RS_VIRTUALMEM:
			break;
		default:
			return(EINVAL);	
	}
	
	if (action == PROC_POLICY_ACTION_RESTORE)
		error = proc_resetpcontrol(proc_pid(proc));
	else
		error = EINVAL;

	return(error);
}


static int 
handle_resourceuse(__unused int scope, __unused int action, __unused int policy, int policy_subtype, user_addr_t attrp, proc_t proc, __unused uint64_t target_threadid)
{
	proc_policy_cpuusage_attr_t cpuattr;
	int error = 0;

	switch(policy_subtype) {
		case PROC_POLICY_RUSAGE_NONE:
		case PROC_POLICY_RUSAGE_WIREDMEM:
		case PROC_POLICY_RUSAGE_VIRTMEM:
		case PROC_POLICY_RUSAGE_DISK:
		case PROC_POLICY_RUSAGE_NETWORK:
		case PROC_POLICY_RUSAGE_POWER:
			return(ENOTSUP);
			break;
		default:
			return(EINVAL);	
		case PROC_POLICY_RUSAGE_CPU:
			break;
	}

	switch (action) {
		case PROC_POLICY_ACTION_GET: 
			error = proc_get_task_ruse_cpu(proc->task, &cpuattr.ppattr_cpu_attr,
                                        &cpuattr.ppattr_cpu_percentage,
                                        &cpuattr.ppattr_cpu_attr_interval,
                                        &cpuattr.ppattr_cpu_attr_deadline);
			if (error == 0)
				error = copyout((proc_policy_cpuusage_attr_t *)&cpuattr, (user_addr_t)attrp, sizeof(proc_policy_cpuusage_attr_t));
			break;

		case PROC_POLICY_ACTION_APPLY: 
		case PROC_POLICY_ACTION_SET: 
			error = copyin((user_addr_t)attrp, (proc_policy_cpuusage_attr_t *)&cpuattr, sizeof(proc_policy_cpuusage_attr_t));

			if (error == 0) {
			error = proc_set_task_ruse_cpu(proc->task, cpuattr.ppattr_cpu_attr, 
					cpuattr.ppattr_cpu_percentage, 
					cpuattr.ppattr_cpu_attr_interval, 
					cpuattr.ppattr_cpu_attr_deadline); 
			}
			break;

		case PROC_POLICY_ACTION_RESTORE:
			error = proc_clear_task_ruse_cpu(proc->task);
			break;

		default:
			error = EINVAL;
			break;

	}
				
	return(error);
}

#if CONFIG_EMBEDDED
static int 
handle_applifecycle(__unused int scope, int action, __unused int policy, int policy_subtype, user_addr_t attrp, proc_t proc, uint64_t target_threadid)
{

	int error = 0;
	int state = 0, oldstate = 0; 
	int noteval = 0;

	

	switch(policy_subtype) {
		case PROC_POLICY_APPLIFE_NONE:
			error = 0;
			break;

		case PROC_POLICY_APPLIFE_STATE:
#if CONFIG_MACF
			error = mac_proc_check_sched(current_proc(), proc);
			if (error) 
				goto out;
#endif
			switch (action) {
				case PROC_POLICY_ACTION_GET :
					state = proc_lf_getappstate(proc->task);
					error = copyout((int *)&state, (user_addr_t)attrp, sizeof(int));
					break;
				case PROC_POLICY_ACTION_APPLY :
				case PROC_POLICY_ACTION_SET :
					error = copyin((user_addr_t)attrp, (int  *)&state, sizeof(int));
					if ((error == 0) && (state != TASK_APPSTATE_NONE)) {
						oldstate = proc_lf_getappstate(proc->task);
						error = proc_lf_setappstate(proc->task, state);
						if (error == 0) {
							switch (state) {
								case TASK_APPSTATE_ACTIVE:
									noteval = NOTE_APPACTIVE;
									break;
								case TASK_APPSTATE_BACKGROUND:
									noteval = NOTE_APPBACKGROUND;
									break;
								case TASK_APPSTATE_NONUI:
									noteval = NOTE_APPNONUI;
									break;
								case TASK_APPSTATE_INACTIVE:
									noteval = NOTE_APPINACTIVE;
									break;
							}
					
							proc_lock(proc);	
							proc_knote(proc, noteval);
							proc_unlock(proc);	
						}
					}
					break;

				default:
					error = EINVAL;
					break;
			}
			break;

		case PROC_POLICY_APPLIFE_DEVSTATUS:
#if CONFIG_MACF
			/* ToDo - this should be a generic check, since we could potentially hang other behaviours here. */
			error = mac_proc_check_suspend_resume(current_proc(), MAC_PROC_CHECK_HIBERNATE);
			if (error) {
				error = EPERM;
				goto out;
			}
#endif
			if (action == PROC_POLICY_ACTION_APPLY) {
				/* Used as a freeze hint */
				memorystatus_on_inactivity(-1);
				
				/* in future use devicestatus for pid_socketshutdown() */
				error = 0;
			 } else {
				error = EINVAL;
			}
			break;

		case PROC_POLICY_APPLIFE_PIDBIND:
#if CONFIG_MACF
			error = mac_proc_check_suspend_resume(current_proc(), MAC_PROC_CHECK_PIDBIND);
			if (error) {
				error = EPERM;
				goto out;
			}
#endif
			error = copyin((user_addr_t)attrp, (int  *)&state, sizeof(int));
			if (error != 0)
				goto out;
			if (action == PROC_POLICY_ACTION_APPLY) {
				/* bind the thread in target_thread in current process to target_proc */
				error = proc_lf_pidbind(current_task(), target_threadid, proc->task, state);
			 } else
				error = EINVAL;
			break;
		default:
			error = EINVAL;
			break;	
	}

out:
	return(error);
}
#endif /* CONFIG_EMBEDDED */


static int 
handle_apptype(__unused int scope, int action, __unused int policy, int policy_subtype, __unused user_addr_t attrp, proc_t target_proc, __unused uint64_t target_threadid)
{
	int error = 0;

	switch(policy_subtype) {
#if !CONFIG_EMBEDDED
		case PROC_POLICY_OSX_APPTYPE_TAL:
			/* need to be super user to do this */
			if (kauth_cred_issuser(kauth_cred_get()) == 0) {
				error = EPERM;
				goto out;
			}
			break;
		case PROC_POLICY_OSX_APPTYPE_DASHCLIENT:
			/* no special priv needed */
			break;
#endif /* !CONFIG_EMBEDDED */
		case PROC_POLICY_OSX_APPTYPE_NONE:
#if CONFIG_EMBEDDED
		case PROC_POLICY_IOS_RESV1_APPTYPE:
		case PROC_POLICY_IOS_APPLE_DAEMON:
		case PROC_POLICY_IOS_APPTYPE:
		case PROC_POLICY_IOS_NONUITYPE:
#endif /* CONFIG_EMBEDDED */
			return(ENOTSUP);
			break;
		default:
			return(EINVAL);	
	}

	switch (action) {
		case PROC_POLICY_ACTION_ENABLE:
			/* reapply the app foreground/background policy */
			error = proc_enable_task_apptype(target_proc->task, policy_subtype);
			break;
		case PROC_POLICY_ACTION_DISABLE: 
			/* remove the app foreground/background policy */
			error = proc_disable_task_apptype(target_proc->task, policy_subtype);
			break;
		default:
			error = EINVAL;
			break;
	}
				
#if !CONFIG_EMBEDDED
out:
#endif /* !CONFIG_EMBEDDED */
	return(error);
}


int
proc_apply_resource_actions(void * bsdinfo, int type, int action)
{
	proc_t p = (proc_t)bsdinfo;

	switch(action) {
		case PROC_POLICY_RSRCACT_THROTTLE:
			/* no need to do anything */
			break;

		case PROC_POLICY_RSRCACT_SUSPEND:
			task_suspend(p->task);
			break;

		case PROC_POLICY_RSRCACT_TERMINATE:
			psignal(p, SIGKILL);
			break;

		case PROC_POLICY_RSRCACT_NOTIFY_KQ:
			proc_lock(p);
			proc_knote(p, NOTE_RESOURCEEND | (type & 0xff));
			proc_unlock(p);
			break;
		
		case PROC_POLICY_RSRCACT_NOTIFY_EXC:
			panic("shouldn't be applying exception notification to process!");
			break;
	}

	return(0);
}


int
proc_restore_resource_actions(void * bsdinfo, __unused int type, int action)
{
	proc_t p = (proc_t)bsdinfo;

	switch(action) {
		case PROC_POLICY_RSRCACT_THROTTLE:
		case PROC_POLICY_RSRCACT_TERMINATE:
		case PROC_POLICY_RSRCACT_NOTIFY_KQ:
		case PROC_POLICY_RSRCACT_NOTIFY_EXC:
			/* no need to do anything */
			break;

		case PROC_POLICY_RSRCACT_SUSPEND:
			task_resume(p->task);
			break;

	}

	return(0);
}

