/**
 * @file
 * User Datagram Protocol module\n
 * The code for the User Datagram Protocol UDP & UDPLite (RFC 3828).\n
 * See also @ref udp_raw
 *
 * @defgroup udp_raw UDP
 * @ingroup callbackstyle_api
 * User Datagram Protocol module\n
 * @see @ref api
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

/* @todo Check the use of '(UdpPcb).chksum_len_rx'!
 */
#include <udp.h>
#include <def.h>
#include <icmp.h>
#include <icmp6.h>
#include <inet_chksum.h>
#include <ip6.h>
#include <ip6_addr.h>
#include <ip_addr.h>
#include <lwip_debug.h>

#include <netif.h>
#include <opt.h>
#include <stats.h>
#include <cstring>

/* From http://www.iana.org/assignments/port-numbers:
   "The Dynamic and/or Private Ports are those from 49152 through 65535" */
#define UDP_LOCAL_PORT_RANGE_START  0xc000
#define UDP_LOCAL_PORT_RANGE_END    0xffff
#define UDP_ENSURE_LOCAL_PORT_RANGE(port) ((uint16_t)(((port) & (uint16_t)~UDP_LOCAL_PORT_RANGE_START) + UDP_LOCAL_PORT_RANGE_START))

/* last local UDP port */
static uint16_t udp_port = UDP_LOCAL_PORT_RANGE_START;

/* The list of UDP PCBs */
/* exported in udp.h (was static) */
struct UdpPcb *udp_pcbs;

/**
 * Initialize this module.
 */
void udp_init(void)
{
    udp_port = UDP_ENSURE_LOCAL_PORT_RANGE(lwip_rand());
}

//
// Allocate a new local UDP port.
//
// @return a new (free) local UDP port number
//
// todo: rewrite 
static uint16_t udp_new_port(void)
{
    uint16_t n = 0;
again: if (udp_port++ == UDP_LOCAL_PORT_RANGE_END)
    {
        udp_port = UDP_LOCAL_PORT_RANGE_START;
    } 
    
    // Check all PCBs.
    for (struct UdpPcb* pcb = udp_pcbs; pcb != nullptr; pcb = pcb->next)
    {
        if (pcb->local_port == udp_port)
        {
            if (++n > (UDP_LOCAL_PORT_RANGE_END - UDP_LOCAL_PORT_RANGE_START))
            {
                return 0;
            }
            goto again;
        }
    }
    return udp_port;
}

/** Common code to see if the current input packet matches the pcb
 * (current input packet is accessed via ip(4/6)_current_* macros)
 *
 * @param pcb pcb to check
 * @param inp network interface on which the datagram was received (only used for IPv4)
 * @param broadcast 1 if his is an IPv4 broadcast (global or subnet-only), 0 otherwise (only used for IPv4)
 * @return 1 on match, 0 otherwise
 */
static uint8_t udp_input_local_match(struct UdpPcb* pcb,
                                     NetIfc** inp,
                                     const uint8_t broadcast)
{
    lwip_assert("udp_input_local_match: invalid pcb", pcb != nullptr);
    lwip_assert("udp_input_local_match: invalid netif", inp != nullptr);
    
    // check if PCB is bound to specific netif 
    if ((pcb->netif_idx != NETIF_NO_INDEX) && (pcb->netif_idx != netif_get_index(
        ip_data.current_input_netif)))
    {
        return 0;
    } 
    
    // Dual-stack: PCBs listening to any IP type also listen to any IP address
    if (is_ip_addr_any(&pcb->local_ip))
    {
        if ((broadcast != 0) && !ip_get_option(pcb, SOF_BROADCAST))
        {
            return 0;
        }
        return 1;
    } 
    
    // Only need to check PCB if incoming IP version matches PCB IP version
    if (match_exact_ip_addr_pcb_vers(pcb, ip_current_dest_addr()))
    {
        /* Special case: IPv4 broadcast: all or broadcasts in my subnet
         * Note: broadcast variable can only be 1 if it is an IPv4 broadcast */
        if (broadcast != 0)
        {
            if (ip_get_option(pcb, SOF_BROADCAST))
            {
                if (ip4_addr_isany(convert_ip_addr_to_ip4_addr(&pcb->local_ip)) || ((ip4_current_dest_addr()
                    ->addr == IP4_ADDR_BCAST)) || ip4_addr_netcmp(
                    convert_ip_addr_to_ip4_addr(&pcb->local_ip),
                    ip4_current_dest_addr(),
                    netif_ip4_netmask(inp)))
                {
                    return 1;
                }
            }
        }
        else /* Handle IPv4 and IPv6: all or exact match */
            if (is_ip_addr_any(&pcb->local_ip) || compare_ip_addr(
                &pcb->local_ip,
                ip_current_dest_addr()))
            {
                return 1;
            }
    }
    return 0;
}

/**
 * Process an incoming UDP datagram.
 *
 * Given an incoming UDP datagram (as a chain of pbufs) this function
 * finds a corresponding UDP PCB and hands over the PacketBuffer to the pcbs
 * recv function. If no pcb is found or the datagram is incorrect, the
 * PacketBuffer is freed.
 *
 * @param p PacketBuffer to be demultiplexed to a UDP PCB (p->payload pointing to the UDP header)
 * @param inp network interface on which the datagram was received.
 *
 */
