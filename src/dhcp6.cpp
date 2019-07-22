/**
 * @file
 *
 * @defgroup dhcp6 DHCPv6
 * @ingroup ip6
 * DHCPv6 client: IPv6 address autoconfiguration as per
 * RFC 3315 (stateful DHCPv6) and
 * RFC 3736 (stateless DHCPv6).
 *
 * For now, only stateless DHCPv6 is implemented!
 *
 * TODO:
 * - enable/disable API to not always start when RA is received
 * - stateful DHCPv6 (for now, only stateless DHCPv6 for DNS and NTP servers works)
 * - create Client Identifier?
 * - only start requests if a valid local address is available on the netif
 * - only start information requests if required (not for every RA)
 *
 * dhcp6_enable_stateful() enables stateful DHCPv6 for a netif (stateless disabled)\n
 * dhcp6_enable_stateless() enables stateless DHCPv6 for a netif (stateful disabled)\n
 * dhcp6_disable() disable DHCPv6 for a netif
 *
 * When enabled, requests are only issued after receipt of RA with the
 * corresponding bits set.
 */

/*
 * Copyright (c) 2018 Simon Goldschmidt
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 *
 * Author: Simon Goldschmidt <goldsimon@gmx.de>
 */

#include <opt.h>
#include <dhcp6.h>
#include <udp.h>
#include <dns.h>
#include <cstring>
#include <lwip_debug.h>
#include <ip.h>
#define LWIP_HOOK_DHCP6_APPEND_OPTIONS(netif, dhcp6, state, msg, msg_type, options_len_ptr, max_len)

#define LWIP_HOOK_DHCP6_PARSE_OPTION(netif, dhcp6, state, msg, msg_type, option, len, PacketBuffer, offset) do { 

#define LWIP_DHCP6_PROVIDE_DNS_SERVERS LWIP_DHCP6_MAX_DNS_SERVERS




/** Option handling: options are parsed in dhcp6_parse_reply
 * and saved in an array where other functions can load them from.
 * This might be moved into the Dhcp6 (not necessarily since
 * lwIP is single-threaded and the array is only used while in recv
 * callback). */
enum Dhcp6OptionIdx {
  DHCP6_OPTION_IDX_CLI_ID = 0,
  DHCP6_OPTION_IDX_SERVER_ID,
  DHCP6_OPTION_IDX_DNS_SERVER,
  DHCP6_OPTION_IDX_DOMAIN_LIST,
  DHCP6_OPTION_IDX_NTP_SERVER,
  DHCP6_OPTION_IDX_MAX
};

struct Dhcp6OptionInfo {
  uint8_t option_given;
  uint16_t val_start;
  uint16_t val_length;
};

// Holds the decoded option info, only valid while in dhcp6_recv.
struct Dhcp6OptionInfo dhcp6_rx_options[DHCP6_OPTION_IDX_MAX];

#define dhcp6_option_given(dhcp6, idx)           (dhcp6_rx_options[idx].option_given != 0)
#define dhcp6_got_option(dhcp6, idx)             (dhcp6_rx_options[idx].option_given = 1)
#define dhcp6_clear_option(dhcp6, idx)           (dhcp6_rx_options[idx].option_given = 0)
#define dhcp6_clear_all_options(dhcp6)           (memset(dhcp6_rx_options, 0, sizeof(dhcp6_rx_options)))
#define dhcp6_get_option_start(dhcp6, idx)       (dhcp6_rx_options[idx].val_start)
#define dhcp6_get_option_length(dhcp6, idx)      (dhcp6_rx_options[idx].val_length)
#define dhcp6_set_option(dhcp6, idx, start, len) do { dhcp6_rx_options[idx].val_start = (start); dhcp6_rx_options[idx].val_length = (len); }while(0)


const IpAddr dhcp6_All_DHCP6_Relay_Agents_and_Servers = init_ip_addr_ip6_host(0xFF020000, 0, 0, 0x00010002);
const IpAddr dhcp6_All_DHCP6_Servers = init_ip_addr_ip6_host(0xFF020000, 0, 0, 0x00010003);

static struct UdpPcb *dhcp6_pcb;
static uint8_t dhcp6_pcb_refcount;


/* receive, unfold, parse and free incoming messages */
static void dhcp6_recv(uint8_t *arg, struct UdpPcb *pcb, struct PacketBuffer *p, const IpAddr *addr, uint16_t port, NetIfc
                       * netif);

