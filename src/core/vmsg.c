/*
 * $Id$
 *
 * Copyright (c) 2003, Raphael Manfredi
 *
 *----------------------------------------------------------------------
 * This file is part of gtk-gnutella.
 *
 *  gtk-gnutella is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  gtk-gnutella is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with gtk-gnutella; if not, write to the Free Software
 *  Foundation, Inc.:
 *      59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *----------------------------------------------------------------------
 */

/**
 * @file
 *
 * Vendor-specific messages.
 */

#include "common.h"

RCSID("$Id$");

#include "vmsg.h"
#include "nodes.h"
#include "search.h"
#include "gmsg.h"
#include "routing.h"		/* For message_set_muid() */
#include "gnet_stats.h"
#include "dq.h"
#include "udp.h"
#include "settings.h"		/* For listen_ip() */
#include "guid.h"			/* For blank_guid[] */
#include "inet.h"
#include "oob.h"
#include "mq.h"
#include "mq_udp.h"
#include "clock.h"
#include "tsync.h"
#include "hosts.h"
#include "pmsg.h"

#include "if/gnet_property_priv.h"

#include "lib/endian.h"
#include "lib/glib-missing.h"
#include "lib/tm.h"
#include "lib/vendors.h"
#include "lib/override.h"	/* Must be the last header included */

static gchar v_tmp[4128];	/* Large enough for a payload of 4K */

/*
 * Vendor message handler.
 */

struct vmsg;

typedef void (*vmsg_handler_t)(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size);

/*
 * Definition of vendor messages
 */
struct vmsg {
	guint32 vendor;
	guint16 id;
	guint16 version;
	vmsg_handler_t handler;
	gchar *name;
};

static void handle_messages_supported(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size);
static void handle_features_supported(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size);
static void handle_hops_flow(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size);
static void handle_tcp_connect_back(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size);
static void handle_udp_connect_back(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size);
static void handle_proxy_req(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size);
static void handle_proxy_ack(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size);
static void handle_qstat_req(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size);
static void handle_qstat_answer(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size);
static void handle_proxy_cancel(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size);
static void handle_oob_reply_ind(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size);
static void handle_oob_reply_ack(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size);
static void handle_time_sync_req(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size);
static void handle_time_sync_reply(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size);
static void handle_udp_crawler_ping(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size);

/*
 * Known vendor-specific messages.
 */
static struct vmsg vmsg_map[] = {
	/* This list MUST be sorted by vendor, id, version */

	{ T_0000, 0x0000, 0x0000, handle_messages_supported, "Messages Supported" },
	{ T_0000, 0x000a, 0x0000, handle_features_supported, "Features Supported" },
	{ T_BEAR, 0x0004, 0x0001, handle_hops_flow, "Hops Flow" },
	{ T_BEAR, 0x0007, 0x0001, handle_tcp_connect_back, "TCP Connect Back" },
	{ T_BEAR, 0x000b, 0x0001, handle_qstat_req, "Query Status Request" },
	{ T_BEAR, 0x000c, 0x0001, handle_qstat_answer, "Query Status Response" },
	{ T_GTKG, 0x0007, 0x0001, handle_udp_connect_back, "UDP Connect Back" },
	{ T_GTKG, 0x0007, 0x0002, handle_udp_connect_back, "UDP Connect Back" },
	{ T_GTKG, 0x0009, 0x0001, handle_time_sync_req, "Time Sync Request" },
	{ T_GTKG, 0x000a, 0x0001, handle_time_sync_reply, "Time Sync Reply" },
	{ T_GTKG, 0x0015, 0x0001, handle_proxy_cancel, "Push-Proxy Cancel" },
	{ T_LIME, 0x0005, 0x0001, handle_udp_crawler_ping, "UDP Crawler Ping" },
	{ T_LIME, 0x000b, 0x0002, handle_oob_reply_ack, "OOB Reply Ack" },
	{ T_LIME, 0x000c, 0x0001, handle_oob_reply_ind, "OOB Reply Indication" },
	{ T_LIME, 0x000c, 0x0002, handle_oob_reply_ind, "OOB Reply Indication" },
	{ T_LIME, 0x0015, 0x0001, handle_proxy_req, "Push-Proxy Request" },
	{ T_LIME, 0x0015, 0x0002, handle_proxy_req, "Push-Proxy Request" },
	{ T_LIME, 0x0016, 0x0001, handle_proxy_ack, "Push-Proxy Acknowledgment" },
	{ T_LIME, 0x0016, 0x0002, handle_proxy_ack, "Push-Proxy Acknowledgment" },

	/* Above line intentionally left blank (for "!}sort" in vi) */
};

/*
 * Items in the "Messages Supported" vector.
 */
struct vms_item {
	guint32 vendor;
	guint16 selector_id;
	guint16 version;
};

#define VMS_ITEM_SIZE		8		/* Each entry is 8 bytes (4+2+2) */

/*
 * Items in the "Features Supported" vector.
 */
struct vms_feature {
	guint32 vendor;
	guint16 version;
};

#define VMS_FEATURE_SIZE	6		/* Each entry is 6 bytes (4+2) */

#define PAIR_CMP(x, y, a0, a1, b0, b1) \
( \
  (x = CMP(a0, a1)) \
	? x \
	: (y = CMP(b0, b1)) \
			? y \
			: 0 \
)

/**
 * Find message, given vendor code, and id, version.
 *
 * Returns handler callback if found, NULL otherwise.
 */
