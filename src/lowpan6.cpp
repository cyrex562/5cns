/**
 * @file
 *
 * 6LowPAN output for IPv6. Uses ND tables for link-layer addressing. Fragments packets to 6LowPAN units.
 *
 * This implementation aims to conform to IEEE 802.15.4(-2015), RFC 4944 and RFC 6282.
 * @todo: RFC 6775.
 */

/*
 * Copyright (c) 2015 Inico Technologies Ltd.
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

/**
 * @defgroup sixlowpan 6LoWPAN (RFC4944)
 * @ingroup netifs
 * 6LowPAN netif implementation
 */


#include <lowpan6.h>
#include <ip.h>
#include <packet_buffer.h>
#include <ip_addr.h>
#include <netif.h>
#include <nd6.h>
#include <udp.h>
#include <tcpip.h>
#include <ieee802154.h>
#include <lowpan6_common.h>
#include <lwip_debug.h>
#include <cstring>

// LWIP_6LOWPAN_DO_CALC_CRC(buf, len) LWIP_6LOWPAN_CALC_CRC(buf, len)



/** This is a helper struct for reassembly of fragments
 * (IEEE 802.15.4 limits to 127 bytes)
 */
struct lowpan6_reass_helper {
  struct lowpan6_reass_helper *next_packet;
  struct PacketBuffer *reass;
  struct PacketBuffer *frags;
  uint8_t timer;
  struct Lowpan6LinkAddr sender_addr;
  uint16_t datagram_size;
  uint16_t datagram_tag;
};

/** This struct keeps track of per-netif state */
struct lowpan6_ieee802154_data {
  /** fragment reassembly list */
  struct lowpan6_reass_helper *reass_list;


  /** address context for compression */
  Ip6Addr lowpan6_context[LWIP_6LOWPAN_NUM_CONTEXTS];


  /** Datagram Tag for fragmentation */
  uint16_t tx_datagram_tag;
  /** local PAN ID for IEEE 802.15.4 header */
  uint16_t ieee_802154_pan_id;
  /** Sequence Number for IEEE 802.15.4 transmission */
  uint8_t tx_frame_seq_num;
};

/* Maximum frame size is 127 bytes minus CRC size */
#define LOWPAN6_MAX_PAYLOAD (127 - 2)

/** Currently, this state is global, since there's only one 6LoWPAN netif */
static struct lowpan6_ieee802154_data lowpan6_data;


#define LWIP_6LOWPAN_CONTEXTS(netif) lowpan6_data.lowpan6_context





static const struct Lowpan6LinkAddr ieee_802154_broadcast = {2, {0xff, 0xff}};


static struct Lowpan6LinkAddr short_mac_addr = {2, {0, 0}};


/* IEEE 802.15.4 specific functions: */

/** Write the IEEE 802.15.4 header that encapsulates the 6LoWPAN frame.
 * Src and dst PAN IDs are filled with the ID set by @ref lowpan6_set_pan_id.
 *
 * Since the length is variable:
 * @returns the header length
 */
static uint8_t
lowpan6_write_iee802154_header(struct ieee_802154_hdr *hdr, const struct Lowpan6LinkAddr *src,
                               const struct Lowpan6LinkAddr *dst)
{
  uint8_t ieee_header_len;
  uint8_t *buffer;
  uint8_t i;
  uint16_t fc;

  fc = IEEE_802154_FC_FT_DATA; /* send data packet (2003 frame version) */
  fc |= IEEE_802154_FC_PANID_COMPR; /* set PAN ID compression, for now src and dst PANs are equal */
  if (dst != &ieee_802154_broadcast) {
    fc |= IEEE_802154_FC_ACK_REQ; /* data packet, no broadcast: ack required. */
  }
  if (dst->addr_len == 2) {
    fc |= IEEE_802154_FC_DST_ADDR_MODE_SHORT;
  } else {
    lwip_assert("invalid dst address length", dst->addr_len == 8);
    fc |= IEEE_802154_FC_DST_ADDR_MODE_EXT;
  }
  if (src->addr_len == 2) {
    fc |= IEEE_802154_FC_SRC_ADDR_MODE_SHORT;
  } else {
    lwip_assert("invalid src address length", src->addr_len == 8);
    fc |= IEEE_802154_FC_SRC_ADDR_MODE_EXT;
  }
  hdr->frame_control = fc;
  hdr->sequence_number = lowpan6_data.tx_frame_seq_num++;
  hdr->destination_pan_id = lowpan6_data.ieee_802154_pan_id; /* pan id */

  buffer = (uint8_t *)hdr;
  ieee_header_len = 5;
  i = dst->addr_len;
  /* reverse memcpy of dst addr */
  while (i-- > 0) {
    buffer[ieee_header_len++] = dst->addr[i];
  }
  /* Source PAN ID skipped due to PAN ID Compression */
  i = src->addr_len;
  /* reverse memcpy of src addr */
  while (i-- > 0) {
    buffer[ieee_header_len++] = src->addr[i];
  }
  return ieee_header_len;
}