/** Ensure DHCP PCB is allocated and bound */
static LwipStatus dhcp6_inc_pcb_refcount()
{
    if (dhcp6_pcb_refcount == 0)
    {
        lwip_assert("dhcp6_inc_pcb_refcount(): memory leak", dhcp6_pcb == nullptr);
        /* allocate UDP PCB */
        dhcp6_pcb = udp_new_ip_type(IPADDR_TYPE_V6);
        if (dhcp6_pcb == nullptr)
        {
            return ERR_MEM;
        }
        ip_set_option(&dhcp6_pcb->so_options, SOF_BROADCAST);
        /* set up local and remote port for the pcb -> listen on all interfaces on all src/dest IPs */
        auto addr_any = ip_addr_ip6_any();
        udp_bind(dhcp6_pcb, &addr_any, DHCP6_CLIENT_PORT);
        // udp_recv(dhcp6_pcb, dhcp6_recv, nullptr);
    }
    dhcp6_pcb_refcount++;
    return ERR_OK;
}

/** Free DHCP PCB if the last netif stops using it */
static void
dhcp6_dec_pcb_refcount(void)
{
  lwip_assert("dhcp6_pcb_refcount(): refcount error", (dhcp6_pcb_refcount > 0));
  dhcp6_pcb_refcount--;

  if (dhcp6_pcb_refcount == 0) {
    udp_remove(dhcp6_pcb);
    dhcp6_pcb = nullptr;
  }
}

/**
 * @ingroup dhcp6
 * Set a statically allocated Dhcp6 to work with.
 * Using this prevents dhcp6_start to allocate it using mem_malloc.
 *
 * @param netif the netif for which to set the struct dhcp
 * @param dhcp6 (uninitialised) dhcp6 struct allocated by the application
 */
void dhcp6_set_struct(NetIfc* netif, struct Dhcp6* dhcp6)
{
    lwip_assert("netif != NULL", netif != nullptr);
    lwip_assert("dhcp6 != NULL", dhcp6 != nullptr);
    lwip_assert("netif already has a Dhcp6 set",
                netif_dhcp6_data(netif) == nullptr); /* clear data structure */
    memset(dhcp6, 0, sizeof(struct Dhcp6)); /* dhcp6_set_state(&dhcp, DHCP6_STATE_OFF); */
    netif->client_data[LWIP_NETIF_CLIENT_DATA_INDEX_DHCP6] = dhcp6;
}

/**
 * @ingroup dhcp6
 * Removes a Dhcp6 from a netif.
 *
 * ATTENTION: Only use this when not using dhcp6_set_struct() to allocate the
 *            Dhcp6 since the memory is passed back to the heap.
 *
 * @param netif the netif from which to remove the struct dhcp
 */
void dhcp6_cleanup(NetIfc* netif)
{
    lwip_assert("netif != NULL", netif != nullptr);
    if (netif_dhcp6_data(netif) != nullptr)
    {
        delete netif_dhcp6_data(netif);
        netif->client_data[LWIP_NETIF_CLIENT_DATA_INDEX_DHCP6] = nullptr;
    }
}

static struct Dhcp6* dhcp6_get_struct(NetIfc* netif, const char* dbg_requester)
{
    struct Dhcp6* dhcp6 = netif_dhcp6_data(netif);
    if (dhcp6 == nullptr)
    {
        Logf(DHCP6_DEBUG | LWIP_DBG_TRACE,
             ("%s: mallocing new DHCPv6 client\n", dbg_requester));
        dhcp6 = new struct Dhcp6;
        if (dhcp6 == nullptr)
        {
            Logf(DHCP6_DEBUG | LWIP_DBG_TRACE,
                 ("%s: could not allocate dhcp6\n", dbg_requester));
            return nullptr;
        } /* clear data structure, this implies DHCP6_STATE_OFF */
        memset(dhcp6, 0, sizeof(struct Dhcp6)); /* store this dhcp6 client in the netif */
        netif->client_data[LWIP_NETIF_CLIENT_DATA_INDEX_DHCP6] = dhcp6;
    }
    else
    {
        /* already has DHCP6 client attached */
        Logf(DHCP6_DEBUG | LWIP_DBG_TRACE | LWIP_DBG_STATE,
             ("%s: using existing DHCPv6 client\n", dbg_requester));
    }
    if (!dhcp6->pcb_allocated)
    {
        if (dhcp6_inc_pcb_refcount() != ERR_OK)
        {
            /* ensure DHCP6 PCB is allocated */
            delete dhcp6;
            netif->client_data[LWIP_NETIF_CLIENT_DATA_INDEX_DHCP6] = nullptr;
            return nullptr;
        }
        Logf(DHCP6_DEBUG | LWIP_DBG_TRACE, ("%s: allocated dhcp6", dbg_requester));
        dhcp6->pcb_allocated = 1;
    }
    return dhcp6;
}