static struct vmsg *
find_message(guint32 vendor, guint16 id, guint16 version)
{
  gint c_vendor, c_id, c_version;

#define GET_KEY(i) (&vmsg_map[(i)])
#define FOUND(i) do { return &vmsg_map[(i)]; } while (0)
#define COMPARE(item, key) \
	0 != (c_vendor = VENDOR_CODE_CMP((item)->key, key)) \
		? c_vendor \
		: PAIR_CMP(c_id, c_version, (item)->id, id, (item)->version, version)

	BINARY_SEARCH(struct vmsg *, vendor, G_N_ELEMENTS(vmsg_map), COMPARE,
		GET_KEY, FOUND);

#undef COMPARE	
#undef FOUND
#undef GET_KEY
	return NULL;		/* Not found */
}

/**
 * Decompiles vendor-message name given the data payload of the Gnutella
 * message and its size.  The leading bytes give us the identification
 * unless it's too short.
 *
 * @return vendor message name in the form "NAME/1v1 'Known name'" as
 * a static string.
 */
const gchar *
vmsg_infostr(gpointer data, gint size)
{
	static gchar msg[80];
	struct gnutella_vendor *v = (struct gnutella_vendor *) data;
	guint32 vendor;
	guint16 id;
	guint16 version;
	struct vmsg *vm;

	if ((size_t) size < sizeof(*v))
		return "????";

	READ_GUINT32_BE(v->vendor, vendor);
	READ_GUINT16_LE(v->selector_id, id);
	READ_GUINT16_LE(v->version, version);

	vm = find_message(vendor, id, version);

	if (vm == NULL)
		gm_snprintf(msg, sizeof(msg), "%s/%uv%u",
			vendor_code_str(vendor), id, version);
	else
		gm_snprintf(msg, sizeof(msg), "%s/%uv%u '%s'",
			vendor_code_str(vendor), id, version, vm->name);

	return msg;
}

/**
 * Main entry point to handle reception of vendor-specific message.
 */
void
vmsg_handle(struct gnutella_node *n)
{
	struct gnutella_vendor *v = (struct gnutella_vendor *) n->data;
	guint32 vendor;
	guint16 id;
	guint16 version;
	struct vmsg *vm;

	if (n->size < sizeof(*v)) {
		gnet_stats_count_dropped(n, MSG_DROP_TOO_SMALL);
		if (dbg || vmsg_debug)
			gmsg_log_bad(n, "message has only %d bytes, needs at least %d",
				n->size, (int) sizeof(*v));
		return;
	}

	READ_GUINT32_BE(v->vendor, vendor);
	READ_GUINT16_LE(v->selector_id, id);
	READ_GUINT16_LE(v->version, version);

	vm = find_message(vendor, id, version);

	if (vmsg_debug > 4)
		printf("VMSG %s \"%s\": %s/%uv%u\n",
			gmsg_infostr(&n->header), vm == NULL ? "UNKNOWN" : vm->name,
			vendor_code_str(vendor), id, version);

	/*
	 * If we can't handle the message, we count it as "unknown type", which
	 * is not completely exact because the type (vendor-specific) is known,
	 * it was only the subtype of that message which was unknown.  Still, I
	 * don't think it is ambiguous enough to warrant another drop type.
	 *		--RAM, 04/01/2003.
	 */

	if (vm == NULL) {
		gnet_stats_count_dropped(n, MSG_DROP_UNKNOWN_TYPE);
		if (dbg || vmsg_debug)
			gmsg_log_bad(n, "unknown vendor message");
		return;
	}

	(*vm->handler)(n, vm, n->data + sizeof(*v), n->size - sizeof(*v));
}

/**
 * Fill common message header part for all vendor-specific messages.
 * The GUID is blanked (all zero bytes), TTL is set to 1 and hops to 0.
 * Those common values can be superseded by the caller if needed.
 *
 * `size' is only the size of the payload we filled so far.
 * `maxsize' is the size of the already allocated vendor messsage.
 *
 * Returns the total size of the whole Gnutella message.
 */
static guint32
vmsg_fill_header(struct gnutella_header *header, guint32 size, guint32 maxsize)
{
	guint32 msize;

	memset(header->muid, 0, 16);				/* Default GUID: all blank */
	header->function = GTA_MSG_VENDOR;
	header->ttl = 1;
	header->hops = 0;

	msize = size + sizeof(struct gnutella_vendor);

	WRITE_GUINT32_LE(msize, header->size);

	msize += sizeof(struct gnutella_header);

	if (msize > maxsize)
		g_error("allocated vendor message is only %u bytes, would need %u",
			maxsize, msize);

	return msize;
}

/**
 * Fill leading part of the payload data, containing the common part for
 * all vendor-specific messages.
 *
 * Returns start of payload after that common part.
 */
static guchar *
vmsg_fill_type(
	struct gnutella_vendor *base, guint32 vendor, guint16 id, guint16 version)
{
	WRITE_GUINT32_BE(vendor, base->vendor);
	WRITE_GUINT16_LE(id, base->selector_id);
	WRITE_GUINT16_LE(version, base->version);

	return (guchar *) (base + 1);
}

/**
 * Report a vendor-message with bad payload to the stats.
 */
static void
vmsg_bad_payload(
	struct gnutella_node *n, struct vmsg *vmsg, gint size, gint expected)
{
	n->n_bad++;
	gnet_stats_count_dropped(n, MSG_DROP_BAD_SIZE);

	if (dbg || vmsg_debug)
		gmsg_log_bad(n, "Bad payload size %d for %s/%dv%d (%s), expected %d",
			size, vendor_code_str(vmsg->vendor), vmsg->id, vmsg->version,
			vmsg->name, expected);
}

/**
 * Handle the "Messages Supported" message.
 */
