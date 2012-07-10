/*
Copyright 2011 by Matthieu Boutier and Juliusz Chroboczek

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include <zebra.h>
#include "memory.h"
#include "log.h"
#include "command.h"
#include "prefix.h"
#include "vector.h"
#include "distribute.h"

#include "babel_main.h"
#include "util.h"
#include "kernel.h"
#include "babel_interface.h"
#include "message.h"
#include "route.h"
#include "babel_zebra.h"
#include "neighbour.h"
#include "route.h"
#include "xroute.h"


#define IS_ENABLE(ifp) (babel_enable_if_lookup(ifp->name) >= 0)

static int babel_enable_if_lookup (const char *ifname);
static int babel_enable_if_add (const char *ifname);
static int babel_enable_if_delete (const char *ifname);
static int interface_recalculate(struct interface *ifp);
static int interface_reset(struct interface *ifp);
static int babel_if_new_hook    (struct interface *ifp);
static int babel_if_delete_hook (struct interface *ifp);
static int interface_config_write (struct vty *vty);
static babel_interface_nfo * babel_interface_allocate (void);
static void babel_interface_free (babel_interface_nfo *bi);


static vector babel_enable_if;                 /* enable interfaces (by cmd). */
static struct cmd_node babel_interface_node =  /* babeld's interface node.    */
{
    INTERFACE_NODE,
    "%s(config-if)# ",
    1 /* VTYSH */
};


int
babel_interface_up (int cmd, struct zclient *client, zebra_size_t length)
{
    struct stream *s = NULL;
    struct interface *ifp = NULL;

    debugf(BABEL_DEBUG_IF, "receive a 'interface up'");

    s = zclient->ibuf;
    ifp = zebra_interface_state_read(s); /* it updates iflist */

    if (ifp == NULL) {
        return 0;
    }

    interface_recalculate(ifp);
    return 0;
}

int
babel_interface_down (int cmd, struct zclient *client, zebra_size_t length)
{
    struct stream *s = NULL;
    struct interface *ifp = NULL;

    debugf(BABEL_DEBUG_IF, "receive a 'interface down'");

    s = zclient->ibuf;
    ifp = zebra_interface_state_read(s); /* it updates iflist */

    if (ifp == NULL) {
        return 0;
    }

    interface_reset(ifp);
    return 0;
}

int
babel_interface_add (int cmd, struct zclient *client, zebra_size_t length)
{
    struct interface *ifp = NULL;

    debugf(BABEL_DEBUG_IF, "receive a 'interface add'");

    /* read and add the interface in the iflist. */
    ifp = zebra_interface_add_read (zclient->ibuf);

    if (ifp == NULL) {
        return 0;
    }

    interface_recalculate(ifp);
    return 0;
}

int
babel_interface_delete (int cmd, struct zclient *client, zebra_size_t length)
{
    struct interface *ifp;
    struct stream *s;

    debugf(BABEL_DEBUG_IF, "receive a 'interface delete'");

    s = zclient->ibuf;
    ifp = zebra_interface_state_read(s); /* it updates iflist */

    if (ifp == NULL)
        return 0;

    if (IS_ENABLE(ifp))
        interface_reset(ifp);

    /* To support pseudo interface do not free interface structure.  */
    /* if_delete(ifp); */
    ifp->ifindex = IFINDEX_INTERNAL;

    return 0;
}

int
babel_interface_address_add (int cmd, struct zclient *client,
                             zebra_size_t length)
{
    babel_interface_nfo *babel_ifp;
    struct connected *ifc;
    struct prefix *prefix;

    debugf(BABEL_DEBUG_IF, "receive a 'interface address add'");

    ifc = zebra_interface_address_read (ZEBRA_INTERFACE_ADDRESS_ADD,
                                        zclient->ibuf);

    if (ifc == NULL)
        return 0;

    prefix = ifc->address;

    if (prefix->family == AF_INET) {
        flush_interface_routes(ifc->ifp, 0);
        babel_ifp = babel_get_if_nfo(ifc->ifp);
        if (babel_ifp->ipv4 == NULL) {
            babel_ifp->ipv4 = malloc(4);
            if (babel_ifp->ipv4 == NULL) {
                zlog_err("not einough memory");
            } else {
                memcpy(babel_ifp->ipv4, &prefix->u.prefix4, 4);
            }
        }
    }

    send_request(ifc->ifp, NULL, 0);
    send_update(ifc->ifp, 0, NULL, 0);

    return 0;
}

int
babel_interface_address_delete (int cmd, struct zclient *client,
                                zebra_size_t length)
{
    babel_interface_nfo *babel_ifp;
    struct connected *ifc;
    struct prefix *prefix;

    debugf(BABEL_DEBUG_IF, "receive a 'interface address delete'");

    ifc = zebra_interface_address_read (ZEBRA_INTERFACE_ADDRESS_DELETE,
                                        zclient->ibuf);

    if (ifc == NULL)
        return 0;

    prefix = ifc->address;

    if (prefix->family == AF_INET) {
        flush_interface_routes(ifc->ifp, 0);
        babel_ifp = babel_get_if_nfo(ifc->ifp);
        if (babel_ifp->ipv4 != NULL
            && memcmp(babel_ifp->ipv4, &prefix->u.prefix4, 4) == 0) {
            free(babel_ifp->ipv4);
            babel_ifp->ipv4 = NULL;
        }
    }

    send_request(ifc->ifp, NULL, 0);
    send_update(ifc->ifp, 0, NULL, 0);

    return 0;
}

