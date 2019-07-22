
#include <opt.h>
#include <igmp.h>
#include <def.h>
#include <ip.h>
#include <inet_chksum.h>
#include <netif.h>
#include <stats.h>
#include <cstring>
static struct IgmpGroup* igmp_lookup_group(NetIfc* ifp, const Ip4Addr* addr);
static LwipStatus igmp_remove_group(NetIfc* netif, struct IgmpGroup* group);
static void igmp_timeout(NetIfc* netif, struct IgmpGroup* group);
static void igmp_start_timer(struct IgmpGroup* group, uint8_t max_time);
static void igmp_delaying_member(struct IgmpGroup* group, uint8_t maxresp);
static LwipStatus igmp_ip_output_if(struct PacketBuffer* p,
                                   const Ip4Addr* src,
                                   const Ip4Addr* dest,
                                   NetIfc* netif);
static void igmp_send(NetIfc* netif, struct IgmpGroup* group, uint8_t type);
static Ip4Addr allsystems;
static Ip4Addr allrouters;

/**
 * Initialize the IGMP module
 */
void init_igmp_module(void)
{
    Logf(IGMP_DEBUG, ("igmp_init: initializing\n"));
    Ipv4AddrFromBytes(&allsystems, 224, 0, 0, 1);
    Ipv4AddrFromBytes(&allrouters, 224, 0, 0, 2);
}

/**
 * Start IGMP processing on interface
 *
 * @param netif network interface on which start IGMP processing
 */
LwipStatus igmp_start(NetIfc* netif)
{
    struct IgmpGroup* group;
    // Logf(IGMP_DEBUG, ("igmp_start: starting IGMP processing on if %p\n", (uint8_t *)netif));
    group = igmp_lookup_group(netif, &allsystems);
    if (group != nullptr)
    {
        group->group_state = IGMP_GROUP_IDLE_MEMBER;
        group->use++; /* Allow the igmp messages at the MAC level */
        if (netif->igmp_mac_filter != nullptr)
        {
            Logf(IGMP_DEBUG, ("igmp_start: igmp_mac_filter(ADD "));
            // ip4_addr_debug_print_val(IGMP_DEBUG, allsystems);
            // Logf(IGMP_DEBUG, (") on if %p\n", (uint8_t *)netif));
            netif->igmp_mac_filter(netif, &allsystems, NETIF_ADD_MAC_FILTER);
        }
        return ERR_OK;
    }
    return ERR_MEM;
}

/**
 * Stop IGMP processing on interface
 *
 * @param netif network interface on which stop IGMP processing
 */
LwipStatus igmp_stop(NetIfc* netif)
{
    IgmpGroup* group = netif_igmp_data(netif);
    netif->client_data[LWIP_NETIF_CLIENT_DATA_INDEX_IGMP] = nullptr;
    while (group != nullptr)
    {
        struct IgmpGroup* next = group->next; /* avoid use-after-free below */
        /* disable the group at the MAC level */
        if (netif->igmp_mac_filter != nullptr)
        {
            Logf(IGMP_DEBUG, ("igmp_stop: igmp_mac_filter(DEL "));
            // ip4_addr_debug_print_val(IGMP_DEBUG, group->group_address);
            // Logf(IGMP_DEBUG, (") on if %p\n", (uint8_t *)netif));
            netif->igmp_mac_filter(netif, &(group->group_address), NETIF_DEL_MAC_FILTER);
        } /* free group */ // memp_free(MEMP_IGMP_GROUP, group);
        delete group; /* move to "next" */
        group = next;
    }
    return ERR_OK;
}

/**
 * Report IGMP memberships for this interface
 *
 * @param netif network interface on which report IGMP memberships
 */
