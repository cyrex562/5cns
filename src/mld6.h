/**
 * @file
 *
 * Multicast listener discovery for IPv6. Aims to be compliant with RFC 2710.
 * No support for MLDv2.
 */

/*
 * Copyright (c) 2010 Inico Technologies Ltd.
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
 * Author: Ivan Delamer <delamer@inicotech.com>
 *
 *
 * Please coordinate changes and requests with Ivan Delamer
 * <delamer@inicotech.com>
 */
#pragma once
#include <opt.h>


#define MLD6_HBH_HLEN 8
 /** Multicast listener report/query/done message header. */


struct mld_header {
    uint8_t type;
    uint8_t code;
    uint16_t chksum;
    uint16_t max_resp_delay;
   uint16_t reserved;
    Ip6Addr multicast_address;
    /* Options follow. */
} ;

#include <packet_buffer.h>
#include <netif.h>

/** MLD group */
struct mld_group {
  /** next link */
  struct mld_group *next;
  /** multicast address */
  Ip6Addr         group_address;
  /** signifies we were the last person to report */
  uint8_t               last_reporter_flag;
  /** current state of the group */
  uint8_t               group_state;
  /** timer for reporting */
  uint16_t              timer;
  /** counter of simultaneous uses */
  uint8_t               use;
};

#define MLD6_TMR_INTERVAL              100 /* Milliseconds */

LwipStatus  mld6_stop(NetIfc*netif);
void   mld6_report_groups(NetIfc*netif);
void   mld6_tmr(void);
struct mld_group *mld6_lookfor_group(NetIfc*ifp, const Ip6Addr *addr);
void   mld6_input(struct PacketBuffer *pkt_buf, NetIfc*in_netif);
LwipStatus  mld6_joingroup(const Ip6Addr *srcaddr, const Ip6Addr *groupaddr);
LwipStatus  mld6_joingroup_netif(NetIfc*netif, const Ip6Addr *groupaddr);
LwipStatus  mld6_leavegroup(const Ip6Addr *srcaddr, const Ip6Addr *groupaddr);
LwipStatus  mld6_leavegroup_netif(NetIfc*netif, const Ip6Addr *groupaddr);

/** @ingroup mld6
 * Get list head of MLD6 groups for netif.
 * Note: The allnodes group IP is NOT in the list, since it must always 
 * be received for correct IPv6 operation.
 * @see @ref netif_set_mld_mac_filter()
 */
#define netif_mld6_data(netif) ((struct mld_group *)netif_get_client_data(netif, LWIP_NETIF_CLIENT_DATA_INDEX_MLD6))

