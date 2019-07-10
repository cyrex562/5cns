/**
 * @file
 * raw API (to be used from TCPIP thread)\n
 * See also @ref raw_raw
 */

/*
 * Copyright (c) 2001-2004 Swedish Institute of Computer Science.
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
 * Author: Adam Dunkels <adam@sics.se>
 *
 */
#pragma once

#include "opt.h"

#include "packet_buffer.h"
#include "def.h"
#include "ip.h"
#include "ip_addr.h"
#include "ip6_addr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RAW_FLAGS_CONNECTED      0x01U
#define RAW_FLAGS_HDRINCL        0x02U
#define RAW_FLAGS_MULTICAST_LOOP 0x04U

struct raw_pcb;

/** Function prototype for raw pcb receive callback functions.
 * @param arg user supplied argument (raw_pcb.recv_arg)
 * @param pcb the raw_pcb which received data
 * @param p the packet buffer that was received
 * @param addr the remote IP address from which the packet was received
 * @return 1 if the packet was 'eaten' (aka. deleted),
 *         0 if the packet lives on
 * If returning 1, the callback is responsible for freeing the PacketBuffer
 * if it's not used any more.
 */
typedef uint8_t (*raw_recv_fn)(void *arg, struct raw_pcb *pcb, struct PacketBuffer *p,
    const IpAddr *addr);

/** the RAW protocol control block */
struct raw_pcb {
  /* Common members of all PCB types */
    IpAddr local_ip;
    /* Bound netif index */
    uint8_t netif_idx;
    /* Socket options */
    uint8_t so_options;
    /* Type Of Service */
    uint8_t tos;
    /* Time To Live */
    uint8_t ttl;
    struct netif_hint netif_hints;;

  struct raw_pcb *next;

  uint8_t protocol;
  uint8_t flags;


  /** outgoing network interface for multicast packets, by interface index (if nonzero) */
  uint8_t mcast_ifindex;
  /** TTL for outgoing multicast packets */
  uint8_t mcast_ttl;


  /** receive callback function */
  raw_recv_fn recv;
  /* user-supplied argument for the recv callback */
  void *recv_arg;

  /* fields for handling checksum computations as per RFC3542. */
  uint16_t chksum_offset;
  uint8_t  chksum_reqd;

};

/* The following functions is the application layer interface to the
   RAW code. */
struct raw_pcb * raw_new        (uint8_t proto);
struct raw_pcb * raw_new_ip_type(uint8_t type, uint8_t proto);
void             raw_remove     (struct raw_pcb *pcb);
LwipError            raw_bind       (struct raw_pcb *pcb, const IpAddr *ipaddr);
void             raw_bind_netif (struct raw_pcb *pcb, const struct NetIfc *netif);
LwipError            raw_connect    (struct raw_pcb *pcb, const IpAddr *ipaddr);
void             raw_disconnect (struct raw_pcb *pcb);

LwipError            raw_sendto     (struct raw_pcb *pcb, struct PacketBuffer *p, const IpAddr *ipaddr);
LwipError            raw_sendto_if_src(struct raw_pcb *pcb, struct PacketBuffer *p, const IpAddr *dst_ip, struct NetIfc *netif, const IpAddr *src_ip);
LwipError            raw_send       (struct raw_pcb *pcb, struct PacketBuffer *p);

void             raw_recv       (struct raw_pcb *pcb, raw_recv_fn recv, void *recv_arg);

#define          raw_flags(pcb) ((pcb)->flags)
#define          raw_setflags(pcb,f)  ((pcb)->flags = (f))

#define          raw_set_flags(pcb, set_flags)     do { (pcb)->flags = (uint8_t)((pcb)->flags |  (set_flags)); } while(0)
#define          raw_clear_flags(pcb, clr_flags)   do { (pcb)->flags = (uint8_t)((pcb)->flags & (uint8_t)(~(clr_flags) & 0xff)); } while(0)
#define          raw_is_flag_set(pcb, flag)        (((pcb)->flags & (flag)) != 0)

#define raw_init() /* Compatibility define, no init needed. */

/* for compatibility with older implementation */
#define raw_new_ip6(proto) raw_new_ip_type(IPADDR_TYPE_V6, proto)

#define raw_set_multicast_netif_index(pcb, idx) ((pcb)->mcast_ifindex = (idx))
#define raw_get_multicast_netif_index(pcb)      ((pcb)->mcast_ifindex)
#define raw_set_multicast_ttl(pcb, value)       ((pcb)->mcast_ttl = (value))
#define raw_get_multicast_ttl(pcb)              ((pcb)->mcast_ttl)


#ifdef __cplusplus
}
#endif