void igmp_report_groups(NetIfc* netif)
{
    struct IgmpGroup* group = netif_igmp_data(netif);
    // Logf(IGMP_DEBUG, ("igmp_report_groups: sending IGMP reports on if %p\n", (uint8_t *)netif));
    /* Skip the first group in the list, it is always the allsystems group added in igmp_start() */
    if (group != nullptr)
    {
        group = group->next;
    }
    while (group != nullptr)
    {
        igmp_delaying_member(group, IGMP_JOIN_DELAYING_MEMBER_TMR);
        group = group->next;
    }
}

/**
 * Search for a group in the netif's igmp group list
 *
 * @param ifp the network interface for which to look
 * @param addr the group ip address to search for
 * @return a struct igmp_group* if the group has been found,
 *         NULL if the group wasn't found.
 */
struct IgmpGroup* igmp_lookfor_group(NetIfc* ifp, const Ip4Addr* addr)
{
    struct IgmpGroup* group = netif_igmp_data(ifp);
    while (group != nullptr)
    {
        if (ip4_addr_cmp(&(group->group_address), addr))
        {
            return group;
        }
        group = group->next;
    } /* to be clearer, we return NULL here instead of
   * 'group' (which is also NULL at this point).
   */
    return nullptr;
}

/**
 * Search for a specific igmp group and create a new one if not found-
 *
 * @param ifp the network interface for which to look
 * @param addr the group ip address to search
 * @return a struct igmp_group*,
 *         NULL on memory error.
 */
static struct IgmpGroup* igmp_lookup_group(NetIfc* ifp, const Ip4Addr* addr)
{
    auto list_head = netif_igmp_data(ifp);
    /* Search if the group already exists */
    auto group = igmp_lookfor_group(ifp, addr);
    if (group != nullptr)
    {
        /* Group already exists. */
        return group;
    } /* Group doesn't exist yet, create a new one */
    // group = (struct IgmpGroup *)memp_malloc(MEMP_IGMP_GROUP);
    group = new IgmpGroup;
    if (group != nullptr)
    {
        ip4_addr_set(&(group->group_address), addr);
        group->timer = 0; /* Not running */
        group->group_state = IGMP_GROUP_NON_MEMBER;
        group->last_reporter_flag = 0;
        group->use = 0; /* Ensure allsystems group is always first in list */
        if (list_head == nullptr)
        {
            /* this is the first entry in linked list */
            lwip_assert("igmp_lookup_group: first group must be allsystems",
                        (ip4_addr_cmp(addr, &allsystems) != 0));
            group->next = nullptr;
            ifp->client_data[LWIP_NETIF_CLIENT_DATA_INDEX_IGMP] = group;
        }
        else
        {
            /* append _after_ first entry */
            lwip_assert(
                "igmp_lookup_group: all except first group must not be allsystems",
                (ip4_addr_cmp(addr, &allsystems) == 0));
            group->next = list_head->next;
            list_head->next = group;
        }
    }
    Logf(IGMP_DEBUG,
         ("igmp_lookup_group: %sallocated a new group with address ", (
             group ? "" : "impossible to ")));
    // ip4_addr_debug_print(IGMP_DEBUG, addr)
    // ;
    // Logf(IGMP_DEBUG, (" on if %p\n", (uint8_t *)ifp));
    return group;
}

/**
 * Remove a group from netif's igmp group list, but don't free it yet
 *
 * @param group the group to remove from the netif's igmp group list
 * @return ERR_OK if group was removed from the list, an LwipStatus otherwise
 */
static LwipStatus igmp_remove_group(NetIfc* netif, struct IgmpGroup* group)
{
    LwipStatus err = ERR_OK;
    struct IgmpGroup* tmp_group;
    /* Skip the first group in the list, it is always the allsystems group added in igmp_start() */
    for (tmp_group = netif_igmp_data(netif); tmp_group != nullptr; tmp_group = tmp_group->
         next)
    {
        if (tmp_group->next == group)
        {
            tmp_group->next = group->next;
            break;
        }
    } /* Group not found in netif's igmp group list */
    if (tmp_group == nullptr)
    {
        err = ERR_ARG;
    }
    return err;
}