void
udp_input(struct PacketBuffer *p, NetIfc*inp)
{
  struct UdpHdr *udphdr;
  struct UdpPcb *pcb, *prev;
  struct UdpPcb *uncon_pcb;
  uint16_t src, dest;
  uint8_t broadcast;
  uint8_t for_us = 0;

  ;

 

  lwip_assert("udp_input: invalid pbuf", p != nullptr);
  lwip_assert("udp_input: invalid netif", inp != nullptr);

  PERF_START;

  UDP_STATS_INC(udp.recv);

  /* Check minimum length (UDP header) */
  if (p->len < UDP_HLEN) {
    /* drop short packets */
//    Logf(UDP_DEBUG,
//                ("udp_input: short UDP datagram (%d bytes) discarded\n", p->tot_len));
    UDP_STATS_INC(udp.lenerr);
    UDP_STATS_INC(udp.drop);
    
    free_pkt_buf(p);
    goto end;
  }

  udphdr = (struct UdpHdr *)p->payload;

  /* is broadcast packet ? */
  broadcast = ip_addr_isbroadcast(ip_current_dest_addr(), ip_current_netif());

  Logf(UDP_DEBUG, ("udp_input: received datagram of length %d\n", p->tot_len));

  /* convert src and dest ports to host byte order */
  src = lwip_ntohs(udphdr->src);
  dest = lwip_ntohs(udphdr->dest);

  udp_debug_print(udphdr);

  /* print the UDP source and destination */
  Logf(UDP_DEBUG, ("udp ("));
  ip_addr_debug_print_val(UDP_DEBUG, *ip_current_dest_addr());
  Logf(UDP_DEBUG, (", %d) <-- (", lwip_ntohs(udphdr->dest)));
  ip_addr_debug_print_val(UDP_DEBUG, *ip_current_src_addr());
  Logf(UDP_DEBUG, (", %d)\n", lwip_ntohs(udphdr->src)));

  pcb = nullptr;
  prev = nullptr;
  uncon_pcb = nullptr;
  /* Iterate through the UDP pcb list for a matching pcb.
   * 'Perfect match' pcbs (connected to the remote port & ip address) are
   * preferred. If no perfect match is found, the first unconnected pcb that
   * matches the local port and ip address gets the datagram. */
  for (pcb = udp_pcbs; pcb != nullptr; pcb = pcb->next) {
    /* print the PCB local and remote address */
    Logf(UDP_DEBUG, ("pcb ("));
    ip_addr_debug_print_val(UDP_DEBUG, pcb->local_ip);
    Logf(UDP_DEBUG, (", %d) <-- (", pcb->local_port));
    ip_addr_debug_print_val(UDP_DEBUG, pcb->remote_ip);
    Logf(UDP_DEBUG, (", %d)\n", pcb->remote_port));

    /* compare PCB local addr+port to UDP destination addr+port */
    if ((pcb->local_port == dest) &&
        (udp_input_local_match(pcb, inp, broadcast) != 0)) {
      if ((pcb->flags & UDP_FLAGS_CONNECTED) == 0) {
        if (uncon_pcb == nullptr) {
          /* the first unconnected matching PCB */
          uncon_pcb = pcb;

        } else if (broadcast && ip4_current_dest_addr()->addr == IP4_ADDR_BCAST) {
          /* global broadcast address (only valid for IPv4; match was checked before) */
          if (!IP_IS_V4_VAL(uncon_pcb->local_ip) || !ip4_addr_cmp(convert_ip_addr_to_ip4_addr(&uncon_pcb->local_ip), get_net_ifc_ip4_addr(inp))) {
            /* uncon_pcb does not match the input netif, check this pcb */
            if (IP_IS_V4_VAL(pcb->local_ip) && ip4_addr_cmp(convert_ip_addr_to_ip4_addr(&pcb->local_ip), get_net_ifc_ip4_addr(inp))) {
              /* better match */
              uncon_pcb = pcb;
            }
          }

        }

        else if (!ip_addr_isany(&pcb->local_ip)) {
          /* prefer specific IPs over catch-all */
          uncon_pcb = pcb;
        }

      }

      /* compare PCB remote addr+port to UDP source addr+port */
      if ((pcb->remote_port == src) &&
          (ip_addr_isany_val(pcb->remote_ip) ||
           compare_ip_addr(&pcb->remote_ip, ip_current_src_addr()))) {
        /* the first fully matching PCB */
        if (prev != nullptr) {
          /* move the pcb to the front of udp_pcbs so that is
             found faster next time */
          prev->next = pcb->next;
          pcb->next = udp_pcbs;
          udp_pcbs = pcb;
        } else {
          UDP_STATS_INC(udp.cachehit);
        }
        break;
      }
    }

    prev = pcb;
  }
  /* no fully matching pcb found? then look for an unconnected pcb */
  if (pcb == nullptr) {
    pcb = uncon_pcb;
  }

  /* Check checksum if this is a match or if it was directed at us. */
  if (pcb != nullptr) {
    for_us = 1;
  } else {

    if (ip_current_is_v6()) {
      for_us = netif_get_ip6_addr_match(inp, ip6_current_dest_addr()) >= 0;
    }

    if (!ip_current_is_v6()) {
      for_us = ip4_addr_cmp(get_net_ifc_ip4_addr(inp), ip4_current_dest_addr());
    }

  }

  if (for_us) {
    Logf(UDP_DEBUG | LWIP_DBG_TRACE, ("udp_input: calculating checksum\n"));

    is_netif_checksum_enabled(inp, NETIF_CHECKSUM_CHECK_UDP) {

      if (ip_current_header_proto() == IP_PROTO_UDPLITE) {
        /* Do the UDP Lite checksum */
        uint16_t chklen = lwip_ntohs(udphdr->len);
        if (chklen < sizeof(struct udp_hdr)) {
          if (chklen == 0) {
            /* For UDP-Lite, checksum length of 0 means checksum
               over the complete packet (See RFC 3828 chap. 3.1) */
            chklen = p->tot_len;
          } else {
            /* At least the UDP-Lite header must be covered by the
               checksum! (Again, see RFC 3828 chap. 3.1) */
            goto chkerr;
          }
        }
        if (ip_chksum_pseudo_partial(p, IP_PROTO_UDPLITE,
                                     p->tot_len, chklen,
                                     ip_current_src_addr(), ip_current_dest_addr()) != 0) {
          goto chkerr;
        }
      } else

      {
        if (udphdr->chksum != 0) {
          if (ip_chksum_pseudo(p, IP_PROTO_UDP, p->tot_len,
                               ip_current_src_addr(),
                               ip_current_dest_addr()) != 0) {
            goto chkerr;
          }
        }
      }
    }

    if (pbuf_remove_header(p, UDP_HLEN)) {
      /* Can we cope with this failing? Just assert for now */
      lwip_assert("pbuf_remove_header failed\n", 0);
      UDP_STATS_INC(udp.drop);
      
      free_pkt_buf(p);
      goto end;
    }

    if (pcb != nullptr) {

      if (ip_get_option(pcb, SOF_REUSEADDR) &&
          (broadcast || ip_addr_ismulticast(ip_current_dest_addr()))) {
        /* pass broadcast- or multicast packets to all multicast pcbs
           if SOF_REUSEADDR is set on the first match */
        UdpPcb *mpcb;
        for (mpcb = udp_pcbs; mpcb != nullptr; mpcb = mpcb->next) {
          if (mpcb != pcb) {
            /* compare PCB local addr+port to UDP destination addr+port */
            if ((mpcb->local_port == dest) &&
                (udp_input_local_match(mpcb, inp, broadcast) != 0)) {
              /* pass a copy of the packet to all local matches */
              if (mpcb->recv != nullptr) {
                struct PacketBuffer *q;
                q = pbuf_clone(PBUF_RAW, PBUF_POOL, p);
                if (q != nullptr) {
                  mpcb->recv(mpcb->recv_arg, mpcb, q, ip_current_src_addr(), src);
                }
              }
            }
          }
        }
      }

      /* callback */
      if (pcb->recv != nullptr) {
        /* now the recv function is responsible for freeing p */
        pcb->recv(pcb->recv_arg, pcb, p, ip_current_src_addr(), src);
      } else {
        /* no recv function registered? then we have to free the PacketBuffer! */
        free_pkt_buf(p);
        goto end;
      }
    } else {
      Logf(UDP_DEBUG | LWIP_DBG_TRACE, ("udp_input: not for us.\n"));


      /* No match was found, send ICMP destination port unreachable unless
         destination address was broadcast/multicast. */
      if (!broadcast && !ip_addr_ismulticast(ip_current_dest_addr())) {
        /* move payload pointer back to ip header */
        pbuf_header_force(p, (int16_t)(ip_current_header_tot_len() + UDP_HLEN));
        icmp_port_unreach(ip_current_is_v6(), p);
      }

      UDP_STATS_INC(udp.proterr);
      UDP_STATS_INC(udp.drop);
      
      free_pkt_buf(p);
    }
  } else {
    free_pkt_buf(p);
  }
end:
  PERF_STOP("udp_input");
  return;

chkerr:
  Logf(UDP_DEBUG | LWIP_DBG_LEVEL_SERIOUS,
       ("udp_input: UDP (or UDP Lite) datagram discarded due to failing checksum\n"));
  UDP_STATS_INC(udp.chkerr);
  UDP_STATS_INC(udp.drop);
  
  free_pkt_buf(p);
  PERF_STOP("udp_input");

}