/** Parse the IEEE 802.15.4 header from a PacketBuffer.
 * If successful, the header is hidden from the PacketBuffer.
 *
 * PAN IDs and seuqence number are not checked
 *
 * @param p input PacketBuffer, p->payload pointing at the IEEE 802.15.4 header
 * @param src pointer to source address filled from the header
 * @param dest pointer to destination address filled from the header
 * @returns ERR_OK if successful
 */
static LwipStatus
lowpan6_parse_iee802154_header(struct PacketBuffer *p, struct Lowpan6LinkAddr *src,
                               struct Lowpan6LinkAddr *dest)
{
  uint8_t *puc;
  int8_t i;
  uint16_t frame_control, addr_mode;
  uint16_t datagram_offset;

  /* Parse IEEE 802.15.4 header */
  puc = (uint8_t *)p->payload;
  frame_control = puc[0] | (puc[1] << 8);
  datagram_offset = 2;
  if (frame_control & IEEE_802154_FC_SEQNO_SUPPR) {
    if (IEEE_802154_FC_FRAME_VERSION_GET(frame_control) <= 1) {
      /* sequence number suppressed, this is not valid for versions 0/1 */
      return ERR_VAL;
    }
  } else {
    datagram_offset++;
  }
  datagram_offset += 2; /* Skip destination PAN ID */
  addr_mode = frame_control & IEEE_802154_FC_DST_ADDR_MODE_MASK;
  if (addr_mode == IEEE_802154_FC_DST_ADDR_MODE_EXT) {
    /* extended address (64 bit) */
    dest->addr_len = 8;
    /* reverse memcpy: */
    for (i = 0; i < 8; i++) {
      dest->addr[i] = puc[datagram_offset + 7 - i];
    }
    datagram_offset += 8;
  } else if (addr_mode == IEEE_802154_FC_DST_ADDR_MODE_SHORT) {
    /* short address (16 bit) */
    dest->addr_len = 2;
    /* reverse memcpy: */
    dest->addr[0] = puc[datagram_offset + 1];
    dest->addr[1] = puc[datagram_offset];
    datagram_offset += 2;
  } else {
    /* unsupported address mode (do we need "no address"?) */
    return ERR_VAL;
  }

  if (!(frame_control & IEEE_802154_FC_PANID_COMPR)) {
    /* No PAN ID compression, skip source PAN ID */
    datagram_offset += 2;
  }

  addr_mode = frame_control & IEEE_802154_FC_SRC_ADDR_MODE_MASK;
  if (addr_mode == IEEE_802154_FC_SRC_ADDR_MODE_EXT) {
    /* extended address (64 bit) */
    src->addr_len = 8;
    /* reverse memcpy: */
    for (i = 0; i < 8; i++) {
      src->addr[i] = puc[datagram_offset + 7 - i];
    }
    datagram_offset += 8;
  } else if (addr_mode == IEEE_802154_FC_DST_ADDR_MODE_SHORT) {
    /* short address (16 bit) */
    src->addr_len = 2;
    src->addr[0] = puc[datagram_offset + 1];
    src->addr[1] = puc[datagram_offset];
    datagram_offset += 2;
  } else {
    /* unsupported address mode (do we need "no address"?) */
    return ERR_VAL;
  }