static void
handle_messages_supported(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size)
{
	guint16 count;
	gint i;
	gchar *description;
	gint expected;

	if (NODE_IS_UDP(n))			/* Don't waste time if we get this via UDP */
		return;

	READ_GUINT16_LE(payload, count);

	if (vmsg_debug)
		printf("VMSG node %s <%s> supports %u vendor message%s\n",
			node_ip(n), node_vendor(n), count,
			count == 1 ? "" : "s");

	expected = (gint) sizeof(count) + count * VMS_ITEM_SIZE;

	if (size != expected) {
		vmsg_bad_payload(n, vmsg, size, expected);
		return;
	}

	description = payload + 2;		/* Skip count */

	/*
	 * Analyze the supported messages.
	 */

	for (i = 0; i < count; i++) {
		guint32 vendor;
		gint id;
		gint version;
		struct vmsg *vm;

		READ_GUINT32_BE(description, vendor);
		description += 4;
		READ_GUINT16_LE(description, id);
		description += 2;
		READ_GUINT16_LE(description, version);
		description += 2;

		vm = find_message(vendor, id, version);

		if (vm == NULL) {
			if (vmsg_debug > 1)
				g_warning("VMSG node %s <%s> supports unknown %s/%dv%d",
					node_ip(n), node_vendor(n),
					vendor_code_str(vendor), id, version);
			continue;
		}

		if (vmsg_debug > 2)
			printf("VMSG ...%s/%dv%d\n", vendor_code_str(vendor), id, version);

		/*
		 * Look for leaf-guided dynamic query support.
		 *
		 * Remote can advertise only one of the two messages needed, we
		 * can infer support for the other!.
		 */

		if (
			vm->handler == handle_qstat_req ||
			vm->handler == handle_qstat_answer
		)
			n->attrs |= NODE_A_LEAF_GUIDE;

		/*
		 * Time synchronization support.
		 */

		if (
			vm->handler == handle_time_sync_req ||
			vm->handler == handle_time_sync_reply
		)
			node_can_tsync(n);

		/*
		 * UDP-crawling support.
		 */

		if (vm->handler == handle_udp_crawler_ping)
			n->attrs |= NODE_A_CRAWLABLE;
	}
}

/**
 * Send a "Messages Supported" message to specified node, telling it which
 * subset of the vendor messages we can understand.  We don't send information
 * about the "Messages Supported" message itself, since this one is guarateeed
 * to be always understood
 */
void
vmsg_send_messages_supported(struct gnutella_node *n)
{
	struct gnutella_msg_vendor *m = (struct gnutella_msg_vendor *) v_tmp;
	guint16 count = G_N_ELEMENTS(vmsg_map) - 1;
	guint32 paysize = sizeof(count) + count * VMS_ITEM_SIZE;
	guint32 msgsize;
	guchar *payload;
	guint i;

	msgsize = vmsg_fill_header(&m->header, paysize, sizeof(v_tmp));
	payload = vmsg_fill_type(&m->data, T_0000, 0, 0);

	/*
	 * First 2 bytes is the number of entries in the vector.
	 */

	WRITE_GUINT16_LE(count, payload);
	payload += 2;

	/*
	 * Fill one entry per message type supported, excepted ourselves.
	 */

	for (i = 0; i < G_N_ELEMENTS(vmsg_map); i++) {
		struct vmsg *msg = &vmsg_map[i];

		if (msg->vendor == T_0000)		/* Don't send info about ourselves */
			continue;

		WRITE_GUINT32_BE(msg->vendor, payload);
		payload += 4;
		WRITE_GUINT16_LE(msg->id, payload);
		payload += 2;
		WRITE_GUINT16_LE(msg->version, payload);
		payload += 2;
	}

	gmsg_sendto_one(n, (gchar *) m, msgsize);
}

/**
 * Handle the "Features Supported" message.
 */
static void
handle_features_supported(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size)
{
	guint16 count;
	gint i;
	gchar *description;
	gint expected;

	READ_GUINT16_LE(payload, count);

	if (vmsg_debug)
		printf("VMSG node %s <%s> supports %u extra feature%s\n",
			node_ip(n), node_vendor(n), count,
			count == 1 ? "" : "s");

	expected = (gint) sizeof(count) + count * VMS_FEATURE_SIZE;

	if (size != expected) {
		vmsg_bad_payload(n, vmsg, size, expected);
		return;
	}

	description = payload + 2;		/* Skip count */

	/*
	 * Analyze the supported features.
	 */

	for (i = 0; i < count; i++) {
		guint32 vendor;
		gint version;

		READ_GUINT32_BE(description, vendor);
		description += 4;
		READ_GUINT16_LE(description, version);
		description += 2;

		if (vmsg_debug > 1)
			printf("VMSG node %s <%s> supports feature %s/%u\n",
				node_ip(n), node_vendor(n),
				vendor_code_str(vendor), version);

		/* XXX -- look for specific features not present in handshake */
	}
}

/**
 * Handle the "Hops Flow" message.
 */
static void
handle_hops_flow(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size)
{
	guint8 hops;

	g_assert(vmsg->version <= 1);

	if (size != 1) {
		vmsg_bad_payload(n, vmsg, size, 1);
		return;
	}

	hops = *payload;
	node_set_hops_flow(n, hops);
}

/**
 * Send an "Hops Flow" message to specified node.
 */
void
vmsg_send_hops_flow(struct gnutella_node *n, guint8 hops)
{
	struct gnutella_msg_vendor *m = (struct gnutella_msg_vendor *) v_tmp;
	guint32 paysize = sizeof(hops);
	guint32 msgsize;
	guchar *payload;

	msgsize = vmsg_fill_header(&m->header, paysize, sizeof(v_tmp));
	payload = vmsg_fill_type(&m->data, T_BEAR, 4, 1);

	*payload = hops;

	/*
	 * Send the message as a control message, so that it gets sent ASAP.
	 */

	gmsg_ctrl_sendto_one(n, (gchar *) m, msgsize);
}