/*
 * Set the DHCPv6 state
 * If the state changed, reset the number of tries.
 */
static void
dhcp6_set_state(struct Dhcp6 *dhcp6, uint8_t new_state, const char *dbg_caller)
{
  Logf(DHCP6_DEBUG | LWIP_DBG_TRACE | LWIP_DBG_STATE, ("DHCPv6 state: %d -> %d (%s)\n",
           dhcp6->state, new_state, dbg_caller));
  if (new_state != dhcp6->state) {
    dhcp6->state = new_state;
    dhcp6->tries = 0;
    dhcp6->request_timeout = 0;
  }
}

static int
dhcp6_stateless_enabled(struct Dhcp6 *dhcp6)
{
  if ((dhcp6->state == DHCP6_STATE_STATELESS_IDLE) ||
      (dhcp6->state == DHCP6_STATE_REQUESTING_CONFIG)) {
    return 1;
  }
  return 0;
}

/*static int
dhcp6_stateful_enabled(Dhcp6 *dhcp6)
{
  if (dhcp6->state == DHCP6_STATE_OFF) {
    return 0;
  }
  if (dhcp6_stateless_enabled(dhcp6)) {
    return 0;
  }
  return 1;
}*/

/**
 * @ingroup dhcp6
 * Enable stateful DHCPv6 on this netif
 * Requests are sent on receipt of an RA message with the
 * ND6_RA_FLAG_MANAGED_ADDR_CONFIG flag set.
 *
 * A Dhcp6 will be allocated for this netif if not
 * set via @ref dhcp6_set_struct before.
 *
 * @todo: stateful DHCPv6 not supported, yet
 */
LwipStatus dhcp6_enable_stateful(NetIfc* netif)
{
    Logf(DHCP6_DEBUG | LWIP_DBG_TRACE, ("stateful dhcp6 not implemented yet"));
    return ERR_VAL;
}

/**
 * @ingroup dhcp6
 * Enable stateless DHCPv6 on this netif
 * Requests are sent on receipt of an RA message with the
 * ND6_RA_FLAG_OTHER_CONFIG flag set.
 *
 * A Dhcp6 will be allocated for this netif if not
 * set via @ref dhcp6_set_struct before.
 */
LwipStatus dhcp6_enable_stateless(NetIfc* netif)
{
    // Logf(DHCP_DEBUG | LWIP_DBG_TRACE | LWIP_DBG_STATE, ("dhcp6_enable_stateless(netif=%p) %c%c%d\n", (uint8_t *)netif, netif->name[0], netif->name[1], (uint16_t)netif->num));
    struct Dhcp6* dhcp6 = dhcp6_get_struct(netif, "dhcp6_enable_stateless()");
    if (dhcp6 == nullptr)
    {
        return ERR_MEM;
    }
    if (dhcp6_stateless_enabled(dhcp6))
    {
        Logf(DHCP_DEBUG | LWIP_DBG_TRACE,
             ("dhcp6_enable_stateless(): stateless DHCPv6 already enabled"));
        return ERR_OK;
    }
    else if (dhcp6->state != DHCP6_STATE_OFF)
    {
        /* stateful running */ /* @todo: stop stateful once it is implemented */
        Logf(DHCP_DEBUG | LWIP_DBG_TRACE,
             ("dhcp6_enable_stateless(): switching from stateful to stateless DHCPv6"));
    }
    Logf(DHCP_DEBUG | LWIP_DBG_TRACE,
         ("dhcp6_enable_stateless(): stateless DHCPv6 enabled\n"));
    dhcp6_set_state(dhcp6, DHCP6_STATE_STATELESS_IDLE, "dhcp6_enable_stateless");
    return ERR_OK;
}

/**
 * @ingroup dhcp6
 * Disable stateful or stateless DHCPv6 on this netif
 * Requests are sent on receipt of an RA message with the
 * ND6_RA_FLAG_OTHER_CONFIG flag set.
 */