/**
 * @ingroup udp_raw
 * Sends the PacketBuffer p using UDP. The PacketBuffer is not deallocated.
 *
 *
 * @param pcb UDP PCB used to send the data.
 * @param p chain of PacketBuffer's to be sent.
 *
 * The datagram will be sent to the current remote_ip & remote_port
 * stored in pcb. If the pcb is not bound to a port, it will
 * automatically be bound to a random port.
 *
 * @return lwIP error code.
 * - ERR_OK. Successful. No error occurred.
 * - ERR_MEM. Out of memory.
 * - ERR_RTE. Could not find route to destination address.
 * - ERR_VAL. No PCB or PCB is dual-stack
 * - More errors could be returned by lower protocol layers.
 *
 * @see udp_disconnect() udp_sendto()
 */
LwipStatus
udp_send(struct UdpPcb *pcb, struct PacketBuffer *p)
{
  
  

  if (IP_IS_ANY_TYPE_VAL(pcb->remote_ip)) {
    return ERR_VAL;
  }

  /* send to the packet using remote ip and port stored in the pcb */
  return udp_sendto(pcb, p, &pcb->remote_ip, pcb->remote_port);
}

/** @ingroup udp_raw
 * Same as udp_send() but with checksum
 */
LwipStatus
udp_send_chksum(UdpPcb *pcb, struct PacketBuffer *p,
                uint8_t have_chksum, uint16_t chksum)
{
  
  

  if (IP_IS_ANY_TYPE_VAL(pcb->remote_ip)) {
    return ERR_VAL;
  }

  /* send to the packet using remote ip and port stored in the pcb */
  return udp_sendto_chksum(pcb, p, &pcb->remote_ip, pcb->remote_port,
                           have_chksum, chksum);
}