/**
 * Handle the "TCP Connect Back" message.
 */
static void
handle_tcp_connect_back(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size)
{
	guint16 port;

	g_assert(vmsg->version <= 1);

	if (size != 2) {
		vmsg_bad_payload(n, vmsg, size, 2);
		return;
	}

	READ_GUINT16_LE(payload, port);

	if (port == 0) {
		g_warning("got improper port #%d in %s from %s <%s>",
			port, vmsg->name, node_ip(n), node_vendor(n));
		return;
	}

	/* XXX forward to neighbours supporting the remote connect back message? */

	node_connect_back(n, port);
}

/**
 * Send a "TCP Connect Back" message to specified node, telling it to connect
 * back to us on the specified port.
 */
void
vmsg_send_tcp_connect_back(struct gnutella_node *n, guint16 port)
{
	struct gnutella_msg_vendor *m = (struct gnutella_msg_vendor *) v_tmp;
	guint32 paysize = sizeof(port);
	guint32 msgsize;
	guchar *payload;

	msgsize = vmsg_fill_header(&m->header, paysize, sizeof(v_tmp));
	payload = vmsg_fill_type(&m->data, T_BEAR, 7, 1);

	WRITE_GUINT16_LE(port, payload);

	gmsg_sendto_one(n, (gchar *) m, msgsize);
}

/**
 * Handle the "UDP Connect Back" message.
 */
static void
handle_udp_connect_back(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size)
{
	guint16 port;
	gchar guid_buf[16];

	g_assert(vmsg->version <= 2);

	/*
	 * Version 1 included the GUID at the end of the payload.
	 * Version 2 uses the message's GUID itself to store the GUID
	 * of the PING to send back.
	 */

	switch (vmsg->version) {
	case 1:
		if (size != 18) {
			vmsg_bad_payload(n, vmsg, size, 18);
			return;
		}
		memcpy(guid_buf, payload + 2, 16);		/* Get GUID from payload */
		break;
	case 2:
		if (size != 2) {
			vmsg_bad_payload(n, vmsg, size, 2);
			return;
		}
		memcpy(guid_buf, n->header.muid, 16);	/* Get GUID from MUID */
		break;
	default:
		g_assert_not_reached();
	}

	READ_GUINT16_LE(payload, port);

	if (port == 0) {
		g_warning("got improper port #%d in %s from %s <%s>",
			port, vmsg->name, node_ip(n), node_vendor(n));
		return;
	}

	udp_connect_back(n->ip, port, guid_buf);
}

/**
 * Send a "UDP Connect Back" message to specified node, telling it to ping
 * us back via UDP on the specified port.
 *
 * XXX for now, we only send GTKG/7v1, although GTKG/7v2 is more compact.
 */
void
vmsg_send_udp_connect_back(struct gnutella_node *n, guint16 port)
{
	struct gnutella_msg_vendor *m = (struct gnutella_msg_vendor *) v_tmp;
	guint32 paysize = sizeof(port) + 16;
	guint32 msgsize;
	guchar *payload;

	msgsize = vmsg_fill_header(&m->header, paysize, sizeof(v_tmp));
	payload = vmsg_fill_type(&m->data, T_GTKG, 7, 1);

	WRITE_GUINT16_LE(port, payload);
	memcpy(payload + 2, guid, 16);

	gmsg_sendto_one(n, (gchar *) m, msgsize);
}

/**
 * Handle reception of the "Push Proxy Request" message.
 */
static void
handle_proxy_req(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *unused_payload, gint size)
{
	(void) unused_payload;

	if (size != 0) {
		vmsg_bad_payload(n, vmsg, size, 0);
		return;
	}

	/*
	 * Normally, a firewalled host should be a leaf node, not an UP.
	 * Warn if node is not a leaf, but accept to be the push proxy
	 * nonetheless.
	 */

	if (!NODE_IS_LEAF(n))
		g_warning("got %s from non-leaf node %s <%s>",
			vmsg->name, node_ip(n), node_vendor(n));

	/*
	 * Add proxying info for this node.  On successful completion,
	 * we'll send an acknowledgement.
	 *
	 * We'll reply with a message at the same version as the one we got.
	 */

	if (node_proxying_add(n, n->header.muid))	/* MUID is the node's GUID */
		vmsg_send_proxy_ack(n, n->header.muid, vmsg->version);
}

/**
 * Send a "Push Proxy Request" message to specified node, using supplied
 * `muid' as the message ID (which is our GUID).
 */
void
vmsg_send_proxy_req(struct gnutella_node *n, const gchar *muid)
{
	struct gnutella_msg_vendor *m = (struct gnutella_msg_vendor *) v_tmp;
	guint32 msgsize;

	g_assert(!NODE_IS_LEAF(n));

	msgsize = vmsg_fill_header(&m->header, 0, sizeof(v_tmp));
	memcpy(m->header.muid, muid, 16);
	(void) vmsg_fill_type(&m->data, T_LIME, 21, 2);

	gmsg_sendto_one(n, (gchar *) m, msgsize);

	if (vmsg_debug > 2)
		g_warning("sent proxy REQ to %s <%s>", node_ip(n), node_vendor(n));
}

/**
 * Handle reception of the "Push Proxy Acknowledgment" message.
 *
 * Version 1 only bears the port.  The IP address must be gathered from n->ip.
 * Version 2 holds both the IP and port of our push-proxy.
 */
