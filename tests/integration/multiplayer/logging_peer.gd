extends MultiplayerPeerExtension

# An ENetMultiplayerPeer that writes down how each packet travelled. SceneMultiplayer sets the
# transfer mode and channel from the called method's RPC config just before it puts the packet
# (SceneRPCInterface::_send_rpc), and ENet reports the mode and channel a packet arrived on, so the
# two logs are what `@rpc`'s reliability words did on the wire, at each end.

var sent: Array = []
var received: Array = []
var inner: ENetMultiplayerPeer:
	set(value):
		inner = value
		inner.peer_connected.connect(func(id: int) -> void: peer_connected.emit(id))
		inner.peer_disconnected.connect(func(id: int) -> void: peer_disconnected.emit(id))


func _put_packet_script(buffer: PackedByteArray) -> Error:
	sent.append([inner.transfer_mode, inner.transfer_channel])
	return inner.put_packet(buffer)


# Read before the packet is taken: ENet answers both off the front of its queue.
func _get_packet_script() -> PackedByteArray:
	received.append([inner.get_packet_mode(), inner.get_packet_channel()])
	return inner.get_packet()


func _get_available_packet_count() -> int:
	return inner.get_available_packet_count()


func _get_max_packet_size() -> int:
	return 1 << 24


func _get_packet_channel() -> int:
	return inner.get_packet_channel()


func _get_packet_mode() -> MultiplayerPeer.TransferMode:
	return inner.get_packet_mode()


func _set_transfer_channel(channel: int) -> void:
	inner.transfer_channel = channel


func _get_transfer_channel() -> int:
	return inner.transfer_channel


func _set_transfer_mode(mode: MultiplayerPeer.TransferMode) -> void:
	inner.transfer_mode = mode


func _get_transfer_mode() -> MultiplayerPeer.TransferMode:
	return inner.transfer_mode


func _set_target_peer(peer: int) -> void:
	inner.set_target_peer(peer)


func _get_packet_peer() -> int:
	return inner.get_packet_peer()


func _is_server() -> bool:
	return inner.get_unique_id() == 1


func _poll() -> void:
	inner.poll()


func _close() -> void:
	inner.close()


func _disconnect_peer(peer: int, force: bool) -> void:
	inner.disconnect_peer(peer, force)


func _get_unique_id() -> int:
	return inner.get_unique_id()


func _set_refuse_new_connections(enable: bool) -> void:
	inner.refuse_new_connections = enable


func _is_refusing_new_connections() -> bool:
	return inner.refuse_new_connections


func _is_server_relay_supported() -> bool:
	return inner.is_server_relay_supported()


func _get_connection_status() -> MultiplayerPeer.ConnectionStatus:
	return inner.get_connection_status()