/**
 * @ingroup udp_raw
 * Send data to a specified address using UDP.
 *
 * @param pcb UDP PCB used to send the data.
 * @param p chain of PacketBuffer's to be sent.
 * @param dst_ip Destination IP address.
 * @param dst_port Destination UDP port.
 *
 * dst_ip & dst_port are expected to be in the same byte order as in the pcb.
 *
 * If the PCB already has a remote address association, it will
 * be restored after the data is sent.
 *
 * @return lwIP error code (@see udp_send for possible error codes)
 *
 * @see udp_disconnect() udp_send()
 */
LwipStatus
udp_sendto(struct UdpPcb *pcb, struct PacketBuffer *p,
           const IpAddr *dst_ip, uint16_t dst_port)
{

  return udp_sendto_chksum(pcb, p, dst_ip, dst_port, 0, 0);
}

/** @ingroup udp_raw
 * Same as udp_sendto(), but with checksum */
LwipStatus
udp_sendto_chksum(UdpPcb *pcb, struct PacketBuffer *p, const IpAddr *dst_ip,
                  uint16_t dst_port, uint8_t have_chksum, uint16_t chksum)
{

  NetIfc*netif;

  
  
  

  if (!match_ip_addr_pcb_version(pcb, dst_ip)) {
    return ERR_VAL;
  }

  Logf(UDP_DEBUG | LWIP_DBG_TRACE, ("udp_send\n"));

  if (pcb->netif_idx != NETIF_NO_INDEX) {
    netif = netif_get_by_index(pcb->netif_idx);
  } else {

    netif = nullptr;
    if (ip_addr_ismulticast(dst_ip)) {
      /* For IPv6, the interface to use for packets with a multicast destination
       * is specified using an interface index. The same approach may be used for
       * IPv4 as well, in which case it overrides the IPv4 multicast override
       * address below. Here we have to look up the netif by going through the
       * list, but by doing so we skip a route lookup. If the interface index has
       * gone stale, we fall through and do the regular route lookup after all. */
      if (pcb->mcast_ifindex != NETIF_NO_INDEX) {
        netif = netif_get_by_index(pcb->mcast_ifindex);
      }

      else

        if (IP_IS_V4(dst_ip))

        {
          /* IPv4 does not use source-based routing by default, so we use an
             administratively selected interface for multicast by default.
             However, this can be overridden by setting an interface address
             in pcb->mcast_ip4 that is used for routing. If this routing lookup
             fails, we try regular routing as though no override was set. */
          if (!ip4_addr_isany_val(pcb->mcast_ip4) &&
              !ip4_addr_cmp(&pcb->mcast_ip4, IP4_ADDR_BROADCAST)) {
            netif = ip4_route_src(ip_2_ip4(&pcb->local_ip), &pcb->mcast_ip4);
          }
        }

    }

    if (netif == nullptr)

    {
      /* find the outgoing network interface for this packet */
      netif = ip_route(&pcb->local_ip, dst_ip);
    }
  }

  /* no outgoing network interface could be found? */
  if (netif == nullptr) {
    Logf(UDP_DEBUG | LWIP_DBG_LEVEL_SERIOUS, ("udp_send: No route to "));
    ip_addr_debug_print(UDP_DEBUG | LWIP_DBG_LEVEL_SERIOUS, dst_ip);
    Logf(UDP_DEBUG, ("\n"));
    UDP_STATS_INC(udp.rterr);
    return ERR_RTE;
  }

  return udp_sendto_if_chksum(pcb, p, dst_ip, dst_port, netif, have_chksum, chksum);

}

/**
 * @ingroup udp_raw
 * Send data to a specified address using UDP.
 * The netif used for sending can be specified.
 *
 * This function exists mainly for DHCP, to be able to send UDP packets
 * on a netif that is still down.
 *
 * @param pcb UDP PCB used to send the data.
 * @param p chain of PacketBuffer's to be sent.
 * @param dst_ip Destination IP address.
 * @param dst_port Destination UDP port.
 * @param netif the netif used for sending.
 *
 * dst_ip & dst_port are expected to be in the same byte order as in the pcb.
 *
 * @return lwIP error code (@see udp_send for possible error codes)
 *
 * @see udp_disconnect() udp_send()
 */
LwipStatus
udp_sendto_if(struct UdpPcb *pcb, struct PacketBuffer *p,
              const IpAddr *dst_ip, uint16_t dst_port, NetIfc*netif)
{

  return udp_sendto_if_chksum(pcb, p, dst_ip, dst_port, netif, 0, 0);
}

/** Same as udp_sendto_if(), but with checksum */
LwipStatus
udp_sendto_if_chksum(UdpPcb *pcb, struct PacketBuffer *p, const IpAddr *dst_ip,
                     uint16_t dst_port, NetIfc*netif, uint8_t have_chksum,
                     uint16_t chksum)
{

  const IpAddr *src_ip;

  
  
  
  

  if (!match_ip_addr_pcb_version(pcb, dst_ip)) {
    return ERR_VAL;
  }

  /* PCB local address is IP_ANY_ADDR or multicast? */

  if (IpIsV6(dst_ip)) {
    if (ip6_addr_isany(ip_2_ip6(&pcb->local_ip)) ||
        ip6_addr_ismulticast(ip_2_ip6(&pcb->local_ip))) {
      src_ip = ip6_select_source_address(netif, ip_2_ip6(dst_ip));
      if (src_ip == nullptr) {
        /* No suitable source address was found. */
        return ERR_RTE;
      }
    } else {
      /* use UDP PCB local IPv6 address as source address, if still valid. */
      if (netif_get_ip6_addr_match(netif, ip_2_ip6(&pcb->local_ip)) < 0) {
        /* Address isn't valid anymore. */
        return ERR_RTE;
      }
      src_ip = &pcb->local_ip;
    }
  }


  else

    if (ip4_addr_isany(convert_ip_addr_to_ip4_addr(&pcb->local_ip)) ||
        ip4_addr_ismulticast(convert_ip_addr_to_ip4_addr(&pcb->local_ip))) {
      /* if the local_ip is any or multicast
       * use the outgoing network interface IP address as source address */
      src_ip = netif_ip_addr4(netif);
    } else {
      /* check if UDP PCB local IP address is correct
       * this could be an old address if netif->ip_addr has changed */
      if (!ip4_addr_cmp(convert_ip_addr_to_ip4_addr(&(pcb->local_ip)), get_net_ifc_ip4_addr(netif))) {
        /* local_ip doesn't match, drop the packet */
        return ERR_RTE;
      }
      /* use UDP PCB local IP address as source address */
      src_ip = &pcb->local_ip;
    }


  return udp_sendto_if_src_chksum(pcb, p, dst_ip, dst_port, netif, have_chksum, chksum, src_ip);

}