static void
handle_proxy_ack(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size)
{
	guint32 ip;
	guint16 port;
	gint expected_size;

	expected_size = (vmsg->version < 2) ? 2 : 6;

	if (size != expected_size) {
		vmsg_bad_payload(n, vmsg, size, expected_size);
		return;
	}

	if (vmsg->version >= 2) {
		READ_GUINT32_BE(payload, ip);
		payload += 4;
	} else
		ip = n->ip;

	READ_GUINT16_LE(payload, port);

	if (vmsg_debug > 2)
		g_message("got proxy ACK from %s <%s>: proxy at %s",
			node_ip(n), node_vendor(n), ip_port_to_gchar(ip, port));


	if (!host_is_valid(ip, port)) {
		g_warning("got improper address %s in %s from %s <%s>",
			ip_port_to_gchar(ip, port), vmsg->name,
			node_ip(n), node_vendor(n));
		return;
	}

	node_proxy_add(n, ip, port);
}

/**
 * Send a "Push Proxy Acknowledgment" message to specified node, using
 * supplied `muid' as the message ID (which is the target node's GUID).
 *
 * The version 1 of this message did not have the listening IP, only the
 * port: the recipient was supposed to gather the IP address from the
 * connected socket.
 *
 * The version 2 includes both our IP and port.
 */
void
vmsg_send_proxy_ack(struct gnutella_node *n, gchar *muid, gint version)
{
	struct gnutella_msg_vendor *m = (struct gnutella_msg_vendor *) v_tmp;
	guint32 paysize = sizeof(guint32) + sizeof(guint16);
	guint32 msgsize;
	guchar *payload;

	if (version == 1)
		paysize -= sizeof(guint32);		/* No IP address for v1 */

	msgsize = vmsg_fill_header(&m->header, paysize, sizeof(v_tmp));
	memcpy(m->header.muid, muid, 16);
	payload = vmsg_fill_type(&m->data, T_LIME, 22, version);

	if (version >= 2) {
		WRITE_GUINT32_BE(listen_ip(), payload);
		payload += 4;
	}

	WRITE_GUINT16_LE(listen_port, payload);

	/*
	 * Reply with a control message, so that the issuer knows that we can
	 * proxyfy pushes to it ASAP.
	 */

	gmsg_ctrl_sendto_one(n, (gchar *) m, msgsize);
}

/**
 * Handle reception of "Query Status Request", where the UP requests how
 * many results the search filters of the leave (ourselves) let pass through.
 */
static void
handle_qstat_req(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *unused_payload, gint size)
{
	guint32 kept;

	(void) unused_payload;

	if (size != 0) {
		vmsg_bad_payload(n, vmsg, size, 0);
		return;
	}

	if (!search_get_kept_results(n->header.muid, &kept)) {
		/*
		 * We did not find any search for this MUID.  Either the remote
		 * side goofed, or they closed the search.
		 */

		kept = 0xffff;		/* Magic value telling them to stop the search */
	}

	vmsg_send_qstat_answer(n, n->header.muid, (guint16) MIN(kept, 0xfffe));
}

/**
 * Send a "Query Status Request" message to specified node, using supplied
 * `muid' as the message ID (which is the query ID).
 */
void
vmsg_send_qstat_req(struct gnutella_node *n, gchar *muid)
{
	struct gnutella_msg_vendor *m = (struct gnutella_msg_vendor *) v_tmp;
	guint32 msgsize;

	msgsize = vmsg_fill_header(&m->header, 0, sizeof(v_tmp));
	memcpy(m->header.muid, muid, 16);
	(void) vmsg_fill_type(&m->data, T_BEAR, 11, 1);

	gmsg_ctrl_sendto_one(n, (gchar *) m, msgsize);	/* Send ASAP */
}

/**
 * Handle "Query Status Response" where the leave notifies us about the
 * amount of results its search filters let pass through for the specified
 * query.
 */
static void
handle_qstat_answer(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size)
{
	guint16 kept;

	if (size != 2) {
		vmsg_bad_payload(n, vmsg, size, 2);
		return;
	}

	/*
	 * Let the dynamic querying side about the reply.
	 */

	READ_GUINT16_LE(payload, kept);

	if (kept)
		dq_got_query_status(n->header.muid, NODE_ID(n), kept);
}

/**
 * Send a "Query Status Response" message to specified node.
 *
 * @param n the Gnutella node to sent the message to
 * @param muid is the query ID
 * @param hits is the number of hits our filters did not drop.
 */
void
vmsg_send_qstat_answer(struct gnutella_node *n, gchar *muid, guint16 hits)
{
	struct gnutella_msg_vendor *m = (struct gnutella_msg_vendor *) v_tmp;
	guint32 msgsize;
	guint32 paysize = sizeof(guint16);
	gchar *payload;

	msgsize = vmsg_fill_header(&m->header, paysize, sizeof(v_tmp));
	memcpy(m->header.muid, muid, 16);
	payload = vmsg_fill_type(&m->data, T_BEAR, 12, 1);

	WRITE_GUINT16_LE(hits, payload);

	if (vmsg_debug > 1)
		printf("VMSG sending %s with hits=%d to %s <%s>\n",
			gmsg_infostr_full(m), hits, node_ip(n), node_vendor(n));

	gmsg_ctrl_sendto_one(n, (gchar *) m, msgsize);	/* Send it ASAP */
}

/**
 * Handle reception of "Push Proxy Cancel" request, when remote node no longer
 * wishes to have us as a push-proxy.  This is an indication that the host
 * determined it was not TCP-firewalled.
 */
static void
handle_proxy_cancel(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *unused_payload, gint size)
{
	(void) unused_payload;

	if (size != 0) {
		vmsg_bad_payload(n, vmsg, size, 0);
		return;
	}

	/*
	 * We keep the GUID route for that node, to honour further push-proxy
	 * requests coming from past hits sent away by the proxied node.
	 * However, we clear the flag marking the node as proxied, and we know
	 * it is no longer TCP-firewalled.
	 */

	node_proxying_remove(n, FALSE);
}

