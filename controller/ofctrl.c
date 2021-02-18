/* Copyright (c) 2015, 2016, 2017 Nicira, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>
#include "bitmap.h"
#include "byte-order.h"
#include "dirs.h"
#include "dp-packet.h"
#include "flow.h"
#include "hash.h"
#include "hindex.h"
#include "lflow.h"
#include "ofctrl.h"
#include "openflow/openflow.h"
#include "openvswitch/dynamic-string.h"
#include "openvswitch/hmap.h"
#include "openvswitch/list.h"
#include "openvswitch/match.h"
#include "openvswitch/ofp-actions.h"
#include "openvswitch/ofp-flow.h"
#include "openvswitch/ofp-group.h"
#include "openvswitch/ofp-match.h"
#include "openvswitch/ofp-msgs.h"
#include "openvswitch/ofp-meter.h"
#include "openvswitch/ofp-packet.h"
#include "openvswitch/ofp-print.h"
#include "openvswitch/ofp-util.h"
#include "openvswitch/ofpbuf.h"
#include "openvswitch/vlog.h"
#include "ovn-controller.h"
#include "ovn/actions.h"
#include "lib/extend-table.h"
#include "openvswitch/poll-loop.h"
#include "physical.h"
#include "openvswitch/rconn.h"
#include "socket-util.h"
#include "util.h"
#include "vswitch-idl.h"

VLOG_DEFINE_THIS_MODULE(ofctrl);

/* An OpenFlow flow. */
struct ovn_flow {
    /* Key. */
    uint8_t table_id;
    uint16_t priority;
    struct minimatch match;

    /* Hash. */
    uint32_t hash;

    /* Data. */
    struct ofpact *ofpacts;
    size_t ofpacts_len;
    uint64_t cookie;
};

/* A desired flow, in struct ovn_desired_flow_table, calculated by the
 * incremental processing engine.
 * - They are added/removed incrementally when I-P engine is able to process
 *   the changes incrementally, or
 * - Completely cleared and recomputed by I-P engine when recompute happens.
 *
 * Links are maintained between desired flows and SB data. The relationship
 * is M to N. The struct sb_flow_ref is used to link a pair of desired flow
 * and SB UUID. The below diagram depicts the data structure.
 *
 *                   SB UUIDs
 *                 +-----+-----+-----+-----+-----+-----+-----+
 *                 |     |     |     |     |     |     |     |
 *                 +--+--+--+--+--+--+-----+--+--+--+--+--+--+
 *                    |     |     |           |     |     |
 *  Desired Flows     |     |     |           |     |     |
 *     +----+       +-+-+   |   +-+-+         |   +-+-+   |
 *     |    +-------+   +-------+   +-------------+   |   |
 *     +----+       +---+   |   +-+-+         |   +---+   |
 *     |    |               |     |           |           |
 *     +----+               |     |         +-+-+         |
 *     |    +-------------------------------+   |         |
 *     +----+             +---+   |         +---+         |
 *     |    +-------------+   |   |                       |
 *     +----+             +---+   |                       |
 *     |    |                     |                       |
 *     +----+                   +-+-+                   +-+-+
 *     |    +-------------------+   +-------------------+   |
 *     +----+                   +---+                   +---+
 *     |    |
 *     +----+
 *
 * The links are updated whenever there is a change in desired flows, which is
 * usually triggered by a SB data change in I-P engine.
 */
struct desired_flow {
    struct ovn_flow flow;
    struct hmap_node match_hmap_node; /* For match based hashing. */
    struct ovs_list list_node; /* For handling lists of flows. */

    /* A list of struct sb_flow_ref nodes, which references this flow. (There
     * are cases that multiple SB entities share the same desired OpenFlow
     * flow, e.g. when conjunction is used.) */
    struct ovs_list references;

    /* The corresponding flow in installed table. */
    struct installed_flow *installed_flow;

    /* Node in installed_flow.desired_refs list. */
    struct ovs_list installed_ref_list_node;
};

struct sb_to_flow {
    struct hmap_node hmap_node; /* Node in
                                   ovn_desired_flow_table.uuid_flow_table. */
    struct uuid sb_uuid;
    struct ovs_list flows; /* A list of struct sb_flow_ref nodes that
                                      are referenced by the sb_uuid. */
};

struct sb_flow_ref {
    struct ovs_list sb_list; /* List node in desired_flow.references. */
    struct ovs_list flow_list; /* List node in sb_to_flow.desired_flows. */
    struct desired_flow *flow;
    struct uuid sb_uuid;
};

/* A installed flow, in static variable installed_flows.
 *
 * Installed flows are updated in ofctrl_put for maintaining the flow
 * installation to OVS. They are updated according to desired flows: either by
 * processing the tracked desired flow changes, or by comparing desired flows
 * with currently installed flows when tracked desired flows changes are not
 * available.
 *
 * In addition, when ofctrl state machine enters S_CLEAR, the installed flows
 * will be cleared. (This happens in initialization phase and also when
 * ovs-vswitchd is disconnected/reconnected).
 *
 * Links are maintained between installed flows and desired flows. The
 * relationship is 1 to N. A link is added when a flow addition is processed.
 * A link is removed when a flow deletion is processed, the desired flow
 * table is cleared, or the installed flow table is cleared.
 */
struct installed_flow {
    struct ovn_flow flow;
    struct hmap_node match_hmap_node; /* For match based hashing. */

    /* A list of desired ovn_flow nodes (linked by
     * desired_flow.installed_ref_list_node), which reference this installed
     * flow.  (There are cases that multiple desired flows reference the same
     * installed flow, e.g. when there are conflict/duplicated ACLs that
     * generates same match conditions). */
    struct ovs_list desired_refs;

    /* The corresponding flow in desired table. It must be one of the flows in
     * desired_refs list.  If there are more than one flows in references list,
     * this is the one that is actually installed. */
    struct desired_flow *desired_flow;
};

static struct desired_flow *desired_flow_alloc(
    uint8_t table_id,
    uint16_t priority,
    uint64_t cookie,
    const struct match *match,
    const struct ofpbuf *actions);
static struct desired_flow *desired_flow_lookup(
    struct ovn_desired_flow_table *,
    const struct ovn_flow *target,
    const struct uuid *sb_uuid);
static void desired_flow_destroy(struct desired_flow *);

static struct installed_flow *installed_flow_lookup(
    const struct ovn_flow *target);
static void installed_flow_destroy(struct installed_flow *);
static struct installed_flow *installed_flow_dup(struct desired_flow *);

static uint32_t ovn_flow_match_hash(const struct ovn_flow *);
static char *ovn_flow_to_string(const struct ovn_flow *);
static void ovn_flow_log(const struct ovn_flow *, const char *action);

/* OpenFlow connection to the switch. */
static struct rconn *swconn;

/* Symbol table for OVN expressions. */
static struct shash symtab;

/* Last seen sequence number for 'swconn'.  When this differs from
 * rconn_get_connection_seqno(rconn), 'swconn' has reconnected. */
static unsigned int seqno;

/* Connection state machine. */
#define STATES                                  \
    STATE(S_NEW)                                \
    STATE(S_TLV_TABLE_REQUESTED)                \
    STATE(S_TLV_TABLE_MOD_SENT)                 \
    STATE(S_CLEAR_FLOWS)                        \
    STATE(S_UPDATE_FLOWS)
enum ofctrl_state {
#define STATE(NAME) NAME,
    STATES
#undef STATE
};

/* An in-flight update to the switch's flow table.
 *
 * When we receive a barrier reply from the switch with the given 'xid', we
 * know that the switch is caught up to northbound database sequence number
 * 'nb_cfg' (and make that available to the client via ofctrl_get_cur_cfg(), so
 * that it can store it into our Chassis record's nb_cfg column). */
struct ofctrl_flow_update {
    struct ovs_list list_node;  /* In 'flow_updates'. */
    ovs_be32 xid;               /* OpenFlow transaction ID for barrier. */
    int64_t nb_cfg;             /* Northbound database sequence number. */
};

static struct ofctrl_flow_update *
ofctrl_flow_update_from_list_node(const struct ovs_list *list_node)
{
    return CONTAINER_OF(list_node, struct ofctrl_flow_update, list_node);
}

/* Currently in-flight updates. */
static struct ovs_list flow_updates;