  /* hide IEEE802.15.4 header. */
  if (pbuf_remove_header(p, datagram_offset)) {
    return ERR_VAL;
  }
  return ERR_OK;
}

/** Calculate the 16-bit CRC as required by IEEE 802.15.4 */
uint16_t
lowpan6_calc_crc(const void* buf, uint16_t len)
{
#define CCITT_POLY_16 0x8408U
  uint16_t i;
  uint8_t b;
  uint16_t crc = 0;
  const uint8_t* p = (const uint8_t*)buf;

  for (i = 0; i < len; i++) {
    uint8_t data = *p;
    for (b = 0U; b < 8U; b++) {
      if (((data ^ crc) & 1) != 0) {
        crc = (uint16_t)((crc >> 1) ^ CCITT_POLY_16);
      } else {
        crc = (uint16_t)(crc >> 1);
      }
      data = (uint8_t)(data >> 1);
    }
    p++;
  }
  return crc;
}

/* Fragmentation specific functions: */

static void
free_reass_datagram(struct lowpan6_reass_helper *lrh)
{
  if (lrh->reass) {
    free_pkt_buf(lrh->reass);
  }
  if (lrh->frags) {
    free_pkt_buf(lrh->frags);
  }
  delete lrh;
}

/**
 * Removes a datagram from the reassembly queue.
 **/
static void
dequeue_datagram(struct lowpan6_reass_helper *lrh, struct lowpan6_reass_helper *prev)
{
  if (lowpan6_data.reass_list == lrh) {
    lowpan6_data.reass_list = lowpan6_data.reass_list->next_packet;
  } else {
    /* it wasn't the first, so it must have a valid 'prev' */
    lwip_assert("sanity check linked list", prev != nullptr);
    prev->next_packet = lrh->next_packet;
  }
}

/**
 * Periodic timer for 6LowPAN functions:
 *
 * - Remove incomplete/old packets
 */
void
lowpan6_tmr(void)
{
  struct lowpan6_reass_helper *lrh, *lrh_next, *lrh_prev = nullptr;

  lrh = lowpan6_data.reass_list;
  while (lrh != nullptr) {
    lrh_next = lrh->next_packet;
    if ((--lrh->timer) == 0) {
      dequeue_datagram(lrh, lrh_prev);
      free_reass_datagram(lrh);
    } else {
      lrh_prev = lrh;
    }
    lrh = lrh_next;
  }
}

/*
 * Encapsulates data into IEEE 802.15.4 frames.
 * Fragments an IPv6 datagram into 6LowPAN units, which fit into IEEE 802.15.4 frames.
 * If configured, will compress IPv6 and or UDP headers.
 * */
