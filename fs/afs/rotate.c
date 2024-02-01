// SPDX-License-Identifier: GPL-2.0-or-later
/* Handle fileserver selection and rotation.
 *
 * Copyright (C) 2017 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/sched/signal.h>
#include "internal.h"
#include "afs_fs.h"
#include "protocol_uae.h"

/*
 * Begin iteration through a server list, starting with the vnode's last used
 * server if possible, or the last recorded good server if not.
 */
static bool afs_start_fs_iteration(struct afs_operation *op,
				   struct afs_vnode *vnode)
{
	struct afs_server *server;
	void *cb_server;
	int i;

	read_lock(&op->volume->servers_lock);
	op->server_list = afs_get_serverlist(
		rcu_dereference_protected(op->volume->servers,
					  lockdep_is_held(&op->volume->servers_lock)));
	read_unlock(&op->volume->servers_lock);

	op->untried = (1UL << op->server_list->nr_servers) - 1;
	op->index = READ_ONCE(op->server_list->preferred);

	cb_server = vnode->cb_server;
	if (cb_server) {
		/* See if the vnode's preferred record is still available */
		for (i = 0; i < op->server_list->nr_servers; i++) {
			server = op->server_list->servers[i].server;
			if (server == cb_server) {
				op->index = i;
				goto found_interest;
			}
		}

		/* If we have a lock outstanding on a server that's no longer
		 * serving this vnode, then we can't switch to another server
		 * and have to return an error.
		 */
		if (op->flags & AFS_OPERATION_CUR_ONLY) {
			afs_op_set_error(op, -ESTALE);
			return false;
		}

		/* Note that the callback promise is effectively broken */
		write_seqlock(&vnode->cb_lock);
		ASSERTCMP(cb_server, ==, vnode->cb_server);
		vnode->cb_server = NULL;
		if (test_and_clear_bit(AFS_VNODE_CB_PROMISED, &vnode->flags))
			vnode->cb_break++;
		write_sequnlock(&vnode->cb_lock);
	}

found_interest:
	return true;
}

/*
 * Post volume busy note.
 */
static void afs_busy(struct afs_volume *volume, u32 abort_code)
{
	const char *m;

	switch (abort_code) {
	case VOFFLINE:		m = "offline";		break;
	case VRESTARTING:	m = "restarting";	break;
	case VSALVAGING:	m = "being salvaged";	break;
	default:		m = "busy";		break;
	}

	pr_notice("kAFS: Volume %llu '%s' is %s\n", volume->vid, volume->name, m);
}

/*
 * Sleep and retry the operation to the same fileserver.
 */
static bool afs_sleep_and_retry(struct afs_operation *op)
{
	if (!(op->flags & AFS_OPERATION_UNINTR)) {
		msleep_interruptible(1000);
		if (signal_pending(current)) {
			afs_op_set_error(op, -ERESTARTSYS);
			return false;
		}
	} else {
		msleep(1000);
	}

	return true;
}

/*
 * Select the fileserver to use.  May be called multiple times to rotate
 * through the fileservers.
 */