void dhcp6_disable(NetIfc* netif)
{
    struct Dhcp6* dhcp6;
    // Logf(DHCP_DEBUG | LWIP_DBG_TRACE | LWIP_DBG_STATE, ("dhcp6_disable(netif=%p) %c%c%d\n", (uint8_t *)netif, netif->name[0], netif->name[1], (uint16_t)netif->num));
    dhcp6 = netif_dhcp6_data(netif);
    if (dhcp6 != nullptr)
    {
        if (dhcp6->state != DHCP6_STATE_OFF)
        {
            Logf(DHCP6_DEBUG | LWIP_DBG_TRACE,
                 ("dhcp6_disable(): DHCPv6 disabled (old state: %s)\n", (
                     dhcp6_stateless_enabled(dhcp6) ? "stateless" : "stateful")));
            dhcp6_set_state(dhcp6, DHCP6_STATE_OFF, "dhcp6_disable");
            if (dhcp6->pcb_allocated != 0)
            {
                dhcp6_dec_pcb_refcount(); /* free DHCPv6 PCB if not needed any more */
                dhcp6->pcb_allocated = 0;
            }
        }
    }
}

/**
 * Create a DHCPv6 request, fill in common headers
 *
 * @param netif the netif under DHCPv6 control
 * @param dhcp6 dhcp6 control struct
 * @param message_type message type of the request
 * @param opt_len_alloc option length to allocate
 * @param options_out_len option length on exit
 * @return a PacketBuffer for the message
 */
static struct PacketBuffer* dhcp6_create_msg(NetIfc* netif,
                                     struct Dhcp6* dhcp6,
                                     uint8_t message_type,
                                     uint16_t opt_len_alloc,
                                     uint16_t* options_out_len)
{
    if (netif == nullptr)
    {
        return nullptr;
    }
    if (dhcp6 == nullptr)
    {
        return nullptr;
    }
    struct PacketBuffer* p_out = pbuf_alloc(PBUF_TRANSPORT,
                                    sizeof(struct Dhcp6Msg) + opt_len_alloc,
                                    PBUF_RAM);
    if (p_out == nullptr)
    {
        Logf(DHCP6_DEBUG | LWIP_DBG_TRACE | LWIP_DBG_LEVEL_SERIOUS,
             ("dhcp6_create_msg(): could not allocate pbuf\n"));
        return nullptr;
    }
    lwip_assert("dhcp6_create_msg: check that first pbuf can hold struct dhcp6_msg",
                (p_out->len >= sizeof(struct Dhcp6Msg) + opt_len_alloc));
    /* @todo: limit new xid for certain message types? */
    /* reuse transaction identifier in retransmissions */
    if (dhcp6->tries == 0)
    {
        dhcp6->xid = LwipRand() & 0xFFFFFF;
    }
    // Logf(DHCP6_DEBUG | LWIP_DBG_TRACE,
    //      ("transaction id xid(%"X32_F")\n", dhcp6->xid));
    struct Dhcp6Msg* msg_out = (struct Dhcp6Msg *)p_out->payload;
    memset(msg_out, 0, sizeof(struct Dhcp6Msg) + opt_len_alloc);
    msg_out->msgtype = message_type;
    msg_out->transaction_id[0] = (uint8_t)(dhcp6->xid >> 16);
    msg_out->transaction_id[1] = (uint8_t)(dhcp6->xid >> 8);
    msg_out->transaction_id[2] = (uint8_t)dhcp6->xid;
    *options_out_len = 0;
    return p_out;
}

static uint16_t dhcp6_option_short(uint16_t options_out_len,
                                   uint8_t* options,
                                   uint16_t value)
{
    options[options_out_len++] = (uint8_t)((value & 0xff00U) >> 8);
    options[options_out_len++] = (uint8_t)(value & 0x00ffU);
    return options_out_len;
}

static uint16_t dhcp6_option_optionrequest(uint16_t options_out_len,
                                           uint8_t* options,
                                           const uint16_t* req_options,
                                           uint16_t num_req_options,
                                           uint16_t max_len)
{
    size_t i;
    uint16_t ret;
    lwip_assert(
        "dhcp6_option_optionrequest: options_out_len + sizeof(struct dhcp6_msg) + addlen <= max_len",
        sizeof(struct Dhcp6Msg) + options_out_len + 4U + (2U * num_req_options) <=
        max_len);
    ret = dhcp6_option_short(options_out_len, options, DHCP6_OPTION_ORO);
    ret = dhcp6_option_short(ret, options, 2 * num_req_options);
    for (i = 0; i < num_req_options; i++)
    {
        ret = dhcp6_option_short(ret, options, req_options[i]);
    }
    return ret;
}

