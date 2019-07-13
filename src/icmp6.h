/**
 * @file
 *
 * IPv6 version of ICMP, as per RFC 4443.
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

#include "opt.h"
#include "packet_buffer.h"
#include "ip6_addr.h"
#include "netif.h"
#include "arch.h"

#ifdef __cplusplus
extern "C" {
#endif

// ICMP TYPE
enum Icmp6Type
{
    /** Destination unreachable */
    ICMP6_TYPE_DUR = 1,
    /** Packet too big */
    ICMP6_TYPE_PTB = 2,
    /** Time exceeded */
    ICMP6_TYPE_TE = 3,
    /** Parameter problem */
    ICMP6_TYPE_PP = 4,
    /** Private experimentation */
    ICMP6_TYPE_PE1 = 100,
    /** Private experimentation */
    ICMP6_TYPE_PE2 = 101,
    /** Reserved for expansion of error messages */
    ICMP6_TYPE_RSV_ERR = 127,
    /** Echo request */
    ICMP6_TYPE_EREQ = 128,
    /** Echo reply */
    ICMP6_TYPE_EREP = 129,
    /** Multicast listener query */
    ICMP6_TYPE_MLQ = 130,
    /** Multicast listener report */
    ICMP6_TYPE_MLR = 131,
    /** Multicast listener done */
    ICMP6_TYPE_MLD = 132,
    /** Router solicitation */
    ICMP6_TYPE_RS = 133,
    /** Router advertisement */
    ICMP6_TYPE_RA = 134,
    /** Neighbor solicitation */
    ICMP6_TYPE_NS = 135,
    /** Neighbor advertisement */
    ICMP6_TYPE_NA = 136,
    /** Redirect */
    ICMP6_TYPE_RD = 137,
    /** Multicast router advertisement */
    ICMP6_TYPE_MRA = 151,
    /** Multicast router solicitation */
    ICMP6_TYPE_MRS = 152,
    /** Multicast router termination */
    ICMP6_TYPE_MRT = 153,
    /** Private experimentation */
    ICMP6_TYPE_PE3 = 200,
    /** Private experimentation */
    ICMP6_TYPE_PE4 = 201,
    /** Reserved for expansion of informational messages */
    ICMP6_TYPE_RSV_INF = 255
};

// ICMP destination unreachable codes
enum Icmp6DurCode {
    /** No route to destination */
    ICMP6_DUR_NO_ROUTE = 0,
    /** Communication with destination administratively prohibited */
    ICMP6_DUR_PROHIBITED = 1,
    /** Beyond scope of source address */
    ICMP6_DUR_SCOPE = 2,
    /** Address unreachable */
    ICMP6_DUR_ADDRESS = 3,
    /** Port unreachable */
    ICMP6_DUR_PORT = 4,
    /** Source address failed ingress/egress policy */
    ICMP6_DUR_POLICY = 5,
    /** Reject route to destination */
    ICMP6_DUR_REJECT_ROUTE = 6
};

// ICMP time exceeded codes
enum Icmp6TeCode
{
    /** Hop limit exceeded in transit */
    ICMP6_TE_HL = 0,
    /** Fragment reassembly time exceeded */
    ICMP6_TE_FRAG = 1
}; 
    
// ICMP parameter code
enum Icmp6PpCode
{
    /** Erroneous header field encountered */
    ICMP6_PP_FIELD = 0,
    /** Unrecognized next header type encountered */
    ICMP6_PP_HEADER = 1,
    /** Unrecognized IPv6 option encountered */
    ICMP6_PP_OPTION = 2
}; 
    
// This is the standard ICMP6 header.
struct Icmp6Hdr
{
    uint8_t type;
    uint8_t code;
    uint16_t chksum;
    uint32_t data;
}; 
    
    
// This is the ICMP6 header adapted for echo req/resp.
struct Icmp6EchoHdr
{
    uint8_t type;
    uint8_t code;
    uint16_t chksum;
    uint16_t id;
    uint16_t seqno;
};

void icmp6_input(struct PacketBuffer *p, NetIfc*inp);
void icmp6_dest_unreach(struct PacketBuffer *p, enum Icmp6DurCode c);
void icmp6_packet_too_big(struct PacketBuffer *p, uint32_t mtu);
void icmp6_time_exceeded(struct PacketBuffer *p, enum Icmp6TeCode c);
void icmp6_time_exceeded_with_addrs(struct PacketBuffer *p, enum Icmp6TeCode c,
                                    const Ip6Addr *src_addr,
                                    const Ip6Addr *dest_addr);
void icmp6_param_problem(struct PacketBuffer *p, enum Icmp6PpCode c, const void *pointer);

#ifdef __cplusplus
}
#endif
