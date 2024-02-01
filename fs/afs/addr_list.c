// SPDX-License-Identifier: GPL-2.0-or-later
/* Server address list management
 *
 * Copyright (C) 2017 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 */

#include <linux/slab.h>
#include <linux/ctype.h>
#include <linux/dns_resolver.h>
#include <linux/inet.h>
#include <keys/rxrpc-type.h>
#include "internal.h"
#include "afs_fs.h"

static void afs_free_addrlist(struct rcu_head *rcu)
{
	struct afs_addr_list *alist = container_of(rcu, struct afs_addr_list, rcu);
	unsigned int i;

	for (i = 0; i < alist->nr_addrs; i++)
		rxrpc_kernel_put_peer(alist->addrs[i].peer);
}

/*
 * Release an address list.
 */
void afs_put_addrlist(struct afs_addr_list *alist)
{
	if (alist && refcount_dec_and_test(&alist->usage))
		call_rcu(&alist->rcu, afs_free_addrlist);
}

/*
 * Allocate an address list.
 */
struct afs_addr_list *afs_alloc_addrlist(unsigned int nr, u16 service_id)
{
	struct afs_addr_list *alist;
	unsigned int i;

	_enter("%u,%u", nr, service_id);

	if (nr > AFS_MAX_ADDRESSES)
		nr = AFS_MAX_ADDRESSES;

	alist = kzalloc(struct_size(alist, addrs, nr), GFP_KERNEL);
	if (!alist)
		return NULL;

	refcount_set(&alist->usage, 1);
	alist->max_addrs = nr;

	for (i = 0; i < nr; i++)
		alist->addrs[i].service_id = service_id;
	return alist;
}

/*
 * Parse a text string consisting of delimited addresses.
 */
struct afs_vlserver_list *afs_parse_text_addrs(struct afs_net *net,
					       const char *text, size_t len,
					       char delim,
					       unsigned short service,
					       unsigned short port)
{
	struct afs_vlserver_list *vllist;
	struct afs_addr_list *alist;
	const char *p, *end = text + len;
	const char *problem;
	unsigned int nr = 0;
	int ret = -ENOMEM;

	_enter("%*.*s,%c", (int)len, (int)len, text, delim);

	if (!len) {
		_leave(" = -EDESTADDRREQ [empty]");
		return ERR_PTR(-EDESTADDRREQ);
	}

	if (delim == ':' && (memchr(text, ',', len) || !memchr(text, '.', len)))
		delim = ',';

	/* Count the addresses */
	p = text;
	do {
		if (!*p) {
			problem = "nul";
			goto inval;
		}
		if (*p == delim)
			continue;
		nr++;
		if (*p == '[') {
			p++;
			if (p == end) {
				problem = "brace1";
				goto inval;
			}
			p = memchr(p, ']', end - p);
			if (!p) {
				problem = "brace2";
				goto inval;
			}
			p++;
			if (p >= end)
				break;
		}

		p = memchr(p, delim, end - p);
		if (!p)
			break;
		p++;
	} while (p < end);

	_debug("%u/%u addresses", nr, AFS_MAX_ADDRESSES);

	vllist = afs_alloc_vlserver_list(1);
	if (!vllist)
		return ERR_PTR(-ENOMEM);

	vllist->nr_servers = 1;
	vllist->servers[0].server = afs_alloc_vlserver("<dummy>", 7, AFS_VL_PORT);
	if (!vllist->servers[0].server)
		goto error_vl;

	alist = afs_alloc_addrlist(nr, service);
	if (!alist)
		goto error;

	/* Extract the addresses */
	p = text;
	do {
		const char *q, *stop;
		unsigned int xport = port;
		__be32 x[4];
		int family;

		if (*p == delim) {
			p++;
			continue;
		}

		if (*p == '[') {
			p++;
			q = memchr(p, ']', end - p);
		} else {
			for (q = p; q < end; q++)
				if (*q == '+' || *q == delim)
					break;
		}

		if (in4_pton(p, q - p, (u8 *)&x[0], -1, &stop)) {
			family = AF_INET;
		} else if (in6_pton(p, q - p, (u8 *)x, -1, &stop)) {
			family = AF_INET6;
		} else {
			problem = "family";
			goto bad_address;
		}

		p = q;
		if (stop != p) {
			problem = "nostop";
			goto bad_address;
		}

		if (q < end && *q == ']')
			p++;

		if (p < end) {
			if (*p == '+') {
				/* Port number specification "+1234" */
				xport = 0;
				p++;
				if (p >= end || !isdigit(*p)) {
					problem = "port";
					goto bad_address;
				}
				do {
					xport *= 10;
					xport += *p - '0';
					if (xport > 65535) {
						problem = "pval";
						goto bad_address;
					}
					p++;
				} while (p < end && isdigit(*p));
			} else if (*p == delim) {
				p++;
			} else {
				problem = "weird";
				goto bad_address;
			}
		}

		if (family == AF_INET)
			ret = afs_merge_fs_addr4(net, alist, x[0], xport);
		else
			ret = afs_merge_fs_addr6(net, alist, x, xport);
		if (ret < 0)
			goto error;

	} while (p < end);

	rcu_assign_pointer(vllist->servers[0].server->addresses, alist);
	_leave(" = [nr %u]", alist->nr_addrs);
	return vllist;

inval:
	_leave(" = -EINVAL [%s %zu %*.*s]",
	       problem, p - text, (int)len, (int)len, text);
	return ERR_PTR(-EINVAL);
bad_address:
	_leave(" = -EINVAL [%s %zu %*.*s]",
	       problem, p - text, (int)len, (int)len, text);
	ret = -EINVAL;
error:
	afs_put_addrlist(alist);
error_vl:
	afs_put_vlserverlist(net, vllist);
	return ERR_PTR(ret);
}