/* nb_cfg of latest committed flow update. */
static int64_t cur_cfg;

/* Current state. */
static enum ofctrl_state state;

/* Transaction IDs for messages in flight to the switch. */
static ovs_be32 xid, xid2;

/* Counter for in-flight OpenFlow messages on 'swconn'.  We only send a new
 * round of flow table modifications to the switch when the counter falls to
 * zero, to avoid unbounded buffering. */
static struct rconn_packet_counter *tx_counter;

/* Flow table of "struct ovn_flow"s, that holds the flow table currently
 * installed in the switch. */
static struct hmap installed_flows;

/* A reference to the group_table. */
static struct ovn_extend_table *groups;

/* A reference to the meter_table. */
static struct ovn_extend_table *meters;

/* MFF_* field ID for our Geneve option.  In S_TLV_TABLE_MOD_SENT, this is
 * the option we requested (we don't know whether we obtained it yet).  In
 * S_CLEAR_FLOWS or S_UPDATE_FLOWS, this is really the option we have. */
static enum mf_field_id mff_ovn_geneve;

/* Indicates if flows need to be reinstalled for scenarios when ovs
 * is restarted, even if there is no change in the desired flow table. */
static bool need_reinstall_flows;

static ovs_be32 queue_msg(struct ofpbuf *);

static struct ofpbuf *encode_flow_mod(struct ofputil_flow_mod *);

static struct ofpbuf *encode_group_mod(const struct ofputil_group_mod *);

static struct ofpbuf *encode_meter_mod(const struct ofputil_meter_mod *);

static void ovn_installed_flow_table_clear(void);
static void ovn_installed_flow_table_destroy(void);


static void ofctrl_recv(const struct ofp_header *, enum ofptype);

void
ofctrl_init(struct ovn_extend_table *group_table,
            struct ovn_extend_table *meter_table,
            int inactivity_probe_interval)
{
    swconn = rconn_create(inactivity_probe_interval, 0,
                          DSCP_DEFAULT, 1 << OFP13_VERSION);
    tx_counter = rconn_packet_counter_create();
    hmap_init(&installed_flows);
    ovs_list_init(&flow_updates);
    ovn_init_symtab(&symtab);
    groups = group_table;
    meters = meter_table;
}

/* S_NEW, for a new connection.
 *
 * Sends NXT_TLV_TABLE_REQUEST and transitions to
 * S_TLV_TABLE_REQUESTED. */

static void
run_S_NEW(void)
{
    struct ofpbuf *buf = ofpraw_alloc(OFPRAW_NXT_TLV_TABLE_REQUEST,
                                      rconn_get_version(swconn), 0);
    xid = queue_msg(buf);
    state = S_TLV_TABLE_REQUESTED;
}

static void
recv_S_NEW(const struct ofp_header *oh OVS_UNUSED,
           enum ofptype type OVS_UNUSED,
           struct shash *pending_ct_zones OVS_UNUSED)
{
    OVS_NOT_REACHED();
}

/* S_TLV_TABLE_REQUESTED, when NXT_TLV_TABLE_REQUEST has been sent
 * and we're waiting for a reply.
 *
 * If we receive an NXT_TLV_TABLE_REPLY:
 *
 *     - If it contains our tunnel metadata option, assign its field ID to
 *       mff_ovn_geneve and transition to S_CLEAR_FLOWS.
 *
 *     - Otherwise, if there is an unused tunnel metadata field ID, send
 *       NXT_TLV_TABLE_MOD and OFPT_BARRIER_REQUEST, and transition to
 *       S_TLV_TABLE_MOD_SENT.
 *
 *     - Otherwise, log an error, disable Geneve, and transition to
 *       S_CLEAR_FLOWS.
 *
 * If we receive an OFPT_ERROR:
 *
 *     - Log an error, disable Geneve, and transition to S_CLEAR_FLOWS. */

static void
run_S_TLV_TABLE_REQUESTED(void)
{
}

static bool
process_tlv_table_reply(const struct ofputil_tlv_table_reply *reply)
{
    const struct ofputil_tlv_map *map;
    uint64_t md_free = UINT64_MAX;
    BUILD_ASSERT(TUN_METADATA_NUM_OPTS == 64);

    LIST_FOR_EACH (map, list_node, &reply->mappings) {
        if (map->option_class == OVN_GENEVE_CLASS
            && map->option_type == OVN_GENEVE_TYPE
            && map->option_len == OVN_GENEVE_LEN) {
            if (map->index >= TUN_METADATA_NUM_OPTS) {
                VLOG_ERR("desired Geneve tunnel option 0x%"PRIx16","
                         "%"PRIu8",%"PRIu8" already in use with "
                         "unsupported index %"PRIu16,
                         map->option_class, map->option_type,
                         map->option_len, map->index);
                return false;
            } else {
                mff_ovn_geneve = MFF_TUN_METADATA0 + map->index;
                state = S_CLEAR_FLOWS;
                return true;
            }
        }

        if (map->index < TUN_METADATA_NUM_OPTS) {
            md_free &= ~(UINT64_C(1) << map->index);
        }
    }

    VLOG_DBG("OVN Geneve option not found");
    if (!md_free) {
        VLOG_ERR("no Geneve options free for use by OVN");
        return false;
    }

    unsigned int index = rightmost_1bit_idx(md_free);
    mff_ovn_geneve = MFF_TUN_METADATA0 + index;
    struct ofputil_tlv_map tm;
    tm.option_class = OVN_GENEVE_CLASS;
    tm.option_type = OVN_GENEVE_TYPE;
    tm.option_len = OVN_GENEVE_LEN;
    tm.index = index;

    struct ofputil_tlv_table_mod ttm;
    ttm.command = NXTTMC_ADD;
    ovs_list_init(&ttm.mappings);
    ovs_list_push_back(&ttm.mappings, &tm.list_node);

    xid = queue_msg(ofputil_encode_tlv_table_mod(OFP13_VERSION, &ttm));
    xid2 = queue_msg(ofputil_encode_barrier_request(OFP13_VERSION));
    state = S_TLV_TABLE_MOD_SENT;

    return true;
}

static void
recv_S_TLV_TABLE_REQUESTED(const struct ofp_header *oh, enum ofptype type,
                           struct shash *pending_ct_zones OVS_UNUSED)
{
    if (oh->xid != xid) {
        ofctrl_recv(oh, type);
        return;
    } else if (type == OFPTYPE_NXT_TLV_TABLE_REPLY) {
        struct ofputil_tlv_table_reply reply;
        enum ofperr error = ofputil_decode_tlv_table_reply(oh, &reply);
        if (!error) {
            bool ok = process_tlv_table_reply(&reply);
            ofputil_uninit_tlv_table(&reply.mappings);
            if (ok) {
                return;
            }
        } else {
            VLOG_ERR("failed to decode TLV table request (%s)",
                     ofperr_to_string(error));
        }
    } else if (type == OFPTYPE_ERROR) {
        VLOG_ERR("switch refused to allocate Geneve option (%s)",
                 ofperr_to_string(ofperr_decode_msg(oh, NULL)));
    } else {
        char *s = ofp_to_string(oh, ntohs(oh->length), NULL, NULL, 1);
        VLOG_ERR("unexpected reply to TLV table request (%s)", s);
        free(s);
    }

    /* Error path. */
    mff_ovn_geneve = 0;
    state = S_CLEAR_FLOWS;
}

/* S_TLV_TABLE_MOD_SENT, when NXT_TLV_TABLE_MOD and OFPT_BARRIER_REQUEST
 * have been sent and we're waiting for a reply to one or the other.
 *
 * If we receive an OFPT_ERROR:
 *
 *     - If the error is NXTTMFC_ALREADY_MAPPED or NXTTMFC_DUP_ENTRY, we
 *       raced with some other controller.  Transition to S_NEW.
 *
 *     - Otherwise, log an error, disable Geneve, and transition to
 *       S_CLEAR_FLOWS.
 *
 * If we receive OFPT_BARRIER_REPLY:
 *
 *     - Set the tunnel metadata field ID to the one that we requested.
 *       Transition to S_CLEAR_FLOWS.
 */

static void
run_S_TLV_TABLE_MOD_SENT(void)
{
}

