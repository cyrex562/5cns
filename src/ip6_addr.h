//
// file: ip6_addr.h
//
#pragma once

#include <def.h>
#include <lwip_debug.h>

//
//
//
enum Ip6Zone : uint8_t
{
    IP6_NO_ZONE = 0,
};

// This is the aligned version of Ip6Addr used as local variable, on the stack, etc.
struct Ip6Addr
{
    uint32_t addr[4];
    Ip6Zone zone;
};

// struct Ip6HdrAddr
// {
//     uint32_t addr[4];  // NOLINT(cppcoreguidelines-avoid-c-arrays)
// };

struct Ip6AddrWireFmt
{
    uint32_t addr[4];
};

/** Symbolic constants for the 'type' parameters in some of the macros.
 * These exist for efficiency only, allowing the macros to avoid certain tests
 * when the address is known not to be of a certain type. Dead code elimination
 * will do the rest. IP6_MULTICAST is supported but currently not optimized.
 * @see ip6_addr_has_scope, ip6_addr_assign_zone, ip6_addr_lacks_zone.
 */
enum Ip6ScopeTypes {
  /** Unknown */
  IP6_UNKNOWN = 0,
  /** Unicast */
  IP6_UNICAST = 1,
  /** Multicast */
  IP6_MULTICAST = 2
};

/** Identifier for "no zone". */

/** Return the zone index of the given IPv6 address; possibly "no zone". */
inline Ip6Zone ip6_addr_zone(const Ip6Addr& ip6_addr)
{
    return ip6_addr.zone;
}

/** Does the given IPv6 address have a zone set? (0/1) */
inline bool ip6_addr_has_zone(const Ip6Addr& ip6addr)
{
    return ip6addr.zone != IP6_NO_ZONE;
    // return (ip6_addr_zone(ip6addr) != IP6_NO_ZONE);
}

/** Set the zone field of an IPv6 address to a particular value. */
inline void ip6_addr_set_zone(Ip6Addr& ip6addr, int zone_idx) {
  ip6addr.zone = Ip6Zone(zone_idx);
}

/** Clear the zone field of an IPv6 address, setting it to "no zone". */
inline void ip6_addr_clear_zone(Ip6Addr& ip6_addr) {
  ip6_addr.zone = IP6_NO_ZONE;
}

/** Is the zone field of the given IPv6 address equal to the given zone index?
 * (0/1) */
inline bool ip6_addr_equals_zone(const Ip6Addr& ip6addr, const int zone_idx)
{
    return ip6addr.zone == zone_idx;
}

/** Are the zone fields of the given IPv6 addresses equal? (0/1)
 * This macro must only be used on IPv6 addresses of the same scope. */
inline bool ip6_addr_cmp_zone(const Ip6Addr& ip6_addr1, const Ip6Addr& ip6_addr2)
{
    return ip6_addr1.zone == ip6_addr2.zone;
}

/** IPV6_CUSTOM_SCOPES: together, the following three macro definitions,
 * @ref ip6_addr_has_scope, @ref ip6_addr_assign_zone, and
 * @ref LwipIp6Addrest_zone, completely define the lwIP scoping policy.
 * The definitions below implement the default policy from RFC 4007 Sec. 6.
 * Should an implementation desire to implement a different policy, it can
 * define IPV6_CUSTOM_SCOPES to 1 and supply its own definitions for the three
 * macros instead.
 */

constexpr auto IPV6_CUSTOM_SCOPES = 0;

inline bool ip6_addr_islinklocal(const Ip6Addr& ip6_addr)
{
    return (ip6_addr.addr[0] & pp_htonl(0xffc00000UL)) == pp_htonl(0xfe800000UL);
}

inline bool ip6_addr_ismulticast_iflocal(const Ip6Addr& ip6addr)
{
    return (ip6addr.addr[0] & pp_htonl(0xff8f0000UL)) == pp_htonl(0xff010000UL);
}

inline bool ip6_addr_ismulticast_linklocal(const Ip6Addr& ip6addr)
{
    return (ip6addr.addr[0] & pp_htonl(0xff8f0000UL)) == pp_htonl(0xff020000UL);
}