/* All options are added, shrink the PacketBuffer to the required size */
static void
dhcp6_msg_finalize(uint16_t options_out_len, struct PacketBuffer *p_out)
{
  /* shrink the PacketBuffer to the actual content length */
  pbuf_realloc(p_out, (uint16_t)(sizeof(struct Dhcp6Msg) + options_out_len));
}

static void dhcp6_information_request(NetIfc* netif, Dhcp6* dhcp6)
{
    const uint16_t requested_options[] = {
        DHCP6_OPTION_DNS_SERVERS,
        DHCP6_OPTION_DOMAIN_LIST,
        DHCP6_OPTION_SNTP_SERVERS
    };
    uint16_t msecs;
    uint16_t options_out_len;
    Logf(DHCP6_DEBUG | LWIP_DBG_TRACE, ("dhcp6_information_request()\n"));
    /* create and initialize the DHCP message header */
    struct PacketBuffer* p_out = dhcp6_create_msg(netif,
                                                  dhcp6,
                                                  DHCP6_INFOREQUEST,
                                                  4 + sizeof(requested_options),
                                                  &options_out_len);
    if (p_out != nullptr)
    {
        LwipStatus err;
        struct Dhcp6Msg* msg_out = (struct Dhcp6Msg *)p_out->payload;
        uint8_t* options = (uint8_t *)(msg_out + 1);
        Logf(DHCP6_DEBUG | LWIP_DBG_TRACE,
             ("dhcp6_information_request: making request\n"));
        options_out_len = dhcp6_option_optionrequest(options_out_len,
                                                     options,
                                                     requested_options,
                                                     LWIP_ARRAYSIZE(requested_options),
                                                     p_out->len);
        LWIP_HOOK_DHCP6_APPEND_OPTIONS(netif,
                                       dhcp6,
                                       DHCP6_STATE_REQUESTING_CONFIG,
                                       msg_out,
                                       DHCP6_INFOREQUEST,
                                       options_out_len,
                                       p_out->len);
        dhcp6_msg_finalize(options_out_len, p_out);
        err = udp_sendto_if(dhcp6_pcb,
                            p_out,
                            &dhcp6_All_DHCP6_Relay_Agents_and_Servers,
                            DHCP6_SERVER_PORT,
                            netif);
        pbuf_free(p_out);
        Logf(DHCP6_DEBUG | LWIP_DBG_TRACE | LWIP_DBG_STATE,
             "dhcp6_information_request: INFOREQUESTING -> %d\n", (int)err);
    }
    else
    {
        Logf(DHCP6_DEBUG | LWIP_DBG_TRACE | LWIP_DBG_LEVEL_SERIOUS,
             ("dhcp6_information_request: could not allocate DHCP6 request\n"));
    }
    dhcp6_set_state(dhcp6, DHCP6_STATE_REQUESTING_CONFIG, "dhcp6_information_request");
    if (dhcp6->tries < 255)
    {
        dhcp6->tries++;
    }
    msecs = (uint16_t)((dhcp6->tries < 6 ? 1 << dhcp6->tries : 60) * 1000);
    dhcp6->request_timeout = (uint16_t)((msecs + DHCP6_TIMER_MSECS - 1) /
        DHCP6_TIMER_MSECS);
    Logf(DHCP6_DEBUG | LWIP_DBG_TRACE | LWIP_DBG_STATE,
         "dhcp6_information_request(): set request timeout %d msecs\n", msecs);
}

static LwipStatus
dhcp6_request_config(NetIfc*netif, Dhcp6 *dhcp6)
{
  /* stateless mode enabled and no request running? */
  if (dhcp6->state == DHCP6_STATE_STATELESS_IDLE) {
    /* send Information-request and wait for answer; setup receive timeout */
    dhcp6_information_request(netif, dhcp6);
  }

  return ERR_OK;
}

static void
dhcp6_abort_config_request(Dhcp6 *dhcp6)
{
  if (dhcp6->state == DHCP6_STATE_REQUESTING_CONFIG) {
    /* abort running request */
    dhcp6_set_state(dhcp6, DHCP6_STATE_STATELESS_IDLE, "dhcp6_abort_config_request");
  }
}