/* Lookup function. */
static int
babel_enable_if_lookup (const char *ifname)
{
    unsigned int i;
    char *str;

    for (i = 0; i < vector_active (babel_enable_if); i++)
        if ((str = vector_slot (babel_enable_if, i)) != NULL)
            if (strcmp (str, ifname) == 0)
                return i;
    return -1;
}

/* Add interface to babel_enable_if. */
static int
babel_enable_if_add (const char *ifname)
{
    int ret;
    struct interface *ifp = NULL;

    ret = babel_enable_if_lookup (ifname);
    if (ret >= 0)
        return -1;

    vector_set (babel_enable_if, strdup (ifname));

    ifp = if_lookup_by_name(ifname);
    if (ifp != NULL)
        interface_recalculate(ifp);

    return 1;
}

/* Delete interface from babel_enable_if. */
static int
babel_enable_if_delete (const char *ifname)
{
    int babel_enable_if_index;
    char *str;
    struct interface *ifp = NULL;

    babel_enable_if_index = babel_enable_if_lookup (ifname);
    if (babel_enable_if_index < 0)
        return -1;

    str = vector_slot (babel_enable_if, babel_enable_if_index);
    free (str);
    vector_unset (babel_enable_if, babel_enable_if_index);

    ifp = if_lookup_by_name(ifname);
    if (ifp != NULL)
        interface_reset(ifp);

    return 1;
}

/* [Babel Command] Babel enable on specified interface or matched network. */
DEFUN (babel_network,
       babel_network_cmd,
       "network IF_OR_ADDR",
       "Enable Babel protocol on specified interface or network.\n"
       "Interface or address")
{
    int ret;
    struct prefix p;

    ret = str2prefix (argv[0], &p);

    /* Given string is:               */
    if (ret) /* an IPv4 or v6 network */
        return CMD_ERR_NO_MATCH; /* not implemented yet */
    else     /* an interface name     */
        ret = babel_enable_if_add (argv[0]);

    if (ret < 0) {
        vty_out (vty, "There is same network configuration %s%s", argv[0],
                 VTY_NEWLINE);
        return CMD_WARNING;
    }

    return CMD_SUCCESS;
}

/* [Babel Command] Babel enable on specified interface or matched network. */
DEFUN (no_babel_network,
       no_babel_network_cmd,
       "no network IF_OR_ADDR",
       NO_STR
       "Disable Babel protocol on specified interface or network.\n"
       "Interface or address")
{
    int ret;
    struct prefix p;

    ret = str2prefix (argv[0], &p);

    /* Given string is:               */
    if (ret) /* an IPv4 or v6 network */
        return CMD_ERR_NO_MATCH; /* not implemented yet */
    else     /* an interface name     */
        ret = babel_enable_if_delete (argv[0]);

    if (ret < 0) {
        vty_out (vty, "can't find network %s%s", argv[0],
                 VTY_NEWLINE);
        return CMD_WARNING;
    }

    return CMD_SUCCESS;
}

/* There are a number of interface parameters that must be changed when
   an interface becomes wired/wireless.  In Quagga, they cannot be
   configured separately. */

static void
babel_set_wired_internal(babel_interface_nfo *babel_ifp, int wired)
{
    if(wired) {
        babel_ifp->flags |= BABEL_IF_WIRED;
        babel_ifp->flags |= BABEL_IF_SPLIT_HORIZON;
        babel_ifp->cost = BABEL_DEFAULT_RXCOST_WIRED;
        babel_ifp->flags &= ~BABEL_IF_LQ;
    } else {
        babel_ifp->flags &= ~BABEL_IF_WIRED;
        babel_ifp->flags &= ~BABEL_IF_SPLIT_HORIZON;
        babel_ifp->cost = BABEL_DEFAULT_RXCOST_WIRELESS;
        babel_ifp->flags |= BABEL_IF_LQ;
    }

}

/* [Interface Command] Tell the interface is wire. */
DEFUN (babel_set_wired,
       babel_set_wired_cmd,
       "babel wired",
       "Babel interface commands\n"
       "Enable wired optimisations")
{
    struct interface *ifp;
    babel_interface_nfo *babel_ifp;

    ifp = vty->index;
    babel_ifp = babel_get_if_nfo(ifp);

    assert (babel_ifp != NULL);
    babel_set_wired_internal(babel_ifp, 1);
    return CMD_SUCCESS;
}