/**
 * Called from ip_input() if a new IGMP packet is received.
 *
 * @param p received igmp packet, p->payload pointing to the igmp header
 * @param inp network interface on which the packet was received
 * @param dest destination ip address of the igmp packet
 */
void
igmp_input(struct PacketBuffer *p, NetIfc*inp, const Ip4Addr *dest)
{
  struct IgmpMsg   *igmp;
  struct IgmpGroup *group;
  struct IgmpGroup *groupref;

  IGMP_STATS_INC(igmp.recv);

  /* Note that the length CAN be greater than 8 but only 8 are used - All are included in the checksum */
  if (p->len < kIgmpMinlen) {
    pbuf_free(p);
    IGMP_STATS_INC(igmp.lenerr);
    Logf(IGMP_DEBUG, ("igmp_input: length error\n"));
    return;
  }

  Logf(IGMP_DEBUG, ("igmp_input: message from "));
  // ip4_addr_debug_print_val(IGMP_DEBUG, ip4_current_header()->src);
  Logf(IGMP_DEBUG, (" to address "));
  // ip4_addr_debug_print_val(IGMP_DEBUG, ip4_current_header()->dest);
  // Logf(IGMP_DEBUG, (" on if %p\n", (uint8_t *)inp));

  /* Now calculate and check the checksum */
  igmp = (struct IgmpMsg *)p->payload;
  if (inet_chksum(igmp, p->len)) {
    pbuf_free(p);
    IGMP_STATS_INC(igmp.chkerr);
    Logf(IGMP_DEBUG, ("igmp_input: checksum error\n"));
    return;
  }

  /* Packet is ok so find an existing group */
  group = igmp_lookfor_group(inp, dest); /* use the destination IP address of incoming packet */

  /* If group can be found or create... */
  if (!group) {
    pbuf_free(p);
    IGMP_STATS_INC(igmp.drop);
    Logf(IGMP_DEBUG, ("igmp_input: IGMP frame not for us\n"));
    return;
  }

  /* NOW ACT ON THE INCOMING MESSAGE TYPE... */
  switch (igmp->igmp_msgtype) {
    case IGMP_MEMB_QUERY:
      /* IGMP_MEMB_QUERY to the "all systems" address ? */
      if ((ip4_addr_cmp(dest, &allsystems)) && ip4_addr_isany(reinterpret_cast<Ip4Addr*>(&igmp->igmp_group_address))) {
        /* THIS IS THE GENERAL QUERY */
        // Logf(IGMP_DEBUG, ("igmp_input: General IGMP_MEMB_QUERY on \"ALL SYSTEMS\" address (224.0.0.1) [igmp_maxresp=%i]\n", (int)(igmp->igmp_maxresp)));

        if (igmp->igmp_maxresp == 0) {
          IGMP_STATS_INC(igmp.rx_v1);
          Logf(IGMP_DEBUG, ("igmp_input: got an all hosts query with time== 0 - this is V1 and not implemented - treat as v2\n"));
          igmp->igmp_maxresp = IGMP_V1_DELAYING_MEMBER_TMR;
        } else {
          IGMP_STATS_INC(igmp.rx_general);
        }

        groupref = netif_igmp_data(inp);

        /* Do not send messages on the all systems group address! */
        /* Skip the first group in the list, it is always the allsystems group added in igmp_start() */
        if (groupref != nullptr) {
          groupref = groupref->next;
        }

        while (groupref) {
          igmp_delaying_member(groupref, igmp->igmp_maxresp);
          groupref = groupref->next;
        }
      } else {
        /* IGMP_MEMB_QUERY to a specific group ? */
        if (!ip4_addr_isany(reinterpret_cast<Ip4Addr*>(&igmp->igmp_group_address))) {
          // Logf(IGMP_DEBUG, ("igmp_input: IGMP_MEMB_QUERY to a specific group "));
          // ip4_addr_debug_print_val(IGMP_DEBUG, igmp->igmp_group_address);
          if (ip4_addr_cmp(dest, &allsystems)) {
            Ip4Addr groupaddr;
            // Logf(IGMP_DEBUG, (" using \"ALL SYSTEMS\" address (224.0.0.1) [igmp_maxresp=%i]\n", (int)(igmp->igmp_maxresp)));
            /* we first need to re-look for the group since we used dest last time */
            copy_ip4_addr(&groupaddr, reinterpret_cast<Ip4Addr*>(&igmp->igmp_group_address));
            group = igmp_lookfor_group(inp, &groupaddr);
          } else {
            // Logf(IGMP_DEBUG, (" with the group address as destination [igmp_maxresp=%i]\n", (int)(igmp->igmp_maxresp)));
          }

          if (group != nullptr) {
            IGMP_STATS_INC(igmp.rx_group);
            igmp_delaying_member(group, igmp->igmp_maxresp);
          } else {
            IGMP_STATS_INC(igmp.drop);
          }
        } else {
          IGMP_STATS_INC(igmp.proterr);
        }
      }
      break;
    case IGMP_V2_MEMB_REPORT:
      Logf(IGMP_DEBUG, ("igmp_input: IGMP_V2_MEMB_REPORT\n"));
      IGMP_STATS_INC(igmp.rx_report);
      if (group->group_state == IGMP_GROUP_DELAYING_MEMBER) {
        /* This is on a specific group we have already looked up */
        group->timer = 0; /* stopped */
        group->group_state = IGMP_GROUP_IDLE_MEMBER;
        group->last_reporter_flag = 0;
      }
      break;
    default:
      // Logf(IGMP_DEBUG, ("igmp_input: unexpected msg %d in state %d on group %p on if %p\n",
      //                          igmp->igmp_msgtype, group->group_state, (uint8_t *)&group, (uint8_t *)inp));
      IGMP_STATS_INC(igmp.proterr);
      break;
  }

  pbuf_free(p);
  return;
}