/**
 * Send a "Push Proxy Cancel" message to specified node.
 */
void
vmsg_send_proxy_cancel(struct gnutella_node *n)
{
	struct gnutella_msg_vendor *m = (struct gnutella_msg_vendor *) v_tmp;
	guint32 msgsize;

	msgsize = vmsg_fill_header(&m->header, 0, sizeof(v_tmp));
	memcpy(m->header.muid, blank_guid, 16);
	(void) vmsg_fill_type(&m->data, T_GTKG, 21, 1);

	gmsg_sendto_one(n, (gchar *) m, msgsize);

	if (vmsg_debug > 2)
		g_message("sent proxy CANCEL to %s <%s>", node_ip(n), node_vendor(n));
}

/**
 * Handle reception of an "OOB Reply Indication" message, whereby the remote
 * host informs us about the amount of query hits it has for us for a
 * given query.  The message bears the MUID of the query we sent out.
 */
static void handle_oob_reply_ind(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size)
{
	gint hits;
	gboolean can_recv_unsolicited = FALSE;

	if (!NODE_IS_UDP(n)) {
		/*
		 * Uh-oh, someone forwarded us a LIME/12 message.  Ignore it!
		 */

		g_warning("got %s/%uv%u from TCP via %s, ignoring",
			vendor_code_str(vmsg->vendor), vmsg->id, vmsg->version, node_ip(n));
		return;
	}

	switch (vmsg->version) {
	case 1:
		if (size != 1) {
			vmsg_bad_payload(n, vmsg, size, 1);
			return;
		}
		hits = *(guchar *) payload;
		break;
	case 2:
		if (size != 2) {
			vmsg_bad_payload(n, vmsg, size, 2);
			return;
		}
		hits = *(guchar *) payload;
		can_recv_unsolicited = (*(guchar *) &payload[1]) & 0x1;
		break;
	default:
		goto not_handling;
	}

	if (hits == 0) {
		g_warning("no results advertised in %s/%uv%u from %s",
			vendor_code_str(vmsg->vendor), vmsg->id, vmsg->version, node_ip(n));
		return;
	}

	search_oob_pending_results(n, n->header.muid, hits, can_recv_unsolicited);
	return;

not_handling:
	g_warning("not handling %s/%uv%u from %s",
		vendor_code_str(vmsg->vendor), vmsg->id, vmsg->version, node_ip(n));
}

/**
 * Build an "OOB Reply Indication" message.
 *
 * @param muid is the query ID
 * @param hits is the number of hits we have to deliver for that query
 */
pmsg_t *
vmsg_build_oob_reply_ind(gchar *muid, guint8 hits)
{
	struct gnutella_msg_vendor *m = (struct gnutella_msg_vendor *) v_tmp;
	guint32 msgsize;
	guint32 paysize = sizeof(guint8) + sizeof(guint8);
	gchar *payload;

	msgsize = vmsg_fill_header(&m->header, paysize, sizeof(v_tmp));
	memcpy(m->header.muid, muid, 16);
	payload = vmsg_fill_type(&m->data, T_LIME, 12, 2);

	*(guchar *) &payload[0] = hits;
	*(guchar *) &payload[1] = is_udp_firewalled ? 0x0 : 0x1;

	return gmsg_to_pmsg(m, msgsize);
}

/**
 * Handle reception of an "OOB Reply Ack" message, whereby the remote
 * host informs us about the amount of query hits it wants delivered
 * for the query identified by the MUID of the message.
 */
static void handle_oob_reply_ack(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size)
{
	gint wanted;

	if (size != 1) {
		vmsg_bad_payload(n, vmsg, size, 1);
		return;
	}

	/*
	 * We expect those ACKs to come back via UDP.
	 */

	if (!NODE_IS_UDP(n)) {
		g_warning("got %s/%uv%u from TCP via %s, ignoring",
			vendor_code_str(vmsg->vendor), vmsg->id, vmsg->version, node_ip(n));
		return;
	}

	wanted = *(guchar *) payload;
	oob_deliver_hits(n, n->header.muid, wanted);
}

/**
 * Send an "OOB Reply Ack" message to specified node, informing it that
 * we want the specified amount of hits delivered for the query identified
 * by the MUID of the message we got (the "OOB Reply Indication").
 */
void
vmsg_send_oob_reply_ack(struct gnutella_node *n, gchar *muid, guint8 want)
{
	struct gnutella_msg_vendor *m = (struct gnutella_msg_vendor *) v_tmp;
	guint32 msgsize;
	guint32 paysize = sizeof(guint8);
	gchar *payload;

	g_assert(NODE_IS_UDP(n));

	msgsize = vmsg_fill_header(&m->header, paysize, sizeof(v_tmp));
	memcpy(m->header.muid, muid, 16);
	payload = vmsg_fill_type(&m->data, T_LIME, 11, 2);

	*payload = want;

	udp_send_msg(n, m, msgsize);

	if (vmsg_debug > 2)
		printf("sent OOB reply ACK %s to %s for %u hit%s\n",
			guid_hex_str(muid), node_ip(n), want, want == 1 ? "" : "s");
}

/**
 * Handle reception of a "Time Sync Request" message, indicating a request
 * from another host about time synchronization.
 */
static void handle_time_sync_req(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *unused_payload, gint size)
{
	tm_t got;

	(void) unused_payload;

	/*
	 * We have received the message well before, but this is the first
	 * time we can timestamp it really...  We're not NTP, so the precision
	 * is not really necessary as long as we stay beneath a second, which
	 * we should.
	 */

	tm_now(&got);			/* Mark when we got the message */
	got.tv_sec = clock_loc2gmt(got.tv_sec);

	if (size != 1) {
		vmsg_bad_payload(n, vmsg, size, 1);
		return;
	}

	tsync_got_request(n, &got);
}

