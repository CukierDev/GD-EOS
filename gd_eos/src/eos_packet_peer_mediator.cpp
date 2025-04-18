/****************************************
 * EOSPacketPeerMediator
 * Authors: Dallin Lovin aka LowFire
 *          忘忧の aka Daylily-Zeleen (daylily-zeleen@foxmail.com)
 * Description: Manages EOS multiplayer instances when they are active.
 * Multiplayer instances register their socket id with the mediator when
 * they become active and unregister their socket id when they close.
 * The mediator receives packets from the EOS P2P interface every process
 * frame and sorts those packets according to their destination socket so
 * that the appropriate multiplayer instance can poll them later. The mediator
 * receives EOS notifications and forwards it to the appropriate multiplayer
 * instance according to the socket the notification was received from.
 * Mediator manages incoming connection requests and forwards them to the
 * appropriate multiplayer instance according to the socket id of the
 * connection request. If there is no matching socket from any of the active
 * multiplayer instances, the mediator will hold onto the connection request
 * until either a multiplayer instance opens with a matching socket or until
 * the connection request times out.
 ****************************************/
#if !defined(EOS_P2P_DISABLED) && !defined(EOS_CONNECT_DISABLED)

#include <eos_p2p.h>
#include <interfaces/eos_p2p_interface.h>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/main_loop.hpp>

#include "eos_packet_peer_mediator.h"

namespace godot::eos {
EOSPacketPeerMediator *EOSPacketPeerMediator::singleton = nullptr;

void EOSPacketPeerMediator::_bind_methods() {
    ClassDB::bind_method(D_METHOD("get_total_packet_count"), &EOSPacketPeerMediator::get_total_packet_count);
    ClassDB::bind_method(D_METHOD("get_sockets"), &EOSPacketPeerMediator::get_sockets);
    ClassDB::bind_method(D_METHOD("get_packet_count_for_socket", "socket_id"), &EOSPacketPeerMediator::get_packet_count_for_socket);
    ClassDB::bind_method(D_METHOD("has_socket"), &EOSPacketPeerMediator::has_socket);
    ClassDB::bind_method(D_METHOD("get_packet_count_from_remote_user", "remote_user_id", "socket_id"), &EOSPacketPeerMediator::get_packet_count_from_remote_user);
    ClassDB::bind_method(D_METHOD("get_connection_request_count"), &EOSPacketPeerMediator::get_connection_request_count);

    ClassDB::bind_method(D_METHOD("get_queue_size_limit"), &EOSPacketPeerMediator::get_queue_size_limit);
    ClassDB::bind_method(D_METHOD("set_queue_size_limit", "limit"), &EOSPacketPeerMediator::set_queue_size_limit);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "queue_size_limit"), "set_queue_size_limit", "get_queue_size_limit");

    ADD_SIGNAL(MethodInfo("packet_queue_full"));
    ADD_SIGNAL(MethodInfo("connection_request_received", EOSMultiPlayerConnectionInfo::make_property_info()));
    ADD_SIGNAL(MethodInfo("connection_request_removed", EOSMultiPlayerConnectionInfo::make_property_info()));
}

/****************************************
 * _on_process_frame
 * Description: Method is connected to the game main loop's process signal so
 * that it can execute every process frame (see _init()). Checks if there are
 * any packets available from the incoming packet queue. If there are, receives
 * the packet and sorts it into separate queues according to it's destination socket. Packets that
 * are peer id packets (packets with EVENT_RECEIVE_PEER_ID) and pushed to the front. Packets will
 * stop being polled if the queue size limit is reached.
 ****************************************/