bool afs_select_fileserver(struct afs_operation *op)
{
	struct afs_addr_list *alist;
	struct afs_server *server;
	struct afs_vnode *vnode = op->file[0].vnode;
	unsigned int rtt;
	s32 abort_code = op->call_abort_code;
	int error = op->call_error, i;

	op->nr_iterations++;

	_enter("OP=%x+%x,%llx,%lx[%d],%lx[%d],%d,%d",
	       op->debug_id, op->nr_iterations, op->volume->vid,
	       op->untried, op->index,
	       op->ac.tried, op->ac.index,
	       error, abort_code);

	if (op->flags & AFS_OPERATION_STOP) {
		_leave(" = f [stopped]");
		return false;
	}

	if (op->nr_iterations == 0)
		goto start;

	/* Evaluate the result of the previous operation, if there was one. */
	switch (op->call_error) {
	case 0:
		op->cumul_error.responded = true;
		fallthrough;
	default:
		/* Success or local failure.  Stop. */
		afs_op_set_error(op, error);
		op->flags |= AFS_OPERATION_STOP;
		_leave(" = f [okay/local %d]", error);
		return false;

	case -ECONNABORTED:
		/* The far side rejected the operation on some grounds.  This
		 * might involve the server being busy or the volume having been moved.
		 *
		 * Note that various V* errors should not be sent to a cache manager
		 * by a fileserver as they should be translated to more modern UAE*
		 * errors instead.  IBM AFS and OpenAFS fileservers, however, do leak
		 * these abort codes.
		 */
		op->cumul_error.responded = true;
		switch (abort_code) {
		case VNOVOL:
			/* This fileserver doesn't know about the volume.
			 * - May indicate that the VL is wrong - retry once and compare
			 *   the results.
			 * - May indicate that the fileserver couldn't attach to the vol.
			 * - The volume might have been temporarily removed so that it can
			 *   be replaced by a volume restore.  "vos" might have ended one
			 *   transaction and has yet to create the next.
			 * - The volume might not be blessed or might not be in-service
			 *   (administrative action).
			 */
			if (op->flags & AFS_OPERATION_VNOVOL) {
				afs_op_accumulate_error(op, -EREMOTEIO, abort_code);
				goto next_server;
			}

			write_lock(&op->volume->servers_lock);
			op->server_list->vnovol_mask |= 1 << op->index;
			write_unlock(&op->volume->servers_lock);

			set_bit(AFS_VOLUME_NEEDS_UPDATE, &op->volume->flags);
			error = afs_check_volume_status(op->volume, op);
			if (error < 0) {
				afs_op_set_error(op, error);
				goto failed;
			}

			if (test_bit(AFS_VOLUME_DELETED, &op->volume->flags)) {
				afs_op_set_error(op, -ENOMEDIUM);
				goto failed;
			}

			/* If the server list didn't change, then assume that
			 * it's the fileserver having trouble.
			 */
			if (rcu_access_pointer(op->volume->servers) == op->server_list) {
				afs_op_accumulate_error(op, -EREMOTEIO, abort_code);
				goto next_server;
			}

			/* Try again */
			op->flags |= AFS_OPERATION_VNOVOL;
			_leave(" = t [vnovol]");
			return true;

		case VVOLEXISTS:
		case VONLINE:
			/* These should not be returned from the fileserver. */
			pr_warn("Fileserver returned unexpected abort %d\n",
				abort_code);
			afs_op_accumulate_error(op, -EREMOTEIO, abort_code);
			goto next_server;

		case VNOSERVICE:
			/* Prior to AFS 3.2 VNOSERVICE was returned from the fileserver
			 * if the volume was neither in-service nor administratively
			 * blessed.  All usage was replaced by VNOVOL because AFS 3.1 and
			 * earlier cache managers did not handle VNOSERVICE and assumed
			 * it was the client OSes errno 105.
			 *
			 * Starting with OpenAFS 1.4.8 VNOSERVICE was repurposed as the
			 * fileserver idle dead time error which was sent in place of
			 * RX_CALL_TIMEOUT (-3).  The error was intended to be sent if the
			 * fileserver took too long to send a reply to the client.
			 * RX_CALL_TIMEOUT would have caused the cache manager to mark the
			 * server down whereas VNOSERVICE since AFS 3.2 would cause cache
			 * manager to temporarily (up to 15 minutes) mark the volume
			 * instance as unusable.
			 *
			 * The idle dead logic resulted in cache inconsistency since a
			 * state changing call that the cache manager assumed was dead
			 * could still be processed to completion by the fileserver.  This
			 * logic was removed in OpenAFS 1.8.0 and VNOSERVICE is no longer
			 * returned.  However, many 1.4.8 through 1.6.24 fileservers are
			 * still in existence.
			 *
			 * AuriStorFS fileservers have never returned VNOSERVICE.
			 *
			 * VNOSERVICE should be treated as an alias for RX_CALL_TIMEOUT.
			 */
		case RX_CALL_TIMEOUT:
			afs_op_accumulate_error(op, -ETIMEDOUT, abort_code);
			goto next_server;

		case VSALVAGING: /* This error should not be leaked to cache managers
				  * but is from OpenAFS demand attach fileservers.
				  * It should be treated as an alias for VOFFLINE.
				  */
		case VSALVAGE: /* VSALVAGE should be treated as a synonym of VOFFLINE */
		case VOFFLINE:
			/* The volume is in use by the volserver or another volume utility
			 * for an operation that might alter the contents.  The volume is
			 * expected to come back but it might take a long time (could be
			 * days).
			 */
			if (!test_and_set_bit(AFS_VOLUME_OFFLINE, &op->volume->flags)) {
				afs_busy(op->volume, abort_code);
				clear_bit(AFS_VOLUME_BUSY, &op->volume->flags);
			}
			if (op->flags & AFS_OPERATION_NO_VSLEEP) {
				afs_op_set_error(op, -EADV);
				goto failed;
			}
			if (op->flags & AFS_OPERATION_CUR_ONLY) {
				afs_op_set_error(op, -ESTALE);
				goto failed;
			}
			goto busy;

		case VRESTARTING: /* The fileserver is either shutting down or starting up. */
		case VBUSY:
			/* The volume is in use by the volserver or another volume
			 * utility for an operation that is not expected to alter the
			 * contents of the volume.  VBUSY does not need to be returned
			 * for a ROVOL or BACKVOL bound to an ITBusy volserver
			 * transaction.  The fileserver is permitted to continue serving
			 * content from ROVOLs and BACKVOLs during an ITBusy transaction
			 * because the content will not change.  However, many fileserver
			 * releases do return VBUSY for ROVOL and BACKVOL instances under
			 * many circumstances.
			 *
			 * Retry after going round all the servers unless we have a file
			 * lock we need to maintain.
			 */
			if (op->flags & AFS_OPERATION_NO_VSLEEP) {
				afs_op_set_error(op, -EBUSY);
				goto failed;
			}
			if (!test_and_set_bit(AFS_VOLUME_BUSY, &op->volume->flags)) {
				afs_busy(op->volume, abort_code);
				clear_bit(AFS_VOLUME_OFFLINE, &op->volume->flags);
			}
		busy:
			if (op->flags & AFS_OPERATION_CUR_ONLY) {
				if (!afs_sleep_and_retry(op))
					goto failed;

				/* Retry with same server & address */
				_leave(" = t [vbusy]");
				return true;
			}

			op->flags |= AFS_OPERATION_VBUSY;
			goto next_server;

		case VMOVED:
			/* The volume migrated to another server.  We consider
			 * consider all locks and callbacks broken and request
			 * an update from the VLDB.
			 *
			 * We also limit the number of VMOVED hops we will
			 * honour, just in case someone sets up a loop.
			 */
			if (op->flags & AFS_OPERATION_VMOVED) {
				afs_op_set_error(op, -EREMOTEIO);
				goto failed;
			}
			op->flags |= AFS_OPERATION_VMOVED;

			set_bit(AFS_VOLUME_WAIT, &op->volume->flags);
			set_bit(AFS_VOLUME_NEEDS_UPDATE, &op->volume->flags);
			error = afs_check_volume_status(op->volume, op);
			if (error < 0) {
				afs_op_set_error(op, error);
				goto failed;
			}

			/* If the server list didn't change, then the VLDB is
			 * out of sync with the fileservers.  This is hopefully
			 * a temporary condition, however, so we don't want to
			 * permanently block access to the file.
			 *
			 * TODO: Try other fileservers if we can.
			 *
			 * TODO: Retry a few times with sleeps.
			 */
			if (rcu_access_pointer(op->volume->servers) == op->server_list) {
				afs_op_accumulate_error(op, -ENOMEDIUM, abort_code);
				goto failed;
			}

			goto restart_from_beginning;

		case UAEIO:
		case VIO:
			afs_op_accumulate_error(op, -EREMOTEIO, abort_code);
			if (op->volume->type != AFSVL_RWVOL)
				goto next_server;
			goto failed;

		case VDISKFULL:
		case UAENOSPC:
			/* The partition is full.  Only applies to RWVOLs.
			 * Translate locally and return ENOSPC.
			 * No replicas to failover to.
			 */
			afs_op_set_error(op, -ENOSPC);
			goto failed_but_online;

		case VOVERQUOTA:
		case UAEDQUOT:
			/* Volume is full.  Only applies to RWVOLs.
			 * Translate locally and return EDQUOT.
			 * No replicas to failover to.
			 */
			afs_op_set_error(op, -EDQUOT);
			goto failed_but_online;

		default:
			afs_op_accumulate_error(op, error, abort_code);
		failed_but_online:
			clear_bit(AFS_VOLUME_OFFLINE, &op->volume->flags);
			clear_bit(AFS_VOLUME_BUSY, &op->volume->flags);
			goto failed;
		}

	case -ETIMEDOUT:
	case -ETIME:
		if (afs_op_error(op) != -EDESTADDRREQ)
			goto iterate_address;
		fallthrough;
	case -ERFKILL:
	case -EADDRNOTAVAIL:
	case -ENETUNREACH:
	case -EHOSTUNREACH:
	case -EHOSTDOWN:
	case -ECONNREFUSED:
		_debug("no conn");
		afs_op_accumulate_error(op, error, 0);
		goto iterate_address;

	case -ENETRESET:
		pr_warn("kAFS: Peer reset %s (op=%x)\n",
			op->type ? op->type->name : "???", op->debug_id);
		fallthrough;
	case -ECONNRESET:
		_debug("call reset");
		afs_op_set_error(op, error);
		goto failed;
	}

restart_from_beginning:
	_debug("restart");
	afs_end_cursor(&op->ac);
	op->server = NULL;
	afs_put_serverlist(op->net, op->server_list);
	op->server_list = NULL;
start:
	_debug("start");
	/* See if we need to do an update of the volume record.  Note that the
	 * volume may have moved or even have been deleted.
	 */
	error = afs_check_volume_status(op->volume, op);
	if (error < 0) {
		afs_op_set_error(op, error);
		goto failed;
	}

	if (!afs_start_fs_iteration(op, vnode))
		goto failed;

	_debug("__ VOL %llx __", op->volume->vid);

pick_server:
	_debug("pick [%lx]", op->untried);

	error = afs_wait_for_fs_probes(op->server_list, op->untried);
	if (error < 0) {
		afs_op_set_error(op, error);
		goto failed;
	}

	/* Pick the untried server with the lowest RTT.  If we have outstanding
	 * callbacks, we stick with the server we're already using if we can.
	 */
	if (op->server) {
		_debug("server %u", op->index);
		if (test_bit(op->index, &op->untried))
			goto selected_server;
		op->server = NULL;
		_debug("no server");
	}

	op->index = -1;
	rtt = UINT_MAX;
	for (i = 0; i < op->server_list->nr_servers; i++) {
		struct afs_server *s = op->server_list->servers[i].server;

		if (!test_bit(i, &op->untried) ||
		    !test_bit(AFS_SERVER_FL_RESPONDING, &s->flags))
			continue;
		if (s->probe.rtt < rtt) {
			op->index = i;
			rtt = s->probe.rtt;
		}
	}

	if (op->index == -1)
		goto no_more_servers;

selected_server:
	_debug("use %d", op->index);
	__clear_bit(op->index, &op->untried);

	/* We're starting on a different fileserver from the list.  We need to
	 * check it, create a callback intercept, find its address list and
	 * probe its capabilities before we use it.
	 */
	ASSERTCMP(op->ac.alist, ==, NULL);
	server = op->server_list->servers[op->index].server;

	if (!afs_check_server_record(op, server))
		goto failed;

	_debug("USING SERVER: %pU", &server->uuid);

	op->flags |= AFS_OPERATION_RETRY_SERVER;
	op->server = server;
	if (vnode->cb_server != server) {
		vnode->cb_server = server;
		vnode->cb_s_break = server->cb_s_break;
		vnode->cb_fs_s_break = atomic_read(&server->cell->fs_s_break);
		vnode->cb_v_break = vnode->volume->cb_v_break;
		clear_bit(AFS_VNODE_CB_PROMISED, &vnode->flags);
	}

	read_lock(&server->fs_lock);
	alist = rcu_dereference_protected(server->addresses,
					  lockdep_is_held(&server->fs_lock));
	afs_get_addrlist(alist);
	read_unlock(&server->fs_lock);

retry_server:
	memset(&op->ac, 0, sizeof(op->ac));

	if (!op->ac.alist)
		op->ac.alist = alist;
	else
		afs_put_addrlist(alist);

	op->ac.index = -1;

iterate_address:
	ASSERT(op->ac.alist);
	/* Iterate over the current server's address list to try and find an
	 * address on which it will respond to us.
	 */
	if (!afs_iterate_addresses(&op->ac))
		goto out_of_addresses;

	_debug("address [%u] %u/%u %pISp",
	       op->index, op->ac.index, op->ac.alist->nr_addrs,
	       rxrpc_kernel_remote_addr(op->ac.alist->addrs[op->ac.index].peer));

	op->call_responded = false;
	_leave(" = t");
	return true;

out_of_addresses:
	/* We've now had a failure to respond on all of a server's addresses -
	 * immediately probe them again and consider retrying the server.
	 */
	afs_probe_fileserver(op->net, op->server);
	if (op->flags & AFS_OPERATION_RETRY_SERVER) {
		alist = op->ac.alist;
		error = afs_wait_for_one_fs_probe(
			op->server, !(op->flags & AFS_OPERATION_UNINTR));
		switch (error) {
		case 0:
			op->flags &= ~AFS_OPERATION_RETRY_SERVER;
			goto retry_server;
		case -ERESTARTSYS:
			afs_op_set_error(op, error);
			goto failed;
		case -ETIME:
		case -EDESTADDRREQ:
			goto next_server;
		}
	}

next_server:
	_debug("next");
	afs_end_cursor(&op->ac);
	goto pick_server;

no_more_servers:
	/* That's all the servers poked to no good effect.  Try again if some
	 * of them were busy.
	 */
	if (op->flags & AFS_OPERATION_VBUSY)
		goto restart_from_beginning;

	for (i = 0; i < op->server_list->nr_servers; i++) {
		struct afs_server *s = op->server_list->servers[i].server;

		error = READ_ONCE(s->probe.error);
		if (error < 0)
			afs_op_accumulate_error(op, error, s->probe.abort_code);
	}

failed:
	op->flags |= AFS_OPERATION_STOP;
	afs_end_cursor(&op->ac);
	_leave(" = f [failed %d]", afs_op_error(op));
	return false;
}

