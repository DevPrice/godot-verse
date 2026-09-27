extends Node

# One of the two games tools/run_multiplayer.py starts for R-EXP-9's second peer
# (docs/editor-test-audit.md step 8, by-hand-findings.md "R-EXP-9's other half"): the same scene in
# both, so `Rpcs` -- a node carrying scripts/rpcs.verse -- is at the same path on each, which is how
# an RPC is addressed. `--mp-role=server` hosts, drives every step and prints the cases;
# `--mp-role=client` connects and does what it is asked. Without a role it does nothing, so the
# scene is inert anywhere else.
#
# The two talk over this node's own GDScript RPCs, which are reliable on channel 0 like
# `TakeDamage`'s, so an answer the client sends after a reliable Verse call reaches the host after
# that call did. `Nudge` travels unreliable_ordered on channel 3, which orders nothing against
# channel 0, so its arrival is waited for rather than assumed.

const LoggingPeer = preload("res://multiplayer/logging_peer.gd")
const TAG := "[multiplayer]"

var role := ""
var peer: LoggingPeer
var client_id := 0
var answers := {}
var _unanswered := {}
var rpcs: Node


func _ready() -> void:
	var port := 0
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--mp-role="):
			role = arg.get_slice("=", 1)
		elif arg.begins_with("--mp-port="):
			port = arg.get_slice("=", 1).to_int()
	if role.is_empty():
		return
	rpcs = $Rpcs
	var enet := ENetMultiplayerPeer.new()
	var made := enet.create_server(port, 1) if role == "server" else enet.create_client("127.0.0.1", port)
	if made != OK:
		print("%s the %s's ENet peer is made on port %d: FAIL (error %d)" % [TAG, role, port, made])
		get_tree().quit(1)
		return
	peer = LoggingPeer.new()
	peer.inner = enet
	multiplayer.multiplayer_peer = peer
	if role == "server":
		multiplayer.peer_connected.connect(_on_peer_connected)
		print("[mp-server] listening on %d" % port)
	else:
		multiplayer.connected_to_server.connect(func() -> void: print("[mp-client] connected"))
		multiplayer.server_disconnected.connect(func() -> void: get_tree().quit(0))


func _on_peer_connected(id: int) -> void:
	if client_id != 0:
		return
	client_id = id
	await _drive()
	get_tree().quit(0)


# --- the client's half ----------------------------------------------------------------------------

@rpc("authority", "call_remote", "reliable")
func do_step(step: String) -> void:
	if step == "done":
		get_tree().quit.call_deferred(0)
		return
	_answer.rpc_id(1, step, _client_step(step))


@rpc("any_peer", "call_remote", "reliable")
func _answer(step: String, answer: Dictionary) -> void:
	answers[step] = answer


func _client_step(step: String) -> Dictionary:
	var answer := {}
	peer.sent.clear()
	match step:
		"send_take_damage":
			answer.ret = rpcs.call("SendTakeDamage", 5)
		"nudge":
			answer.ret = rpcs.rpc("Nudge", 5)
		"ping":
			answer.ret = rpcs.rpc("Ping")
		"ordinary":
			answer.ret = rpcs.rpc("Ordinary")
		"misspelled":
			answer.ret = rpcs.rpc("Misspelled")
	answer.sent = peer.sent.duplicate()
	answer.received = peer.received.duplicate()
	answer.damage = rpcs.call("ReadDamage")
	answer.calls = rpcs.call("ReadCalls")
	return answer


# --- the host's half, which is the six by-hand steps ------------------------------------------