void EOSPacketPeerMediator::_on_process_frame() {
    if (EOSMultiplayerPeer::get_local_user_id() == nullptr)
        return;
    if (socket_packet_queues.size() == 0)
        return;
    if (get_total_packet_count() >= max_queue_size)
        return;

    EOS_ProductUserId local_user_id = EOSMultiplayerPeer::get_local_user_id();
    EOS_P2P_GetNextReceivedPacketSizeOptions packet_size_options;
    packet_size_options.ApiVersion = EOS_P2P_GETNEXTRECEIVEDPACKETSIZE_API_LATEST;
    packet_size_options.LocalUserId = local_user_id;
    packet_size_options.RequestedChannel = nullptr;
    uint32_t max_packet_size;

    EOS_P2P_ReceivePacketOptions receive_packet_options;
    receive_packet_options.ApiVersion = EOS_P2P_RECEIVEPACKET_API_LATEST;
    receive_packet_options.LocalUserId = local_user_id;
    receive_packet_options.MaxDataSizeBytes = EOS_P2P_MAX_PACKET_SIZE;
    receive_packet_options.RequestedChannel = nullptr;

    bool next_packet_available = true;
    EOS_EResult result = EOS_EResult::EOS_Success;

    do {
        result = EOS_P2P_GetNextReceivedPacketSize(EOSP2P::get_singleton()->get_handle(), &packet_size_options, &max_packet_size);

        ERR_FAIL_COND_MSG(result == EOS_EResult::EOS_InvalidParameters, "Failed to get packet size! Invalid parameters.");

        if (result == EOS_EResult::EOS_Success) {
            next_packet_available = true;
        } else {
            next_packet_available = false;
        }

        if (next_packet_available) {
            PackedByteArray packet_data;
            packet_data.resize(max_packet_size);
            uint32_t buffer_size;
            uint8_t channel;
            EOS_P2P_SocketId socket;
            EOS_ProductUserId remote_user;
            result = EOS_P2P_ReceivePacket(EOSP2P::get_singleton()->get_handle(), &receive_packet_options, &remote_user, &socket, &channel, packet_data.ptrw(), &buffer_size);
            String socket_name = socket.SocketName;

            ERR_FAIL_COND_MSG(result == EOS_EResult::EOS_InvalidParameters, "Failed to get packet! Invalid parameters.");
            ERR_FAIL_COND_MSG(result == EOS_EResult::EOS_NotFound, "Failed to get packet! Packet is too large. This should not have happened.");

            if (!socket_packet_queues.has(socket_name))
                return; //invalid socket. Drop the packet.

            SharedPtr<PacketData> packet = SharedPtr<PacketData>::make_shared();
            packet->store(packet_data.ptrw(), max_packet_size);
            packet->set_channel(channel);
            packet->set_sender(remote_user);
            uint8_t event = packet->get_data().ptr()[EOSMultiplayerPeer::INDEX_EVENT_TYPE];

            if (event == EOSMultiplayerPeer::EVENT_RECEIVE_PEER_ID) {
                socket_packet_queues[socket_name].push_front(packet);
            } else {
                socket_packet_queues[socket_name].push_back(packet);
            }

            if (get_total_packet_count() >= max_queue_size) {
                emit_signal(SNAME("packet_queue_full"));
                break;
            }
        }
    } while (next_packet_available);
}

/****************************************
 * poll_next_packet
 * Parameters:
 *   socket_id - The socket to poll a packet from.
 * Description: Polls the next packet available for the given socket.
 * Returns a valid packet if has been successfully polled.
 ****************************************/
EOSPacketPeerMediator::SharedPtr<PacketData> EOSPacketPeerMediator::poll_next_packet(const String &socket_id) {
    if (!socket_packet_queues.has(socket_id))
        return {};
    if (socket_packet_queues[socket_id].size() == 0)
        return {};

    SharedPtr<PacketData> next_packet = socket_packet_queues[socket_id].front()->get();
    socket_packet_queues[socket_id].pop_front();
    return next_packet;
}

/****************************************
 * register_peer
 * Parameters:
 *   peer - The peer to be registered with the mediator.
 * Description: Registers a peer and it's socket with the mediator.
 * Once registered, a peer can receive packets, EOS notifications, and connection requests.
 ****************************************/
bool EOSPacketPeerMediator::register_peer(EOSMultiplayerPeer *peer) {
    ERR_FAIL_COND_V_MSG(!initialized, false, "Failed to register peer. EOSPacketPeerMediator has not been initialized. Call EOSPacketPeerMediator.init() before starting a multiplayer instance.");
    String peer_socket_name = peer->get_socket_name();
    ERR_FAIL_COND_V_MSG(peer_socket_name.is_empty(), false, "Failed to register peer. Peer is not active.");
    ERR_FAIL_COND_V_MSG(active_peers.has(peer_socket_name), false, "Failed to register peer. This peer has already been registered.");

    active_peers.insert(peer_socket_name, peer);
    socket_packet_queues.insert(peer_socket_name, List<SharedPtr<PacketData>>());

    _forward_pending_connection_requests(peer);

    return true;
}

/****************************************
 * unregister_peer
 * Parameters:
 *   peer - The peer to be unregistered with the mediator.
 * Description: Unregisters a peer and it's socket with the mediator.
 * Peers can no longer receive packets, notifications, or connection requests once this is done.
 * unregistration usually happens when a peer closes.
 ****************************************/