/** @ingroup udp_raw
 * Same as @ref udp_sendto_if, but with source address */
LwipStatus
udp_sendto_if_src(struct UdpPcb *pcb, struct PacketBuffer *p,
                  const IpAddr *dst_ip, uint16_t dst_port, NetIfc*netif, const IpAddr *src_ip)
{

  return udp_sendto_if_src_chksum(pcb, p, dst_ip, dst_port, netif, 0, 0, src_ip);
}

/** Same as udp_sendto_if_src(), but with checksum */
LwipStatus
udp_sendto_if_src_chksum(UdpPcb *pcb, struct PacketBuffer *p, const IpAddr *dst_ip,
                         uint16_t dst_port, NetIfc*netif, uint8_t have_chksum,
                         uint16_t chksum, const IpAddr *src_ip)
{

  struct UdpHdr *udphdr;
  LwipStatus err;
  struct PacketBuffer *q; /* q will be sent down the stack */
  uint8_t ip_proto;
  uint8_t ttl;

 

  
  
  
  
  

  if (!match_ip_addr_pcb_version(pcb, src_ip) ||
      !match_ip_addr_pcb_version(pcb, dst_ip)) {
    return ERR_VAL;
  }


  /* broadcast filter? */
  if (!ip_get_option(pcb, SOF_BROADCAST) &&

      IP_IS_V4(dst_ip) &&

      ip_addr_isbroadcast(dst_ip, netif)) {
    Logf(UDP_DEBUG | LWIP_DBG_LEVEL_SERIOUS,
                ("udp_sendto_if: SOF_BROADCAST not enabled on pcb %p\n", (uint8_t *)pcb));
    return ERR_VAL;
  }


  /* if the PCB is not yet bound to a port, bind it here */
  if (pcb->local_port == 0) {
    Logf(UDP_DEBUG | LWIP_DBG_TRACE, ("udp_send: not yet bound to a port, binding now\n"));
    err = udp_bind(pcb, &pcb->local_ip, pcb->local_port);
    if (err != ERR_OK) {
      Logf(UDP_DEBUG | LWIP_DBG_TRACE | LWIP_DBG_LEVEL_SERIOUS, ("udp_send: forced port bind failed\n"));
      return err;
    }
  }

  /* packet too large to add a UDP header without causing an overflow? */
  if ((uint16_t)(p->tot_len + UDP_HLEN) < p->tot_len) {
    return ERR_MEM;
  }
  /* not enough space to add an UDP header to first PacketBuffer in given p chain? */
  if (pbuf_add_header(p, UDP_HLEN)) {
    /* allocate header in a separate new PacketBuffer */
    q = pbuf_alloc(PBUF_IP, UDP_HLEN);
    /* new header PacketBuffer could not be allocated? */
    if (q == nullptr) {
      Logf(UDP_DEBUG | LWIP_DBG_TRACE | LWIP_DBG_LEVEL_SERIOUS, ("udp_send: could not allocate header\n"));
      return ERR_MEM;
    }
    if (p->tot_len != 0) {
      /* chain header q in front of given PacketBuffer p (only if p contains data) */
      pbuf_chain(q, p);
    }
    /* first PacketBuffer q points to header PacketBuffer */
    Logf(UDP_DEBUG,
         ("udp_send: added header PacketBuffer %p before given PacketBuffer %p\n", (uint8_t *)q, (uint8_t *)p));
  } else {
    /* adding space for header within p succeeded */
    /* first PacketBuffer q equals given PacketBuffer */
    q = p;
    Logf(UDP_DEBUG, ("udp_send: added header in given PacketBuffer %p\n", (uint8_t *)p));
  }
  lwip_assert("check that first PacketBuffer can hold struct udp_hdr",
              (q->len >= sizeof(struct UdpHdr)));
  /* q now represents the packet to be sent */
  udphdr = (struct UdpHdr *)q->payload;
  udphdr->src = lwip_htons(pcb->local_port);
  udphdr->dest = lwip_htons(dst_port);
  /* in UDP, 0 checksum means 'no checksum' */
  udphdr->chksum = 0x0000;

  /* Multicast Loop? */

  if (((pcb->flags & UDP_FLAGS_MULTICAST_LOOP) != 0) && ip_addr_ismulticast(dst_ip)) {
    q->flags |= PBUF_FLAG_MCASTLOOP;
  }


  Logf(UDP_DEBUG, ("udp_send: sending datagram of length %d\n", q->tot_len));


  /* UDP Lite protocol? */
  if (pcb->flags & UDP_FLAGS_UDPLITE) {
    uint16_t chklen, chklen_hdr;
    Logf(UDP_DEBUG, ("udp_send: UDP LITE packet length %d\n", q->tot_len));
    /* set UDP message length in UDP header */
    chklen_hdr = chklen = pcb->chksum_len_tx;
    if ((chklen < sizeof(struct udp_hdr)) || (chklen > q->tot_len)) {
      if (chklen != 0) {
        Logf(UDP_DEBUG, ("udp_send: UDP LITE pcb->chksum_len is illegal: %d\n", chklen));
      }
      /* For UDP-Lite, checksum length of 0 means checksum
         over the complete packet. (See RFC 3828 chap. 3.1)
         At least the UDP-Lite header must be covered by the
         checksum, therefore, if chksum_len has an illegal
         value, we generate the checksum over the complete
         packet to be safe. */
      chklen_hdr = 0;
      chklen = q->tot_len;
    }
    udphdr->len = lwip_htons(chklen_hdr);
    /* calculate checksum */

    IF__NETIF_CHECKSUM_ENABLED(netif, NETIF_CHECKSUM_GEN_UDP) {

      if (have_chksum) {
        chklen = UDP_HLEN;
      }

      udphdr->chksum = ip_chksum_pseudo_partial(q, IP_PROTO_UDPLITE,
                       q->tot_len, chklen, src_ip, dst_ip);

      if (have_chksum) {
        uint32_t acc;
        acc = udphdr->chksum + (uint16_t)~(chksum);
        udphdr->chksum = FOLD_U32T(acc);
      }


      /* chksum zero must become 0xffff, as zero means 'no checksum' */
      if (udphdr->chksum == 0x0000) {
        udphdr->chksum = 0xffff;
      }
    }


    ip_proto = IP_PROTO_UDPLITE;
  } else

  {      /* UDP */
    Logf(UDP_DEBUG, ("udp_send: UDP packet length %d\n", q->tot_len));
    udphdr->len = lwip_htons(q->tot_len);
    /* calculate checksum */

    is_netif_checksum_enabled(netif, NETIF_CHECKSUM_GEN_UDP) {
      /* Checksum is mandatory over IPv6. */
      if (IP_IS_V6(dst_ip) || (pcb->flags & UDP_FLAGS_NOCHKSUM) == 0) {
        uint16_t udpchksum;

        if (have_chksum) {
          uint32_t acc;
          udpchksum = ip_chksum_pseudo_partial(q, IP_PROTO_UDP,
                                               q->tot_len, UDP_HLEN, src_ip, dst_ip);
          acc = udpchksum + (uint16_t)~(chksum);
          udpchksum = FOLD_U32T(acc);
        } else

        {
          udpchksum = ip_chksum_pseudo(q, IP_PROTO_UDP, q->tot_len,
                                       src_ip, dst_ip);
        }

        /* chksum zero must become 0xffff, as zero means 'no checksum' */
        if (udpchksum == 0x0000) {
          udpchksum = 0xffff;
        }
        udphdr->chksum = udpchksum;
      }
    }

    ip_proto = IP_PROTO_UDP;
  }

  /* Determine TTL to use */

  ttl = (ip_addr_ismulticast(dst_ip) ? udp_get_multicast_ttl(pcb) : pcb->ttl);


  Logf(UDP_DEBUG, ("udp_send: UDP checksum 0x%04x\n", udphdr->chksum));
  Logf(UDP_DEBUG, ("udp_send: ip_output_if (,,,,0x%02x,)\n", (uint16_t)ip_proto));
  /* output to IP */
  NETIF_SET_HINTS(netif, &(pcb->netif_hints));
  err = ip_output_if_src(q, src_ip, dst_ip, ttl, pcb->tos, ip_proto, netif);
  NETIF_RESET_HINTS(netif);

  /* @todo: must this be increased even if error occurred? */
  

  /* did we chain a separate header PacketBuffer earlier? */
  if (q != p) {
    /* free the header PacketBuffer */
    free_pkt_buf(q);
    q = nullptr;
    /* p is still referenced by the caller, and will live on */
  }

  UDP_STATS_INC(udp.xmit);
  return err;
}