/* Handle a REPLY to INFOREQUEST
 * This parses DNS and NTP server addresses from the reply.
 */
static void dhcp6_handle_config_reply(NetIfc* netif, struct PacketBuffer* p_msg_in)
{
    Dhcp6* dhcp6 = netif_dhcp6_data(netif);;;
    if (dhcp6_option_given(dhcp6, DHCP6_OPTION_IDX_DNS_SERVER))
    {
        IpAddr dns_addr;
        Ip6Addr* dns_addr6 = nullptr;
        uint16_t op_start = dhcp6_get_option_start(dhcp6, DHCP6_OPTION_IDX_DNS_SERVER);
        uint16_t op_len = dhcp6_get_option_length(dhcp6, DHCP6_OPTION_IDX_DNS_SERVER);
        uint16_t idx;
        uint8_t n;
        memset(&dns_addr, 0, sizeof(dns_addr));
        memcpy(dns_addr6, &dns_addr.u_addr.ip6, sizeof(Ip6Addr));
        for (n = 0, idx = op_start; (idx < op_start + op_len) && (n <
                 LWIP_DHCP6_PROVIDE_DNS_SERVERS); n++, idx += sizeof(Ip6Addr))
        {
            auto copied = pbuf_copy_partial(p_msg_in,
                                                reinterpret_cast<uint8_t*>(dns_addr6),
                                                sizeof(Ip6Addr),
                                                idx);
            if (copied != sizeof(Ip6Addr))
            {
                /* PacketBuffer length mismatch */
                return;
            }
            ip6_addr_assign_zone(dns_addr6, IP6_UNKNOWN, netif);
            /* @todo: do we need a different offset than DHCP(v4)? */
            dns_setserver(n, &dns_addr);
        }
    } /* @ todo: parse and set Domain Search List */
    if (dhcp6_option_given(dhcp6, DHCP6_OPTION_IDX_NTP_SERVER))
    {
        IpAddr ntp_server_addrs[LWIP_DHCP6_MAX_NTP_SERVERS];
        uint16_t op_start = dhcp6_get_option_start(dhcp6, DHCP6_OPTION_IDX_NTP_SERVER);
        uint16_t op_len = dhcp6_get_option_length(dhcp6, DHCP6_OPTION_IDX_NTP_SERVER);
        uint16_t idx;
        uint8_t n;
        for (n = 0, idx = op_start; (idx < op_start + op_len) && (n <
                 LWIP_DHCP6_MAX_NTP_SERVERS); n++, idx += sizeof(Ip6Addr))
        {
            Ip6Addr* ntp_addr6;
            memcpy(ntp_addr6, &ntp_server_addrs[n].u_addr.ip6, sizeof(Ip6Addr));
            ip6_addr_set_zero(&ntp_server_addrs[n].u_addr.ip6);
            const auto copied = pbuf_copy_partial(p_msg_in, reinterpret_cast<uint8_t*>(ntp_addr6), sizeof(Ip6Addr), idx);
            if (copied != sizeof(Ip6Addr))
            {
                /* PacketBuffer length mismatch */
                return;
            }
            ip6_addr_assign_zone(ntp_addr6, IP6_UNKNOWN, netif);
        }
        dhcp6_set_ntp_servers(n, ntp_server_addrs);
    }
}


/** This function is called from nd6 module when an RA messsage is received
 * It triggers DHCPv6 requests (if enabled).
 */
void dhcp6_nd6_ra_trigger(NetIfc* netif,
                          uint8_t managed_addr_config,
                          uint8_t other_config)
{
    struct Dhcp6* dhcp6;
    lwip_assert("netif != NULL", netif != nullptr);
    dhcp6 = netif_dhcp6_data(netif);

  if (dhcp6 != nullptr) {
    if (dhcp6_stateless_enabled(dhcp6)) {
      if (other_config) {
        dhcp6_request_config(netif, dhcp6);
      } else {
        dhcp6_abort_config_request(dhcp6);
      }
    }
  }

}

/**
 * Parse the DHCPv6 message and extract the DHCPv6 options.
 *
 * Extract the DHCPv6 options (offset + length) so that we can later easily
 * check for them or extract the contents.
 */