void EOSPacketPeerMediator::unregister_peer(EOSMultiplayerPeer *peer) {
    String peer_socket_name = peer->get_socket_name();
    if (!active_peers.has(peer_socket_name))
        return;

    clear_packet_queue(peer_socket_name);
    socket_packet_queues.erase(peer_socket_name);
    active_peers.erase(peer_socket_name);
}

/****************************************
 * clear_packet_queue
 * Parameters:
 *   socket_id - The socket to clear packets from.
 * Description: Removes all packets queued for the given socket.
 ****************************************/
void EOSPacketPeerMediator::clear_packet_queue(const String &socket_id) {
    ERR_FAIL_COND_MSG(!socket_packet_queues.has(socket_id), vformat("Failed to clear packet queue for socket \"%s\". Socket was not registered.", socket_id));

    socket_packet_queues[socket_id].clear();
}

/****************************************
 * clear_packets_from_remote_user
 * Parameters:
 *   socket_id - The socket to clear packets from.
 *	remote_user_id - The user to remove packets from.
 * Description: Removes all packets queued for the given socket and from the given remote user.
 * This is usually called when a peer disconnects. All packets from that peer are removed.
 ****************************************/
void EOSPacketPeerMediator::clear_packets_from_remote_user(const String &socket_id, EOS_ProductUserId remote_user_id) {
    ERR_FAIL_COND_MSG(!socket_packet_queues.has(socket_id), vformat("Failed to clear packet queue for socket \"%s\". Socket was not registered.", socket_id));

    using ElementTy = List<SharedPtr<PacketData>>::Element;
    List<ElementTy *> del;
    for (ElementTy *e = socket_packet_queues[socket_id].front(); e != nullptr; e = e->next()) {
        if (e->get()->get_sender() != remote_user_id)
            continue;
        del.push_back(e);
    }
    for (ElementTy *e : del) {
        e->erase();
    }
}

/****************************************
 * _init
 * Description: Initialized EOSPacketPeerMediator. Connects _on_process_frame to the
 * main loop's process signal. Adds EOS callbacks so that it can receive notifications.
 ****************************************/
void EOSPacketPeerMediator::_init() {
    ERR_FAIL_NULL_MSG(EOSMultiplayerPeer::get_local_user_id(), "Failed to initialize EOSPacketPeerMediator. Local user id has not been set.");
    if (initialized)
        return;

    MainLoop *main_loop = Engine::get_singleton()->get_main_loop();
    ERR_FAIL_COND_MSG(!main_loop->has_signal("process_frame"), "Failed to initialize EOSPacketPeerMediator. Main loop does not have the \"process_frame\" signal.");
    main_loop->connect("process_frame", callable_mp(this, &EOSPacketPeerMediator::_on_process_frame));

    //Register callbacks
    _add_connection_closed_callback();
    _add_connection_established_callback();
    _add_connection_interrupted_callback();
    _add_connection_request_callback();

    initialized = true;
}

/****************************************
 * _terminate
 * Description: Terminates EOSPacketPeerMediator. Disconnects from the
 * main loop's process signal. Removes all EOS callbacks.
 ****************************************/
void EOSPacketPeerMediator::_terminate() {
    if (!initialized)
        return;

    if (MainLoop *main_loop = Engine::get_singleton()->get_main_loop()) {
        main_loop->disconnect("process_frame", callable_mp(this, &EOSPacketPeerMediator::_on_process_frame));
    }

    EOSMultiplayerPeer::set_local_user_id({});

    //Unregister callbacks
    auto p2p_interface_handle = EOSP2P::get_singleton()->get_handle();
    EOS_P2P_RemoveNotifyPeerConnectionEstablished(p2p_interface_handle, connection_established_callback_id);
    EOS_P2P_RemoveNotifyPeerConnectionInterrupted(p2p_interface_handle, connection_interrupted_callback_id);
    EOS_P2P_RemoveNotifyPeerConnectionClosed(p2p_interface_handle, connection_closed_callback_id);
    EOS_P2P_RemoveNotifyPeerConnectionRequest(p2p_interface_handle, connection_request_callback_id);

    // 相关回调是否会在登入状态改变后自动触发
    // pending_connection_requests.clear();
    // active_peers.clear();

    initialized = false;
}

/****************************************
 * get_packet_count_from_remote_user
 * Parameters:
 *	remote_user - The user to count packets for.
 * 	socket_id - The socket to count packets for.
 * Description: Counts the number of packets from the given remote user and for
 * the given socket. Returns the packet count.
 ****************************************/