static void
recv_S_TLV_TABLE_MOD_SENT(const struct ofp_header *oh, enum ofptype type,
                          struct shash *pending_ct_zones OVS_UNUSED)
{
    if (oh->xid != xid && oh->xid != xid2) {
        ofctrl_recv(oh, type);
    } else if (oh->xid == xid2 && type == OFPTYPE_BARRIER_REPLY) {
        state = S_CLEAR_FLOWS;
    } else if (oh->xid == xid && type == OFPTYPE_ERROR) {
        enum ofperr error = ofperr_decode_msg(oh, NULL);
        if (error == OFPERR_NXTTMFC_ALREADY_MAPPED ||
            error == OFPERR_NXTTMFC_DUP_ENTRY) {
            VLOG_INFO("raced with another controller adding "
                      "Geneve option (%s); trying again",
                      ofperr_to_string(error));
            state = S_NEW;
        } else {
            VLOG_ERR("error adding Geneve option (%s)",
                     ofperr_to_string(error));
            goto error;
        }
    } else {
        char *s = ofp_to_string(oh, ntohs(oh->length), NULL, NULL, 1);
        VLOG_ERR("unexpected reply to Geneve option allocation request (%s)",
                 s);
        free(s);
        goto error;
    }
    return;

error:
    state = S_CLEAR_FLOWS;
}

/* S_CLEAR_FLOWS, after we've established a Geneve metadata field ID and it's
 * time to set up some flows.
 *
 * Sends an OFPT_TABLE_MOD to clear all flows, then transitions to
 * S_UPDATE_FLOWS. */

static void
run_S_CLEAR_FLOWS(void)
{
    VLOG_DBG("clearing all flows");

    need_reinstall_flows = true;
    /* Send a flow_mod to delete all flows. */
    struct ofputil_flow_mod fm = {
        .table_id = OFPTT_ALL,
        .command = OFPFC_DELETE,
    };
    minimatch_init_catchall(&fm.match);
    queue_msg(encode_flow_mod(&fm));
    minimatch_destroy(&fm.match);

    /* Send a group_mod to delete all groups. */
    struct ofputil_group_mod gm;
    memset(&gm, 0, sizeof gm);
    gm.command = OFPGC11_DELETE;
    gm.group_id = OFPG_ALL;
    gm.command_bucket_id = OFPG15_BUCKET_ALL;
    ovs_list_init(&gm.buckets);
    queue_msg(encode_group_mod(&gm));
    ofputil_uninit_group_mod(&gm);

    /* Clear installed_flows, to match the state of the switch. */
    ovn_installed_flow_table_clear();

    /* Clear existing groups, to match the state of the switch. */
    if (groups) {
        ovn_extend_table_clear(groups, true);
    }

    /* Send a meter_mod to delete all meters. */
    struct ofputil_meter_mod mm;
    memset(&mm, 0, sizeof mm);
    mm.command = OFPMC13_DELETE;
    mm.meter.meter_id = OFPM13_ALL;
    queue_msg(encode_meter_mod(&mm));

    /* Clear existing meters, to match the state of the switch. */
    if (meters) {
        ovn_extend_table_clear(meters, true);
    }

    /* All flow updates are irrelevant now. */
    struct ofctrl_flow_update *fup, *next;
    LIST_FOR_EACH_SAFE (fup, next, list_node, &flow_updates) {
        ovs_list_remove(&fup->list_node);
        free(fup);
    }

    state = S_UPDATE_FLOWS;
}

static void
recv_S_CLEAR_FLOWS(const struct ofp_header *oh, enum ofptype type,
                   struct shash *pending_ct_zones OVS_UNUSED)
{
    ofctrl_recv(oh, type);
}

/* S_UPDATE_FLOWS, for maintaining the flow table over time.
 *
 * Compare the installed flows to the ones we want.  Send OFPT_FLOW_MOD as
 * necessary.
 *
 * This is a terminal state.  We only transition out of it if the connection
 * drops. */

static void
run_S_UPDATE_FLOWS(void)
{
    /* Nothing to do here.
     *
     * Being in this state enables ofctrl_put() to work, however. */
}

static void
recv_S_UPDATE_FLOWS(const struct ofp_header *oh, enum ofptype type,
                    struct shash *pending_ct_zones)
{
    if (type == OFPTYPE_BARRIER_REPLY && !ovs_list_is_empty(&flow_updates)) {
        struct ofctrl_flow_update *fup = ofctrl_flow_update_from_list_node(
            ovs_list_front(&flow_updates));
        if (fup->xid == oh->xid) {
            if (fup->nb_cfg >= cur_cfg) {
                cur_cfg = fup->nb_cfg;
            }
            ovs_list_remove(&fup->list_node);
            free(fup);
        }

        /* If the barrier xid is associated with an outstanding conntrack
         * flush, the flush succeeded.  Move the pending ct zone entry
         * to the next stage. */
        struct shash_node *iter;
        SHASH_FOR_EACH(iter, pending_ct_zones) {
            struct ct_zone_pending_entry *ctzpe = iter->data;
            if (ctzpe->state == CT_ZONE_OF_SENT && ctzpe->of_xid == oh->xid) {
                ctzpe->state = CT_ZONE_DB_QUEUED;
            }
        }
    } else {
        ofctrl_recv(oh, type);
    }
}


enum mf_field_id
ofctrl_get_mf_field_id(void)
{
    if (!rconn_is_connected(swconn)) {
        return 0;
    }
    return (state == S_CLEAR_FLOWS || state == S_UPDATE_FLOWS
            ? mff_ovn_geneve : 0);
}

/* Runs the OpenFlow state machine against 'br_int', which is local to the
 * hypervisor on which we are running.  Attempts to negotiate a Geneve option
 * field for class OVN_GENEVE_CLASS, type OVN_GENEVE_TYPE. */
void
ofctrl_run(const struct ovsrec_bridge *br_int, struct shash *pending_ct_zones)
{
    char *target = xasprintf("unix:%s/%s.mgmt", ovs_rundir(), br_int->name);
    if (strcmp(target, rconn_get_target(swconn))) {
        VLOG_INFO("%s: connecting to switch", target);
        rconn_connect(swconn, target, target);
    }
    free(target);

    rconn_run(swconn);

    if (!rconn_is_connected(swconn)) {
        return;
    }
    if (seqno != rconn_get_connection_seqno(swconn)) {
        seqno = rconn_get_connection_seqno(swconn);
        state = S_NEW;

        /* Reset the state of any outstanding ct flushes to resend them. */
        struct shash_node *iter;
        SHASH_FOR_EACH(iter, pending_ct_zones) {
            struct ct_zone_pending_entry *ctzpe = iter->data;
            if (ctzpe->state == CT_ZONE_OF_SENT) {
                ctzpe->state = CT_ZONE_OF_QUEUED;
            }
        }
    }

    bool progress = true;
    for (int i = 0; progress && i < 50; i++) {
        /* Allow the state machine to run. */
        enum ofctrl_state old_state = state;
        switch (state) {
#define STATE(NAME) case NAME: run_##NAME(); break;
            STATES
#undef STATE
        default:
            OVS_NOT_REACHED();
        }

        /* Try to process a received packet. */
        struct ofpbuf *msg = rconn_recv(swconn);
        if (msg) {
            const struct ofp_header *oh = msg->data;
            enum ofptype type;
            enum ofperr error;

            error = ofptype_decode(&type, oh);
            if (!error) {
                switch (state) {
#define STATE(NAME) case NAME: recv_##NAME(oh, type, pending_ct_zones); break;
                    STATES
#undef STATE
                default:
                    OVS_NOT_REACHED();
                }
            } else {
                char *s = ofp_to_string(oh, ntohs(oh->length), NULL, NULL, 1);
                VLOG_WARN("could not decode OpenFlow message (%s): %s",
                          ofperr_to_string(error), s);
                free(s);
            }

            ofpbuf_delete(msg);
        }

        /* If we did some work, plan to go around again. */
        progress = old_state != state || msg;
    }
    if (progress) {
        /* We bailed out to limit the amount of work we do in one go, to allow
         * other code a chance to run.  We were still making progress at that
         * point, so ensure that we come back again without waiting. */
        poll_immediate_wake();
    }
}

void
ofctrl_wait(void)
{
    rconn_run_wait(swconn);
    rconn_recv_wait(swconn);
}

void
ofctrl_destroy(void)
{
    rconn_destroy(swconn);
    ovn_installed_flow_table_destroy();
    rconn_packet_counter_destroy(tx_counter);
    expr_symtab_destroy(&symtab);
    shash_destroy(&symtab);
}