static LwipStatus
lowpan6_frag(NetworkInterface*netif, struct PacketBuffer *p, const struct Lowpan6LinkAddr *src, const struct Lowpan6LinkAddr *dst)
{
  struct PacketBuffer *p_frag;
  uint16_t frag_len, remaining_len, max_data_len;
  uint8_t *buffer;
  uint8_t ieee_header_len;
  uint8_t lowpan6_header_len;
  uint8_t hidden_header_len;
  uint16_t crc;
  uint16_t datagram_offset;
  LwipStatus err = ERR_IF;

  lwip_assert("lowpan6_frag: netif->linkoutput not set", netif->linkoutput != nullptr);

  /* We'll use a dedicated PacketBuffer for building 6LowPAN fragments. */
  p_frag = pbuf_alloc(PBUF_RAW, 127);
  if (p_frag == nullptr) {
    // MIB2_STATS_NETIF_INC(netif, ifoutdiscards);
    return ERR_MEM;
  }
  lwip_assert("this needs a PacketBuffer in one piece", p_frag->len == p_frag->tot_len);

  /* Write IEEE 802.15.4 header. */
  buffer = (uint8_t *)p_frag->payload;
  ieee_header_len = lowpan6_write_iee802154_header((struct ieee_802154_hdr *)buffer, src, dst);
  lwip_assert("ieee_header_len < p_frag->len", ieee_header_len < p_frag->len);

  /* Perform 6LowPAN IPv6 header compression according to RFC 6282 */
  /* do the header compression (this does NOT copy any non-compressed data) */
  err = lowpan6_compress_headers(netif, (uint8_t *)p->payload, p->len,
    &buffer[ieee_header_len], p_frag->len - ieee_header_len, &lowpan6_header_len,
    &hidden_header_len, LWIP_6LOWPAN_CONTEXTS(netif), src, dst);
  if (err != ERR_OK) {
    // MIB2_STATS_NETIF_INC(netif, ifoutdiscards);
    free_pkt_buf(p_frag);
    return err;
  }
  pbuf_remove_header(p, hidden_header_len);




  /* Calculate remaining packet length */
  remaining_len = p->tot_len;

  if (remaining_len > 0x7FF) {
    // MIB2_STATS_NETIF_INC(netif, ifoutdiscards);
    /* datagram_size must fit into 11 bit */
    free_pkt_buf(p_frag);
    return ERR_VAL;
  }

  /* Fragment, or 1 packet? */
  max_data_len = LOWPAN6_MAX_PAYLOAD - ieee_header_len - lowpan6_header_len;
  if (remaining_len > max_data_len) {
    uint16_t data_len;
    /* We must move the 6LowPAN header to make room for the FRAG header. */
    memmove(&buffer[ieee_header_len + 4], &buffer[ieee_header_len], lowpan6_header_len);

    /* Now we need to fragment the packet. FRAG1 header first */
    buffer[ieee_header_len] = 0xc0 | (((p->tot_len + hidden_header_len) >> 8) & 0x7);
    buffer[ieee_header_len + 1] = (p->tot_len + hidden_header_len) & 0xff;

    lowpan6_data.tx_datagram_tag++;
    buffer[ieee_header_len + 2] = (lowpan6_data.tx_datagram_tag >> 8) & 0xff;
    buffer[ieee_header_len + 3] = lowpan6_data.tx_datagram_tag & 0xff;

    /* Fragment follows. */
    data_len = (max_data_len - 4) & 0xf8;
    frag_len = data_len + lowpan6_header_len;

    pbuf_copy_partial(p, buffer + ieee_header_len + lowpan6_header_len + 4, frag_len - lowpan6_header_len, 0);
    remaining_len -= frag_len - lowpan6_header_len;
    /* datagram offset holds the offset before compression */
    datagram_offset = frag_len - lowpan6_header_len + hidden_header_len;
    lwip_assert("datagram offset must be a multiple of 8", (datagram_offset & 7) == 0);

    /* Calculate frame length */
    p_frag->len = p_frag->tot_len = ieee_header_len + 4 + frag_len + 2; /* add 2 bytes for crc*/

    /* 2 bytes CRC */
    crc = lowpan6_calc_crc(p_frag->payload, p_frag->len - 2);
    pbuf_take_at(p_frag, (uint8_t*)&crc, 2, p_frag->len - 2);

    /* send the packet */
  

    Logf(LWIP_LOWPAN6_DEBUG | LWIP_DBG_TRACE, "lowpan6_send: sending packet %p\n", (uint8_t *)p);
    err = netif->linkoutput(netif, p_frag);

    while ((remaining_len > 0) && (err == ERR_OK)) {
      struct ieee_802154_hdr *hdr = (struct ieee_802154_hdr *)buffer;
      /* new frame, new seq num for ACK */
      hdr->sequence_number = lowpan6_data.tx_frame_seq_num++;

      buffer[ieee_header_len] |= 0x20; /* Change FRAG1 to FRAGN */

      lwip_assert("datagram offset must be a multiple of 8", (datagram_offset & 7) == 0);
      buffer[ieee_header_len + 4] = (uint8_t)(datagram_offset >> 3); /* datagram offset in FRAGN header (datagram_offset is max. 11 bit) */

      frag_len = (127 - ieee_header_len - 5 - 2) & 0xf8;
      if (frag_len > remaining_len) {
        frag_len = remaining_len;
      }

      pbuf_copy_partial(p, buffer + ieee_header_len + 5, frag_len, p->tot_len - remaining_len);
      remaining_len -= frag_len;
      datagram_offset += frag_len;

      /* Calculate frame length */
      p_frag->len = p_frag->tot_len = frag_len + 5 + ieee_header_len + 2;

      /* 2 bytes CRC */
      crc = lowpan6_calc_crc(p_frag->payload, p_frag->len - 2);
      pbuf_take_at(p_frag, (uint8_t*)&crc, 2, p_frag->len - 2);

      /* send the packet */
   
      Logf(LWIP_LOWPAN6_DEBUG | LWIP_DBG_TRACE, "lowpan6_send: sending packet %p\n", (uint8_t *)p);
      err = netif->linkoutput(netif, p_frag);
    }
  } else {
    /* It fits in one frame. */
    frag_len = remaining_len;

    /* Copy IPv6 packet */
    pbuf_copy_partial(p, buffer + ieee_header_len + lowpan6_header_len, frag_len, 0);
    remaining_len = 0;

    /* Calculate frame length */
    p_frag->len = p_frag->tot_len = frag_len + lowpan6_header_len + ieee_header_len + 2;
    lwip_assert("", p_frag->len <= 127);

    /* 2 bytes CRC */
    crc = lowpan6_calc_crc(p_frag->payload, p_frag->len - 2);
    pbuf_take_at(p_frag, (uint8_t*)&crc, 2, p_frag->len - 2);

    /* send the packet */

    Logf(LWIP_LOWPAN6_DEBUG | LWIP_DBG_TRACE, "lowpan6_send: sending packet %p\n", (uint8_t *)p);
    err = netif->linkoutput(netif, p_frag);
  }

  free_pkt_buf(p_frag);

  return err;
}