/**
 * @ingroup udp_raw
 * Bind an UDP PCB.
 * 
 * @param pcb UDP PCB to be bound with a local address ipaddr and port.
 * @param ipaddr local IP address to bind with. Use IP_ANY_TYPE to
 * bind to all local interfaces.
 * @param port local UDP port to bind with. Use 0 to automatically bind
 * to a random port between UDP_LOCAL_PORT_RANGE_START and
 * UDP_LOCAL_PORT_RANGE_END.
 *
 * ipaddr & port are expected to be in the same byte order as in the pcb.
 *
 * @return lwIP error code.
 * - ERR_OK. Successful. No error occurred.
 * - ERR_USE. The specified ipaddr and port are already bound to by
 * another UDP PCB.
 *
 * @see udp_disconnect()
 */
LwipStatus
udp_bind(struct UdpPcb *pcb, const IpAddr *ipaddr, uint16_t port)
{
  struct UdpPcb *ipcb;
  uint8_t rebind;

  IpAddr zoned_ipaddr;


 


  /* Don't propagate NULL pointer (IPv4 ANY) to subsequent functions */
  if (ipaddr == nullptr) {
    ipaddr = create_ip4_addr_any;
  }


  

  Logf(UDP_DEBUG | LWIP_DBG_TRACE, ("udp_bind(ipaddr = "));
  ip_addr_debug_print(UDP_DEBUG | LWIP_DBG_TRACE, ipaddr);
  Logf(UDP_DEBUG | LWIP_DBG_TRACE, (", port = %d)\n", port));

  rebind = 0;
  /* Check for double bind and rebind of the same pcb */
  for (ipcb = udp_pcbs; ipcb != nullptr; ipcb = ipcb->next) {
    /* is this UDP PCB already on active list? */
    if (pcb == ipcb) {
      rebind = 1;
      break;
    }
  }


  /* If the given IP address should have a zone but doesn't, assign one now.
   * This is legacy support: scope-aware callers should always provide properly
   * zoned source addresses. Do the zone selection before the address-in-use
   * check below; as such we have to make a temporary copy of the address. */
  if (IpIsV6(ipaddr) && ip6_addr_lacks_zone(ip_2_ip6(ipaddr), IP6_UNKNOWN)) {
    ip_addr_copy(zoned_ipaddr, *ipaddr);
    ip6_addr_select_zone(ip_2_ip6(&zoned_ipaddr), ip_2_ip6(&zoned_ipaddr));
    ipaddr = &zoned_ipaddr;
  }

  /* no port specified? */
  if (port == 0) {
    port = udp_new_port();
    if (port == 0) {
      /* no more ports available in local range */
      Logf(UDP_DEBUG, ("udp_bind: out of free UDP ports\n"));
      return ERR_USE;
    }
  } else {
    for (ipcb = udp_pcbs; ipcb != nullptr; ipcb = ipcb->next) {
      if (pcb != ipcb) {
        /* By default, we don't allow to bind to a port that any other udp
           PCB is already bound to, unless *all* PCBs with that port have tha
           REUSEADDR flag set. */

        if (!ip_get_option(pcb, SOF_REUSEADDR) ||
            !ip_get_option(ipcb, SOF_REUSEADDR))

        {
          /* port matches that of PCB in list and REUSEADDR not set -> reject */
          if ((ipcb->local_port == port) &&
              /* IP address matches or any IP used? */
              (compare_ip_addr(&ipcb->local_ip, ipaddr) || is_ip_addr_any(ipaddr) ||
              is_ip_addr_any(&ipcb->local_ip))) {
            /* other PCB already binds to this local IP and port */
            Logf(UDP_DEBUG,
                 ("udp_bind: local port %d already bound by another pcb\n", port));
            return ERR_USE;
          }
        }
      }
    }
  }

  set_ip_addr(&pcb->local_ip, ipaddr);

  pcb->local_port = port;
  mib2_udp_bind(pcb);
  /* pcb not active yet? */
  if (rebind == 0) {
    /* place the PCB on the active list if not already there */
    pcb->next = udp_pcbs;
    udp_pcbs = pcb;
  }
  Logf(UDP_DEBUG | LWIP_DBG_TRACE | LWIP_DBG_STATE, ("udp_bind: bound to "));
  ip_addr_debug_print_val(UDP_DEBUG | LWIP_DBG_TRACE | LWIP_DBG_STATE, pcb->local_ip);
  Logf(UDP_DEBUG | LWIP_DBG_TRACE | LWIP_DBG_STATE, (", port %d)\n", pcb->local_port));
  return ERR_OK;
}