int EOSPacketPeerMediator::_get_packet_count_from_remote_user(EOS_ProductUserId remote_user_id, const String &socket_id) {
    int ret = 0;
    for (const SharedPtr<PacketData> &data : socket_packet_queues[socket_id]) {
        if (data->get_sender() == remote_user_id) {
            ret++;
        }
    }
    return ret;
}

int EOSPacketPeerMediator::get_packet_count_from_remote_user(const Ref<EOSProductUserId> &remote_user_id, const String &socket_id) {
    ERR_FAIL_NULL_V(remote_user_id, 0);
    ERR_FAIL_COND_V_MSG(!socket_packet_queues.has(socket_id), 0, vformat("Failed to get packet count for remote user. Socket \"%s\" does not exist", socket_id));
    return _get_packet_count_from_remote_user(remote_user_id->get_handle(), socket_id);
}

/****************************************
 * next_packet_is_peer_id_packet
 * Parameters:
 * 	socket_id - The socket to check
 * Description: Checks if there is a peer id packet queued for the given socket.
 * Returns true if there is, false otherwise.
 ****************************************/
bool EOSPacketPeerMediator::next_packet_is_peer_id_packet(const String &socket_id) {
    ERR_FAIL_COND_V_MSG(!socket_packet_queues.has(socket_id), false, "Failed to check next packet. Socket \"%s\" does not exist.");
    if (socket_packet_queues[socket_id].size() == 0)
        return false;
    List<SharedPtr<PacketData>> &packet_list = socket_packet_queues[socket_id];
    if (!packet_list.is_empty()) {
        const SharedPtr<PacketData> &packet = packet_list.front()->get();
        
        uint8_t event = packet->get_data().ptr()[EOSMultiplayerPeer::INDEX_EVENT_TYPE];

        return event == EOSMultiplayerPeer::EVENT_RECEIVE_PEER_ID;
    }
    
    return false;
}

/****************************************
 * _on_peer_connection_established
 * Parameters:
 * 	data - Data returned from the notification
 * Description: An EOS callback that is called when a connection is established with a peer.
 * Forwards the data to the appropriate multiplayer instance using the socket id provided in the data.
 ****************************************/
void EOS_CALL EOSPacketPeerMediator::_on_peer_connection_established(const EOS_P2P_OnPeerConnectionEstablishedInfo *data) {
    String socket_id = data->SocketId->SocketName;
    if (!singleton->active_peers.has(socket_id))
        return;
    singleton->active_peers[socket_id]->peer_connection_established_callback(data);
}

/****************************************
 * _on_peer_connection_interrupted
 * Parameters:
 * 	data - Data returned from the notification
 * Description: An EOS callback that is called when the connection with a peer is interrupted.
 * Forwards the data to the appropriate multiplayer instance using the socket id provided in the data.
 ****************************************/
void EOS_CALL EOSPacketPeerMediator::_on_peer_connection_interrupted(const EOS_P2P_OnPeerConnectionInterruptedInfo *data) {
    String socket_id = data->SocketId->SocketName;
    if (!singleton->active_peers.has(socket_id))
        return;
    singleton->active_peers[socket_id]->peer_connection_interrupted_callback(data);
}

/****************************************
 * _on_remote_connection_closed
 * Parameters:
 * 	data - Data returned from the notification
 * Description: An EOS callback that is called when the connection with a peer is closed.
 * Checks to see if there were any connection requests associated with the closed connection.
 * If so, it removes that connection request. Forwards the data to the appropriate multiplayer instance using
 * the socket id provided in the data.
 ****************************************/
void EOS_CALL EOSPacketPeerMediator::_on_remote_connection_closed(const EOS_P2P_OnRemoteConnectionClosedInfo *data) {
    ERR_FAIL_COND(EOSMultiplayerPeer::get_local_user_id() != data->LocalUserId);
    String socket_name = data->SocketId->SocketName;
    //Check if any connection requests need to be removed.
    List<ConnectionRequestData>::Element *e = singleton->pending_connection_requests.front();
    for (; e != nullptr; e = e->next()) {
        String request_socket_name = e->get().socket_name;
        if (e->get().remote_user_id == data->RemoteUserId && socket_name == request_socket_name) {
            singleton->emit_signal(SNAME("connection_request_removed"), EOSMultiPlayerConnectionInfo::make(e->get()));
            e->erase();
            break;
        }
    }
    if (!singleton->active_peers.has(socket_name))
        return;
    singleton->active_peers[socket_name]->remote_connection_closed_callback(data);
}