/**
 * @ingroup sixlowpan
 * Set context
 */
LwipStatus
lowpan6_set_context(uint8_t idx, const Ip6Addr*context)
{

  if (idx >= LWIP_6LOWPAN_NUM_CONTEXTS) {
    return ERR_ARG;
  }

  ip6_addr_zonecheck(context);

  ip6_addr_set(&lowpan6_data.lowpan6_context[idx], context);

  return ERR_OK;

}

/**
 * @ingroup sixlowpan
 * Set short address
 */
LwipStatus
lowpan6_set_short_addr(uint8_t addr_high, uint8_t addr_low)
{
  short_mac_addr.addr[0] = addr_high;
  short_mac_addr.addr[1] = addr_low;

  return ERR_OK;
}

/* Create IEEE 802.15.4 address from netif address */
static LwipStatus
lowpan6_hwaddr_to_addr(NetworkInterface*netif, struct Lowpan6LinkAddr *addr)
{
  addr->addr_len = 8;
  if (netif->hwaddr_len == 8) {
    // 
    if (sizeof(netif->hwaddr) < 8)
    {
        printf("netif hwaddr must be greater than 8\n");
        return ERR_VAL;
    }

    memcpy(addr->addr, netif->hwaddr, 8);
  } else if (netif->hwaddr_len == 6) {
    /* Copy from MAC-48 */
    memcpy(addr->addr, netif->hwaddr, 3);
    addr->addr[3] = addr->addr[4] = 0xff;
    memcpy(&addr->addr[5], &netif->hwaddr[3], 3);
  } else {
    /* Invalid address length, don't know how to convert this */
    return ERR_VAL;
  }
  return ERR_OK;
}

/**
 * @ingroup sixlowpan
 * Resolve and fill-in IEEE 802.15.4 address header for outgoing IPv6 packet.
 *
 * Perform Header Compression and fragment if necessary.
 *
 * @param netif The lwIP network interface which the IP packet will be sent on.
 * @param q The PacketBuffer(s) containing the IP packet to be sent.
 * @param ip6addr The IP address of the packet destination.
 *
 * @return LwipStatus
 */