/**
 * Determine whether an IPv6 address has a constrained scope, and as such is
 * meaningful only if accompanied by a zone index to identify the scope's zone.
 * The given address type may be used to eliminate at compile time certain
 * checks that will evaluate to false at run time anyway.
 *
 * This default implementation follows the default model of RFC 4007, where
 * only interface-local and link-local scopes are defined.
 *
 * Even though the unicast loopback address does have an implied link-local
 * scope, in this implementation it does not have an explicitly assigned zone
 * index. As such it should not be tested for in this macro.
 *
 * @param ip6_addr the IPv6 address (const); only its address part is examined.
 * @param type address type; see @ref lwip_ipv6_scope_type.
 * @return 1 if the address has a constrained scope, 0 if it does not.
 */
inline bool ip6_addr_has_scope(const Ip6Addr& ip6_addr, const Ip6ScopeTypes type)
{
    return ip6_addr_islinklocal(ip6_addr) || type != IP6_UNICAST && (
        ip6_addr_ismulticast_iflocal(ip6_addr) || ip6_addr_ismulticast_linklocal(ip6_addr)
    );
}

/**
 * Test whether an IPv6 address is "zone-compatible" with a network interface.
 * That is, test whether the network interface is part of the zone associated
 * with the address. For efficiency, this macro is only ever called if the
 * given address is either scoped or zoned, and thus, it need not test this.
 * If an address is scoped but not zoned, or zoned and not scoped, it is
 * considered not zone-compatible with any netif.
 *
 * This default implementation follows the default model of RFC 4007, where
 * only interface-local and link-local scopes are defined, and the zone index
 * of both of those scopes always equals the index of the network interface.
 * As such, there is always only one matching netif for a specific zone index,
 * but all call sites of this macro currently support multiple matching netifs
 * as well (at no additional expense in the common case).
 *
 * @param ip6addr the IPv6 address (const).
 * @param netif the network interface (const).
 * @return 1 if the address is scope-compatible with the netif, 0 if not.
 */


/** Does the given IPv6 address have a scope, and as such should also have a
 * zone to be meaningful, but does not actually have a zone? (0/1) */
inline bool ip6_addr_lacks_zone(const Ip6Addr& ip6addr, const Ip6ScopeTypes type)
{
    return
        !ip6_addr_has_zone(ip6addr) && ip6_addr_has_scope(ip6addr, type);
}

/**
 * Try to select a zone for a scoped address that does not yet have a zone.
 * Called from PCB bind and connect routines, for two reasons: 1) to save on
 * this (relatively expensive) selection for every individual packet route
 * operation and 2) to allow the application to obtain the selected zone from
 * the PCB as is customary for e.g. getsockname/getpeername BSD socket calls.
 *
 * Ideally, callers would always supply a properly zoned address, in which case
 * this function would not be needed. It exists both for compatibility with the
 * BSD socket API (which accepts zoneless destination addresses) and for
 * backward compatibility with pre-scoping lwIP code.
 *
 * It may be impossible to select a zone, e.g. if there are no netifs.  In that
 * case, the address's zone field will be left as is.
 *
 * @param dest the IPv6 address for which to select and set a zone.
 * @param src source IPv6 address (const); may be equal to dest.
 */


///
/// Verify that the given IPv6 address is properly zoned.
///
inline void ip6_addr_zonecheck(const Ip6Addr& ip6_addr) {
  lwip_assert(
      "IPv6 zone check failed",
      ip6_addr_has_scope(ip6_addr, IP6_UNKNOWN) == ip6_addr_has_zone(ip6_addr));
}

inline bool ip6_addr_cmp_zoneless(const Ip6Addr& addr1, const Ip6Addr& addr2) {
  return addr1.addr[0] == addr2.addr[0] &&
      addr1.addr[1] == addr2.addr[1] &&
      addr1.addr[2] == addr2.addr[2] &&
      addr1.addr[3] == addr2.addr[3];
}

inline bool ip6_addr_cmp(const Ip6Addr& addr1, const Ip6Addr& addr2) {
  return ip6_addr_cmp_zoneless(addr1, addr2) &&
      ip6_addr_cmp_zone(addr1, addr2);
}

/** Set an IPv6 partial address given by byte-parts */
inline void set_ip6_addr_part(Ip6Addr* ip6_addr, const size_t index, const uint32_t a, const uint32_t b,
                              const uint32_t c,
                              const uint32_t d)
{
    ip6_addr->addr[index] = pp_htonl(make_u32(a, b, c, d));
}

/** Set a full IPv6 address by passing the 4 uint32_t indices in network byte
   order (use pp_htonl() for constants) */