int64_t
ofctrl_get_cur_cfg(void)
{
    return cur_cfg;
}

static ovs_be32
queue_msg(struct ofpbuf *msg)
{
    const struct ofp_header *oh = msg->data;
    ovs_be32 xid_ = oh->xid;
    rconn_send(swconn, msg, tx_counter);
    return xid_;
}

static void
log_openflow_rl(struct vlog_rate_limit *rl, enum vlog_level level,
                const struct ofp_header *oh, const char *title)
{
    if (!vlog_should_drop(&this_module, level, rl)) {
        char *s = ofp_to_string(oh, ntohs(oh->length), NULL, NULL, 2);
        vlog(&this_module, level, "%s: %s", title, s);
        free(s);
    }
}

static void
ofctrl_recv(const struct ofp_header *oh, enum ofptype type)
{
    if (type == OFPTYPE_ECHO_REQUEST) {
        queue_msg(ofputil_encode_echo_reply(oh));
    } else if (type == OFPTYPE_ERROR) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(30, 300);
        log_openflow_rl(&rl, VLL_INFO, oh, "OpenFlow error");
    } else {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(30, 300);
        log_openflow_rl(&rl, VLL_DBG, oh, "OpenFlow packet ignored");
    }
}

static void
link_installed_to_desired(struct installed_flow *i, struct desired_flow *d)
{
    if (i->desired_flow == d) {
        return;
    }

    if (ovs_list_is_empty(&i->desired_refs)) {
        ovs_assert(!i->desired_flow);
        i->desired_flow = d;
    }
    ovs_list_insert(&i->desired_refs, &d->installed_ref_list_node);
    d->installed_flow = i;
}

static void
unlink_installed_to_desired(struct installed_flow *i, struct desired_flow *d)
{
    ovs_assert(i && i->desired_flow && !ovs_list_is_empty(&i->desired_refs));
    ovs_assert(d && d->installed_flow == i);
    ovs_list_remove(&d->installed_ref_list_node);
    d->installed_flow = NULL;
    if (i->desired_flow == d) {
        i->desired_flow = ovs_list_is_empty(&i->desired_refs) ? NULL :
            CONTAINER_OF(ovs_list_front(&i->desired_refs),
                         struct desired_flow,
                         installed_ref_list_node);
    }
}

static void
unlink_all_refs_for_installed_flow(struct installed_flow *i)
{
    struct desired_flow *d, *next;
    LIST_FOR_EACH_SAFE (d, next, installed_ref_list_node, &i->desired_refs) {
        unlink_installed_to_desired(i, d);
    }
}

static struct sb_to_flow *
sb_to_flow_find(struct hmap *uuid_flow_table, const struct uuid *sb_uuid)
{
    struct sb_to_flow *stf;
    HMAP_FOR_EACH_WITH_HASH (stf, hmap_node, uuid_hash(sb_uuid),
                            uuid_flow_table) {
        if (uuid_equals(sb_uuid, &stf->sb_uuid)) {
            return stf;
        }
    }
    return NULL;
}

static void
link_flow_to_sb(struct ovn_desired_flow_table *flow_table,
                struct desired_flow *f, const struct uuid *sb_uuid)
{
    struct sb_flow_ref *sfr = xmalloc(sizeof *sfr);
    sfr->flow = f;
    sfr->sb_uuid = *sb_uuid;
    ovs_list_insert(&f->references, &sfr->sb_list);
    struct sb_to_flow *stf = sb_to_flow_find(&flow_table->uuid_flow_table,
                                             sb_uuid);
    if (!stf) {
        stf = xmalloc(sizeof *stf);
        stf->sb_uuid = *sb_uuid;
        ovs_list_init(&stf->flows);
        hmap_insert(&flow_table->uuid_flow_table, &stf->hmap_node,
                    uuid_hash(sb_uuid));
    }
    ovs_list_insert(&stf->flows, &sfr->flow_list);
}

/* Flow table interfaces to the rest of ovn-controller. */

/* Adds a flow to 'desired_flows' with the specified 'match' and 'actions' to
 * the OpenFlow table numbered 'table_id' with the given 'priority' and
 * OpenFlow 'cookie'.  The caller retains ownership of 'match' and 'actions'.
 *
 * The flow is also linked to the sb_uuid that generates it.
 *
 * This just assembles the desired flow table in memory.  Nothing is actually
 * sent to the switch until a later call to ofctrl_put().
 *
 * The caller should initialize its own hmap to hold the flows. */
void
ofctrl_check_and_add_flow(struct ovn_desired_flow_table *flow_table,
                          uint8_t table_id, uint16_t priority,
                          uint64_t cookie, const struct match *match,
                          const struct ofpbuf *actions,
                          const struct uuid *sb_uuid,
                          bool log_duplicate_flow)
{
    struct desired_flow *f = desired_flow_alloc(table_id, priority, cookie,
                                                match, actions);

    if (desired_flow_lookup(flow_table, &f->flow, sb_uuid)) {
        if (log_duplicate_flow) {
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 5);
            if (!VLOG_DROP_DBG(&rl)) {
                char *s = ovn_flow_to_string(&f->flow);
                VLOG_DBG("dropping duplicate flow: %s", s);
                free(s);
            }
        }
        desired_flow_destroy(f);
        return;
    }

    hmap_insert(&flow_table->match_flow_table, &f->match_hmap_node,
                f->flow.hash);
    link_flow_to_sb(flow_table, f, sb_uuid);
    ovn_flow_log(&f->flow, "ofctrl_add_flow");
}

void
ofctrl_add_flow(struct ovn_desired_flow_table *desired_flows,
                uint8_t table_id, uint16_t priority, uint64_t cookie,
                const struct match *match, const struct ofpbuf *actions,
                const struct uuid *sb_uuid)
{
    ofctrl_check_and_add_flow(desired_flows, table_id, priority, cookie,
                              match, actions, sb_uuid, true);
}

/* Either add a new flow, or append actions on an existing flow. If the
 * flow existed, a new link will also be created between the new sb_uuid
 * and the existing flow. */
void
ofctrl_add_or_append_flow(struct ovn_desired_flow_table *desired_flows,
                          uint8_t table_id, uint16_t priority, uint64_t cookie,
                          const struct match *match,
                          const struct ofpbuf *actions,
                          const struct uuid *sb_uuid)
{
    struct desired_flow *f = desired_flow_alloc(table_id, priority, cookie,
                                                match, actions);

    struct desired_flow *existing;
    existing = desired_flow_lookup(desired_flows, &f->flow, NULL);
    if (existing) {
        /* There's already a flow with this particular match. Append the
         * action to that flow rather than adding a new flow
         */
        uint64_t compound_stub[64 / 8];
        struct ofpbuf compound;
        ofpbuf_use_stub(&compound, compound_stub, sizeof(compound_stub));
        ofpbuf_put(&compound, existing->flow.ofpacts,
                   existing->flow.ofpacts_len);
        ofpbuf_put(&compound, f->flow.ofpacts, f->flow.ofpacts_len);

        free(existing->flow.ofpacts);
        existing->flow.ofpacts = xmemdup(compound.data, compound.size);
        existing->flow.ofpacts_len = compound.size;

        ofpbuf_uninit(&compound);
        desired_flow_destroy(f);
        f = existing;
    } else {
        hmap_insert(&desired_flows->match_flow_table, &f->match_hmap_node,
                    f->flow.hash);
    }
    link_flow_to_sb(desired_flows, f, sb_uuid);

    if (existing) {
        ovn_flow_log(&f->flow, "ofctrl_add_or_append_flow (append)");
    } else {
        ovn_flow_log(&f->flow, "ofctrl_add_or_append_flow (add)");
    }
}

/* Remove ovn_flows for the given "sb_to_flow" node in the uuid_flow_table.
 * Optionally log the message for each flow that is acturally removed, if
 * log_msg is not NULL. */
