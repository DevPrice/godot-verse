// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AutoRTFM.h"
#include "Containers/Array.h"
#include "Containers/StringView.h"
#include "HostResult.h"
#include "verse_host_abi.h"

class UObject;

/// Signals at run time: the binding rows a `signal(t)` or an `@export_signal` event member stands
/// for, emitting, subscribing, and the waits R-SIG-5 connects for exactly as long as they last.
///
/// The natives GodotBindings.cpp exposes reach this through HostScript.h, which declares the public
/// half (EmitSignal, SubscribeSignal, BeginSignalAwait and the rest). What is here is what the rest
/// of the host calls: instance construction, first entry and release, and callback dispatch.
namespace GodotVerse {

/// Mints the binding rows for every `signal` member of a fresh instance, and writes each id
/// into the member's own object.
///
/// This is where a signal stops being a declaration and becomes a thing that can be emitted: the
/// member's *type* said what the payload is and its *name* is the signal's name, and both are
/// resolved here once rather than at every emission. S-A is the spike that says the write survives
/// -- two production paths already fill a class-typed member at construction.
///
/// OutEventBindings receives the `@export_signal` event members' binding ids, which the instance
/// keeps for EnsureEventConnections and ReleaseEventBindings.
AUTORTFM_DISABLE void BindSignals(UObject* Instance,
                                  FUtf8StringView ClassName,
                                  int64 Handle,
                                  TArray<int64>& OutEventBindings);

/// Connects each `@export_signal` event member, once, at the first entry into the instance.
///
/// **Not at vh_instantiate, and the reason is Godot's own ordering.** The consumer builds the Verse
/// object *before* it installs the script instance on the node, and `Object::has_signal` answers
/// off the installed instance -- so a connect there is refused with "Attempt to connect nonexistent
/// signal", and the member would register, emit normally, and silently never deliver back. Nor at
/// the end of the consumer's create(): the object does not hold the script instance until
/// `_instance_create` has *returned* to Godot, which is later still and not a point this side can
/// name.
///
/// First entry is both late enough and early enough. It is late enough because a call into the
/// instance is Godot dispatching to an installed script instance; and it is early enough because a
/// Verse awaiter can only exist after Verse code has run on this object, and running Verse code on
/// it *is* an entry. The one ordering left uncovered -- Godot emits before any Verse code runs --
/// has nothing on the Verse side to deliver to.
AUTORTFM_DISABLE void EnsureEventConnections(const TArray<int64>& EventBindings);

/// Drops a released instance's event bindings: the row, the callback, the reference and the strong
/// pointer to the event, together. A strong pointer kept past the node is a GC root per scripted
/// node.
AUTORTFM_DISABLE void ReleaseEventBindings(const TArray<int64>& EventBindings);

/// Resumes whatever is waiting on one await token, with the emission's arguments as its payload.
///
/// The resumption happens **inside the emission**, synchronously, which is where GDScript resumes a
/// coroutine too (`GDScriptFunctionState::_signal_callback` calls `resume()` from the connected
/// Callable). Nothing is queued and nothing is budgeted -- see vh_tick.
///
/// Its own `AutoRTFM::Transact`, nested inside whatever transaction the emitting call is already
/// in. Without that a raise in the resumed task would abort the *emitter's* transaction and drop
/// writes that had nothing to do with it; with it, "a failure undoes the failing computation's
/// writes" stays literally true for a task as well as for a call.
///
/// A wait that already ended is success: a cancelled task is exactly a wait that stopped waiting.
AUTORTFM_DISABLE TResult<void> DeliverToAwaiter(int64 Token, const vh_value* Args, int32 ArgCount);

/// Signals an `@export_signal` member's event with an emission Godot just delivered.
///
/// The permanent-connection counterpart of DeliverToAwaiter, and deliberately the same shape: the
/// payload is rebuilt against the same recorded `FPayloadShape` the descriptor was generated from,
/// so a struct payload comes back a struct and a tuple comes back a tuple whichever spelling
/// declared it. What differs is only where the event comes from -- the binding holds it, rather than
/// it being found by walking a `signal` object's shape.
///
/// Every emission arrives here, including the script's own `Emit`: the emit verb goes out to Godot
/// and Godot dispatches back, which is what makes a Verse handler and a GDScript handler see the
/// same ordering.
AUTORTFM_DISABLE TResult<void> DeliverToEvent(int64 SignalId, const vh_value* Args, int32 ArgCount);

} // namespace GodotVerse