func _drive() -> void:
	var start := await _ask("read")
	_check("(2) the client connects, and its Rpcs node answers at the same path",
			start.get("damage") == 0 and start.get("calls") == 0)

	# (3) "From the client, call SendTakeDamage(5). TakeDamage is @rpc("authority"), so this must
	# be refused: only the node's authority may call it, and the client is not."
	var sent := await _ask("send_take_damage")
	_check_eq("(3) the client's SendTakeDamage(5) leaves Verse as Node.rpc answering OK", sent.get("ret"), OK)
	_check_travelled("(3) and TakeDamage travels reliable on channel 0", sent.get("sent", []),
			MultiplayerPeer.TRANSFER_MODE_RELIABLE, 0)
	_check_eq("(3) the host refuses it: its Damage stays 0", rpcs.call("ReadDamage"), 0)
	_check_eq("(3) and its Calls stays 0", rpcs.call("ReadCalls"), 0)
	_check_eq("(3) nor does it run on the client, which authority does not make call_local", sent.get("damage"), 0)

	# (4) "From the host, call it. ReadDamage() on the client must answer 5, and on the host 0."
	peer.sent.clear()
	_check_eq("(4) the host's SendTakeDamage(5) answers OK", rpcs.call("SendTakeDamage", 5), OK)
	_check_travelled("(4) and travels reliable on channel 0", peer.sent, MultiplayerPeer.TRANSFER_MODE_RELIABLE, 0)
	var after := await _ask("read")
	_check_eq("(4) the client's ReadDamage() answers 5", after.get("damage"), 5)
	_check_eq("(4) and the host's answers 0: authority does not imply call_local", rpcs.call("ReadDamage"), 0)

	# (5) "From either, call Nudge(5), which is @rpc("unreliable_ordered any_peer call_local 3").
	# Both sides' ReadDamage() must move."
	var nudge := await _ask("nudge")
	_check_eq("(5) the client's Nudge(5) answers OK", nudge.get("ret"), OK)
	_check_eq("(5) and runs on the client too, which is call_local", nudge.get("damage"), 10)
	_check_travelled("(5) Nudge leaves unreliable_ordered on channel 3", nudge.get("sent", []),
			MultiplayerPeer.TRANSFER_MODE_UNRELIABLE_ORDERED, 3)
	var moved := await _until(func() -> bool: return rpcs.call("ReadDamage") == 5, 5000)
	_check("(5) and runs on the host, which any_peer lets a client ask of it", moved)
	_check("(5) arriving at the host unreliable_ordered on channel 3",
			peer.received.has([MultiplayerPeer.TRANSFER_MODE_UNRELIABLE_ORDERED, 3]))

	peer.sent.clear()
	_check_eq("(5) the host's Nudge(5) answers OK", rpcs.rpc("Nudge", 5), OK)
	_check_eq("(5) and runs on the host", rpcs.call("ReadDamage"), 10)
	var client_seen := {}
	var asked_since := Time.get_ticks_msec()
	while Time.get_ticks_msec() - asked_since < 5000 and client_seen.get("damage") != 15:
		client_seen = await _ask("read")
	_check("(5) and on the client", client_seen.get("damage") == 15)
	_check("(5) arriving at the client unreliable_ordered on channel 3",
			client_seen.get("received", []).has([MultiplayerPeer.TRANSFER_MODE_UNRELIABLE_ORDERED, 3]))

	var ping := await _ask("ping")
	_check_eq("any_peer: the client's Ping() answers OK", ping.get("ret"), OK)
	_check_travelled("and Ping, which names no transfer mode, travels reliable on channel 0", ping.get("sent", []),
			MultiplayerPeer.TRANSFER_MODE_RELIABLE, 0)
	_check_eq("and the host runs it: its Calls moves to 1", rpcs.call("ReadCalls"), 1)
	_check_eq("but the client does not, with no call_local", ping.get("calls"), 1)

	# (6) "Check Ordinary() is not callable remotely at all -- it carries no @rpc, so it must be
	# absent from the config and refused with Godot's own 'not marked for RPCs in the local script'."
	for method in ["Ordinary", "Misspelled"]:
		var refused := await _ask(method.to_lower())
		var what := "carries no @rpc" if method == "Ordinary" else "carries a refused @rpc"
		_check_eq("(6) %s %s, so Node.rpc refuses it at the client" % [method, what], refused.get("ret"), ERR_INVALID_PARAMETER)
		_check("(6) and sends nothing for %s" % method, refused.get("sent", []).is_empty())
	var last := await _ask("read")
	_check_eq("(6) the host's Calls has not moved", rpcs.call("ReadCalls"), 1)
	_check_eq("(6) nor the client's", last.get("calls"), 1)
	peer.sent.clear()
	_check_eq("(6) the host's own Node.rpc refuses Ordinary too", rpcs.rpc("Ordinary"), ERR_INVALID_PARAMETER)
	_check("(6) and sends nothing for it", peer.sent.is_empty())

	# Not answered: the client leaves on it, and ENet flushes this before the host's own quit does.
	do_step.rpc_id(client_id, "done")
	for i in 30:
		await get_tree().process_frame


func _ask(step: String) -> Dictionary:
	answers.erase(step)
	do_step.rpc_id(client_id, step)
	await _until(func() -> bool: return answers.has(step), 15000)
	if not answers.has(step) and not _unanswered.has(step):
		_unanswered[step] = true
		_check("the client answers the %s step" % step, false)
	return answers.get(step, {})


func _until(condition: Callable, timeout_ms: int) -> bool:
	var started := Time.get_ticks_msec()
	while not condition.call():
		if Time.get_ticks_msec() - started > timeout_ms:
			return false
		await get_tree().process_frame
	return true


# The call's own packet is the last one it put: a first call to a path is preceded by the path
# cache's reliable announcement (SceneCacheInterface), which is Godot's and not the method's.
func _check_travelled(name: String, sent: Array, mode: int, channel: int) -> void:
	_check_eq(name, sent.back() if not sent.is_empty() else [], [mode, channel])


func _check(name: String, ok: bool) -> void:
	print("%s %s: %s" % [TAG, name, "ok" if ok else "FAIL"])


func _check_eq(name: String, got: Variant, expected: Variant) -> void:
	if typeof(got) == typeof(expected) and got == expected:
		print("%s %s: ok" % [TAG, name])
	else:
		print("%s %s: FAIL (got %s %s, expected %s %s)" % [TAG, name, type_string(typeof(got)), str(got),
				type_string(typeof(expected)), str(expected)])