static void
remove_flows_from_sb_to_flow(struct ovn_desired_flow_table *flow_table,
                             struct sb_to_flow *stf,
                             const char *log_msg)
{
    struct sb_flow_ref *sfr, *next;
    LIST_FOR_EACH_SAFE (sfr, next, flow_list, &stf->flows) {
        ovs_list_remove(&sfr->sb_list);
        ovs_list_remove(&sfr->flow_list);
        struct desired_flow *f = sfr->flow;
        free(sfr);

        if (ovs_list_is_empty(&f->references)) {
            if (log_msg) {
                ovn_flow_log(&f->flow, log_msg);
            }
            hmap_remove(&flow_table->match_flow_table,
                        &f->match_hmap_node);
            if (f->installed_flow) {
                unlink_installed_to_desired(f->installed_flow, f);
            }
            desired_flow_destroy(f);
        }
    }
    hmap_remove(&flow_table->uuid_flow_table, &stf->hmap_node);
    free(stf);
}

void
ofctrl_remove_flows(struct ovn_desired_flow_table *flow_table,
                    const struct uuid *sb_uuid)
{
    struct sb_to_flow *stf = sb_to_flow_find(&flow_table->uuid_flow_table,
                                             sb_uuid);
    if (stf) {
        remove_flows_from_sb_to_flow(flow_table, stf, "ofctrl_remove_flow");
    }

    /* remove any related group and meter info */
    ovn_extend_table_remove_desired(groups, sb_uuid);
    ovn_extend_table_remove_desired(meters, sb_uuid);
}

static struct ofctrl_flood_remove_node *
flood_remove_find_node(struct hmap *flood_remove_nodes, struct uuid *sb_uuid)
{
    struct ofctrl_flood_remove_node *ofrn;
    HMAP_FOR_EACH_WITH_HASH (ofrn, hmap_node, uuid_hash(sb_uuid),
                             flood_remove_nodes) {
        if (uuid_equals(&ofrn->sb_uuid, sb_uuid)) {
            return ofrn;
        }
    }
    return NULL;
}

void
ofctrl_flood_remove_add_node(struct hmap *flood_remove_nodes,
                             const struct uuid *sb_uuid)
{
    struct ofctrl_flood_remove_node *ofrn = xmalloc(sizeof *ofrn);
    ofrn->sb_uuid = *sb_uuid;
    hmap_insert(flood_remove_nodes, &ofrn->hmap_node, uuid_hash(sb_uuid));
}

static void
flood_remove_flows_for_sb_uuid(struct ovn_desired_flow_table *flow_table,
                               const struct uuid *sb_uuid,
                               struct hmap *flood_remove_nodes)
{
    struct sb_to_flow *stf = sb_to_flow_find(&flow_table->uuid_flow_table,
                                             sb_uuid);
    if (!stf) {
        return;
    }

    /* ovn_flows that have other references and waiting to be removed. */
    struct ovs_list to_be_removed = OVS_LIST_INITIALIZER(&to_be_removed);

    /* Traverse all flows for the given sb_uuid. */
    struct sb_flow_ref *sfr, *next;
    LIST_FOR_EACH_SAFE (sfr, next, flow_list, &stf->flows) {
        struct desired_flow *f = sfr->flow;
        ovn_flow_log(&f->flow, "flood remove");

        ovs_list_remove(&sfr->sb_list);
        ovs_list_remove(&sfr->flow_list);
        free(sfr);

        ovs_assert(ovs_list_is_empty(&f->list_node));
        if (ovs_list_is_empty(&f->references)) {
            /* This is to optimize the case when most flows have only
             * one referencing sb_uuid, so to_be_removed list should
             * be empty in most cases. */
            hmap_remove(&flow_table->match_flow_table,
                        &f->match_hmap_node);
            if (f->installed_flow) {
                unlink_installed_to_desired(f->installed_flow, f);
            }
            desired_flow_destroy(f);
        } else {
            ovs_list_insert(&to_be_removed, &f->list_node);
        }
    }
    hmap_remove(&flow_table->uuid_flow_table, &stf->hmap_node);
    free(stf);

    /* Traverse other referencing sb_uuids for the flows in the to_be_removed
     * list. */

    /* Detach the items in f->references from the sfr.flow_list lists,
     * so that recursive calls will not mess up the sfr.sb_list list. */
    struct desired_flow *f, *f_next;
    LIST_FOR_EACH (f, list_node, &to_be_removed) {
        ovs_assert(!ovs_list_is_empty(&f->references));
        LIST_FOR_EACH (sfr, sb_list, &f->references) {
            ovs_list_remove(&sfr->flow_list);
        }
    }
    LIST_FOR_EACH_SAFE (f, f_next, list_node, &to_be_removed) {
        LIST_FOR_EACH_SAFE (sfr, next, sb_list, &f->references) {
            if (!flood_remove_find_node(flood_remove_nodes, &sfr->sb_uuid)) {
                ofctrl_flood_remove_add_node(flood_remove_nodes,
                                             &sfr->sb_uuid);
                flood_remove_flows_for_sb_uuid(flow_table, &sfr->sb_uuid,
                                               flood_remove_nodes);
            }
            ovs_list_remove(&sfr->sb_list);
            free(sfr);
        }
        ovs_list_remove(&f->list_node);
        hmap_remove(&flow_table->match_flow_table,
                    &f->match_hmap_node);
        if (f->installed_flow) {
            unlink_installed_to_desired(f->installed_flow, f);
        }
        desired_flow_destroy(f);
    }

}

void
ofctrl_flood_remove_flows(struct ovn_desired_flow_table *flow_table,
                          struct hmap *flood_remove_nodes)
{
    struct ofctrl_flood_remove_node *ofrn;
    HMAP_FOR_EACH (ofrn, hmap_node, flood_remove_nodes) {
        flood_remove_flows_for_sb_uuid(flow_table, &ofrn->sb_uuid,
                                       flood_remove_nodes);
    }

    /* remove any related group and meter info */
    HMAP_FOR_EACH (ofrn, hmap_node, flood_remove_nodes) {
        ovn_extend_table_remove_desired(groups, &ofrn->sb_uuid);
        ovn_extend_table_remove_desired(meters, &ofrn->sb_uuid);
    }
}

/* flow operations. */

static void
ovn_flow_init(struct ovn_flow *f, uint8_t table_id, uint16_t priority,
              uint64_t cookie, const struct match *match,
              const struct ofpbuf *actions)
{
    f->table_id = table_id;
    f->priority = priority;
    minimatch_init(&f->match, match);
    f->ofpacts = xmemdup(actions->data, actions->size);
    f->ofpacts_len = actions->size;
    f->hash = ovn_flow_match_hash(f);
    f->cookie = cookie;
}

static struct desired_flow *
desired_flow_alloc(uint8_t table_id, uint16_t priority, uint64_t cookie,
                   const struct match *match, const struct ofpbuf *actions)
{
    struct desired_flow *f = xmalloc(sizeof *f);
    ovs_list_init(&f->references);
    ovs_list_init(&f->list_node);
    ovs_list_init(&f->installed_ref_list_node);
    f->installed_flow = NULL;
    ovn_flow_init(&f->flow, table_id, priority, cookie, match, actions);

    return f;
}

/* Returns a hash of the match key in 'f'. */
static uint32_t
ovn_flow_match_hash(const struct ovn_flow *f)
{
    return hash_2words((f->table_id << 16) | f->priority,
                       minimatch_hash(&f->match, 0));
}

/* Duplicate a desired flow to an installed flow. */
static struct installed_flow *
installed_flow_dup(struct desired_flow *src)
{
    struct installed_flow *dst = xmalloc(sizeof *dst);
    ovs_list_init(&dst->desired_refs);
    dst->desired_flow = NULL;
    dst->flow.table_id = src->flow.table_id;
    dst->flow.priority = src->flow.priority;
    minimatch_clone(&dst->flow.match, &src->flow.match);
    dst->flow.ofpacts = xmemdup(src->flow.ofpacts, src->flow.ofpacts_len);
    dst->flow.ofpacts_len = src->flow.ofpacts_len;
    dst->flow.hash = src->flow.hash;
    dst->flow.cookie = src->flow.cookie;
    return dst;
}

/* Finds and returns a desired_flow in 'flow_table' whose key is identical to
 * 'target''s key, or NULL if there is none.
 *
 * If sb_uuid is not NULL, the function will also check if the found flow is
 * referenced by the sb_uuid. */