inline void set_ip6_addr(Ip6Addr& ip6_addr, const uint32_t idx0, const uint32_t idx1,
                     const uint32_t idx2, const uint32_t idx3)
{
    ip6_addr.addr[0] = idx0;
    ip6_addr.addr[1] = idx1;
    ip6_addr.addr[2] = idx2;
    ip6_addr.addr[3] = idx3;
    ip6_addr_clear_zone(ip6_addr);
}

/** Access address in 16-bit block */
inline uint16_t IP6_ADDR_BLOCK1(const Ip6Addr* ip6_addr)
{
    return (uint16_t)(lwip_htonl(ip6_addr->addr[0]) >> 16 & 0xffff);
}


/** Access address in 16-bit block */
inline uint16_t IP6_ADDR_BLOCK2(const Ip6Addr* ip6_addr)
{
    return (uint16_t)(lwip_htonl(ip6_addr->addr[0]) & 0xffff);
}


/** Access address in 16-bit block */
inline uint16_t IP6_ADDR_BLOCK3(const Ip6Addr* ip6_addr)
{
    return (uint16_t)(lwip_htonl(ip6_addr->addr[1]) >> 16 & 0xffff);
}


/** Access address in 16-bit block */
inline uint16_t IP6_ADDR_BLOCK4(const Ip6Addr* ip6_addr)
{
    return (uint16_t)(lwip_htonl(ip6_addr->addr[1]) & 0xffff);
}


/** Access address in 16-bit block */
inline uint16_t IP6_ADDR_BLOCK5(const Ip6Addr* ip6_addr)
{
    return (uint16_t)(lwip_htonl(ip6_addr->addr[2]) >> 16 & 0xffff);
}


/** Access address in 16-bit block */
inline uint16_t IP6_ADDR_BLOCK6(const Ip6Addr* ip6_addr)
{
    return (uint16_t)(lwip_htonl(ip6_addr->addr[2]) & 0xffff);
}


/** Access address in 16-bit block */
inline uint16_t IP6_ADDR_BLOCK7(const Ip6Addr* ip6_addr)
{
    return (uint16_t)(lwip_htonl(ip6_addr->addr[3]) >> 16 & 0xffff);
}


/** Access address in 16-bit block */
inline uint16_t IP6_ADDR_BLOCK8(const Ip6Addr* ip6_addr)
{
    return (uint16_t)(lwip_htonl(ip6_addr->addr[3]) & 0xffff);
}



/** Safely copy one IPv6 address to another (src may be NULL) */
inline void ip6_addr_set(Ip6Addr& dest, const Ip6Addr& src)
{
    dest.addr[0] = src.addr[0];
    dest.addr[1] = src.addr[1];
    dest.addr[2] = src.addr[2];
    dest.addr[3] = src.addr[3];
    ip6_addr_set_zone(dest, src.zone);
}

/** Copy packed IPv6 address to unpacked IPv6 address; zone is not set */
inline void ip6_addr_copy_from_packed(Ip6Addr& dest, const Ip6AddrWireFmt& src)
{
    dest.addr[0] = src.addr[0];
    dest.addr[1] = src.addr[1];
    dest.addr[2] = src.addr[2];
    dest.addr[3] = src.addr[3];
    ip6_addr_clear_zone(dest);
}

/** Copy unpacked IPv6 address to packed IPv6 address; zone is lost */
inline void ip6_addr_copy_to_packed(Ip6AddrWireFmt* dest, const Ip6Addr* src)
{
    dest->addr[0] = src->addr[0];
    dest->addr[1] = src->addr[1];
    dest->addr[2] = src->addr[2];
    dest->addr[3] = src->addr[3];
}

/** Set complete address to zero */
inline void ip6_addr_set_zero(Ip6Addr& ip6_addr)
{
    ip6_addr.addr[0] = 0;
    ip6_addr.addr[1] = 0;
    ip6_addr.addr[2] = 0;
    ip6_addr.addr[3] = 0;
    ip6_addr.zone = IP6_NO_ZONE;
}

/** Set address to ipv6 'any' (no need for lwip_htonl()) */
inline void ip6_addr_set_any(Ip6Addr& ip6_addr)
{
    ip6_addr_set_zero(ip6_addr);
}

/** Set address to ipv6 loopback address */
inline void ip6_addr_set_loopback(Ip6Addr& ip6_addr)
{
    ip6_addr.addr[0] = 0;
    ip6_addr.addr[1] = 0;
    ip6_addr.addr[2] = 0;
    ip6_addr.addr[3] = pp_htonl(0x00000001UL);
    ip6_addr_clear_zone(ip6_addr);
}