static LwipStatus dhcp6_parse_reply(struct PacketBuffer* p, struct Dhcp6* dhcp6)
{
    /* clear received options */
    dhcp6_clear_all_options(dhcp6);
    struct Dhcp6Msg* msg_in = (struct Dhcp6Msg *)p->payload; /* parse options */
    uint16_t options_idx = sizeof(struct Dhcp6Msg);
    /* parse options to the end of the received packet */
    uint16_t offset_max = p->tot_len;
    uint16_t offset = options_idx; /* at least 4 byte to read? */
    while ((offset + 4 <= offset_max))
    {
        uint8_t op_len_buf[4];
        auto val_offset = (uint16_t)(offset + 4);
        if (val_offset < offset)
        {
            /* overflow */
            return ERR_BUF;
        } /* copy option + length, might be split accross pbufs */
        uint8_t* op_len = (uint8_t *)pbuf_get_contiguous(p, op_len_buf, 4, 4, offset);
        if (op_len == nullptr)
        {
            /* failed to get option and length */
            return ERR_VAL;
        }
        uint16_t op = (op_len[0] << 8) | op_len[1];
        uint16_t len = (op_len[2] << 8) | op_len[3];
        offset = val_offset + len;
        if (offset < val_offset)
        {
            /* overflow */
            return ERR_BUF;
        }
        switch (op)
        {
        case (DHCP6_OPTION_CLIENTID): dhcp6_got_option(dhcp6, DHCP6_OPTION_IDX_CLI_ID);
            dhcp6_set_option(dhcp6, DHCP6_OPTION_IDX_CLI_ID, val_offset, len);
            break;
        case (DHCP6_OPTION_SERVERID): dhcp6_got_option(dhcp6, DHCP6_OPTION_IDX_SERVER_ID);
            dhcp6_set_option(dhcp6, DHCP6_OPTION_IDX_SERVER_ID, val_offset, len);
            break;

      case (DHCP6_OPTION_DNS_SERVERS):
        dhcp6_got_option(dhcp6, DHCP6_OPTION_IDX_DNS_SERVER);
        dhcp6_set_option(dhcp6, DHCP6_OPTION_IDX_DNS_SERVER, val_offset, len);
        break;
      case (DHCP6_OPTION_DOMAIN_LIST):
        dhcp6_got_option(dhcp6, DHCP6_OPTION_IDX_DOMAIN_LIST);
        dhcp6_set_option(dhcp6, DHCP6_OPTION_IDX_DOMAIN_LIST, val_offset, len);
        break;


      case (DHCP6_OPTION_SNTP_SERVERS):
        dhcp6_got_option(dhcp6, DHCP6_OPTION_IDX_NTP_SERVER);
        dhcp6_set_option(dhcp6, DHCP6_OPTION_IDX_NTP_SERVER, val_offset, len);
        break;

        default:
            // Logf(DHCP6_DEBUG, ("skipping option %d in options\n", op));
            // LWIP_HOOK_DHCP6_PARSE_OPTION(ip_current_netif(),
            //                              dhcp6,
            //                              dhcp6->state,
            //                              msg_in,
            //                              msg_in->msgtype,
            //                              op,
            //                              len,
            //                              q,
            //                              val_offset);
            break;
        }
    }
    return ERR_OK;
}