static struct desired_flow *
desired_flow_lookup(struct ovn_desired_flow_table *flow_table,
                    const struct ovn_flow *target,
                    const struct uuid *sb_uuid)
{
    struct desired_flow *d;
    HMAP_FOR_EACH_WITH_HASH (d, match_hmap_node, target->hash,
                             &flow_table->match_flow_table) {
        struct ovn_flow *f = &d->flow;
        if (f->table_id == target->table_id
            && f->priority == target->priority
            && minimatch_equal(&f->match, &target->match)) {
            if (!sb_uuid) {
                return d;
            }
            struct sb_flow_ref *sfr;
            LIST_FOR_EACH (sfr, sb_list, &d->references) {
                if (uuid_equals(sb_uuid, &sfr->sb_uuid)) {
                    return d;
                }
            }
        }
    }
    return NULL;
}

/* Finds and returns an installed_flow in installed_flows whose key is
 * identical to 'target''s key, or NULL if there is none. */
static struct installed_flow *
installed_flow_lookup(const struct ovn_flow *target)
{
    struct installed_flow *i;
    HMAP_FOR_EACH_WITH_HASH (i, match_hmap_node, target->hash,
                             &installed_flows) {
        struct ovn_flow *f = &i->flow;
        if (f->table_id == target->table_id
            && f->priority == target->priority
            && minimatch_equal(&f->match, &target->match)) {
            return i;
        }
    }
    return NULL;
}

static char *
ovn_flow_to_string(const struct ovn_flow *f)
{
    struct ds s = DS_EMPTY_INITIALIZER;

    ds_put_format(&s, "cookie=%"PRIx64", ", f->cookie);
    ds_put_format(&s, "table_id=%"PRIu8", ", f->table_id);
    ds_put_format(&s, "priority=%"PRIu16", ", f->priority);
    minimatch_format(&f->match, NULL, NULL, &s, OFP_DEFAULT_PRIORITY);
    ds_put_cstr(&s, ", actions=");
    struct ofpact_format_params fp = { .s = &s };
    ofpacts_format(f->ofpacts, f->ofpacts_len, &fp);
    return ds_steal_cstr(&s);
}

static void
ovn_flow_log(const struct ovn_flow *f, const char *action)
{
    if (VLOG_IS_DBG_ENABLED()) {
        char *s = ovn_flow_to_string(f);
        VLOG_DBG("%s flow: %s", action, s);
        free(s);
    }
}

static void
ovn_flow_uninit(struct ovn_flow *f)
{
    minimatch_destroy(&f->match);
    free(f->ofpacts);
}

static void
desired_flow_destroy(struct desired_flow *f)
{
    if (f) {
        ovs_assert(ovs_list_is_empty(&f->references));
        ovs_assert(!f->installed_flow);
        ovn_flow_uninit(&f->flow);
        free(f);
    }
}

static void
installed_flow_destroy(struct installed_flow *f)
{
    if (f) {
        ovs_assert(ovs_list_is_empty(&f->desired_refs));
        ovs_assert(!f->desired_flow);
        ovn_flow_uninit(&f->flow);
        free(f);
    }
}

/* Desired flow table operations. */
void
ovn_desired_flow_table_init(struct ovn_desired_flow_table *flow_table)
{
    hmap_init(&flow_table->match_flow_table);
    hmap_init(&flow_table->uuid_flow_table);
}

void
ovn_desired_flow_table_clear(struct ovn_desired_flow_table *flow_table)
{
    struct sb_to_flow *stf, *next;
    HMAP_FOR_EACH_SAFE (stf, next, hmap_node,
                        &flow_table->uuid_flow_table) {
        remove_flows_from_sb_to_flow(flow_table, stf, NULL);
    }
}

void
ovn_desired_flow_table_destroy(struct ovn_desired_flow_table *flow_table)
{
    ovn_desired_flow_table_clear(flow_table);
    hmap_destroy(&flow_table->match_flow_table);
    hmap_destroy(&flow_table->uuid_flow_table);
}


/* Installed flow table operations. */
static void
ovn_installed_flow_table_clear(void)
{
    struct installed_flow *f, *next;
    HMAP_FOR_EACH_SAFE (f, next, match_hmap_node, &installed_flows) {
        hmap_remove(&installed_flows, &f->match_hmap_node);
        unlink_all_refs_for_installed_flow(f);
        installed_flow_destroy(f);
    }
}

static void
ovn_installed_flow_table_destroy(void)
{
    ovn_installed_flow_table_clear();
    hmap_destroy(&installed_flows);
}

/* Flow table update. */

static struct ofpbuf *
encode_flow_mod(struct ofputil_flow_mod *fm)
{
    fm->buffer_id = UINT32_MAX;
    fm->out_port = OFPP_ANY;
    fm->out_group = OFPG_ANY;
    return ofputil_encode_flow_mod(fm, OFPUTIL_P_OF13_OXM);
}

static void
add_flow_mod(struct ofputil_flow_mod *fm, struct ovs_list *msgs)
{
    struct ofpbuf *msg = encode_flow_mod(fm);
    ovs_list_push_back(msgs, &msg->list_node);
}

/* group_table. */

static struct ofpbuf *
encode_group_mod(const struct ofputil_group_mod *gm)
{
    return ofputil_encode_group_mod(OFP13_VERSION, gm, NULL, -1);
}

static void
add_group_mod(const struct ofputil_group_mod *gm, struct ovs_list *msgs)
{
    struct ofpbuf *msg = encode_group_mod(gm);
    ovs_list_push_back(msgs, &msg->list_node);
}


static struct ofpbuf *
encode_meter_mod(const struct ofputil_meter_mod *mm)
{
    return ofputil_encode_meter_mod(OFP13_VERSION, mm);
}

static void
add_meter_mod(const struct ofputil_meter_mod *mm, struct ovs_list *msgs)
{
    struct ofpbuf *msg = encode_meter_mod(mm);
    ovs_list_push_back(msgs, &msg->list_node);
}

static void
add_ct_flush_zone(uint16_t zone_id, struct ovs_list *msgs)
{
    struct ofpbuf *msg = ofpraw_alloc(OFPRAW_NXT_CT_FLUSH_ZONE,
                                      rconn_get_version(swconn), 0);
    struct nx_zone_id *nzi = ofpbuf_put_zeros(msg, sizeof *nzi);
    nzi->zone_id = htons(zone_id);

    ovs_list_push_back(msgs, &msg->list_node);
}

static void
add_meter_string(struct ovn_extend_table_info *m_desired,
                 struct ovs_list *msgs)
{
    /* Create and install new meter. */
    struct ofputil_meter_mod mm;
    enum ofputil_protocol usable_protocols;
    char *meter_string = xasprintf("meter=%"PRIu32",%s",
                                   m_desired->table_id,
                                   &m_desired->name[52]);
    char *error = parse_ofp_meter_mod_str(&mm, meter_string, OFPMC13_ADD,
                                          &usable_protocols);
    if (!error) {
        add_meter_mod(&mm, msgs);
        free(mm.meter.bands);
    } else {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
        VLOG_ERR_RL(&rl, "new meter %s %s", error, meter_string);
        free(error);
    }
    free(meter_string);
}

static void
add_meter(struct ovn_extend_table_info *m_desired,
          const struct sbrec_meter_table *meter_table,
          struct ovs_list *msgs)
{
    const struct sbrec_meter *sb_meter;
    SBREC_METER_TABLE_FOR_EACH (sb_meter, meter_table) {
        if (!strcmp(m_desired->name, sb_meter->name)) {
            break;
        }
    }

    if (!sb_meter) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
        VLOG_ERR_RL(&rl, "could not find meter named \"%s\"", m_desired->name);
        return;
    }

    struct ofputil_meter_mod mm;
    mm.command = OFPMC13_ADD;
    mm.meter.meter_id = m_desired->table_id;
    mm.meter.flags = OFPMF13_STATS;

    if (!strcmp(sb_meter->unit, "pktps")) {
        mm.meter.flags |= OFPMF13_PKTPS;
    } else {
        mm.meter.flags |= OFPMF13_KBPS;
    }

    mm.meter.n_bands = sb_meter->n_bands;
    mm.meter.bands = xcalloc(mm.meter.n_bands, sizeof *mm.meter.bands);

    for (size_t i = 0; i < sb_meter->n_bands; i++) {
        struct sbrec_meter_band *sb_band = sb_meter->bands[i];
        struct ofputil_meter_band *mm_band = &mm.meter.bands[i];

        if (!strcmp(sb_band->action, "drop")) {
            mm_band->type = OFPMBT13_DROP;
        }

        mm_band->prec_level = 0;
        mm_band->rate = sb_band->rate;
        mm_band->burst_size = sb_band->burst_size;

        if (mm_band->burst_size) {
            mm.meter.flags |= OFPMF13_BURST;
        }
    }

    add_meter_mod(&mm, msgs);
    free(mm.meter.bands);
}