/**
 * @ingroup igmp
 * Join a group on one network interface.
 *
 * @param ifaddr ip address of the network interface which should join a new group
 * @param groupaddr the ip address of the group which to join
 * @return ERR_OK if group was joined on the netif(s), an LwipStatus otherwise
 */
LwipStatus
igmp_joingroup(const Ip4Addr *ifaddr, const Ip4Addr *groupaddr)
{
  LwipStatus err = ERR_VAL; /* no matching interface */
  NetIfc*netif;

  LWIP_ASSERT_CORE_LOCKED();

  /* make sure it is multicast address */
  // 
  // 

  /* loop through netif's */
  NETIF_FOREACH(netif) {
    /* Should we join this interface ? */
    if ((netif->flags & kNetifFlagIgmp) && ((ip4_addr_isany(ifaddr) || ip4_addr_cmp(get_net_ifc_ip4_addr(netif), ifaddr)))) {
      err = igmp_joingroup_netif(netif, groupaddr);
      if (err != ERR_OK) {
        /* Return an error even if some network interfaces are joined */
        /** @todo undo any other netif already joined */
        return err;
      }
    }
  }

  return err;
}

/**
 * @ingroup igmp
 * Join a group on one network interface.
 *
 * @param netif the network interface which should join a new group
 * @param groupaddr the ip address of the group which to join
 * @return ERR_OK if group was joined on the netif, an LwipStatus otherwise
 */
