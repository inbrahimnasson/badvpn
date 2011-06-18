/**
 * @file DPReceive.c
 * @author Ambroz Bizjak <ambrop7@gmail.com>
 * 
 * @section LICENSE
 * 
 * This file is part of BadVPN.
 * 
 * BadVPN is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 * 
 * BadVPN is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stddef.h>
#include <limits.h>

#include <protocol/dataproto.h>
#include <misc/byteorder.h>
#include <misc/offset.h>
#include <base/BLog.h>

#include <client/DPReceive.h>

#include <generated/blog_channel_DPReceive.h>

static DPReceivePeer * find_peer (DPReceiveDevice *o, peerid_t id)
{
    LinkedList2Node *node = LinkedList2_GetFirst(&o->peers_list);
    while (node) {
        DPReceivePeer *p = UPPER_OBJECT(node, DPReceivePeer, list_node);
        if (p->peer_id == id) {
            return p;
        }
        node = LinkedList2Node_Next(node);
    }
    
    return NULL;
}

static void receiver_recv_handler_send (DPReceiveReceiver *o, uint8_t *packet, int packet_len)
{
    DebugObject_Access(&o->d_obj);
    ASSERT(o->peer)
    DPReceivePeer *peer = o->peer;
    DPReceiveDevice *device = peer->device;
    ASSERT(packet_len >= 0)
    ASSERT(packet_len <= device->packet_mtu)
    
    uint8_t *data = packet;
    int data_len = packet_len;
    
    int local = 0;
    DPReceivePeer *src_peer;
    DPReceivePeer *relay_dest_peer = NULL;
    
    // check header
    if (data_len < sizeof(struct dataproto_header)) {
        BLog(BLOG_WARNING, "no dataproto header");
        goto out;
    }
    struct dataproto_header *header = (struct dataproto_header *)data;
    data += sizeof(*header);
    data_len -= sizeof(*header);
    uint8_t flags = ltoh8(header->flags);
    peerid_t from_id = ltoh16(header->from_id);
    int num_ids = ltoh16(header->num_peer_ids);
    
    // check destination ID
    if (!(num_ids == 0 || num_ids == 1)) {
        BLog(BLOG_WARNING, "wrong number of destinations");
        goto out;
    }
    peerid_t to_id;
    if (num_ids == 1) {
        if (data_len < sizeof(to_id)) {
            BLog(BLOG_WARNING, "missing destination");
            goto out;
        }
        to_id = ((struct dataproto_peer_id *)data)->id;
        data += sizeof(to_id);
        data_len -= sizeof(to_id);
    }
    
    // check remaining data
    if (data_len > device->device_mtu) {
        BLog(BLOG_WARNING, "frame too large");
        goto out;
    }
    
    // inform sink of received packet
    if (peer->dp_sink) {
        DataProtoSink_Received(peer->dp_sink, !!(flags & DATAPROTO_FLAGS_RECEIVING_KEEPALIVES));
    }
    
    if (num_ids == 1) {
        // find source peer
        if (!(src_peer = find_peer(device, from_id))) {
            BLog(BLOG_INFO, "source peer %d not known", (int)from_id);
            goto out;
        }
        
        // is frame for device or another peer?
        if (device->have_peer_id && to_id == device->peer_id) {
            // let the frame decider analyze the frame
            FrameDeciderPeer_Analyze(src_peer->decider_peer, data, data_len);
            
            // pass frame to device
            local = 1;
        } else {
            // check if relaying is allowed
            if (!peer->is_relay_client) {
                BLog(BLOG_WARNING, "relaying not allowed");
                goto out;
            }
            
            // provided source ID must be the peer sending the frame
            if (src_peer != peer) {
                BLog(BLOG_WARNING, "relay source must be the sending peer");
                goto out;
            }
            
            // find destination peer
            DPReceivePeer *dest_peer = find_peer(device, to_id);
            if (!dest_peer) {
                BLog(BLOG_INFO, "relay destination peer not known");
                goto out;
            }
            
            // destination cannot be source
            if (dest_peer == src_peer) {
                BLog(BLOG_WARNING, "relay destination cannot be the source");
                goto out;
            }
            
            relay_dest_peer = dest_peer;
        }
    }
    
out:
    // accept packet
    PacketPassInterface_Done(&o->recv_if);
    
    // pass packet to device
    if (local) {
        o->device->output_func(o->device->output_func_user, data, data_len);
    }
    
    // relay frame
    if (relay_dest_peer) {
        DPRelayRouter_SubmitFrame(&device->relay_router, &src_peer->relay_source, &relay_dest_peer->relay_sink, data, data_len, device->relay_flow_buffer_size, device->relay_flow_inactivity_time);
    }
}

int DPReceiveDevice_Init (DPReceiveDevice *o, int device_mtu, DPReceiveDevice_output_func output_func, void *output_func_user, BReactor *reactor, int relay_flow_buffer_size, int relay_flow_inactivity_time)
{
    ASSERT(device_mtu >= 0)
    ASSERT(device_mtu <= INT_MAX - DATAPROTO_MAX_OVERHEAD)
    ASSERT(relay_flow_buffer_size > 0)
    
    // init arguments
    o->device_mtu = device_mtu;
    o->output_func = output_func;
    o->output_func_user = output_func_user;
    o->reactor = reactor;
    o->relay_flow_buffer_size = relay_flow_buffer_size;
    o->relay_flow_inactivity_time = relay_flow_inactivity_time;
    
    // remember packet MTU
    o->packet_mtu = DATAPROTO_MAX_OVERHEAD + o->device_mtu;
    
    // init relay router
    if (!DPRelayRouter_Init(&o->relay_router, o->device_mtu, o->reactor)) {
        BLog(BLOG_ERROR, "DPRelayRouter_Init failed");
        goto fail0;
    }
    
    // have no peer ID
    o->have_peer_id = 0;
    
    // init peers list
    LinkedList2_Init(&o->peers_list);
    
    DebugObject_Init(&o->d_obj);
    return 1;
    
fail0:
    return 0;
}

void DPReceiveDevice_Free (DPReceiveDevice *o)
{
    DebugObject_Free(&o->d_obj);
    ASSERT(LinkedList2_IsEmpty(&o->peers_list))
    
    // free relay router
    DPRelayRouter_Free(&o->relay_router);
}

void DPReceiveDevice_SetPeerID (DPReceiveDevice *o, peerid_t peer_id)
{
    DebugObject_Access(&o->d_obj);
    
    // remember peer ID
    o->peer_id = peer_id;
    o->have_peer_id = 1;
}

void DPReceivePeer_Init (DPReceivePeer *o, DPReceiveDevice *device, peerid_t peer_id, FrameDeciderPeer *decider_peer, int is_relay_client)
{
    DebugObject_Access(&device->d_obj);
    ASSERT(is_relay_client == 0 || is_relay_client == 1)
    
    // init arguments
    o->device = device;
    o->peer_id = peer_id;
    o->decider_peer = decider_peer;
    o->is_relay_client = is_relay_client;
    
    // init relay source
    DPRelaySource_Init(&o->relay_source, &device->relay_router, o->peer_id, device->reactor);
    
    // init relay sink
    DPRelaySink_Init(&o->relay_sink, o->peer_id);
    
    // have no sink
    o->dp_sink = NULL;
    
    // insert to peers list
    LinkedList2_Append(&device->peers_list, &o->list_node);
    
    DebugCounter_Init(&o->d_receivers_ctr);
    DebugObject_Init(&o->d_obj);
}

void DPReceivePeer_Free (DPReceivePeer *o)
{
    DebugObject_Free(&o->d_obj);
    DebugCounter_Free(&o->d_receivers_ctr);
    ASSERT(!o->dp_sink)
    
    // remove from peers list
    LinkedList2_Remove(&o->device->peers_list, &o->list_node);
    
    // free relay sink
    DPRelaySink_Free(&o->relay_sink);
    
    // free relay source
    DPRelaySource_Free(&o->relay_source);
}

void DPReceivePeer_AttachSink (DPReceivePeer *o, DataProtoSink *dp_sink)
{
    DebugObject_Access(&o->d_obj);
    ASSERT(!o->dp_sink)
    ASSERT(dp_sink)
    
    // attach relay sink
    DPRelaySink_Attach(&o->relay_sink, dp_sink);
    
    o->dp_sink = dp_sink;
}

void DPReceivePeer_DetachSink (DPReceivePeer *o)
{
    DebugObject_Access(&o->d_obj);
    ASSERT(o->dp_sink)
    
    // detach relay sink
    DPRelaySink_Detach(&o->relay_sink);
    
    o->dp_sink = NULL;
}

void DPReceiveReceiver_Init (DPReceiveReceiver *o, DPReceivePeer *peer)
{
    DebugObject_Access(&peer->d_obj);
    DPReceiveDevice *device = peer->device;
    
    // init arguments
    o->peer = peer;
    
    // remember device
    o->device = device;
    
    // init receive interface
    PacketPassInterface_Init(&o->recv_if, device->packet_mtu, (PacketPassInterface_handler_send)receiver_recv_handler_send, o, BReactor_PendingGroup(device->reactor));
    
    DebugCounter_Increment(&peer->d_receivers_ctr);
    DebugObject_Init(&o->d_obj);
}

void DPReceiveReceiver_Free (DPReceiveReceiver *o)
{
    DebugObject_Free(&o->d_obj);
    DebugCounter_Decrement(&o->peer->d_receivers_ctr);
    
    // free receive interface
    PacketPassInterface_Free(&o->recv_if);
}

PacketPassInterface * DPReceiveReceiver_GetInput (DPReceiveReceiver *o)
{
    DebugObject_Access(&o->d_obj);
    
    return &o->recv_if;
}