/// Safely copy one IPv6 address to another and change byte order from host- to network-order.
inline void ip6_addr_set_hton(Ip6Addr& dest, Ip6Addr& src)
{
    dest.addr[0] = lwip_htonl(src.addr[0]);
    dest.addr[1] = lwip_htonl(src.addr[1]);
    dest.addr[2] = lwip_htonl(src.addr[2]);
    dest.addr[3] = lwip_htonl(src.addr[3]);
    ip6_addr_set_zone(dest, src.zone);
}

/** Compare IPv6 networks, ignoring zone information. To be used sparingly! */
inline bool ip6_addr_netcmp_zoneless(const Ip6Addr& addr1, const Ip6Addr& addr2)
{
    return addr1.addr[0] == addr2.addr[0] && addr1.addr[1] == addr2.addr[1];
}

/**
 * Determine if two IPv6 address are on the same network.
 *
 * @param addr1 IPv6 address 1
 * @param addr2 IPv6 address 2
 * @return 1 if the network identifiers of both address match, 0 if not
 */
inline bool ip6_addr_netcmp(const Ip6Addr& addr1, const Ip6Addr& addr2)
{
    return ip6_addr_netcmp_zoneless(addr1, addr2) && ip6_addr_cmp_zone(addr1, addr2);
}

/* Exact-host comparison *after* ip6_addr_netcmp() succeeded, for efficiency. */
inline bool ip6_addr_nethostcmp(const Ip6Addr* addr1, const Ip6Addr* addr2)
{
    return addr1->addr[2] == addr2->addr[2] && addr1->addr[3] == addr2->addr[3
    ];
}

///
/// Compare IPv6 address to packed address and zone
/// 
inline bool ip6_addr_cmp_packed(const Ip6Addr& ip6_addr,
                                const Ip6AddrWireFmt& paddr,
                                const unsigned zone_idx)
{
    return ip6_addr.addr[0] == paddr.addr[0] && ip6_addr.addr[1] == paddr.addr[1] &&
        ip6_addr.addr[2] == paddr.addr[2] && ip6_addr.addr[3] == paddr.addr[3] &&
        ip6_addr_equals_zone(ip6_addr, zone_idx);
}

inline uint32_t ip6_get_subnet_id(Ip6Addr& ip6addr)
{
    return lwip_htonl(ip6addr.addr[2]) & 0x0000ffffUL;
}

inline bool ip6_addr_isany_val(const Ip6Addr ip6_addr) {
  return ip6_addr.addr[0] == 0 && ip6_addr.addr[1] == 0 &&
      ip6_addr.addr[2] == 0 && ip6_addr.addr[3] == 0;
}

inline bool is_ip6_addr_any(const Ip6Addr& ip6_addr) {
  return ip6_addr_isany_val(ip6_addr);
}

inline bool ip6_addr_isloopback(const Ip6Addr& ip6_addr) {
  return ip6_addr.addr[0] == 0UL && ip6_addr.addr[1] == 0UL &&
      ip6_addr.addr[2] == 0UL &&
      ip6_addr.addr[3] == pp_htonl(0x00000001UL);
}

inline bool ip6_addr_isglobal(const Ip6Addr* ip6_addr)
{
    return (ip6_addr->addr[0] & pp_htonl(0xe0000000UL)) == pp_htonl(0x20000000UL);
}

inline bool ip6_addr_issitelocal(const Ip6Addr* ip6_addr)
{
    return (ip6_addr->addr[0] & pp_htonl(0xffc00000UL)) == pp_htonl(0xfec00000UL);
}

inline bool ip6_addr_isuniquelocal(const Ip6Addr* ip6_addr)
{
    return (ip6_addr->addr[0] & pp_htonl(0xfe000000UL)) == pp_htonl(0xfc000000UL);
}

inline bool ip6_addr_isipv4mappedipv6(const Ip6Addr& ip6_addr)
{
    return ip6_addr.addr[0] == 0 && ip6_addr.addr[1] == 0 && ip6_addr.addr[2] == pp_htonl(0x0000FFFFUL);
}

inline bool ip6_addr_ismulticast(const Ip6Addr& ip6_addr) {
  return (ip6_addr.addr[0] & pp_htonl(0xff000000UL)) == pp_htonl(0xff000000UL);
}

inline uint32_t ip6_addr_multicast_transient_flag(const Ip6Addr& ip6addr)
{
    return ip6addr.addr[0] & pp_htonl(0x00100000UL);
}