/* [Interface Command] Tell the interface is wireless (default). */
DEFUN (babel_set_wireless,
       babel_set_wireless_cmd,
       "babel wireless",
       "Babel interface commands\n"
       "Disable wired optimiations (assume wireless)")
{
    struct interface *ifp;
    babel_interface_nfo *babel_ifp;

    ifp = vty->index;
    babel_ifp = babel_get_if_nfo(ifp);

    assert (babel_ifp != NULL);
    babel_set_wired_internal(babel_ifp, 0);
    return CMD_SUCCESS;
}

/* [Interface Command] Enable split horizon. */
DEFUN (babel_split_horizon,
       babel_split_horizon_cmd,
       "babel split-horizon",
       "Babel interface commands\n"
       "Enable split horizon processing")
{
    struct interface *ifp;
    babel_interface_nfo *babel_ifp;

    ifp = vty->index;
    babel_ifp = babel_get_if_nfo(ifp);

    assert (babel_ifp != NULL);
    babel_ifp->flags |= BABEL_IF_SPLIT_HORIZON;
    return CMD_SUCCESS;
}

/* [Interface Command] Disable split horizon (default). */
DEFUN (no_babel_split_horizon,
       no_babel_split_horizon_cmd,
       "no babel split-horizon",
       NO_STR
       "Babel interface commands\n"
       "Disable split horizon processing")
{
    struct interface *ifp;
    babel_interface_nfo *babel_ifp;

    ifp = vty->index;
    babel_ifp = babel_get_if_nfo(ifp);

    assert (babel_ifp != NULL);
    babel_ifp->flags &= ~BABEL_IF_SPLIT_HORIZON;
    return CMD_SUCCESS;
}

/* [Interface Command]. */
DEFUN (babel_set_hello_interval,
       babel_set_hello_interval_cmd,
       "babel hello-interval <20-655340>",
       "Babel interface commands\n"
       "Time between scheduled hellos\n"
       "Milliseconds\n")
{
    struct interface *ifp;
    babel_interface_nfo *babel_ifp;
    int interval;

    VTY_GET_INTEGER_RANGE("milliseconds", interval, argv[0], 20, 10 * 0xFFFE);

    ifp = vty->index;
    babel_ifp = babel_get_if_nfo(ifp);
    assert (babel_ifp != NULL);

    babel_ifp->hello_interval = interval;
    return CMD_SUCCESS;
}

/* [Interface Command]. */
DEFUN (babel_set_update_interval,
       babel_set_update_interval_cmd,
       "babel update-interval <20-655340>",
       "Babel interface commands\n"
       "Time between scheduled updates\n"
       "Milliseconds\n")
{
    struct interface *ifp;
    babel_interface_nfo *babel_ifp;
    int interval;

    VTY_GET_INTEGER_RANGE("milliseconds", interval, argv[0], 20, 10 * 0xFFFE);

    ifp = vty->index;
    babel_ifp = babel_get_if_nfo(ifp);
    assert (babel_ifp != NULL);

    babel_ifp->update_interval = interval;
    return CMD_SUCCESS;
}

DEFUN (babel_set_rxcost,
       babel_set_rxcost_cmd,
       "babel rxcost <1-65534>",
       "Babel interface commands\n"
       "Rxcost multiplier\n"
       "Units")
{
    struct interface *ifp;
    babel_interface_nfo *babel_ifp;
    int rxcost;

    VTY_GET_INTEGER_RANGE("units", rxcost, argv[0], 1, 0x10000 - 1);

    ifp = vty->index;
    babel_ifp = babel_get_if_nfo(ifp);
    assert (babel_ifp != NULL);

    babel_ifp->cost = rxcost;
    return CMD_SUCCESS;
}

/* This should be no more than half the hello interval, so that hellos
   aren't sent late.  The result is in milliseconds. */
unsigned
jitter(babel_interface_nfo *babel_ifp, int urgent)
{
    unsigned interval = babel_ifp->hello_interval;
    if(urgent)
        interval = MIN(interval, 100);
    else
        interval = MIN(interval, 4000);
    return roughly(interval) / 4;
}

unsigned
update_jitter(babel_interface_nfo *babel_ifp, int urgent)
{
    unsigned interval = babel_ifp->hello_interval;
    if(urgent)
        interval = MIN(interval, 100);
    else
        interval = MIN(interval, 4000);
    return roughly(interval);
}

/* calculate babeld's specific datas of an interface (change when the interface
 change) */