/**
 * Handle reception of a "Time Sync Reply" message, holding the reply from
 * a previous time synchronization request.
 */
static void handle_time_sync_reply(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size)
{
	gboolean ntp;
	tm_t got;
	tm_t sent;
	tm_t replied;
	tm_t received;
	gchar *muid;
	gchar *data;

	tm_now(&got);			/* Mark when we got (to see) the message */
	got.tv_sec = clock_loc2gmt(got.tv_sec);

	if (size != 9) {
		vmsg_bad_payload(n, vmsg, size, 9);
		return;
	}

	ntp = *payload & 0x1;

	/*
	 * Decompile send time.
	 */

	STATIC_ASSERT(sizeof(sent) >= 2 * sizeof(guint32));

	muid = n->header.muid;
	READ_GUINT32_BE(muid, sent.tv_sec);
	muid += 4;
	READ_GUINT32_BE(muid, sent.tv_usec);
	muid += 4;

	/*
	 * Decompile replied time.
	 */

	READ_GUINT32_BE(muid, replied.tv_sec);
	muid += 4;
	READ_GUINT32_BE(muid, replied.tv_usec);

	/*
	 * Decompile the time at which they got the message.
	 */

	data = payload + 1;
	READ_GUINT32_BE(data, received.tv_sec);
	data += 4;
	READ_GUINT32_BE(data, received.tv_usec);

	tsync_got_reply(n, &sent, &received, &replied, &got, ntp);
}

/**
 * Callback invoked when "Time Sync Request" is about to be sent.
 * Writes current time in the first half of the MUID.
 */
static gboolean
vmsg_time_sync_req_stamp(pmsg_t *mb, struct mqueue *unused_q)
{
	tm_t old;
	tm_t now;
	gchar *muid = pmsg_start(mb);

	(void) unused_q;
	g_assert(pmsg_is_writable(mb));
	STATIC_ASSERT(sizeof(now) >= 2 * sizeof(guint32));

	/*
	 * Read the old timestamp.
	 */

	READ_GUINT32_BE(muid, old.tv_sec);
	muid += 4;
	READ_GUINT32_BE(muid, old.tv_usec);

	tm_now(&now);
	now.tv_sec = clock_loc2gmt(now.tv_sec);

	muid -= 4;								/* Rewind */
	WRITE_GUINT32_BE(now.tv_sec, muid);		/* First half of MUID */
	muid += 4;
	WRITE_GUINT32_BE(now.tv_usec, muid);

	/*
	 * Inform the tsync layer that the "T1" timestamp is not the one
	 * we registered in vmsg_send_time_sync_req().  Tagging via the
	 * timestamp is the only mean we have to update the records since we
	 * can't attach metadata to the "pre-send" callbacks, hence the need
	 * to pass both the old and the new timestamps.
	 */

	tsync_send_timestamp(&old, &now);

	return TRUE;
}

/**
 * Send a "Time Sync Request" message, asking them to echo back their own
 * time so that we can compute our clock differences and measure round trip
 * times.  The time at which we send the message is included in the first
 * half of the MUID.
 *
 * If the node is an UDP node, its IP and port indicate to whom we shall
 * send the message.
 *
 * The `sent' parameter holds the initial "T1" timestamp markup.
 */
void
vmsg_send_time_sync_req(struct gnutella_node *n, gboolean ntp, tm_t *sent)
{
	struct gnutella_msg_vendor *m = (struct gnutella_msg_vendor *) v_tmp;
	guint32 msgsize;
	guint32 paysize = sizeof(guint8);
	gchar *payload;
	gchar *muid;
	pmsg_t *mb;

	if (!NODE_IS_WRITABLE(n))
		return;

	msgsize = vmsg_fill_header(&m->header, paysize, sizeof(v_tmp));
	payload = vmsg_fill_type(&m->data, T_GTKG, 9, 1);
	*payload = ntp ? 0x1 : 0x0;				/* bit0 indicates NTP */

	mb = gmsg_to_ctrl_pmsg(m, msgsize);		/* Send as quickly as possible */
	muid = pmsg_start(mb);

	/*
	 * The first 8 bytes of the MUID are used to store the time at which
	 * we send the message, and we fill that as late as possible.  We write
	 * the current time now, because we have to return it to the caller,
	 * but it will be superseded when the message is finally scheduled to
	 * be sent by the queue.
	 */

	pmsg_set_check(mb, vmsg_time_sync_req_stamp);

	WRITE_GUINT32_BE(sent->tv_sec, muid);
	muid += 4;
	WRITE_GUINT32_BE(sent->tv_usec, muid);

	if (NODE_IS_UDP(n))
		mq_udp_node_putq(n->outq, mb, n);
	else
		mq_putq(n->outq, mb);
}

/**
 * Callback invoked when "Time Sync Reply" is about to be sent.
 * Writes current time in the second half of the MUID.
 */
static gboolean
vmsg_time_sync_reply_stamp(pmsg_t *mb, struct mqueue *unused_q)
{
	tm_t now;
	gchar *muid = pmsg_start(mb);

	(void) unused_q;
	g_assert(pmsg_is_writable(mb));
	STATIC_ASSERT(sizeof(now) >= 2 * sizeof(guint32));

	muid += 8;

	tm_now(&now);
	now.tv_sec = clock_loc2gmt(now.tv_sec);

	WRITE_GUINT32_BE(now.tv_sec, muid);		/* Second half of MUID */
	muid += 4;
	WRITE_GUINT32_BE(now.tv_usec, muid);

	return TRUE;
}