/****************************************
 * _on_incoming_connection_request
 * Parameters:
 * 	data - Data returned from the notification
 * Description: An EOS callback that is called when a connection request is received.
 * Checks if there are any peers available to receive the connection request using
 * the destination socket id. If there isn't, stores the connection request for later.
 * If there is, forward the connection request to that multiplayer instance.
 ****************************************/
void EOS_CALL EOSPacketPeerMediator::_on_incoming_connection_request(const EOS_P2P_OnIncomingConnectionRequestInfo *data) {
    ERR_FAIL_COND(EOSMultiplayerPeer::get_local_user_id() != data->LocalUserId);
    ConnectionRequestData request_data{
        data->SocketId->SocketName,
#ifndef EOS_ASSUME_ONLY_ONE_USER
        data->LocalUserId,
#endif // !EOS_ASSUME_ONLY_ONE_USER
        data->RemoteUserId,
    };

    if (!singleton->active_peers.has(request_data.socket_name)) {
        //Hold onto the connection request just in case a socket does get opened with this socket id
        singleton->pending_connection_requests.push_back(request_data);
        singleton->emit_signal(SNAME("connection_request_received"), EOSMultiPlayerConnectionInfo::make(request_data));
        return;
    }
    singleton->active_peers[request_data.socket_name]->connection_request_callback(request_data);
}

/****************************************
 * _on_connect_interface_login
 * Parameters:
 * 	data - Contains info about the login.
 * Description: Called when the user logs into the connect interface. Sets the
 * local user id received from the login and initialized EOSPacketPeerMediator.
 ****************************************/
void EOSPacketPeerMediator::_on_connect_interface_login(const Ref<EOSConnect_LoginCallbackInfo> &p_login_callback_info) {
    ERR_FAIL_COND(initialized);
    ERR_FAIL_COND(p_login_callback_info->get_result_code() != EOS_EResult::EOS_Success);
    ERR_FAIL_NULL_MSG(p_login_callback_info->get_local_user_id(), "Local user id was not set on connect interface login.");
    EOSMultiplayerPeer::set_local_user_id(p_login_callback_info->get_local_user_id());
    _init();
}

void EOSPacketPeerMediator::_on_connect_interface_login_statues_changed(const Ref<EOSConnect_LoginStatusChangedCallbackInfo> &p_callback_info) {
    if (p_callback_info->get_current_status() == EOS_ELoginStatus::EOS_LS_LoggedIn) {
        return;
    }
    _terminate();
}

/****************************************
 * _add_connection_established_callback
 * Parameters:
 * Description: Adds the peer connection established callback. This is called
 * in _init()
 ****************************************/
bool EOSPacketPeerMediator::_add_connection_established_callback() {
    EOS_ProductUserId local_user_id = EOSMultiplayerPeer::get_local_user_id();
    EOS_P2P_AddNotifyPeerConnectionEstablishedOptions connection_established_options;
    connection_established_options.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONESTABLISHED_API_LATEST;
    connection_established_options.LocalUserId = local_user_id;
    connection_established_options.SocketId = nullptr;
    connection_established_callback_id = EOS_P2P_AddNotifyPeerConnectionEstablished(EOSP2P::get_singleton()->get_handle(),
            &connection_established_options, this, _on_peer_connection_established);
    ERR_FAIL_COND_V_MSG(connection_established_callback_id == EOS_INVALID_NOTIFICATIONID, false, "Failed to add connection established callback.");
    return true;
}

/****************************************
 * _add_connection_interrupted_callback
 * Description: Adds the peer connection interrupted callback. This is called
 * in _init()
 ****************************************/
bool EOSPacketPeerMediator::_add_connection_interrupted_callback() {
    EOS_ProductUserId local_user_id = EOSMultiplayerPeer::get_local_user_id();
    EOS_P2P_AddNotifyPeerConnectionInterruptedOptions connection_interrupted_options;
    connection_interrupted_options.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONINTERRUPTED_API_LATEST;
    connection_interrupted_options.LocalUserId = local_user_id;
    connection_interrupted_options.SocketId = nullptr;
    connection_interrupted_callback_id = EOS_P2P_AddNotifyPeerConnectionInterrupted(EOSP2P::get_singleton()->get_handle(),
            &connection_interrupted_options, this, _on_peer_connection_interrupted);
    ERR_FAIL_COND_V_MSG(connection_interrupted_callback_id == EOS_INVALID_NOTIFICATIONID, false, "Failed to add connection interrupted callback.");
    return true;
}