inline uint32_t ip6_addr_multicast_prefix_flag(const Ip6Addr* ip6addr)
{
    return ip6addr->addr[0] & pp_htonl(0x00200000UL);
}

inline uint32_t ip6_addr_multicast_rendezvous_flag(const Ip6Addr* ip6addr)
{
    return ip6addr->addr[0] & pp_htonl(0x00400000UL);
}

inline uint32_t ip6_addr_multicast_scope(const Ip6Addr* ip6addr)
{
    return lwip_htonl(ip6addr->addr[0]) >> 16 & 0xf;
}

enum Ip6MulticastScopes: uint8_t
{
    IP6_MULTICAST_SCOPE_RESERVED =0x0,
    IP6_MULTICAST_SCOPE_RESERVED0 =0x0,
    IP6_MULTICAST_SCOPE_INTERFACE_LOCAL =0x1,
    IP6_MULTICAST_SCOPE_LINK_LOCAL =0x2,
    IP6_MULTICAST_SCOPE_RESERVED3 =0x3,
    IP6_MULTICAST_SCOPE_ADMIN_LOCAL =0x4,
    IP6_MULTICAST_SCOPE_SITE_LOCAL =0x5,
    IP6_MULTICAST_SCOPE_ORGANIZATION_LOCAL =0x8,
    IP6_MULTICAST_SCOPE_GLOBAL =0xe,
    IP6_MULTICAST_SCOPE_RESERVEDF =0xf,
};


inline bool ip6_addr_ismulticast_adminlocal(Ip6Addr& ip6addr)
{
    return (ip6addr.addr[0] & pp_htonl(0xff8f0000UL)) == pp_htonl(0xff040000UL);
}


inline bool ip6_addr_ismulticast_sitelocal(Ip6Addr& ip6addr)
{
    return (ip6addr.addr[0] & pp_htonl(0xff8f0000UL)) == pp_htonl(0xff050000UL);
}


inline bool ip6_addr_ismulticast_orglocal(Ip6Addr& ip6addr)
{
    return (ip6addr.addr[0] & pp_htonl(0xff8f0000UL)) == pp_htonl(0xff080000UL);
}


inline bool ip6_addr_ismulticast_global(Ip6Addr& ip6addr)
{
    return (ip6addr.addr[0] & pp_htonl(0xff8f0000UL)) == pp_htonl(0xff0e0000UL);
}

/* Scoping note: while interface-local and link-local multicast addresses do
 * have a scope (i.e., they are meaningful only in the context of a particular
 * interface), the following functions are not assigning or comparing zone
 * indices. The reason for this is backward compatibility. Any call site that
 * produces a non-global multicast address must assign a multicast address as
 * appropriate itself. */
inline bool ip6_addr_isallnodes_iflocal(Ip6Addr& ip6addr)
{
    return ip6addr.addr[0] == pp_htonl(0xff010000UL) && ip6addr.addr[1] == 0UL
        && ip6addr.addr[2] == 0UL && ip6addr.addr[3] == pp_htonl(0x00000001UL);
}

inline bool ip6_addr_isallnodes_linklocal(Ip6Addr& ip6addr)
{
    return ip6addr.addr[0] == pp_htonl(0xff020000UL) && ip6addr.addr[1] == 0UL
        && ip6addr.addr[2] == 0UL && ip6addr.addr[3] == pp_htonl(0x00000001UL);
}

inline void ip6_addr_set_allnodes_linklocal(Ip6Addr& ip6addr)
{
    ip6addr.addr[0] = pp_htonl(0xff020000UL);
    ip6addr.addr[1] = 0;
    ip6addr.addr[2] = 0;
    ip6addr.addr[3] = pp_htonl(0x00000001UL);
    ip6_addr_clear_zone(ip6addr);
}

inline bool ip6_addr_isallrouters_linklocal(Ip6Addr& ip6addr)
{
    return ip6addr.addr[0] == pp_htonl(0xff020000UL) && ip6addr.addr[1] == 0UL
        && ip6addr.addr[2] == 0UL && ip6addr.addr[3] == pp_htonl(0x00000002UL);
}

inline void ip6_addr_set_allrouters_linklocal(Ip6Addr& ip6addr)
{
    ip6addr.addr[0] = pp_htonl(0xff020000UL);
    ip6addr.addr[1] = 0;
    ip6addr.addr[2] = 0;
    ip6addr.addr[3] = pp_htonl(0x00000002UL);
    ip6_addr_clear_zone(ip6addr);
}