/*
 * Dump cursor state in the case of the error being EDESTADDRREQ.
 */
void afs_dump_edestaddrreq(const struct afs_operation *op)
{
	static int count;
	int i;

	if (!IS_ENABLED(CONFIG_AFS_DEBUG_CURSOR) || count > 3)
		return;
	count++;

	rcu_read_lock();

	pr_notice("EDESTADDR occurred\n");
	pr_notice("OP: cbb=%x cbb2=%x fl=%x err=%hd\n",
		  op->file[0].cb_break_before,
		  op->file[1].cb_break_before, op->flags, op->cumul_error.error);
	pr_notice("OP: ut=%lx ix=%d ni=%u\n",
		  op->untried, op->index, op->nr_iterations);
	pr_notice("OP: call  er=%d ac=%d r=%u\n",
		  op->call_error, op->call_abort_code, op->call_responded);

	if (op->server_list) {
		const struct afs_server_list *sl = op->server_list;
		pr_notice("FC: SL nr=%u pr=%u vnov=%hx\n",
			  sl->nr_servers, sl->preferred, sl->vnovol_mask);
		for (i = 0; i < sl->nr_servers; i++) {
			const struct afs_server *s = sl->servers[i].server;
			pr_notice("FC: server fl=%lx av=%u %pU\n",
				  s->flags, s->addr_version, &s->uuid);
			if (s->addresses) {
				const struct afs_addr_list *a =
					rcu_dereference(s->addresses);
				pr_notice("FC:  - av=%u nr=%u/%u/%u pr=%u\n",
					  a->version,
					  a->nr_ipv4, a->nr_addrs, a->max_addrs,
					  a->preferred);
				pr_notice("FC:  - R=%lx F=%lx\n",
					  a->responded, a->failed);
				if (a == op->ac.alist)
					pr_notice("FC:  - current\n");
			}
		}
	}

	pr_notice("AC: t=%lx ax=%u ni=%u\n",
		  op->ac.tried, op->ac.index, op->ac.nr_iterations);
	rcu_read_unlock();
}