static int
interface_recalculate(struct interface *ifp)
{
    babel_interface_nfo *babel_ifp = babel_get_if_nfo(ifp);
    unsigned char *tmp = NULL;
    int mtu, rc;
    struct ipv6_mreq mreq;

    if (!IS_ENABLE(ifp))
        return -1;

    if (!if_is_operative(ifp) || !CHECK_FLAG(ifp->flags, IFF_RUNNING)) {
        interface_reset(ifp);
        return -1;
    }

    babel_ifp->flags |= BABEL_IF_IS_UP;

    mtu = MIN(ifp->mtu, ifp->mtu6);

    /* We need to be able to fit at least two messages into a packet,
     so MTUs below 116 require lower layer fragmentation. */
    /* In IPv6, the minimum MTU is 1280, and every host must be able
     to reassemble up to 1500 bytes, but I'd rather not rely on this. */
    if(mtu < 128) {
        debugf(BABEL_DEBUG_IF, "Suspiciously low MTU %d on interface %s (%d).",
               mtu, ifp->name, ifp->ifindex);
        mtu = 128;
    }

    /* 4 for Babel header; 40 for IPv6 header, 8 for UDP header, 12 for good luck. */
    babel_ifp->bufsize = mtu - 4 - 60;
    tmp = babel_ifp->sendbuf;
    babel_ifp->sendbuf = realloc(babel_ifp->sendbuf, babel_ifp->bufsize);
    if(babel_ifp->sendbuf == NULL) {
        zlog_err("Couldn't reallocate sendbuf.");
        free(tmp);
        babel_ifp->bufsize = 0;
        return -1;
    }
    tmp = NULL;

    resize_receive_buffer(mtu);

    memset(&mreq, 0, sizeof(mreq));
    memcpy(&mreq.ipv6mr_multiaddr, protocol_group, 16);
    mreq.ipv6mr_interface = ifp->ifindex;

    rc = setsockopt(protocol_socket, IPPROTO_IPV6, IPV6_JOIN_GROUP,
                    (char*)&mreq, sizeof(mreq));
    if(rc < 0) {
        zlog_err("setsockopt(IPV6_JOIN_GROUP) on interface '%s': %s",
                 ifp->name, safe_strerror(errno));
        /* This is probably due to a missing link-local address,
         so down this interface, and wait until the main loop
         tries to up it again. */
        interface_reset(ifp);
        return -1;
    }

    set_timeout(&babel_ifp->hello_timeout, babel_ifp->hello_interval);
    set_timeout(&babel_ifp->update_timeout, babel_ifp->update_interval);
    send_hello(ifp);
    send_request(ifp, NULL, 0);

    update_interface_metric(ifp);

    debugf(BABEL_DEBUG_COMMON,
           "Upped interface %s (%s, cost=%d, channel=%d%s).",
           ifp->name,
           (babel_ifp->flags & BABEL_IF_WIRED) ? "wired" : "wireless",
           babel_ifp->cost,
           babel_ifp->channel,
           babel_ifp->ipv4 ? ", IPv4" : "");

    if(rc > 0)
        send_update(ifp, 0, NULL, 0);

    return 1;
}

/* Reset the interface as it was new: it's not removed from the interface list,
 and may be considered as a upped interface. */
static int
interface_reset(struct interface *ifp)
{
    int rc;
    struct ipv6_mreq mreq;
    babel_interface_nfo *babel_ifp = babel_get_if_nfo(ifp);

    if (!(babel_ifp->flags & BABEL_IF_IS_UP))
        return 0;

    debugf(BABEL_DEBUG_IF, "interface reset: %s", ifp->name);
    babel_ifp->flags &= ~BABEL_IF_IS_UP;

    flush_interface_routes(ifp, 0);
    babel_ifp->buffered = 0;
    babel_ifp->bufsize = 0;
    free(babel_ifp->sendbuf);
    babel_ifp->num_buffered_updates = 0;
    babel_ifp->update_bufsize = 0;
    if(babel_ifp->buffered_updates)
        free(babel_ifp->buffered_updates);
    babel_ifp->buffered_updates = NULL;
    babel_ifp->sendbuf = NULL;

    if(ifp->ifindex > 0) {
        memset(&mreq, 0, sizeof(mreq));
        memcpy(&mreq.ipv6mr_multiaddr, protocol_group, 16);
        mreq.ipv6mr_interface = ifp->ifindex;
        rc = setsockopt(protocol_socket, IPPROTO_IPV6, IPV6_LEAVE_GROUP,
                        (char*)&mreq, sizeof(mreq));
        if(rc < 0)
            zlog_err("setsockopt(IPV6_LEAVE_GROUP) on interface '%s': %s",
                     ifp->name, safe_strerror(errno));
    }

    update_interface_metric(ifp);

    debugf(BABEL_DEBUG_COMMON,"Upped network %s (%s, cost=%d%s).",
           ifp->name,
           (babel_ifp->flags & BABEL_IF_WIRED) ? "wired" : "wireless",
           babel_ifp->cost,
           babel_ifp->ipv4 ? ", IPv4" : "");

    return 1;
}

/* Send retraction to all, and reset all interfaces statistics. */
void
babel_interface_close_all(void)
{
    struct interface *ifp = NULL;
    struct listnode *linklist_node = NULL;

    FOR_ALL_INTERFACES(ifp, linklist_node) {
        if(!if_up(ifp))
            continue;
        send_wildcard_retraction(ifp);
        /* Make sure that we expire quickly from our neighbours'
         association caches. */
        send_hello_noupdate(ifp, 10);
        flushbuf(ifp);
        usleep(roughly(1000));
        gettime(&babel_now);
    }
    FOR_ALL_INTERFACES(ifp, linklist_node) {
        if(!if_up(ifp))
            continue;
        /* Make sure they got it. */
        send_wildcard_retraction(ifp);
        send_hello_noupdate(ifp, 1);
        flushbuf(ifp);
        usleep(roughly(10000));
        gettime(&babel_now);
        interface_reset(ifp);
    }
}