static void
installed_flow_add(struct ovn_flow *d, struct ovs_list *msgs)
{
    /* Send flow_mod to add flow. */
    struct ofputil_flow_mod fm = {
        .match = d->match,
        .priority = d->priority,
        .table_id = d->table_id,
        .ofpacts = d->ofpacts,
        .ofpacts_len = d->ofpacts_len,
        .new_cookie = htonll(d->cookie),
        .command = OFPFC_ADD,
    };
    add_flow_mod(&fm, msgs);
}

static void
installed_flow_mod(struct ovn_flow *i, struct ovn_flow *d,
                   struct ovs_list *msgs)
{
    /* Update actions in installed flow. */
    struct ofputil_flow_mod fm = {
        .match = i->match,
        .priority = i->priority,
        .table_id = i->table_id,
        .ofpacts = d->ofpacts,
        .ofpacts_len = d->ofpacts_len,
        .command = OFPFC_MODIFY_STRICT,
    };
    /* Update cookie if it is changed. */
    if (i->cookie != d->cookie) {
        fm.modify_cookie = true;
        fm.new_cookie = htonll(d->cookie);
        /* Use OFPFC_ADD so that cookie can be updated. */
        fm.command = OFPFC_ADD;
    }
    add_flow_mod(&fm, msgs);

    /* Replace 'i''s actions and cookie by 'd''s. */
    free(i->ofpacts);
    i->ofpacts = xmemdup(d->ofpacts, d->ofpacts_len);
    i->ofpacts_len = d->ofpacts_len;
    i->cookie = d->cookie;
}

static void
installed_flow_del(struct ovn_flow *i, struct ovs_list *msgs)
{
    struct ofputil_flow_mod fm = {
        .match = i->match,
        .priority = i->priority,
        .table_id = i->table_id,
        .command = OFPFC_DELETE_STRICT,
    };
    add_flow_mod(&fm, msgs);
}

/* The flow table can be updated if the connection to the switch is up and
 * in the correct state and not backlogged with existing flow_mods.  (Our
 * criteria for being backlogged appear very conservative, but the socket
 * between ovn-controller and OVS provides some buffering.) */
bool
ofctrl_can_put(void)
{
    if (state != S_UPDATE_FLOWS
        || rconn_packet_counter_n_packets(tx_counter)
        || rconn_get_version(swconn) < 0) {
        return false;
    }
    return true;
}

/* Replaces the flow table on the switch, if possible, by the flows added
 * with ofctrl_add_flow().
 *
 * Replaces the group table and meter table on the switch, if possible,
 * by the contents of '->desired'.
 *
 * Sends conntrack flush messages to each zone in 'pending_ct_zones' that
 * is in the CT_ZONE_OF_QUEUED state and then moves the zone into the
 * CT_ZONE_OF_SENT state.
 *
 * This should be called after ofctrl_run() within the main loop. */
void
ofctrl_put(struct ovn_desired_flow_table *flow_table,
           struct shash *pending_ct_zones,
           const struct sbrec_meter_table *meter_table,
           int64_t nb_cfg,
           bool flow_changed)
{
    static bool skipped_last_time = false;
    static int64_t old_nb_cfg = 0;
    bool need_put = false;
    if (flow_changed || skipped_last_time || need_reinstall_flows) {
        need_put = true;
    } else if (nb_cfg != old_nb_cfg) {
        /* nb_cfg changed since last ofctrl_put() call */
        if (cur_cfg == old_nb_cfg) {
            /* we were up-to-date already, so just update with the
             * new nb_cfg */
            cur_cfg = nb_cfg;
        } else {
            need_put = true;
        }
    }

    old_nb_cfg = nb_cfg;

    if (!need_put) {
        VLOG_DBG("ofctrl_put not needed");
        return;
    }
    if (!ofctrl_can_put()) {
        VLOG_DBG("ofctrl_put can't be performed");
        skipped_last_time = true;
        return;
    }

    skipped_last_time = false;
    need_reinstall_flows = false;

    /* OpenFlow messages to send to the switch to bring it up-to-date. */
    struct ovs_list msgs = OVS_LIST_INITIALIZER(&msgs);

    /* Iterate through ct zones that need to be flushed. */
    struct shash_node *iter;
    SHASH_FOR_EACH(iter, pending_ct_zones) {
        struct ct_zone_pending_entry *ctzpe = iter->data;
        if (ctzpe->state == CT_ZONE_OF_QUEUED) {
            add_ct_flush_zone(ctzpe->zone, &msgs);
            ctzpe->state = CT_ZONE_OF_SENT;
            ctzpe->of_xid = 0;
        }
    }

    /* Iterate through all the desired groups. If there are new ones,
     * add them to the switch. */
    struct ovn_extend_table_info *desired;
    EXTEND_TABLE_FOR_EACH_UNINSTALLED (desired, groups) {
        /* Create and install new group. */
        struct ofputil_group_mod gm;
        enum ofputil_protocol usable_protocols;
        char *group_string = xasprintf("group_id=%"PRIu32",%s",
                                       desired->table_id,
                                       desired->name);
        char *error = parse_ofp_group_mod_str(&gm, OFPGC11_ADD, group_string,
                                              NULL, NULL, &usable_protocols);
        if (!error) {
            add_group_mod(&gm, &msgs);
        } else {
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
            VLOG_ERR_RL(&rl, "new group %s %s", error, group_string);
            free(error);
        }
        free(group_string);
        ofputil_uninit_group_mod(&gm);
    }

    /* Iterate through all the desired meters. If there are new ones,
     * add them to the switch. */
    struct ovn_extend_table_info *m_desired;
    EXTEND_TABLE_FOR_EACH_UNINSTALLED (m_desired, meters) {
        if (!strncmp(m_desired->name, "__string: ", 10)) {
            /* The "set-meter" action creates a meter entry name that
             * describes the meter itself. */
            add_meter_string(m_desired, &msgs);
        } else {
            add_meter(m_desired, meter_table, &msgs);
        }
    }

    /* Iterate through all of the installed flows.  If any of them are no
     * longer desired, delete them; if any of them should have different
     * actions, update them. */
    struct installed_flow *i, *next;
    HMAP_FOR_EACH_SAFE (i, next, match_hmap_node, &installed_flows) {
        unlink_all_refs_for_installed_flow(i);
        struct desired_flow *d = desired_flow_lookup(flow_table, &i->flow,
                                                     NULL);
        if (!d) {
            /* Installed flow is no longer desirable.  Delete it from the
             * switch and from installed_flows. */
            installed_flow_del(&i->flow, &msgs);
            ovn_flow_log(&i->flow, "removing installed");
            hmap_remove(&installed_flows, &i->match_hmap_node);
            installed_flow_destroy(i);
        } else {
            if (!ofpacts_equal(i->flow.ofpacts, i->flow.ofpacts_len,
                               d->flow.ofpacts, d->flow.ofpacts_len) ||
                i->flow.cookie != d->flow.cookie) {
                ovn_flow_log(&i->flow, "updating installed");
                installed_flow_mod(&i->flow, &d->flow, &msgs);
            }
            link_installed_to_desired(i, d);

        }
    }

    /* Iterate through the desired flows and add those that aren't found
     * in the installed flow table. */
    struct desired_flow *d;
    HMAP_FOR_EACH (d, match_hmap_node, &flow_table->match_flow_table) {
        i = installed_flow_lookup(&d->flow);
        if (!i) {
            installed_flow_add(&d->flow, &msgs);
            ovn_flow_log(&d->flow, "adding installed");

            /* Copy 'd' from 'flow_table' to installed_flows. */
            i = installed_flow_dup(d);
            hmap_insert(&installed_flows, &i->match_hmap_node,
                        i->flow.hash);
        }
        link_installed_to_desired(i, d);
    }

    /* Iterate through the installed groups from previous runs. If they
     * are not needed delete them. */
    struct ovn_extend_table_info *installed, *next_group;
    EXTEND_TABLE_FOR_EACH_INSTALLED (installed, next_group, groups) {
        /* Delete the group. */
        struct ofputil_group_mod gm;
        enum ofputil_protocol usable_protocols;
        char *group_string = xasprintf("group_id=%"PRIu32"",
                                       installed->table_id);
        char *error = parse_ofp_group_mod_str(&gm, OFPGC11_DELETE,
                                              group_string, NULL, NULL,
                                              &usable_protocols);
        if (!error) {
            add_group_mod(&gm, &msgs);
        } else {
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
            VLOG_ERR_RL(&rl, "Error deleting group %d: %s",
                        installed->table_id, error);
            free(error);
        }
        free(group_string);
        ofputil_uninit_group_mod(&gm);
        ovn_extend_table_remove_existing(groups, installed);
    }

    /* Sync the contents of groups->desired to groups->existing. */
    ovn_extend_table_sync(groups);

    /* Iterate through the installed meters from previous runs. If they
     * are not needed delete them. */
    struct ovn_extend_table_info *m_installed, *next_meter;
    EXTEND_TABLE_FOR_EACH_INSTALLED (m_installed, next_meter, meters) {
        /* Delete the meter. */
        struct ofputil_meter_mod mm = {
            .command = OFPMC13_DELETE,
            .meter = { .meter_id = m_installed->table_id },
        };
        add_meter_mod(&mm, &msgs);

        ovn_extend_table_remove_existing(meters, m_installed);
    }

    /* Sync the contents of meters->desired to meters->existing. */
    ovn_extend_table_sync(meters);

    if (!ovs_list_is_empty(&msgs)) {
        /* Add a barrier to the list of messages. */
        struct ofpbuf *barrier = ofputil_encode_barrier_request(OFP13_VERSION);
        const struct ofp_header *oh = barrier->data;
        ovs_be32 xid_ = oh->xid;
        ovs_list_push_back(&msgs, &barrier->list_node);

        /* Queue the messages. */
        struct ofpbuf *msg;
        LIST_FOR_EACH_POP (msg, list_node, &msgs) {
            queue_msg(msg);
        }

        /* Store the barrier's xid with any newly sent ct flushes. */
        SHASH_FOR_EACH(iter, pending_ct_zones) {
            struct ct_zone_pending_entry *ctzpe = iter->data;
            if (ctzpe->state == CT_ZONE_OF_SENT && !ctzpe->of_xid) {
                ctzpe->of_xid = xid_;
            }
        }

        /* Track the flow update. */
        struct ofctrl_flow_update *fup, *prev;
        LIST_FOR_EACH_REVERSE_SAFE (fup, prev, list_node, &flow_updates) {
            if (nb_cfg < fup->nb_cfg) {
                /* This ofctrl_flow_update is for a configuration later than
                 * 'nb_cfg'.  This should not normally happen, because it means
                 * that 'nb_cfg' in the SB_Global table of the southbound
                 * database decreased, and it should normally be monotonically
                 * increasing. */
                VLOG_WARN("nb_cfg regressed from %"PRId64" to %"PRId64,
                          fup->nb_cfg, nb_cfg);
                ovs_list_remove(&fup->list_node);
                free(fup);
            } else if (nb_cfg == fup->nb_cfg) {
                /* This ofctrl_flow_update is for the same configuration as
                 * 'nb_cfg'.  Probably, some change to the physical topology
                 * means that we had to revise the OpenFlow flow table even
                 * though the logical topology did not change.  Update fp->xid,
                 * so that we don't send a notification that we're up-to-date
                 * until we're really caught up. */
                VLOG_DBG("advanced xid target for nb_cfg=%"PRId64, nb_cfg);
                fup->xid = xid_;
                goto done;
            } else {
                break;
            }
        }

        /* Add a flow update. */
        fup = xmalloc(sizeof *fup);
        ovs_list_push_back(&flow_updates, &fup->list_node);
        fup->xid = xid_;
        fup->nb_cfg = nb_cfg;
    done:;
    } else if (!ovs_list_is_empty(&flow_updates)) {
        /* Getting up-to-date with 'nb_cfg' didn't require any extra flow table
         * changes, so whenever we get up-to-date with the most recent flow
         * table update, we're also up-to-date with 'nb_cfg'. */
        struct ofctrl_flow_update *fup = ofctrl_flow_update_from_list_node(
            ovs_list_back(&flow_updates));
        fup->nb_cfg = nb_cfg;
    } else {
        /* We were completely up-to-date before and still are. */
        cur_cfg = nb_cfg;
    }
}