/****************************************
 * _add_connection_closed_callback
 * Description: Adds the peer connection closed callback. This is called
 * in _init()
 ****************************************/
bool EOSPacketPeerMediator::_add_connection_closed_callback() {
    EOS_ProductUserId local_user_id = EOSMultiplayerPeer::get_local_user_id();
    EOS_P2P_AddNotifyPeerConnectionClosedOptions connection_closed_options;
    connection_closed_options.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONCLOSED_API_LATEST;
    connection_closed_options.LocalUserId = local_user_id;
    connection_closed_options.SocketId = nullptr;
    connection_closed_callback_id = EOS_P2P_AddNotifyPeerConnectionClosed(EOSP2P::get_singleton()->get_handle(),
            &connection_closed_options, this, _on_remote_connection_closed);
    ERR_FAIL_COND_V_MSG(connection_closed_callback_id == EOS_INVALID_NOTIFICATIONID, false, "Failed to add connection closed callback.");
    return true;
}

/****************************************
 * _add_connection_request_callback
 * Description: Adds the peer connection request callback. This is called
 * in _init()
 ****************************************/
bool EOSPacketPeerMediator::_add_connection_request_callback() {
    EOS_ProductUserId local_user_id = EOSMultiplayerPeer::get_local_user_id();
    EOS_P2P_AddNotifyPeerConnectionRequestOptions connection_request_options;
    connection_request_options.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONREQUEST_API_LATEST;
    connection_request_options.LocalUserId = local_user_id;
    connection_request_options.SocketId = nullptr;
    connection_request_callback_id = EOS_P2P_AddNotifyPeerConnectionRequest(EOSP2P::get_singleton()->get_handle(),
            &connection_request_options, this, _on_incoming_connection_request);
    ERR_FAIL_COND_V_MSG(connection_request_callback_id == EOS_INVALID_NOTIFICATIONID, false, "Failed to add connection request callback.");
    return true;
}

/****************************************
 * _forward_pending_connection_requests
 * Parameters:
 *	peer - The peer to forward connection requests to.
 * Description: Attempts to forward any pending connection requests to the given multiplayer instance.
 * If none of the pending requests match the multiplayer instance's socket, then no connection requests
 * are forwarded.
 ****************************************/
void EOSPacketPeerMediator::_forward_pending_connection_requests(EOSMultiplayerPeer *peer) {
    using ElementTy = List<ConnectionRequestData>::Element;
    ElementTy *e = pending_connection_requests.front();
    List<ElementTy *> del;
    for (; e != nullptr; e = e->next()) {
        if (peer->get_socket_name() != e->get().socket_name)
            continue;
        peer->connection_request_callback(e->get());
        del.push_back(e);
    }
    for (ElementTy *e : del) {
        emit_signal(SNAME("connection_request_removed"), EOSMultiPlayerConnectionInfo::make(e->get()));
        e->erase();
    }
}

void EOSPacketPeerMediator::_notification(int p_what) {
    if (p_what == NOTIFICATION_POSTINITIALIZE) {
        EOSConnect::get_singleton()->connect("on_login", callable_mp(this, &EOSPacketPeerMediator::_on_connect_interface_login));
        EOSConnect::get_singleton()->connect("login_status_changed", callable_mp(this, &EOSPacketPeerMediator::_on_connect_interface_login_statues_changed));
    }
}

/****************************************
 * EOSPacketPeerMediator
 * Description: Default constructor. Sets the singleton. Connects
 * to the connect interface login callback so that the class knows
 * when to initialize.
 ****************************************/
EOSPacketPeerMediator::EOSPacketPeerMediator() {
    ERR_FAIL_COND_MSG(singleton != nullptr, "EOSPacketPeerMediator already initialized");
    singleton = this;
}

/****************************************
 * !EOSPacketPeerMediator
 * Description: Destructor. Sets singleton to null.
 ****************************************/
EOSPacketPeerMediator::~EOSPacketPeerMediator() {
    if (singleton != this)
        return;

    _terminate();
    singleton = nullptr;
}

String EOSPacketPeerMediator::_to_string() const {
    return vformat("<%s#%d>", get_class_static(), get_instance_id());
}

} //namespace godot::eos

#endif // !defined(EOS_P2P_DISABLED) && !defined(EOS_CONNECT_DISABLED)