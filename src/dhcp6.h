/**
 * @file
 *
 * DHCPv6 client: IPv6 address autoconfiguration as per
 * RFC 3315 (stateful DHCPv6) and
 * RFC 3736 (stateless DHCPv6).
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
#pragma once

#include <opt.h>
#include <lwip_status.h>
#include <netif.h>
#include <cstdint>

constexpr auto DHCP6_CLIENT_PORT = 546;
constexpr auto DHCP6_SERVER_PORT = 547;


 /* DHCPv6 message item offsets and length */
constexpr auto DHCP6_TRANSACTION_ID_LEN = 3;

/** minimum set of fields of any DHCPv6 message */
struct Dhcp6Msg
{
    uint8_t msgtype;
    uint8_t transaction_id[DHCP6_TRANSACTION_ID_LEN]; /* options follow */
};



/* DHCP6 client states */
 enum Dhcp6States{
    DHCP6_STATE_OFF = 0,
    DHCP6_STATE_STATELESS_IDLE = 1,
    DHCP6_STATE_REQUESTING_CONFIG = 2
} ;

/* DHCPv6 message types */
#define DHCP6_SOLICIT               1
#define DHCP6_ADVERTISE             2
#define DHCP6_REQUEST               3
#define DHCP6_CONFIRM               4
#define DHCP6_RENEW                 5
#define DHCP6_REBIND                6
#define DHCP6_REPLY                 7
#define DHCP6_RELEASE               8
#define DHCP6_DECLINE               9
#define DHCP6_RECONFIGURE           10
#define DHCP6_INFOREQUEST           11
#define DHCP6_RELAYFORW             12
#define DHCP6_RELAYREPL             13
/* More message types see https://www.iana.org/assignments/dhcpv6-parameters/dhcpv6-parameters.xhtml */

/** DHCPv6 status codes */
#define DHCP6_STATUS_SUCCESS        0 /* Success. */
#define DHCP6_STATUS_UNSPECFAIL     1 /* Failure, reason unspecified; this status code is sent by either a client or a server to indicate a failure not explicitly specified in this document. */
#define DHCP6_STATUS_NOADDRSAVAIL   2 /* Server has no addresses available to assign to the IA(s). */
#define DHCP6_STATUS_NOBINDING      3 /* Client record (binding) unavailable. */
#define DHCP6_STATUS_NOTONLINK      4 /* The prefix for the address is not appropriate for the link to which the client is attached. */
#define DHCP6_STATUS_USEMULTICAST   5 /* Sent by a server to a client to force the client to send messages to the server using the All_DHCP_Relay_Agents_and_Servers address. */
/* More status codes see https://www.iana.org/assignments/dhcpv6-parameters/dhcpv6-parameters.xhtml */

/** DHCPv6 DUID types */
#define DHCP6_DUID_LLT              1 /* LLT: Link-layer Address Plus Time */
#define DHCP6_DUID_EN               2 /* EN: Enterprise number */
#define DHCP6_DUID_LL               3 /* LL: Link-layer Address */
#define DHCP6_DUID_UUID             4 /* UUID (RFC 6355) */

/* DHCPv6 options */
#define DHCP6_OPTION_CLIENTID       1
#define DHCP6_OPTION_SERVERID       2
#define DHCP6_OPTION_IA_NA          3
#define DHCP6_OPTION_IA_TA          4
#define DHCP6_OPTION_IAADDR         5
#define DHCP6_OPTION_ORO            6
#define DHCP6_OPTION_PREFERENCE     7
#define DHCP6_OPTION_ELAPSED_TIME   8
#define DHCP6_OPTION_RELAY_MSG      9
#define DHCP6_OPTION_AUTH           11
#define DHCP6_OPTION_UNICAST        12
#define DHCP6_OPTION_STATUS_CODE    13
#define DHCP6_OPTION_RAPID_COMMIT   14
#define DHCP6_OPTION_USER_CLASS     15
#define DHCP6_OPTION_VENDOR_CLASS   16
#define DHCP6_OPTION_VENDOR_OPTS    17
#define DHCP6_OPTION_INTERFACE_ID   18
#define DHCP6_OPTION_RECONF_MSG     19
#define DHCP6_OPTION_RECONF_ACCEPT  20
/* More options see https://www.iana.org/assignments/dhcpv6-parameters/dhcpv6-parameters.xhtml */
#define DHCP6_OPTION_DNS_SERVERS    23 /* RFC 3646 */
#define DHCP6_OPTION_DOMAIN_LIST    24 /* RFC 3646 */
#define DHCP6_OPTION_SNTP_SERVERS   31 /* RFC 4075 */


/** period (in milliseconds) of the application calling dhcp6_tmr() */
constexpr auto DHCP6_TIMER_MSECS = 500;

struct Dhcp6
{
    /** transaction identifier of last sent request */
    uint32_t xid; /** track PCB allocation state */
    uint8_t pcb_allocated; /** current DHCPv6 state machine state */
    uint8_t state; /** retries of current request */
    uint8_t tries;
    /** if request config is triggered while another action is active, this keeps track of it */
    uint8_t request_config_pending;
    /** #ticks with period DHCP6_TIMER_MSECS for request timeout */
    uint16_t request_timeout;
    /* @todo: add more members here to keep track of stateful DHCPv6 data, like lease times */
};

void dhcp6_set_struct(NetIfc*netif, struct Dhcp6 *dhcp6);
/** Remove a Dhcp6 previously set to the netif using dhcp6_set_struct() */
#define dhcp6_remove_struct(netif) netif_set_client_data(netif, LWIP_NETIF_CLIENT_DATA_INDEX_DHCP6, NULL)
void dhcp6_cleanup(NetIfc*netif);

LwipStatus dhcp6_enable_stateful(NetIfc*netif);
LwipStatus dhcp6_enable_stateless(NetIfc*netif);
void dhcp6_disable(NetIfc*netif);

void dhcp6_tmr(void);

void dhcp6_nd6_ra_trigger(NetIfc*netif, uint8_t managed_addr_config, uint8_t other_config);

/** This function must exist, in other to add offered NTP servers to
 * the NTP (or SNTP) engine.
 * See LWIP_DHCP6_MAX_NTP_SERVERS */
extern void dhcp6_set_ntp_servers(uint8_t num_ntp_servers, const IpAddr* ntp_server_addrs);


inline Dhcp6* netif_dhcp6_data(NetIfc* netif)
{
    return static_cast<struct Dhcp6 *>(netif_get_client_data(
        netif,
        LWIP_NETIF_CLIENT_DATA_INDEX_DHCP6));
}