/* return "true" if address is one of our ipv6 addresses */
int
is_interface_ll_address(struct interface *ifp, const unsigned char *address)
{
    struct connected *connected;
    struct listnode *node;

    if(!if_up(ifp))
        return 0;

    FOR_ALL_INTERFACES_ADDRESSES(ifp, connected, node) {
        if(connected->address->family == AF_INET6 &&
           memcmp(&connected->address->u.prefix6, address, 16) == 0)
            return 1;
    }

    return 0;
}

static void
show_babel_interface_sub (struct vty *vty, struct interface *ifp)
{
  int is_up;
  babel_interface_nfo *babel_ifp;

  vty_out (vty, "%s is %s%s", ifp->name,
    ((is_up = if_is_operative(ifp)) ? "up" : "down"), VTY_NEWLINE);
  vty_out (vty, "  ifindex %u, MTU %u bytes %s%s",
    ifp->ifindex, ifp->mtu, if_flag_dump(ifp->flags), VTY_NEWLINE);

  if (babel_enable_if_lookup (ifp->name) < 0)
  {
    vty_out (vty, "  Babel protocol is not enabled on this interface%s", VTY_NEWLINE);
    return;
  }
  if (!is_up)
  {
    vty_out (vty, "  Babel protocol is enabled, but not running on this interface%s", VTY_NEWLINE);
    return;
  }
  babel_ifp = babel_get_if_nfo (ifp);
  vty_out (vty, "  Babel protocol is running on this interface%s", VTY_NEWLINE);
  vty_out (vty, "  Operating mode is \"%s\"%s",
           CHECK_FLAG (babel_ifp->flags, BABEL_IF_WIRED) ? "wired" : "wireless", VTY_NEWLINE);
  vty_out (vty, "  Split horizon mode is %s%s",
           CHECK_FLAG (babel_ifp->flags, BABEL_IF_SPLIT_HORIZON) ? "On" : "Off", VTY_NEWLINE);
  vty_out (vty, "  Hello interval is %u ms%s", babel_ifp->hello_interval, VTY_NEWLINE);
  vty_out (vty, "  Update interval is %u ms%s", babel_ifp->update_interval, VTY_NEWLINE);
  vty_out (vty, "  Rxcost multiplier is %u%s", babel_ifp->cost, VTY_NEWLINE);
}

DEFUN (show_babel_interface,
       show_babel_interface_cmd,
       "show babel interface",
       SHOW_STR
       "Babel information\n"
       "Interface information\n")
{
  struct interface *ifp;
  struct listnode *node;

  if (argc == 0)
  {
    for (ALL_LIST_ELEMENTS_RO (iflist, node, ifp))
      show_babel_interface_sub (vty, ifp);
    return CMD_SUCCESS;
  }
  if ((ifp = if_lookup_by_name (argv[0])) == NULL)
  {
    vty_out (vty, "No such interface name%s", VTY_NEWLINE);
    return CMD_WARNING;
  }
  show_babel_interface_sub (vty, ifp);
  return CMD_SUCCESS;
}

ALIAS (show_babel_interface,
       show_babel_interface_ifname_cmd,
       "show babel interface IFNAME",
       SHOW_STR
       "Babel information\n"
       "Interface information\n"
       "Interface name\n")

static void
show_babel_neighbour_sub (struct vty *vty, struct neighbour *neigh)
{
    vty_out (vty,
             "Neighbour %s dev %s reach %04x rxcost %d txcost %d%s.%s",
             format_address(neigh->address),
             neigh->ifp->name,
             neigh->reach,
             neighbour_rxcost(neigh),
             neigh->txcost,
             if_up(neigh->ifp) ? "" : " (down)",
             VTY_NEWLINE);
}

DEFUN (show_babel_neighbour,
       show_babel_neighbour_cmd,
       "show babel neighbor",
       SHOW_STR
       "Babel information\n"
       "Print neighbors\n")
{
    struct neighbour *neigh;
    struct interface *ifp;

    if (argc == 0) {
        FOR_ALL_NEIGHBOURS(neigh) {
            show_babel_neighbour_sub(vty, neigh);
        }
        return CMD_SUCCESS;
    }
    if ((ifp = if_lookup_by_name (argv[0])) == NULL)
    {
        vty_out (vty, "No such interface name%s", VTY_NEWLINE);
        return CMD_WARNING;
    }
    FOR_ALL_NEIGHBOURS(neigh) {
        if(ifp->ifindex == neigh->ifp->ifindex) {
            show_babel_neighbour_sub(vty, neigh);
        }
    }
    return CMD_SUCCESS;
}