LwipStatus
igmp_joingroup_netif(NetIfc*netif, const Ip4Addr *groupaddr)
{
  struct IgmpGroup *group;

  LWIP_ASSERT_CORE_LOCKED();

  /* make sure it is multicast address */
  // 
  // 

  /* make sure it is an igmp-enabled netif */
  // 

  /* find group or create a new one if not found */
  group = igmp_lookup_group(netif, groupaddr);

  if (group != nullptr) {
    /* This should create a new group, check the state to make sure */
    if (group->group_state != IGMP_GROUP_NON_MEMBER) {
      Logf(IGMP_DEBUG, ("igmp_joingroup_netif: join to group not in state IGMP_GROUP_NON_MEMBER\n"));
    } else {
      /* OK - it was new group */
      Logf(IGMP_DEBUG, ("igmp_joingroup_netif: join to new group: "));
      // ip4_addr_debug_print(IGMP_DEBUG, groupaddr);
      Logf(IGMP_DEBUG, ("\n"));

      /* If first use of the group, allow the group at the MAC level */
      if ((group->use == 0) && (netif->igmp_mac_filter != nullptr)) {
        Logf(IGMP_DEBUG, ("igmp_joingroup_netif: igmp_mac_filter(ADD "));
        // ip4_addr_debug_print(IGMP_DEBUG, groupaddr);
        // Logf(IGMP_DEBUG, (") on if %p\n", (uint8_t *)netif));
        netif->igmp_mac_filter(netif, groupaddr, NETIF_ADD_MAC_FILTER);
      }

      IGMP_STATS_INC(igmp.tx_join);
      igmp_send(netif, group, IGMP_V2_MEMB_REPORT);

      igmp_start_timer(group, IGMP_JOIN_DELAYING_MEMBER_TMR);

      /* Need to work out where this timer comes from */
      group->group_state = IGMP_GROUP_DELAYING_MEMBER;
    }
    /* Increment group use */
    group->use++;
    /* Join on this interface */
    return ERR_OK;
  } else {
    Logf(IGMP_DEBUG, ("igmp_joingroup_netif: Not enough memory to join to group\n"));
    return ERR_MEM;
  }
}

/**
 * @ingroup igmp
 * Leave a group on one network interface.
 *
 * @param ifaddr ip address of the network interface which should leave a group
 * @param groupaddr the ip address of the group which to leave
 * @return ERR_OK if group was left on the netif(s), an LwipStatus otherwise
 */
LwipStatus
igmp_leavegroup(const Ip4Addr *ifaddr, const Ip4Addr *groupaddr)
{
  LwipStatus err = ERR_VAL; /* no matching interface */
  NetIfc*netif;

  LWIP_ASSERT_CORE_LOCKED();

  /* make sure it is multicast address */
  // 
  // 

  /* loop through netif's */
  NETIF_FOREACH(netif) {
    /* Should we leave this interface ? */
    if ((netif->flags & kNetifFlagIgmp) && ((ip4_addr_isany(ifaddr) || ip4_addr_cmp(get_net_ifc_ip4_addr(netif), ifaddr)))) {
      LwipStatus res = igmp_leavegroup_netif(netif, groupaddr);
      if (err != ERR_OK) {
        /* Store this result if we have not yet gotten a success */
        err = res;
      }
    }
  }

  return err;
}

/**
 * @ingroup igmp
 * Leave a group on one network interface.
 *
 * @param netif the network interface which should leave a group
 * @param groupaddr the ip address of the group which to leave
 * @return ERR_OK if group was left on the netif, an LwipStatus otherwise
 */
