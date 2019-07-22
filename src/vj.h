/*
 * Definitions for tcp compression routines.
 *
 * $Id: vj.h,v 1.7 2010/02/22 17:52:09 goldsimon Exp $
 *
 * Copyright (c) 1989 Regents of the University of California.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms are permitted
 * provided that the above copyright notice and this paragraph are
 * duplicated in all such forms and that any documentation,
 * advertising materials, and other materials related to such
 * distribution and use acknowledge that the software was developed
 * by the University of California, Berkeley.  The name of the
 * University may not be used to endorse or promote products derived
 * from this software without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 *
 * Van Jacobson (van@helios.ee.lbl.gov), Dec 31, 1989:
 * - Initial distribution.
 */

#pragma once
#include <ppp_opts.h>
#include <ip.h>
#include <tcp_priv.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_SLOTS 16 /* must be > 2 and < 256 */
constexpr auto kMaxHdr = 128;

/*
 * Compressed packet format:
 *
 * The first octet contains the packet type (top 3 bits), TCP
 * 'push' bit, and flags that indicate which of the 4 TCP sequence
 * numbers have changed (bottom 5 bits).  The next octet is a
 * conversation number that associates a saved IP/TCP header with
 * the compressed packet.  The next two octets are the TCP checksum
 * from the original datagram.  The next 0 to 15 octets are
 * sequence number changes, one change per bit set in the header
 * (there may be no changes and there are two special cases where
 * the receiver implicitly knows what changed -- see below).
 *
 * There are 5 numbers which can change (they are always inserted
 * in the following order): TCP urgent pointer, window,
 * acknowlegement, sequence number and IP ID.  (The urgent pointer
 * is different from the others in that its value is sent, not the
 * change in value.)  Since typical use of SLIP links is biased
 * toward small packets (see comments on MTU/MSS below), changes
 * use a variable length coding with one octet for numbers in the
 * range 1 - 255 and 3 octets (0, MSB, LSB) for numbers in the
 * range 256 - 65535 or 0.  (If the change in sequence number or
 * ack is more than 65535, an uncompressed packet is sent.)
 */

/*
 * Packet types (must not conflict with IP protocol version)
 *
 * The top nibble of the first octet is the packet type.  There are
 * three possible types: IP (not proto TCP or tcp with one of the
 * control flags set); uncompressed TCP (a normal IP/TCP packet but
 * with the 8-bit protocol field replaced by an 8-bit connection id --
 * this type of packet syncs the sender & receiver); and compressed
 * TCP (described above).
 *
 * LSB of 4-bit field is TCP "PUSH" bit (a worthless anachronism) and
 * is logically part of the 4-bit "changes" field that follows.  Top
 * three bits are actual packet type.  For backward compatibility
 * and in the interest of conserving bits, numbers are chosen so the
 * IP protocol version number (4) which normally appears in this nibble
 * means "IP packet".
 */

/* packet types */
constexpr auto kTypeIp = 0x40;
constexpr auto kTypeUncompressedTcp = 0x70;
constexpr auto kTypeCompressedTcp = 0x80;
constexpr auto kTypeError = 0x00;

/* Bits in first octet of compressed packet */
constexpr auto kNewC = 0x40 /* flag bits for what changed in a packet */;
constexpr auto kNewI = 0x20;
constexpr auto kNewS = 0x08;
constexpr auto kNewA = 0x04;
constexpr auto kNewW = 0x02;
constexpr auto kNewU = 0x01;

/* reserved, special-case values of above */
#define SPECIAL_I (NEW_S|NEW_W|NEW_U) /* echoed interactive traffic */
#define SPECIAL_D (NEW_S|NEW_A|NEW_W|NEW_U) /* unidirectional data */
#define SPECIALS_MASK (NEW_S|NEW_A|NEW_W|NEW_U)

constexpr auto kTcpPushBit = 0x10;


/*
 * "state" data for each active tcp conversation on the wire.  This is
 * basically a copy of the entire IP/TCP header from the last packet
 * we saw from the conversation together with a small identifier
 * the transmit & receive ends of the line use to locate saved header.
 */
struct Cstate {
  struct Cstate *cs_next; /* next most recently used state (xmit only) */
  uint16_t cs_hlen;        /* size of hdr (receive only) */
  uint8_t cs_id;           /* connection # associated with this state */
  uint8_t cs_filler;
  union {
    char csu_hdr[kMaxHdr];
    struct Ip4Hdr csu_ip;     /* ip/tcp hdr from most recent packet */
  } vjcs_u;
};
#define CS_IP vjcs_u.csu_ip
#define CS_HDR vjcs_u.csu_hdr


struct Vjstat {
  uint32_t vjs_packets;        /* outbound packets */
  uint32_t vjs_compressed;     /* outbound compressed packets */
  uint32_t vjs_searches;       /* searches for connection state */
  uint32_t vjs_misses;         /* times couldn't find conn. state */
  uint32_t vjs_uncompressedin; /* inbound uncompressed packets */
  uint32_t vjs_compressedin;   /* inbound compressed packets */
  uint32_t vjs_errorin;        /* inbound unknown type packets */
  uint32_t vjs_tossed;         /* inbound packets tossed because of error */
};

/*
 * all the state data for one serial line (we need one of these per line).
 */
struct vjcompress {
  struct cstate *last_cs;          /* most recently used tstate */
  uint8_t last_recv;                /* last rcvd conn. id */
  uint8_t last_xmit;                /* last sent conn. id */
  uint16_t flags;
  uint8_t maxSlotIndex;
  uint8_t compressSlot;             /* Flag indicating OK to compress slot ID. */

  struct Cstate tstate[MAX_SLOTS]; /* xmit connection states */
  struct Cstate rstate[MAX_SLOTS]; /* receive connection states */
};

/* flag values */
constexpr auto kVjfToss = 1U /* tossing rcvd frames because of input err */;

extern void  vj_compress_init    (struct vjcompress *comp);
extern uint8_t  vj_compress_tcp     (struct vjcompress *comp, struct PacketBuffer **pb);
extern void  vj_uncompress_err   (struct vjcompress *comp);
extern int   vj_uncompress_uncomp(struct PacketBuffer *nb, struct vjcompress *comp);
extern int   vj_uncompress_tcp   (struct PacketBuffer **nb, struct vjcompress *comp);

#ifdef __cplusplus
}
#endif