ALIAS (show_babel_neighbour,
       show_babel_neighbour_ifname_cmd,
       "show babel neighbor IFNAME",
       SHOW_STR
       "Babel information\n"
       "Print neighbors\n"
       "Interface name\n")

struct show_babel_routes_closure {
    struct vty *vty;
    struct prefix *prefix;
};

static int
babel_prefix_eq(struct prefix *prefix, unsigned char *p, int plen)
{
    if(prefix->family == AF_INET6) {
        if(prefix->prefixlen != plen ||
           memcmp(&prefix->u.prefix6, p, 16) != 0)
            return 0;
    } else if(prefix->family == AF_INET) {
        if(plen < 96 || !v4mapped(p) || prefix->prefixlen != plen - 96 ||
           memcmp(&prefix->u.prefix4, p + 12, 4) != 0)
            return 0;
    } else {
        return 0;
    }

    return 1;
}

static void
show_babel_routes_sub (struct babel_route *route, void *closure)
{
    struct show_babel_routes_closure *c =
        (struct show_babel_routes_closure*)closure;
    struct vty *vty = c->vty;
    struct prefix *prefix = c->prefix;
    const unsigned char *nexthop =
        memcmp(route->nexthop, route->neigh->address, 16) == 0 ?
        NULL : route->nexthop;
    char channels[100];

    if(prefix && !babel_prefix_eq(prefix, route->src->prefix, route->src->plen))
        return;

    if(route->channels[0] == 0)
        channels[0] = '\0';
    else {
        int k, j = 0;
        snprintf(channels, 100, " chan (");
        j = strlen(channels);
        for(k = 0; k < DIVERSITY_HOPS; k++) {
            if(route->channels[k] == 0)
                break;
            if(k > 0)
                channels[j++] = ',';
            snprintf(channels + j, 100 - j, "%d", route->channels[k]);
            j = strlen(channels);
        }
        snprintf(channels + j, 100 - j, ")");
        if(k == 0)
            channels[0] = '\0';
    }

    vty_out(vty,
            "%s metric %d refmetric %d id %s seqno %d%s age %d "
            "via %s neigh %s%s%s%s%s",
            format_prefix(route->src->prefix, route->src->plen),
            route_metric(route), route->refmetric,
            format_eui64(route->src->id),
            (int)route->seqno,
            channels,
            (int)(babel_now.tv_sec - route->time),
            route->neigh->ifp->name,
            format_address(route->neigh->address),
            nexthop ? " nexthop " : "",
            nexthop ? format_address(nexthop) : "",
            route->installed ? " (installed)" :
            route_feasible(route) ? " (feasible)" : "",
            VTY_NEWLINE);
}

static void
show_babel_xroutes_sub (struct xroute *xroute, void *closure)
{
    struct show_babel_routes_closure *c =
        (struct show_babel_routes_closure*)closure;
    struct vty *vty = c->vty;

    if(c->prefix && !babel_prefix_eq(c->prefix, xroute->prefix, xroute->plen))
        return;

    vty_out(vty, "%s metric %d (exported)%s",
            format_prefix(xroute->prefix, xroute->plen),
            xroute->metric,
            VTY_NEWLINE);
}

DEFUN (show_babel_route,
       show_babel_route_cmd,
       "show babel route",
       SHOW_STR
       "Babel information\n"
       "Babel internal routing table\n")
{
    struct show_babel_routes_closure c = {vty, NULL};
    for_all_routes(show_babel_routes_sub, &c);
    for_all_xroutes(show_babel_xroutes_sub, &c);
    return CMD_SUCCESS;
}

DEFUN (show_babel_route_prefix,
       show_babel_route_prefix_cmd,
       "show babel route A.B.C.D/M",
       SHOW_STR
       "Babel information\n"
       "IPv4 prefix <network>/<length>\n")
{
    struct show_babel_routes_closure c = {vty, NULL};
    struct prefix prefix;
    int ret;

    ret = str2prefix(argv[0], &prefix);
    if(ret == 0) {
      vty_out (vty, "%% Malformed address%s", VTY_NEWLINE);
      return CMD_WARNING;
    }
    c.prefix = &prefix;
        
    for_all_routes(show_babel_routes_sub, &c);
    for_all_xroutes(show_babel_xroutes_sub, &c);
    return CMD_SUCCESS;
}

ALIAS (show_babel_route_prefix,
       show_babel_route_prefix6_cmd,
       "show babel route X:X::X:X/M",
       SHOW_STR
       "Babel information\n"
       "IPv6 prefix <network>/<length>\n")