LwipStatus
igmp_leavegroup_netif(NetIfc*netif, const Ip4Addr *groupaddr)
{
  struct IgmpGroup *group;

  LWIP_ASSERT_CORE_LOCKED();

  /* make sure it is multicast address */
  // 
  // 

  /* make sure it is an igmp-enabled netif */
  // 

  /* find group */
  group = igmp_lookfor_group(netif, groupaddr);

  if (group != nullptr) {
    /* Only send a leave if the flag is set according to the state diagram */
    Logf(IGMP_DEBUG, ("igmp_leavegroup_netif: Leaving group: "));
    // ip4_addr_debug_print(IGMP_DEBUG, groupaddr);
    Logf(IGMP_DEBUG, ("\n"));

    /* If there is no other use of the group */
    if (group->use <= 1) {
      /* Remove the group from the list */
      igmp_remove_group(netif, group);

      /* If we are the last reporter for this group */
      if (group->last_reporter_flag) {
        Logf(IGMP_DEBUG, ("igmp_leavegroup_netif: sending leaving group\n"));
        IGMP_STATS_INC(igmp.tx_leave);
        igmp_send(netif, group, IGMP_LEAVE_GROUP);
      }

      /* Disable the group at the MAC level */
      if (netif->igmp_mac_filter != nullptr) {
        Logf(IGMP_DEBUG, ("igmp_leavegroup_netif: igmp_mac_filter(DEL "));
        // ip4_addr_debug_print(IGMP_DEBUG, groupaddr);
        // Logf(IGMP_DEBUG, (") on if %p\n", (uint8_t *)netif));
        netif->igmp_mac_filter(netif, groupaddr, NETIF_DEL_MAC_FILTER);
      }

      /* Free group struct */
      delete group;
    } else {
      /* Decrement group use */
      group->use--;
    }
    return ERR_OK;
  } else {
    Logf(IGMP_DEBUG, ("igmp_leavegroup_netif: not member of group\n"));
    return ERR_VAL;
  }
}

/**
 * The igmp timer function (both for NO_SYS=1 and =0)
 * Should be called every IGMP_TMR_INTERVAL milliseconds (100 ms is default).
 */
void
igmp_tmr(void)
{
  NetIfc*netif;

  NETIF_FOREACH(netif) {
    struct IgmpGroup *group = netif_igmp_data(netif);

    while (group != nullptr) {
      if (group->timer > 0) {
        group->timer--;
        if (group->timer == 0) {
          igmp_timeout(netif, group);
        }
      }
      group = group->next;
    }
  }
}

/**
 * Called if a timeout for one group is reached.
 * Sends a report for this group.
 *
 * @param group an igmp_group for which a timeout is reached
 */
static void
igmp_timeout(NetIfc*netif, struct IgmpGroup *group)
{
  /* If the state is IGMP_GROUP_DELAYING_MEMBER then we send a report for this group
     (unless it is the allsystems group) */
  if ((group->group_state == IGMP_GROUP_DELAYING_MEMBER) &&
      (!(ip4_addr_cmp(&(group->group_address), &allsystems)))) {
    Logf(IGMP_DEBUG, ("igmp_timeout: report membership for group with address "));
    // ip4_addr_debug_print_val(IGMP_DEBUG, group->group_address);
    // Logf(IGMP_DEBUG, (" on if %p\n", (uint8_t *)netif));

    group->group_state = IGMP_GROUP_IDLE_MEMBER;

    IGMP_STATS_INC(igmp.tx_report);
    igmp_send(netif, group, IGMP_V2_MEMB_REPORT);
  }
}

/**
 * Start a timer for an igmp group
 *
 * @param group the igmp_group for which to start a timer
 * @param max_time the time in multiples of IGMP_TMR_INTERVAL (decrease with
 *        every call to igmp_tmr())
 */
static void
igmp_start_timer(struct IgmpGroup *group, uint8_t max_time)
{
#ifdef lwip_rand
  group->timer = (uint16_t)(max_time > 2 ? (lwip_rand() % max_time) : 1);
#else /* lwip_rand */
  /* ATTENTION: use this only if absolutely necessary! */
  group->timer = max_time / 2;
#endif /* lwip_rand */

  if (group->timer == 0) {
    group->timer = 1;
  }
}

/**
 * Delaying membership report for a group if necessary
 *
 * @param group the igmp_group for which "delaying" membership report
 * @param maxresp query delay
 */