LwipStatus
lowpan6_output(NetworkInterface*netif, struct PacketBuffer *q, const Ip6Addr*ip6addr)
{
  LwipStatus result;
  const uint8_t *hwaddr;
  struct Lowpan6LinkAddr src, dest;

  Ip6Addr ip6_src;
  struct Ip6Hdr *ip6_hdr;

  /* Check if we can compress source address (use aligned copy) */
  ip6_hdr = (struct Ip6Hdr *)q->payload;
  ip6_addr_copy_from_packed(&ip6_src, &ip6_hdr->src);
  ip6_addr_assign_zone(&ip6_src, IP6_UNICAST, netif);
  if (lowpan6_get_address_mode(&ip6_src, &short_mac_addr) == 3) {
    src.addr_len = 2;
    src.addr[0] = short_mac_addr.addr[0];
    src.addr[1] = short_mac_addr.addr[1];
  } else

  {
    result = lowpan6_hwaddr_to_addr(netif, &src);
    if (result != ERR_OK) {
      return result;
    }
  }

  /* multicast destination IP address? */
  if (ip6_addr_ismulticast(ip6addr)) {

    /* We need to send to the broadcast address.*/
    return lowpan6_frag(netif, q, &src, &ieee_802154_broadcast);
  }

  /* We have a unicast destination IP address */
  /* @todo anycast? */

  if (src.addr_len == 2) {
    /* If source address was compressable to short_mac_addr, and dest has same subnet and
     * is also compressable to 2-bytes, assume we can infer dest as a short address too. */
    dest.addr_len = 2;
    dest.addr[0] = ((uint8_t *)q->payload)[38];
    dest.addr[1] = ((uint8_t *)q->payload)[39];
    if ((src.addr_len == 2) && (ip6_addr_netcmp_zoneless((Ip6Addr*)&ip6_hdr->src, (Ip6Addr*)&ip6_hdr->dest)) &&
        (lowpan6_get_address_mode(ip6addr, &dest) == 3)) {
      return lowpan6_frag(netif, q, &src, &dest);
    }
  }


  /* Ask ND6 what to do with the packet. */
  result = nd6_get_next_hop_addr_or_queue(netif, q, ip6addr, &hwaddr);
  if (result != ERR_OK) {
    return result;
  }

  /* If no hardware address is returned, nd6 has queued the packet for later. */
  if (hwaddr == nullptr) {
    return ERR_OK;
  }

  /* Send out the packet using the returned hardware address. */
  dest.addr_len = netif->hwaddr_len;
  /* XXX: Inferring the length of the source address from the destination address
   * is not correct for IEEE 802.15.4, but currently we don't get this information
   * from the neighbor cache */
  memcpy(dest.addr, hwaddr, netif->hwaddr_len);
  return lowpan6_frag(netif, q, &src, &dest);
}
/**
 * @ingroup sixlowpan
 * NETIF input function: don't free the input PacketBuffer when returning != ERR_OK!
 */