DEFUN (show_babel_route_addr,
       show_babel_route_addr_cmd,
       "show babel route A.B.C.D",
       SHOW_STR
       "Babel information\n"
       "IPv4 address <network>/<length>\n")
{
    struct show_babel_routes_closure c = {vty, NULL};
    struct in_addr addr;
    char buf[INET_ADDRSTRLEN + 8];
    struct prefix prefix;
    int ret;

    ret = inet_aton (argv[0], &addr);
    if (ret <= 0) {
        vty_out (vty, "%% Malformed address%s", VTY_NEWLINE);
        return CMD_WARNING;
    }

    /* Quagga has no convenient prefix constructors. */
    snprintf(buf, sizeof(buf), "%s/%d", inet_ntoa(addr), 32);

    ret = str2prefix(buf, &prefix);
    if (ret == 0) {
        vty_out (vty, "%% Parse error -- this shouldn't happen%s", VTY_NEWLINE);
        return CMD_WARNING;
    }

    c.prefix = &prefix;
    for_all_routes(show_babel_routes_sub, &c);
    for_all_xroutes(show_babel_xroutes_sub, &c);
    return CMD_SUCCESS;
}

DEFUN (show_babel_route_addr6,
       show_babel_route_addr6_cmd,
       "show babel route X:X::X:X",
       SHOW_STR
       "Babel information\n"
       "IPv6 address <network>/<length>\n")
{
    struct show_babel_routes_closure c = {vty, NULL};
    struct in6_addr addr;
    char buf1[INET6_ADDRSTRLEN];
    char buf[INET6_ADDRSTRLEN + 8];
    struct prefix prefix;
    int ret;

    ret = inet_pton (AF_INET6, argv[0], &addr);
    if (ret <= 0) {
        vty_out (vty, "%% Malformed address%s", VTY_NEWLINE);
        return CMD_WARNING;
    }

    /* Quagga has no convenient prefix constructors. */
    snprintf(buf, sizeof(buf), "%s/%d",
             inet_ntop(AF_INET6, &addr, buf1, sizeof(buf1)), 128);

    ret = str2prefix(buf, &prefix);
    if (ret == 0) {
        vty_out (vty, "%% Parse error -- this shouldn't happen%s", VTY_NEWLINE);
        return CMD_WARNING;
    }

    c.prefix = &prefix;
    for_all_routes(show_babel_routes_sub, &c);
    for_all_xroutes(show_babel_xroutes_sub, &c);
    return CMD_SUCCESS;
}

DEFUN (show_babel_parameters,
       show_babel_parameters_cmd,
       "show babel parameters",
       SHOW_STR
       "Babel information\n"
       "Configuration information\n")
{
    vty_out(vty, "    -- Babel running configuration --%s", VTY_NEWLINE);
    show_babel_main_configuration(vty);
    vty_out(vty, "    -- distribution lists --%s", VTY_NEWLINE);
    config_show_distribute(vty);

    return CMD_SUCCESS;
}

void
babel_if_init ()
{
    /* initialize interface list */
    if_init();
    if_add_hook (IF_NEW_HOOK,    babel_if_new_hook);
    if_add_hook (IF_DELETE_HOOK, babel_if_delete_hook);

    babel_enable_if = vector_init (1);

    /* install interface node and commands */
    install_element (CONFIG_NODE, &interface_cmd);
    install_element (CONFIG_NODE, &no_interface_cmd);
    install_node (&babel_interface_node, interface_config_write);
    install_default(INTERFACE_NODE);
    install_element(INTERFACE_NODE, &interface_cmd);
    install_element(INTERFACE_NODE, &no_interface_cmd);

    install_element(BABEL_NODE, &babel_network_cmd);
    install_element(BABEL_NODE, &no_babel_network_cmd);
    install_element(INTERFACE_NODE, &babel_split_horizon_cmd);
    install_element(INTERFACE_NODE, &no_babel_split_horizon_cmd);
    install_element(INTERFACE_NODE, &babel_set_wired_cmd);
    install_element(INTERFACE_NODE, &babel_set_wireless_cmd);
    install_element(INTERFACE_NODE, &babel_set_hello_interval_cmd);
    install_element(INTERFACE_NODE, &babel_set_update_interval_cmd);
    install_element(INTERFACE_NODE, &babel_set_rxcost_cmd);

    /* "show babel ..." commands */
    install_element(VIEW_NODE, &show_babel_interface_cmd);
    install_element(ENABLE_NODE, &show_babel_interface_cmd);
    install_element(VIEW_NODE, &show_babel_interface_ifname_cmd);
    install_element(ENABLE_NODE, &show_babel_interface_ifname_cmd);
    install_element(VIEW_NODE, &show_babel_neighbour_cmd);
    install_element(ENABLE_NODE, &show_babel_neighbour_cmd);
    install_element(VIEW_NODE, &show_babel_neighbour_ifname_cmd);
    install_element(ENABLE_NODE, &show_babel_neighbour_ifname_cmd);
    install_element(VIEW_NODE, &show_babel_route_cmd);
    install_element(ENABLE_NODE, &show_babel_route_cmd);
    install_element(VIEW_NODE, &show_babel_route_prefix_cmd);
    install_element(ENABLE_NODE, &show_babel_route_prefix_cmd);
    install_element(VIEW_NODE, &show_babel_route_prefix6_cmd);
    install_element(ENABLE_NODE, &show_babel_route_prefix6_cmd);
    install_element(VIEW_NODE, &show_babel_route_addr_cmd);
    install_element(ENABLE_NODE, &show_babel_route_addr_cmd);
    install_element(VIEW_NODE, &show_babel_route_addr6_cmd);
    install_element(ENABLE_NODE, &show_babel_route_addr6_cmd);
    install_element(VIEW_NODE, &show_babel_parameters_cmd);
    install_element(ENABLE_NODE, &show_babel_parameters_cmd);
}