static void dhcp6_recv(void* arg,
                       struct UdpPcb* pcb,
                       struct PacketBuffer* p,
                       const IpAddr* addr,
                       uint16_t port,
                       NetIfc* netif)
{
    struct Dhcp6* dhcp6 = netif_dhcp6_data(netif);
    auto reply_msg = reinterpret_cast<Dhcp6Msg *>(p->payload); /* Caught DHCPv6 message from netif that does not have DHCPv6 enabled? -> not interested */
    if ((dhcp6 == nullptr) || (dhcp6->pcb_allocated == 0))
    {
        goto free_pbuf_and_return;
    } // LWIP_ERROR("invalid server address type", IpIsV6(addr), goto free_pbuf_and_return;);
    if (!is_ipaddr_v6(addr))
    {
        printf("invalid server address type\n");
        goto free_pbuf_and_return;
    }
    // Logf(DHCP6_DEBUG | LWIP_DBG_TRACE,
    //      ("dhcp6_recv(PacketBuffer = %p) from DHCPv6 server %s port %d\n", (uint8_t *)p
    //          , ipaddr_ntoa(addr), port));
    // Logf(DHCP6_DEBUG | LWIP_DBG_TRACE, ("PacketBuffer->len = %d\n", p->len));
    // Logf(DHCP6_DEBUG | LWIP_DBG_TRACE,
    //      ("PacketBuffer->tot_len = %d\n", p->tot_len));
    // /* prevent warnings about unused arguments */
    // ;
    // ;
    // ;
    if (p->len < sizeof(struct Dhcp6Msg))
    {
        Logf(DHCP6_DEBUG | LWIP_DBG_TRACE | LWIP_DBG_LEVEL_WARNING,
             ("DHCPv6 reply message or PacketBuffer too short\n"));
        goto free_pbuf_and_return;
    } /* match transaction ID against what we expected */
    uint32_t xid = reply_msg->transaction_id[0] << 16;
    xid |= reply_msg->transaction_id[1] << 8;
    xid |= reply_msg->transaction_id[2];
    if (xid != dhcp6->xid)
    {
        // Logf(DHCP6_DEBUG | LWIP_DBG_TRACE | LWIP_DBG_LEVEL_WARNING,
        //      ("transaction id mismatch reply_msg->xid(%"X32_F")!= dhcp6->xid(%"X32_F")\n",
        //          xid, dhcp6->xid));
        goto free_pbuf_and_return;
    } /* option fields could be unfold? */
    if (dhcp6_parse_reply(p, dhcp6) != ERR_OK)
    {
        Logf(DHCP6_DEBUG | LWIP_DBG_TRACE | LWIP_DBG_LEVEL_SERIOUS,
             ("problem unfolding DHCPv6 message - too short on memory?\n"));
        goto free_pbuf_and_return;
    } /* read DHCP message type */
    uint8_t msg_type = reply_msg->msgtype; /* message type is DHCP6 REPLY? */
    if (msg_type == DHCP6_REPLY)
    {
        Logf(DHCP6_DEBUG | LWIP_DBG_TRACE, ("DHCP6_REPLY received\n"));

    /* in info-requesting state? */
    if (dhcp6->state == DHCP6_STATE_REQUESTING_CONFIG) {
      dhcp6_set_state(dhcp6, DHCP6_STATE_STATELESS_IDLE, "dhcp6_recv");
      dhcp6_handle_config_reply(netif, p);
    } else

        {
            /* @todo: handle reply in other states? */
        }
    }
    else
    {
        /* @todo: handle other message types */
    }
free_pbuf_and_return: pbuf_free(p);
}

/**
 * A DHCPv6 request has timed out.
 *
 * The timer that was started with the DHCPv6 request has
 * timed out, indicating no response was received in time.
 */
static void
dhcp6_timeout(NetIfc*netif, struct Dhcp6 *dhcp6)
{
  Logf(DHCP_DEBUG | LWIP_DBG_TRACE, ("dhcp6_timeout()\n"));

  // ;
  // ;


  /* back-off period has passed, or server selection timed out */
  if (dhcp6->state == DHCP6_STATE_REQUESTING_CONFIG) {
    Logf(DHCP6_DEBUG | LWIP_DBG_TRACE, ("dhcp6_timeout(): retrying information request\n"));
    dhcp6_information_request(netif, dhcp6);
  }

}

/**
 * DHCPv6 timeout handling (this function must be called every 500ms,
 * see @ref DHCP6_TIMER_MSECS).
 *
 * A DHCPv6 server is expected to respond within a short period of time.
 * This timer checks whether an outstanding DHCPv6 request is timed out.
 */
void
dhcp6_tmr(void)
{
  NetIfc*netif;
  /* loop through netif's */
  NETIF_FOREACH(netif) {
    struct Dhcp6 *dhcp6 = netif_dhcp6_data(netif);
    /* only act on DHCPv6 configured interfaces */
    if (dhcp6 != nullptr) {
      /* timer is active (non zero), and is about to trigger now */
      if (dhcp6->request_timeout > 1) {
        dhcp6->request_timeout--;
      } else if (dhcp6->request_timeout == 1) {
        dhcp6->request_timeout--;
        /* { dhcp6->request_timeout == 0 } */
        Logf(DHCP6_DEBUG | LWIP_DBG_TRACE | LWIP_DBG_STATE, ("dhcp6_tmr(): request timeout\n"));
        /* this client's request timeout triggered */
        dhcp6_timeout(netif, dhcp6);
      }
    }
  }
}