/**
 * @ingroup udp_raw
 * Bind an UDP PCB to a specific netif.
 * After calling this function, all packets received via this PCB
 * are guaranteed to have come in via the specified netif, and all
 * outgoing packets will go out via the specified netif.
 *
 * @param pcb UDP PCB to be bound.
 * @param netif netif to bind udp pcb to. Can be NULL.
 *
 * @see udp_disconnect()
 */
void
udp_bind_netif(struct UdpPcb *pcb, const NetIfc*netif)
{
 

  if (netif != nullptr) {
    pcb->netif_idx = netif_get_index(netif);
  } else {
    pcb->netif_idx = NETIF_NO_INDEX;
  }
}

/**
 * @ingroup udp_raw
 * Sets the remote end of the pcb. This function does not generate any
 * network traffic, but only sets the remote address of the pcb.
 *
 * @param pcb UDP PCB to be connected with remote address ipaddr and port.
 * @param ipaddr remote IP address to connect with.
 * @param port remote UDP port to connect with.
 *
 * @return lwIP error code
 *
 * ipaddr & port are expected to be in the same byte order as in the pcb.
 *
 * The udp pcb is bound to a random local port if not already bound.
 *
 * @see udp_disconnect()
 */
LwipStatus
udp_connect(struct UdpPcb *pcb, const IpAddr *ipaddr, uint16_t port)
{
  struct UdpPcb *ipcb;

 

  
  

  if (pcb->local_port == 0) {
    LwipStatus err = udp_bind(pcb, &pcb->local_ip, pcb->local_port);
    if (err != ERR_OK) {
      return err;
    }
  }

  set_ip_addr(&pcb->remote_ip, ipaddr);

  /* If the given IP address should have a zone but doesn't, assign one now,
   * using the bound address to make a more informed decision when possible. */
  if (IpIsV6(&pcb->remote_ip) &&
      ip6_addr_lacks_zone(ip_2_ip6(&pcb->remote_ip), IP6_UNKNOWN)) {
    ip6_addr_select_zone(ip_2_ip6(&pcb->remote_ip), ip_2_ip6(&pcb->local_ip));
  }

  pcb->remote_port = port;
  pcb->flags |= UDP_FLAGS_CONNECTED;

  Logf(UDP_DEBUG | LWIP_DBG_TRACE | LWIP_DBG_STATE, ("udp_connect: connected to "));
  ip_addr_debug_print_val(UDP_DEBUG | LWIP_DBG_TRACE | LWIP_DBG_STATE,
                          pcb->remote_ip);
  Logf(UDP_DEBUG | LWIP_DBG_TRACE | LWIP_DBG_STATE, (", port %d)\n", pcb->remote_port));

  /* Insert UDP PCB into the list of active UDP PCBs. */
  for (ipcb = udp_pcbs; ipcb != nullptr; ipcb = ipcb->next) {
    if (pcb == ipcb) {
      /* already on the list, just return */
      return ERR_OK;
    }
  }
  /* PCB not yet on the list, add PCB now */
  pcb->next = udp_pcbs;
  udp_pcbs = pcb;
  return ERR_OK;
}