/* hooks: functions called respectively when struct interface is
 created or deleted. */
static int
babel_if_new_hook (struct interface *ifp)
{
    ifp->info = babel_interface_allocate();
    return 0;
}

static int
babel_if_delete_hook (struct interface *ifp)
{
    babel_interface_free(ifp->info);
    ifp->info = NULL;
    return 0;
}

/* Output an "interface" section for each of the known interfaces with
babeld-specific statement lines where appropriate. */
static int
interface_config_write (struct vty *vty)
{
    struct listnode *node;
    struct interface *ifp;
    int write = 0;

    for (ALL_LIST_ELEMENTS_RO (iflist, node, ifp)) {
        vty_out (vty, "interface %s%s", ifp->name,
                 VTY_NEWLINE);
        if (ifp->desc)
            vty_out (vty, " description %s%s", ifp->desc,
                     VTY_NEWLINE);
        babel_interface_nfo *babel_ifp = babel_get_if_nfo (ifp);
        /* wireless is the default*/
        if (CHECK_FLAG (babel_ifp->flags, BABEL_IF_WIRED))
        {
            vty_out (vty, " babel wired%s", VTY_NEWLINE);
            write++;
        }
        if (babel_ifp->hello_interval != BABEL_DEFAULT_HELLO_INTERVAL)
        {
            vty_out (vty, " babel hello-interval %u%s", babel_ifp->hello_interval, VTY_NEWLINE);
            write++;
        }
        if (babel_ifp->update_interval != BABEL_DEFAULT_UPDATE_INTERVAL)
        {
            vty_out (vty, " babel update-interval %u%s", babel_ifp->update_interval, VTY_NEWLINE);
            write++;
        }
        /* Some parameters have different defaults for wired/wireless. */
        if (CHECK_FLAG (babel_ifp->flags, BABEL_IF_WIRED)) {
            if (!CHECK_FLAG (babel_ifp->flags, BABEL_IF_SPLIT_HORIZON)) {
                vty_out (vty, " no babel split-horizon%s", VTY_NEWLINE);
                write++;
            }
            if (babel_ifp->cost != BABEL_DEFAULT_RXCOST_WIRED) {
                vty_out (vty, " babel rxcost %u%s", babel_ifp->cost, VTY_NEWLINE);
                write++;
            }
        } else {
            if (CHECK_FLAG (babel_ifp->flags, BABEL_IF_SPLIT_HORIZON)) {
                vty_out (vty, " babel split-horizon%s", VTY_NEWLINE);
                write++;
            }
            if (babel_ifp->cost != BABEL_DEFAULT_RXCOST_WIRELESS) {
                vty_out (vty, " babel rxcost %u%s", babel_ifp->cost, VTY_NEWLINE);
                write++;
            }
        }
        vty_out (vty, "!%s", VTY_NEWLINE);
        write++;
    }
    return write;
}

/* Output a "network" statement line for each of the enabled interfaces. */
int
babel_enable_if_config_write (struct vty * vty)
{
    unsigned int i, lines = 0;
    char *str;

    for (i = 0; i < vector_active (babel_enable_if); i++)
        if ((str = vector_slot (babel_enable_if, i)) != NULL)
        {
            vty_out (vty, " network %s%s", str, VTY_NEWLINE);
            lines++;
        }
    return lines;
}

/* functions to allocate or free memory for a babel_interface_nfo, filling
 needed fields */
static babel_interface_nfo *
babel_interface_allocate (void)
{
    babel_interface_nfo *babel_ifp;
    babel_ifp = XMALLOC(MTYPE_BABEL_IF, sizeof(babel_interface_nfo));
    if(babel_ifp == NULL)
        return NULL;

    /* Here are set the default values for an interface. */
    memset(babel_ifp, 0, sizeof(babel_interface_nfo));
    /* All flags are unset */
    babel_ifp->bucket_time = babel_now.tv_sec;
    babel_ifp->bucket = BUCKET_TOKENS_MAX;
    babel_ifp->hello_seqno = (random() & 0xFFFF);
    babel_ifp->hello_interval = BABEL_DEFAULT_HELLO_INTERVAL;
    babel_ifp->update_interval = BABEL_DEFAULT_UPDATE_INTERVAL;
    babel_ifp->channel = BABEL_IF_CHANNEL_INTERFERING;
    babel_set_wired_internal(babel_ifp, 0);

    return babel_ifp;
}

static void
babel_interface_free (babel_interface_nfo *babel_ifp)
{
    XFREE(MTYPE_BABEL_IF, babel_ifp);
}