/*
 * Compare old and new address lists to see if there's been any change.
 * - How to do this in better than O(Nlog(N)) time?
 *   - We don't really want to sort the address list, but would rather take the
 *     list as we got it so as not to undo record rotation by the DNS server.
 */
#if 0
static int afs_cmp_addr_list(const struct afs_addr_list *a1,
			     const struct afs_addr_list *a2)
{
}
#endif

/*
 * Perform a DNS query for VL servers and build a up an address list.
 */
struct afs_vlserver_list *afs_dns_query(struct afs_cell *cell, time64_t *_expiry)
{
	struct afs_vlserver_list *vllist;
	char *result = NULL;
	int ret;

	_enter("%s", cell->name);

	ret = dns_query(cell->net->net, "afsdb", cell->name, cell->name_len,
			"srv=1", &result, _expiry, true);
	if (ret < 0) {
		_leave(" = %d [dns]", ret);
		return ERR_PTR(ret);
	}

	if (*_expiry == 0)
		*_expiry = ktime_get_real_seconds() + 60;

	if (ret > 1 && result[0] == 0)
		vllist = afs_extract_vlserver_list(cell, result, ret);
	else
		vllist = afs_parse_text_addrs(cell->net, result, ret, ',',
					      VL_SERVICE, AFS_VL_PORT);
	kfree(result);
	if (IS_ERR(vllist) && vllist != ERR_PTR(-ENOMEM))
		pr_err("Failed to parse DNS data %ld\n", PTR_ERR(vllist));

	return vllist;
}

/*
 * Merge an IPv4 entry into a fileserver address list.
 */
int afs_merge_fs_addr4(struct afs_net *net, struct afs_addr_list *alist,
		       __be32 xdr, u16 port)
{
	struct sockaddr_rxrpc srx;
	struct rxrpc_peer *peer;
	int i;

	if (alist->nr_addrs >= alist->max_addrs)
		return 0;

	srx.srx_family = AF_RXRPC;
	srx.transport_type = SOCK_DGRAM;
	srx.transport_len = sizeof(srx.transport.sin);
	srx.transport.sin.sin_family = AF_INET;
	srx.transport.sin.sin_port = htons(port);
	srx.transport.sin.sin_addr.s_addr = xdr;

	peer = rxrpc_kernel_lookup_peer(net->socket, &srx, GFP_KERNEL);
	if (!peer)
		return -ENOMEM;

	for (i = 0; i < alist->nr_ipv4; i++) {
		if (peer == alist->addrs[i].peer) {
			rxrpc_kernel_put_peer(peer);
			return 0;
		}
		if (peer <= alist->addrs[i].peer)
			break;
	}

	if (i < alist->nr_addrs)
		memmove(alist->addrs + i + 1,
			alist->addrs + i,
			sizeof(alist->addrs[0]) * (alist->nr_addrs - i));

	alist->addrs[i].peer = peer;
	alist->nr_ipv4++;
	alist->nr_addrs++;
	return 0;
}

/*
 * Merge an IPv6 entry into a fileserver address list.
 */
int afs_merge_fs_addr6(struct afs_net *net, struct afs_addr_list *alist,
		       __be32 *xdr, u16 port)
{
	struct sockaddr_rxrpc srx;
	struct rxrpc_peer *peer;
	int i;

	if (alist->nr_addrs >= alist->max_addrs)
		return 0;

	srx.srx_family = AF_RXRPC;
	srx.transport_type = SOCK_DGRAM;
	srx.transport_len = sizeof(srx.transport.sin6);
	srx.transport.sin6.sin6_family = AF_INET6;
	srx.transport.sin6.sin6_port = htons(port);
	memcpy(&srx.transport.sin6.sin6_addr, xdr, 16);

	peer = rxrpc_kernel_lookup_peer(net->socket, &srx, GFP_KERNEL);
	if (!peer)
		return -ENOMEM;

	for (i = alist->nr_ipv4; i < alist->nr_addrs; i++) {
		if (peer == alist->addrs[i].peer) {
			rxrpc_kernel_put_peer(peer);
			return 0;
		}
		if (peer <= alist->addrs[i].peer)
			break;
	}

	if (i < alist->nr_addrs)
		memmove(alist->addrs + i + 1,
			alist->addrs + i,
			sizeof(alist->addrs[0]) * (alist->nr_addrs - i));
	alist->addrs[i].peer = peer;
	alist->nr_addrs++;
	return 0;
}

/*
 * Get an address to try.
 */
bool afs_iterate_addresses(struct afs_addr_cursor *ac)
{
	unsigned long set, failed;
	int index;

	if (!ac->alist)
		return false;

	set = ac->alist->responded;
	failed = ac->alist->failed;
	_enter("%lx-%lx-%lx,%d", set, failed, ac->tried, ac->index);

	ac->nr_iterations++;

	set &= ~(failed | ac->tried);

	if (!set)
		return false;

	index = READ_ONCE(ac->alist->preferred);
	if (test_bit(index, &set))
		goto selected;

	index = __ffs(set);

selected:
	ac->index = index;
	set_bit(index, &ac->tried);
	ac->call_responded = false;
	return true;
}

/*
 * Release an address list cursor.
 */
void afs_end_cursor(struct afs_addr_cursor *ac)
{
	struct afs_addr_list *alist;

	alist = ac->alist;
	if (alist) {
		if (ac->call_responded &&
		    ac->index != alist->preferred &&
		    test_bit(ac->alist->preferred, &ac->tried))
			WRITE_ONCE(alist->preferred, ac->index);
		afs_put_addrlist(alist);
		ac->alist = NULL;
	}
}