static void
igmp_delaying_member(struct IgmpGroup *group, uint8_t maxresp)
{
  if ((group->group_state == IGMP_GROUP_IDLE_MEMBER) ||
      ((group->group_state == IGMP_GROUP_DELAYING_MEMBER) &&
       ((group->timer == 0) || (maxresp < group->timer)))) {
    igmp_start_timer(group, maxresp);
    group->group_state = IGMP_GROUP_DELAYING_MEMBER;
  }
}


/**
 * Sends an IP packet on a network interface. This function constructs the IP header
 * and calculates the IP header checksum. If the source IP address is NULL,
 * the IP address of the outgoing network interface is filled in as source address.
 *
 * @param p the packet to send (p->payload points to the data, e.g. next
            protocol header; if dest == LWIP_IP_HDRINCL, p already includes an
            IP header and p->payload points to that IP header)
 * @param src the source IP address to send from (if src == IP4_ADDR_ANY, the
 *         IP  address of the netif used to send is used as source address)
 * @param dest the destination IP address to send the packet to
 * @param netif the netif on which to send this packet
 * @return ERR_OK if the packet was sent OK
 *         ERR_BUF if p doesn't have enough space for IP/LINK headers
 *         returns errors returned by netif->output
 */
static LwipStatus
igmp_ip_output_if(struct PacketBuffer *p, const Ip4Addr *src, const Ip4Addr *dest, NetIfc*netif)
{
  /* This is the "router alert" option */
  uint16_t ra[2];
  ra[0] = pp_htons(kRouterAlert);
  ra[1] = 0x0000; /* Router shall examine packet */
  IGMP_STATS_INC(igmp.xmit);
  return ip4_output_if_opt(p, src, dest, IGMP_TTL, 0, IP_PROTO_IGMP, netif, ra, kRouterAlertlen);
}

/**
 * Send an igmp packet to a specific group.
 *
 * @param group the group to which to send the packet
 * @param type the type of igmp packet to send
 */
static void
igmp_send(NetIfc*netif, struct IgmpGroup *group, uint8_t type)
{
  struct PacketBuffer     *p    = nullptr;
  struct IgmpMsg *igmp = nullptr;
  Ip4Addr   src  = *IP4_ADDR_ANY4;
  Ip4Addr  *dest = nullptr;

  /* IP header + "router alert" option + IGMP header */
  p = pbuf_alloc(PBUF_TRANSPORT, kIgmpMinlen, PBUF_RAM);

  if (p) {
    igmp = static_cast<struct IgmpMsg *>(p->payload);
    lwip_assert("igmp_send: check that first PacketBuffer can hold struct igmp_msg",
                (p->len >= sizeof(struct IgmpMsg)));
    copy_ip4_addr(&src, get_net_ifc_ip4_addr(netif));

    if (type == IGMP_V2_MEMB_REPORT) {
      dest = &(group->group_address);
      copy_ip4_addr(reinterpret_cast<Ip4Addr*>(&igmp->igmp_group_address), &group->group_address);
      group->last_reporter_flag = 1; /* Remember we were the last to report */
    } else {
      if (type == IGMP_LEAVE_GROUP) {
        dest = &allrouters;
        copy_ip4_addr(reinterpret_cast<Ip4Addr*>(&igmp->igmp_group_address), &group->group_address);
      }
    }

    if ((type == IGMP_V2_MEMB_REPORT) || (type == IGMP_LEAVE_GROUP)) {
      igmp->igmp_msgtype  = type;
      igmp->igmp_maxresp  = 0;
      igmp->igmp_checksum = 0;
      igmp->igmp_checksum = inet_chksum(igmp, kIgmpMinlen);

      igmp_ip_output_if(p, &src, dest, netif);
    }

    pbuf_free(p);
  } else {
    Logf(IGMP_DEBUG, ("igmp_send: not enough memory for igmp_send\n"));
    IGMP_STATS_INC(igmp.memerr);
  }
}