inline bool ip6_addr_issolicitednode(Ip6Addr& ip6addr)
{
    return ip6addr.addr[0] == pp_htonl(0xff020000UL) && ip6addr.addr[2] ==
        pp_htonl(0x00000001UL) && (ip6addr.addr[3] & pp_htonl(0xff000000UL)) ==
        pp_htonl(0xff000000UL);
}

inline void ip6_addr_set_solicitednode(Ip6Addr& ip6addr, uint32_t if_id)
{
    ip6addr.addr[0] = pp_htonl(0xff020000UL);
    ip6addr.addr[1] = 0;
    ip6addr.addr[2] = pp_htonl(0x00000001UL);
    ip6addr.addr[3] = pp_htonl(0xff000000UL) | if_id;
    ip6_addr_clear_zone(ip6addr);
}

inline bool ip6_addr_cmp_solicitednode(Ip6Addr* ip6addr, Ip6Addr* sn_addr)
{
    return ip6addr->addr[0] == pp_htonl(0xff020000UL) && ip6addr->addr[1] == 0 &&
        ip6addr->addr[2] == pp_htonl(0x00000001UL) && ip6addr->addr[3] == (
            pp_htonl(0xff000000UL) | sn_addr->addr[3]);
}

/* IPv6 address states. */
enum Ip6AddrStates
{
    IP6_ADDR_INVALID = 0x00,
    IP6_ADDR_TENTATIVE = 0x08,
    IP6_ADDR_TENTATIVE_1 = 0x09,
    IP6_ADDR_TENTATIVE_2 = 0x0,
    IP6_ADDR_TENTATIVE_3 = 0x0,
    IP6_ADDR_TENTATIVE_4 = 0x0,
    IP6_ADDR_TENTATIVE_5 = 0x0,
    IP6_ADDR_TENTATIVE_6 = 0x0,
    IP6_ADDR_TENTATIVE_7 = 0x0,
    IP6_ADDR_VALID = 0x10,
    /* This bit marks an address as valid (preferred or deprecated) */
    IP6_ADDR_PREFERRED = 0x30,
    IP6_ADDR_DEPRECATED = 0x10,
    /* Same as VALID (valid but not preferred) */
    IP6_ADDR_DUPLICATED = 0x40,
    /* Failed DAD test, not valid */
    IP6_ADDR_TENTATIVE_COUNT_MASK = 0x07,
    /* 1-7 probes sent */
};

inline bool ip6_addr_isinvalid(Ip6AddrStates addr_state)
{
    return addr_state == IP6_ADDR_INVALID;
}

inline bool ip6_addr_istentative(Ip6AddrStates addr_state)
{
    return addr_state & IP6_ADDR_TENTATIVE;
}

//
//
//
inline bool ip6_addr_isvalid(Ip6AddrStates addr_state)
{
    return addr_state & IP6_ADDR_VALID;
}


// Include valid, preferred, and deprecated.
inline bool ip6_addr_ispreferred(Ip6AddrStates addr_state)
{
    return addr_state == IP6_ADDR_PREFERRED;
}

inline bool ip6_addr_isdeprecated(Ip6AddrStates addr_state)
{
    return addr_state == IP6_ADDR_DEPRECATED;
}

inline bool ip6_addr_isduplicated(Ip6AddrStates addr_state)
{
    return addr_state == IP6_ADDR_DUPLICATED;
}

constexpr auto IP6_ADDR_LIFE_INFINITE = 0xffffffffUL;

inline bool ip6_addr_life_isstatic(uint32_t addr_life)
{
    return addr_life == 0;
}

inline bool ip6_addr_life_isinfinite(uint32_t addr_life)
{
    return addr_life == IP6_ADDR_LIFE_INFINITE;
}


constexpr auto IP6ADDR_STRLEN_MAX = 46;

int ip6addr_aton(const char* cp, const Ip6Addr* addr);
/** returns ptr to static buffer; not reentrant! */
std::string ip6addr_ntoa(const Ip6Addr& addr);


std::string ip6addr_ntoa_r(const Ip6Addr& addr, std::string& buf);

inline Ip6Addr make_ip6_addr_any()
{
    return {0,0,0,0,IP6_NO_ZONE};
}

inline void set_ip6_addr_any(Ip6Addr* addr)
{
    addr->addr[0] = 0;
    addr->addr[1] = 0;
    addr->addr[2] = 0;
    addr->addr[3] = 0;
    addr->zone = IP6_NO_ZONE;
}