/**
 * Send a "Time Sync Reply" message to the node, including the time at
 * which we send back the message in the second half of the MUID.
 * The time in `got' is the time at which we received their request.
 */
void
vmsg_send_time_sync_reply(struct gnutella_node *n, gboolean ntp, tm_t *got)
{
	struct gnutella_msg_vendor *m = (struct gnutella_msg_vendor *) v_tmp;
	guint32 msgsize;
	guint32 paysize = sizeof(guint8) + 2 * sizeof(guint32);
	gchar *payload;
	gchar *muid;
	pmsg_t *mb;

	if (!NODE_IS_WRITABLE(n))
		return;

	msgsize = vmsg_fill_header(&m->header, paysize, sizeof(v_tmp));
	payload = vmsg_fill_type(&m->data, T_GTKG, 10, 1);

	*payload = ntp ? 0x1 : 0x0;			/* bit 0 indicates NTP */
	payload++;

	/*
	 * Write time at which we got their message, so they can substract
	 * the processing time from the computation of the round-trip time.
	 */

	WRITE_GUINT32_BE(got->tv_sec, payload);
	payload += 4;
	WRITE_GUINT32_BE(got->tv_usec, payload);

	mb = gmsg_to_ctrl_pmsg(m, msgsize);		/* Send as quickly as possible */
	muid = pmsg_start(mb);					/* MUID of the reply */

	/*
	 * Propagate first half of the MUID, which is the time at which
	 * they sent us the message in their clock time, into the reply's MUID
	 *
	 * The second 8 bytes of the MUID are used to store the time at which
	 * we send the message, and we fill that as late as possible, i.e.
	 * when we are about to send the message.
	 */

	memcpy(muid, n->header.muid, 8);		/* First half of MUID */

	pmsg_set_check(mb, vmsg_time_sync_reply_stamp);

	if (NODE_IS_UDP(n))
		mq_udp_node_putq(n->outq, mb, n);
	else
		mq_putq(n->outq, mb);
}

/**
 * Handle reception of an UDP crawler ping.
 */
static void
handle_udp_crawler_ping(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size)
{
	guint8 number_up;
	guint8 number_leaves;
	guint8 features;

	/*
	 * We expect those messages to come via UDP.
	 */

	if (!NODE_IS_UDP(n)) {
		g_warning("got %s/%uv%u from TCP via %s, ignoring",
			vendor_code_str(vmsg->vendor), vmsg->id, vmsg->version, node_ip(n));
		return;
	}

	/*
	 * The format of the message was reverse-engineered from LimeWire's code.
	 * The version 1 message is claimed to be forward compatible with future
	 * versions, meaning the first 3 bytes will remain in newer versions.
	 *
	 * The payload is made of 3 bytes:
	 *
	 *   number_up: 	the # of UP they want to know about (255 means ALL)
	 *   number_leaves: the # of leaves they want to know about (255 means ALL)
	 *	 features:		some flags defining what to return
	 *					0x1 - connection time, in minutes
	 *					0x2 - locale info (2-letter language code)
	 *					0x4 - "new" peers only (supporting this LIME/5 message)
	 *					0x8 - user agent of peers, separated by ";" and deflated
	 *
	 * Upon reception of this message, an "UDP Crawler Pong" (LIME/6v1) is built
	 * and sent back to the requester.
	 */

	if (vmsg->version == 1 && size != 3) {
		vmsg_bad_payload(n, vmsg, size, 3);
		return;
	}

	number_up = payload[0];
	number_leaves = payload[1];
	features = payload[2] & NODE_CR_MASK;

	node_crawl(n, number_up, number_leaves, features);
}

/**
 * Send UDP crawler pong, in reply to their ping.
 * The supplied message block contains the payload to send back.
 */
void
vmsg_send_udp_crawler_pong(struct gnutella_node *n, pmsg_t *mb)
{
	struct gnutella_msg_vendor *m = (struct gnutella_msg_vendor *) v_tmp;
	guint32 msgsize;
	guint32 paysize = pmsg_size(mb);
	gchar *payload;

	g_assert(NODE_IS_UDP(n));

	msgsize = vmsg_fill_header(&m->header, paysize, sizeof(v_tmp));
	payload = vmsg_fill_type(&m->data, T_LIME, 6, 1);
	memcpy(m->header.muid, n->header.muid, 16);		/* Propagate MUID */

	memcpy(payload, pmsg_start(mb), paysize);

	if (vmsg_debug > 1) {
		guint8 nup = payload[0];
		guint8 nleaves = payload[1];

		printf("VMSG sending %s with up=%u and leaves=%u to %s\n",
			gmsg_infostr_full(m), nup, nleaves, node_ip(n));
	}

	udp_send_msg(n, m, msgsize);
}

#if 0
/**
 * Send an "UDP Crawler Ping" message to specified node. -- For testing only
 */
void
vmsg_send_udp_crawler_ping(struct gnutella_node *n,
	guint8 ultras, guint8 leaves, guint8 features)
{
	struct gnutella_msg_vendor *m = (struct gnutella_msg_vendor *) v_tmp;
	guint32 paysize = sizeof(ultras) + sizeof(leaves) + sizeof(features);
	guint32 msgsize;
	guchar *payload;

	g_assert(NODE_IS_UDP(n));

	msgsize = vmsg_fill_header(&m->header, paysize, sizeof(v_tmp));
	payload = vmsg_fill_type(&m->data, T_LIME, 5, 1);

	*payload++ = ultras;
	*payload++ = leaves;
	*payload++ = features;

	udp_send_msg(n, m, msgsize);
}
#endif

/* vi: set ts=4: */