/**
 * @ingroup udp_raw
 * Remove the remote end of the pcb. This function does not generate
 * any network traffic, but only removes the remote address of the pcb.
 *
 * @param pcb the udp pcb to disconnect.
 */
void
udp_disconnect(struct UdpPcb *pcb)
{
 

  

  /* reset remote address association */

  if (IP_IS_ANY_TYPE_VAL(pcb->local_ip)) {
    ip_addr_copy(pcb->remote_ip, *IP_ANY_TYPE);
  } else {

    set_ip_addr_any(IP_IS_V6_VAL(pcb->remote_ip), &pcb->remote_ip);

  }

  pcb->remote_port = 0;
  pcb->netif_idx = NETIF_NO_INDEX;
  /* mark PCB as unconnected */
  udp_clear_flags(pcb, UDP_FLAGS_CONNECTED);
}

/**
 * @ingroup udp_raw
 * Set a receive callback for a UDP PCB.
 * This callback will be called when receiving a datagram for the pcb.
 *
 * @param pcb the pcb for which to set the recv callback
 * @param recv function pointer of the callback function
 * @param recv_arg additional argument to pass to the callback function
 */
void udp_recv(struct UdpPcb* pcb, UdpRecvFn recv, void* recv_arg)
{
 

  

  /* remember recv() callback and user data */
  pcb->recv = recv;
  pcb->recv_arg = recv_arg;
}

/**
 * @ingroup udp_raw
 * Removes and deallocates the pcb.  
 * 
 * @param pcb UDP PCB to be removed. The PCB is removed from the list of
 * UDP PCB's and the data structure is freed from memory.
 *
 * @see udp_new()
 */
void
udp_remove(struct UdpPcb *pcb)
{
  struct UdpPcb *pcb2;

 

  

  mib2_udp_unbind(pcb);
  /* pcb to be removed is first in list? */
  if (udp_pcbs == pcb) {
    /* make list start at 2nd pcb */
    udp_pcbs = udp_pcbs->next;
    /* pcb not 1st in list */
  } else {
    for (pcb2 = udp_pcbs; pcb2 != nullptr; pcb2 = pcb2->next) {
      /* find pcb in udp_pcbs list */
      if (pcb2->next != nullptr && pcb2->next == pcb) {
        /* remove pcb from list */
        pcb2->next = pcb->next;
        break;
      }
    }
  }
  // memp_free(MEMP_UDP_PCB, pcb);
  delete pcb;
}

/**
 * @ingroup udp_raw
 * Creates a new UDP pcb which can be used for UDP communication. The
 * pcb is not active until it has either been bound to a local address
 * or connected to a remote address.
 *
 * @return The UDP PCB which was created. NULL if the PCB data structure
 * could not be allocated.
 *
 * @see udp_remove()
 */
struct UdpPcb *
udp_new(void)
{
  struct UdpPcb *pcb;

 

  // pcb = (UdpPcb *)memp_malloc(MEMP_UDP_PCB);
  pcb = new UdpPcb;
  /* could allocate UDP PCB? */
  if (pcb != nullptr) {
    /* UDP Lite: by initializing to all zeroes, chksum_len is set to 0
     * which means checksum is generated over the whole datagram per default
     * (recommended as default by RFC 3828). */
    /* initialize PCB to all zeroes */
    memset(pcb, 0, sizeof(struct UdpPcb));
    pcb->ttl = UDP_TTL;

    udp_set_multicast_ttl(pcb, UDP_TTL);

  }
  return pcb;
}

/**
 * @ingroup udp_raw
 * Create a UDP PCB for specific IP type.
 * The pcb is not active until it has either been bound to a local address
 * or connected to a remote address.
 * 
 * @param type IP address type, see @ref lwip_ip_addr_type definitions.
 * If you want to listen to IPv4 and IPv6 (dual-stack) packets,
 * supply @ref IPADDR_TYPE_ANY as argument and bind to @ref IP_ANY_TYPE.
 * @return The UDP PCB which was created. NULL if the PCB data structure
 * could not be allocated.
 *
 * @see udp_remove()
 */
struct UdpPcb *
udp_new_ip_type(uint8_t type)
{
   

  struct UdpPcb* pcb = udp_new();
  if (pcb != nullptr) {
    set_ip_addr_type_val(pcb->local_ip,  type);
    set_ip_addr_type_val(pcb->remote_ip, type);
  }

  return pcb;
}

/** This function is called from netif.c when address is changed
 *
 * @param old_addr IP address of the netif before change
 * @param new_addr IP address of the netif after change
 */
void udp_netif_ip_addr_changed(const IpAddr *old_addr, const IpAddr *new_addr)
{
  struct UdpPcb *upcb;

  if (!is_ip_addr_any(old_addr) && !is_ip_addr_any(new_addr)) {
    for (upcb = udp_pcbs; upcb != nullptr; upcb = upcb->next) {
      /* PCB bound to current local interface address? */
      if (compare_ip_addr(&upcb->local_ip, old_addr)) {
        /* The PCB is bound to the old ipaddr and
         * is set to bound to the new one instead */
        copy_ip_addr(upcb->local_ip, *new_addr);
      }
    }
  }
}