LwipStatus
lowpan6_input(struct PacketBuffer *p, NetworkInterface*netif)
{
  uint8_t *puc, b;
  int8_t i;
  struct Lowpan6LinkAddr src, dest;
  uint16_t datagram_size = 0;
  uint16_t datagram_offset, datagram_tag;
  struct lowpan6_reass_helper *lrh, *lrh_next, *lrh_prev = nullptr;

  if (p == nullptr) {
    return ERR_OK;
  }

  if (p->len != p->tot_len) {
    /* for now, this needs a PacketBuffer in one piece */
    goto lowpan6_input_discard;
  }

  if (lowpan6_parse_iee802154_header(p, &src, &dest) != ERR_OK) {
    goto lowpan6_input_discard;
  }

  /* Check dispatch. */
  puc = (uint8_t *)p->payload;

  b = *puc;
  if ((b & 0xf8) == 0xc0) {
    /* FRAG1 dispatch. add this packet to reassembly list. */
    datagram_size = ((uint16_t)(puc[0] & 0x07) << 8) | (uint16_t)puc[1];
    datagram_tag = ((uint16_t)puc[2] << 8) | (uint16_t)puc[3];

    /* check for duplicate */
    lrh = lowpan6_data.reass_list;
    while (lrh != nullptr) {
      uint8_t discard = 0;
      lrh_next = lrh->next_packet;
      if ((lrh->sender_addr.addr_len == src.addr_len) &&
          (memcmp(lrh->sender_addr.addr, src.addr, src.addr_len) == 0)) {
        /* address match with packet in reassembly. */
        if ((datagram_tag == lrh->datagram_tag) && (datagram_size == lrh->datagram_size)) {
          /* duplicate fragment. */
          goto lowpan6_input_discard;
        } else {
          /* We are receiving the start of a new datagram. Discard old one (incomplete). */
          discard = 1;
        }
      }
      if (discard) {
        dequeue_datagram(lrh, lrh_prev);
        free_reass_datagram(lrh);
      } else {
        lrh_prev = lrh;
      }
      /* Check next datagram in queue. */
      lrh = lrh_next;
    }

    pbuf_remove_header(p, 4); /* hide frag1 dispatch */

      lrh = new lowpan6_reass_helper;
    if (lrh == nullptr) {
      goto lowpan6_input_discard;
    }

    lrh->sender_addr.addr_len = src.addr_len;
    for (i = 0; i < src.addr_len; i++) {
      lrh->sender_addr.addr[i] = src.addr[i];
    }
    lrh->datagram_size = datagram_size;
    lrh->datagram_tag = datagram_tag;
    lrh->frags = nullptr;
    if (*(uint8_t *)p->payload == 0x41) {
      /* This is a complete IPv6 packet, just skip dispatch byte. */
      pbuf_remove_header(p, 1); /* hide dispatch byte. */
      lrh->reass = p;
    } else if ((*(uint8_t *)p->payload & 0xe0 ) == 0x60) {
      lrh->reass = lowpan6_decompress(p, datagram_size, LWIP_6LOWPAN_CONTEXTS(netif), &src, &dest);
      if (lrh->reass == nullptr) {
        /* decompression failed */
        delete lrh;
        goto lowpan6_input_discard;
      }
    }
    /* TODO: handle the case where we already have FRAGN received */
    lrh->next_packet = lowpan6_data.reass_list;
    lrh->timer = 2;
    lowpan6_data.reass_list = lrh;

    return ERR_OK;
  } else if ((b & 0xf8) == 0xe0) {
    /* FRAGN dispatch, find packet being reassembled. */
    datagram_size = ((uint16_t)(puc[0] & 0x07) << 8) | (uint16_t)puc[1];
    datagram_tag = ((uint16_t)puc[2] << 8) | (uint16_t)puc[3];
    datagram_offset = (uint16_t)puc[4] << 3;
    pbuf_remove_header(p, 4); /* hide frag1 dispatch but keep datagram offset for reassembly */

    for (lrh = lowpan6_data.reass_list; lrh != nullptr; lrh_prev = lrh, lrh = lrh->next_packet) {
      if ((lrh->sender_addr.addr_len == src.addr_len) &&
          (memcmp(lrh->sender_addr.addr, src.addr, src.addr_len) == 0) &&
          (datagram_tag == lrh->datagram_tag) &&
          (datagram_size == lrh->datagram_size)) {
        break;
      }
    }
    if (lrh == nullptr) {
      /* rogue fragment */
      goto lowpan6_input_discard;
    }
    /* Insert new PacketBuffer into list of fragments. Each fragment is a PacketBuffer,
       this only works for unchained pbufs. */
    lwip_assert("p->next == NULL", p->next == nullptr);
    if (lrh->reass != nullptr) {
      /* FRAG1 already received, check this offset against first len */
      if (datagram_offset < lrh->reass->len) {
        /* fragment overlap, discard old fragments */
        dequeue_datagram(lrh, lrh_prev);
        free_reass_datagram(lrh);
        goto lowpan6_input_discard;
      }
    }
    if (lrh->frags == nullptr) {
      /* first FRAGN */
      lrh->frags = p;
    } else {
      /* find the correct place to insert */
      struct PacketBuffer *q, *last;
      uint16_t new_frag_len = p->len - 1; /* p->len includes datagram_offset byte */
      for (q = lrh->frags, last = nullptr; q != nullptr; last = q, q = q->next) {
        uint16_t q_datagram_offset = ((uint8_t *)q->payload)[0] << 3;
        uint16_t q_frag_len = q->len - 1;
        if (datagram_offset < q_datagram_offset) {
          if (datagram_offset + new_frag_len > q_datagram_offset) {
            /* overlap, discard old fragments */
            dequeue_datagram(lrh, lrh_prev);
            free_reass_datagram(lrh);
            goto lowpan6_input_discard;
          }
          /* insert here */
          break;
        } else if (datagram_offset == q_datagram_offset) {
          if (q_frag_len != new_frag_len) {
            /* fragment mismatch, discard old fragments */
            dequeue_datagram(lrh, lrh_prev);
            free_reass_datagram(lrh);
            goto lowpan6_input_discard;
          }
          /* duplicate, ignore */
          free_pkt_buf(p);
          return ERR_OK;
        }
      }
      /* insert fragment */
      if (last == nullptr) {
        lrh->frags = p;
      } else {
        last->next = p;
        p->next = q;
      }
    }
    /* check if all fragments were received */
    if (lrh->reass) {
      uint16_t offset = lrh->reass->len;
      struct PacketBuffer *q;
      for (q = lrh->frags; q != nullptr; q = q->next) {
        uint16_t q_datagram_offset = ((uint8_t *)q->payload)[0] << 3;
        if (q_datagram_offset != offset) {
          /* not complete, wait for more fragments */
          return ERR_OK;
        }
        offset += q->len - 1;
      }
      if (offset == datagram_size) {
        /* all fragments received, combine pbufs */
        uint16_t datagram_left = datagram_size - lrh->reass->len;
        for (q = lrh->frags; q != nullptr; q = q->next) {
          /* hide datagram_offset byte now */
          pbuf_remove_header(q, 1);
          q->tot_len = datagram_left;
          datagram_left -= q->len;
        }
        lwip_assert("datagram_left == 0", datagram_left == 0);
        q = lrh->reass;
        q->tot_len = datagram_size;
        q->next = lrh->frags;
        lrh->frags = nullptr;
        lrh->reass = nullptr;
        dequeue_datagram(lrh, lrh_prev);
        delete lrh;

        /* @todo: distinguish unicast/multicast */
        return ip6_input(q, netif);
      }
    }
    /* PacketBuffer enqueued, waiting for more fragments */
    return ERR_OK;
  } else {
    if (b == 0x41) {
      /* This is a complete IPv6 packet, just skip dispatch byte. */
      pbuf_remove_header(p, 1); /* hide dispatch byte. */
    } else if ((b & 0xe0 ) == 0x60) {
      /* IPv6 headers are compressed using IPHC. */
      p = lowpan6_decompress(p, datagram_size, LWIP_6LOWPAN_CONTEXTS(netif), &src, &dest);
      if (p == nullptr) {
        return ERR_OK;
      }
    } else {
      goto lowpan6_input_discard;
    }

    /* @todo: distinguish unicast/multicast */

    return ip6_input(p, netif);
  }
lowpan6_input_discard:
  free_pkt_buf(p);
  /* always return ERR_OK here to prevent the caller freeing the PacketBuffer */
  return ERR_OK;
}

/**
 * @ingroup sixlowpan
 */
LwipStatus
lowpan6_if_init(NetworkInterface*netif)
{
  netif->name[0] = 'L';
  netif->name[1] = '6';
  netif->output_ip6 = lowpan6_output;

  /* maximum transfer unit */
  netif->mtu = 1280;

  /* broadcast capability */
  netif->flags = NETIF_FLAG_BCAST /* | NETIF_FLAG_LOWPAN6 */;

  return ERR_OK;
}

/**
 * @ingroup sixlowpan
 * Set PAN ID
 */
LwipStatus
lowpan6_set_pan_id(uint16_t pan_id)
{
  lowpan6_data.ieee_802154_pan_id = pan_id;

  return ERR_OK;
}

/**
 * @ingroup sixlowpan
 * Pass a received packet to tcpip_thread for input processing
 *
 * @param p the received packet, p->payload pointing to the
 *          IEEE 802.15.4 header.
 * @param inp the network interface on which the packet was received
 */
LwipStatus
tcpip_6lowpan_input(struct PacketBuffer *p, NetworkInterface*inp)
{
  return tcpip_inpkt(p, inp, lowpan6_input);
}