/* Looks up the logical port with the name 'port_name' in 'br_int_'.  If
 * found, returns true and sets '*portp' to the OpenFlow port number
 * assigned to the port.  Otherwise, returns false. */
static bool
ofctrl_lookup_port(const void *br_int_, const char *port_name,
                   unsigned int *portp)
{
    const struct ovsrec_bridge *br_int = br_int_;

    for (int i = 0; i < br_int->n_ports; i++) {
        const struct ovsrec_port *port_rec = br_int->ports[i];
        for (int j = 0; j < port_rec->n_interfaces; j++) {
            const struct ovsrec_interface *iface_rec = port_rec->interfaces[j];
            const char *iface_id = smap_get(&iface_rec->external_ids,
                                            "iface-id");

            if (iface_id && !strcmp(iface_id, port_name)) {
                if (!iface_rec->n_ofport) {
                    continue;
                }

                int64_t ofport = iface_rec->ofport[0];
                if (ofport < 1 || ofport > ofp_to_u16(OFPP_MAX)) {
                    continue;
                }
                *portp = ofport;
                return true;
            }
        }
    }

    return false;
}

/* Generates a packet described by 'flow_s' in the syntax of an OVN
 * logical expression and injects it into 'br_int'.  The flow
 * description must contain an ingress logical port that is present on
 * 'br_int'.
 *
 * Returns NULL if successful, otherwise an error message that the caller
 * must free(). */
char *
ofctrl_inject_pkt(const struct ovsrec_bridge *br_int, const char *flow_s,
                  const struct shash *addr_sets,
                  const struct shash *port_groups)
{
    int version = rconn_get_version(swconn);
    if (version < 0) {
        return xstrdup("OpenFlow channel not ready.");
    }

    struct flow uflow;
    char *error = expr_parse_microflow(flow_s, &symtab, addr_sets,
                                       port_groups, ofctrl_lookup_port,
                                       br_int, &uflow);
    if (error) {
        return error;
    }

    /* The physical OpenFlow port was stored in the logical ingress
     * port, so put it in the correct location for a flow structure. */
    uflow.in_port.ofp_port = u16_to_ofp(uflow.regs[MFF_LOG_INPORT - MFF_REG0]);
    uflow.regs[MFF_LOG_INPORT - MFF_REG0] = 0;

    if (!uflow.in_port.ofp_port) {
        return xstrdup("ingress port not found on hypervisor.");
    }

    uint64_t packet_stub[128 / 8];
    struct dp_packet packet;
    dp_packet_use_stub(&packet, packet_stub, sizeof packet_stub);
    flow_compose(&packet, &uflow, NULL, 64);

    uint64_t ofpacts_stub[1024 / 8];
    struct ofpbuf ofpacts = OFPBUF_STUB_INITIALIZER(ofpacts_stub);
    struct ofpact_resubmit *resubmit = ofpact_put_RESUBMIT(&ofpacts);
    resubmit->in_port = OFPP_IN_PORT;
    resubmit->table_id = 0;

    struct ofputil_packet_out po = {
        .packet = dp_packet_data(&packet),
        .packet_len = dp_packet_size(&packet),
        .buffer_id = UINT32_MAX,
        .ofpacts = ofpacts.data,
        .ofpacts_len = ofpacts.size,
    };
    match_set_in_port(&po.flow_metadata, uflow.in_port.ofp_port);
    enum ofputil_protocol proto = ofputil_protocol_from_ofp_version(version);
    queue_msg(ofputil_encode_packet_out(&po, proto));
    dp_packet_uninit(&packet);
    ofpbuf_uninit(&ofpacts);

    return NULL;
}

bool
ofctrl_is_connected(void)
{
    return rconn_is_connected(swconn);
}

void
ofctrl_set_probe_interval(int probe_interval)
{
    if (swconn) {
        rconn_set_probe_interval(swconn, probe_interval);
    }
}
