/* C ABI between godot_verse.dll (MSVC, godot-cpp) and verse_host.dll (UBT, EpicClang/AutoRTFM).
 *
 * The two DLLs cannot share a C++ ABI: verse_host is built by Unreal Build Tool with the
 * AutoRTFM clang driver and a monolithic UE runtime, godot_verse by SCons with MSVC. Everything
 * that crosses the boundary is plain C.
 *
 * Threading: every entry point must be called from the thread that called vh_init (Godot's main
 * thread). UE binds its game thread there. The one piece of work the host does off that thread
 * is the analysis behind vh_check_project_begin, which it runs on a thread it owns; the callbacks
 * in this header are still only ever invoked on the vh_init thread.
 *
 * Lifetimes: all pointers passed in are borrowed for the duration of the call. Values written
 * into a vh_arena are owned by the arena and valid until the call that supplied it returns. The
 * one exception is a reference id (vh_value::Ref), which names an entry in a table the consumer
 * owns and which outlives the call -- see "reference values" below.
 *
 * The design argument behind v2, and the spikes that settled it, are in docs/abi-v2-design.md.
 */
#ifndef VERSE_HOST_ABI_H
#define VERSE_HOST_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- version -- */

/* Compatibility policy (R-QUAL-5).
 *
 * MAJOR changes when a struct's layout, a function's signature, or the meaning of an existing
 * field changes. There is no compatibility window: both DLLs must be rebuilt, and vh_init refuses
 * a descriptor whose major does not match exactly.
 *
 * MINOR changes when something is added that an older consumer can ignore -- a new enumerator, a
 * new callback at the end of vh_godot_api, a new entry point. A host may run against a consumer
 * with a lower minor: it must check StructSize before reading a field added after the version the
 * consumer was built for, and fall back rather than fail.
 *
 * The mismatch surfaces at vh_init, not at compile time, because the two sides are compiled by
 * different toolchains and nothing links them.
 */
#define VH_ABI_VERSION_MAJOR 10
#define VH_ABI_VERSION_MINOR 0
#define VH_ABI_VERSION ((VH_ABI_VERSION_MAJOR * 1000) + VH_ABI_VERSION_MINOR)

typedef int32_t vh_bool;

typedef enum vh_status
{
	VH_OK = 0,
	VH_ERR_ABI,       /* version mismatch or malformed descriptor */
	VH_ERR_STATE,     /* called before vh_init, or twice */
	VH_ERR_INIT,      /* engine boot failed */
	VH_ERR_COMPILE,   /* Verse source did not compile; diagnostics were reported */
	VH_ERR_NOT_FOUND, /* no such function / file */
	VH_ERR_RUNTIME,   /* Verse raised a runtime error; it was reported through OnRuntimeError */
	VH_ERR_ARGUMENT,  /* the call was shaped wrong: arity, or an argument with no Verse spelling */

	/* A <decides> function failed. Distinct from VH_ERR_NOT_FOUND, which means there was no such
	 * function to call: this one ran and declined, which is an ordinary outcome the caller is
	 * expected to have a spelling for. */
	VH_ERR_FAILED,

	/* The VM declined to run the body, so nothing was called and nothing was written.
	 *
	 * Until ABI v6 this was a project-wide state: a raise stopped every script until the next
	 * vh_tick, and this is what every other call got for the rest of that frame. Since Phase 5 a
	 * raise terminates only the raising instance's task scope and that instance is given a fresh
	 * one at its very next call, so this is now a narrow answer rather than a common one -- it
	 * means the scope the call would have run in was terminated and not yet replaced.
	 *
	 * It exists so that "did not run" cannot be mistaken for "ran and found nothing", which is
	 * what it used to look like. Any error was already reported through OnRuntimeError. */
	VH_ERR_HALTED,

	/* An entry point was called from a thread other than the one that called vh_init, having run
	 * nothing (R-ASYNC-8).
	 *
	 * VerseVM asserts the game thread at the top of every VM entry -- VVMEnterVMInline.h's
	 * `ensure(IsInGameThread() && ...)`, above "Verse bytecode and AutoRTFM transactions must run
	 * on the game thread". It is an `ensure`, so proceeding is a logged callstack followed by
	 * undefined behaviour: the worst of the available failure modes. It is thread *identity*, so
	 * serialising entry does not satisfy it and a mutex cannot fix it. */
	VH_ERR_THREAD,

	/* Added at ABI v8.1. The VM is stopped at a breakpoint, and this call is one of the two that
	 * are meaningless in that state. Nothing ran and nothing was written.
	 *
	 * Narrow, because spike S-3 came back positive: re-entering a stopped VM *is* safe, so the
	 * calls the editor makes while its debug loop runs -- a property read for the remote scene
	 * tree, a method call, the vh_debug_* reads -- all work, and the inspector stays live while
	 * paused. What does not is building: vh_compile_project would publish a generation underneath
	 * a frame belonging to the retiring one, and the two vh_check_project entry points reset the
	 * semantic program and block execution for the length of an analysis the stopped frame is
	 * about to resume into. Those three answer this. vh_tick is refused too, and silently, because
	 * it has no status to answer with (phase-6-design.md 13.1). */
	VH_ERR_STOPPED,

	/* Added at ABI v8.2. This build of the host has no Verse compiler, so there was nothing
	 * that could have run this call. Nothing ran and nothing was written.
	 *
	 * The runtime host an exported game ships is built with bBuildWithEditorOnlyData off,
	 * which is what gives Solaris WITH_VERSE_COMPILER=0: it loads Verse out of cooked packages
	 * and cannot compile, analyse, complete or look anything up. Those entry points are still
	 * exported -- one header, one loader -- and answer this.
	 *
	 * Distinct from VH_ERR_STATE, which means the host could have done it and the moment was
	 * wrong. This one never becomes possible, so the consumer's recourse is to stop asking --
	 * which is why vh_host_kind() is readable before vh_init. */
	VH_ERR_UNSUPPORTED
} vh_status;

/* Outcome of a property read/write or a method call. The distinction is load bearing: the host
 * turns a dead receiver into a Verse runtime error -- which unwinds the whole call and rolls the
 * transaction back -- while an absent value is an ordinary Verse failure the script can handle.
 * Collapsing the two (as a plain success/failure bool does) makes a use-after-free look like a
 * miss, which is how a freed node ends up silently doing nothing every frame. */
typedef enum vh_call_status
{
	VH_CALL_OK = 0,
	VH_CALL_DEAD_OBJECT,    /* the handle names a freed, or never valid, instance */
	VH_CALL_NO_SUCH_MEMBER, /* the object is alive but has no such method or property */
	VH_CALL_BAD_VALUE,      /* an argument or the result has no representation on this wire */
	VH_CALL_BAD_ARITY       /* the member exists but was called with the wrong number of arguments */
} vh_call_status;

/* Godot's Object::CONNECT_ONE_SHOT, which is the only connect flag the host passes: a single
 * `Await` resumes once, so Godot dropping the connection as it fires is exactly the lifetime and
 * saves the disconnect. Spelled here rather than included, because this header knows nothing of
 * Godot's own; the value is Godot's and a consumer must forward it unchanged. */
#define VH_CONNECT_ONE_SHOT 4

/* ---------------------------------------------------------------- values -- */

/* How a vh_value's payload is laid out. Deliberately smaller than vh_variant_tag: this says how
 * to *read* the value, that says what to rebuild it as. */
typedef enum vh_type
{
	VH_TYPE_VOID = 0,
	VH_TYPE_LOGIC,
	VH_TYPE_INT,
	VH_TYPE_FLOAT,
	VH_TYPE_CHAR,
	VH_TYPE_STRING, /* utf8, not null terminated */
	VH_TYPE_ARRAY,
	VH_TYPE_MAP,
	VH_TYPE_TUPLE,
	VH_TYPE_OPTION, /* Option == NULL is Verse's false */

	/* An id in the consumer's reference table rather than a value. See "reference values". */
	VH_TYPE_REF,

	/* Any Godot value, whatever its type -- Verse's `variant`, which is the only thing in this
	 * bridge that can hold one.
	 *
	 * A *declaration* type and never a payload: it appears in vh_param_desc::Type and in
	 * vh_method_desc::ResultType, and never in a vh_value. The host converts to and from the lanes,
	 * so the value itself arrives as whatever the variant holds -- VH_TYPE_INT, VH_TYPE_STRING,
	 * VH_TYPE_REF. What this says to the consumer is "describe this argument to Godot as accepting
	 * anything", which Godot spells as Variant::NIL with PROPERTY_USAGE_NIL_IS_VARIANT. */
	VH_TYPE_VARIANT
} vh_type;

typedef struct vh_value vh_value;
typedef struct vh_pair vh_pair;

/* Godot's Variant::Type, as the wire carries it. vh_type says how the payload is laid out;
 * this says which Godot type to rebuild from it, which vh_type alone cannot express -- a
 * two-float tuple is equally a Vector2, a Vector2i or a plain array.
 *
 * 0 (Godot's TYPE_NIL) means "infer from vh_type".
 * The values are Godot's own and must not be renumbered; extension_api.json is the source. */
typedef enum vh_variant_tag
{
	VH_VARIANT_NIL = 0,
	VH_VARIANT_BOOL = 1,
	VH_VARIANT_INT = 2,
	VH_VARIANT_FLOAT = 3,
	VH_VARIANT_STRING = 4,
	VH_VARIANT_VECTOR2 = 5,
	VH_VARIANT_VECTOR2I = 6,
	VH_VARIANT_RECT2 = 7,
	VH_VARIANT_RECT2I = 8,
	VH_VARIANT_VECTOR3 = 9,
	VH_VARIANT_VECTOR3I = 10,
	VH_VARIANT_TRANSFORM2D = 11,
	VH_VARIANT_VECTOR4 = 12,
	VH_VARIANT_VECTOR4I = 13,
	VH_VARIANT_PLANE = 14,
	VH_VARIANT_QUATERNION = 15,
	VH_VARIANT_AABB = 16,
	VH_VARIANT_BASIS = 17,
	VH_VARIANT_TRANSFORM3D = 18,
	VH_VARIANT_PROJECTION = 19,
	VH_VARIANT_COLOR = 20,
	VH_VARIANT_STRING_NAME = 21,
	VH_VARIANT_NODE_PATH = 22,
	VH_VARIANT_RID = 23,
	VH_VARIANT_OBJECT = 24,
	VH_VARIANT_CALLABLE = 25,
	VH_VARIANT_SIGNAL = 26,
	VH_VARIANT_DICTIONARY = 27,
	VH_VARIANT_ARRAY = 28,
	VH_VARIANT_PACKED_BYTE_ARRAY = 29,
	VH_VARIANT_PACKED_INT32_ARRAY = 30,
	VH_VARIANT_PACKED_INT64_ARRAY = 31,
	VH_VARIANT_PACKED_FLOAT32_ARRAY = 32,
	VH_VARIANT_PACKED_FLOAT64_ARRAY = 33,
	VH_VARIANT_PACKED_STRING_ARRAY = 34,
	VH_VARIANT_PACKED_VECTOR2_ARRAY = 35,
	VH_VARIANT_PACKED_VECTOR3_ARRAY = 36,
	VH_VARIANT_PACKED_COLOR_ARRAY = 37,
	VH_VARIANT_PACKED_VECTOR4_ARRAY = 38,

	VH_VARIANT_MAX = 39
} vh_variant_tag;

/* Reference values.
 *
 * godot-cpp splits Godot's type set in two and this ABI follows it: a value type is copied, and a
 * reference type is an 8-byte opaque handle into engine-owned storage that every operation is
 * asked of. `Array` and `Dictionary` have reference semantics an author can observe -- a
 * Dictionary passed to a function and mutated is mutated for the caller -- and marshalling them
 * by value would silently convert that to value semantics. `Callable` and `Signal` cannot be
 * decomposed into scalars at all.
 *
 * So those cross as VH_TYPE_REF carrying an id minted by the *consumer* (the GDExtension), which
 * owns a table from id to Variant. `Object` is the same idea with Godot supplying the id: an
 * instance id is already a stable name for an object, so it needs no table.
 *
 * Ownership: an id handed to the host is retained by the table until the host calls ReleaseRef.
 * The host does that when the Verse value wrapping it is collected -- a native Verse class'
 * UObject shadow reaches BeginDestroy, which is measured in docs/abi-v2-design.md §1a. Release is
 * therefore deferred by up to one collection cycle, and the table's own memory pressure is
 * invisible to UE's garbage collector, so the host requests a cycle when the table grows rather
 * than waiting to be asked. */
struct vh_value
{
	int32_t Type;       /* vh_type */
	int32_t VariantTag; /* vh_variant_tag */
	union
	{
		vh_bool Logic;
		int64_t Int;
		double Float;
		uint32_t Char; /* unicode code point */
		int64_t Ref;   /* VH_TYPE_REF: an id in the consumer's reference table */
		struct
		{
			const char* Utf8;
			int32_t Len;
		} String;
		struct
		{
			const vh_value* Items;
			int32_t Count;
		} Seq; /* VH_TYPE_ARRAY, VH_TYPE_TUPLE */
		struct
		{
			const vh_pair* Pairs;
			int32_t Count;
		} Map;
		const vh_value* Option;
	};
};

struct vh_pair
{
	vh_value Key;
	vh_value Value;
};

/* Bump allocator owned by the caller of whichever function it is handed to. */
typedef struct vh_arena vh_arena;
struct vh_arena
{
	void* (*Alloc)(vh_arena* Self, size_t Size, size_t Align);
};

/* ------------------------------------------------------- Godot callbacks -- */

/* Object handles are Godot instance ids. 0 is never a valid handle. */
typedef int64_t vh_handle;

/* Every callback answering int32_t answers vh_call_status. A dead handle must be reported as
 * VH_CALL_DEAD_OBJECT rather than folded into a missing member: the host raises on the former and
 * fails on the latter.
 *
 * Grown only at the end, and guarded by StructSize, so a host built against a later minor can run
 * against an older consumer by checking before it reads. */
typedef struct vh_godot_api
{
	int32_t StructSize;
	void* Ctx;

	void (*Print)(void* Ctx, const char* Utf8, int32_t Len);
	vh_bool (*IsValid)(void* Ctx, vh_handle Handle);

	int32_t (*GetProperty)(void* Ctx, vh_handle Handle, const char* NameUtf8, int32_t NameLen, vh_arena* Arena, vh_value* OutValue);
	int32_t (*SetProperty)(void* Ctx, vh_handle Handle, const char* NameUtf8, int32_t NameLen, const vh_value* Value);
	int32_t (*CallMethod)(void* Ctx, vh_handle Handle, const char* NameUtf8, int32_t NameLen, const vh_value* Args, int32_t ArgCount, vh_arena* Arena, vh_value* OutValue);

	/* Engine::get_singleton, for Input, Time, and the rest of Godot's global objects. 0 if
	 * there is no such singleton. */
	vh_handle (*GetSingleton)(void* Ctx, const char* NameUtf8, int32_t NameLen);

	/* The Godot class a handle names -- `Node2D`, `Timer` -- written into OutClassName as a
	 * VH_TYPE_STRING allocated from Arena.
	 *
	 * What R-SCN-6 is built on: a handle crossing into Verse has to become an object of the most
	 * derived mirrored class it actually is, or a downcast can never succeed. The host caches the
	 * answer per handle, so this is asked once per Godot object rather than once per crossing.
	 * Instance ids are not reused within a run, which is what makes that cache safe.
	 *
	 * VH_CALL_DEAD_OBJECT for a handle Godot has already freed. */
	int32_t (*GetClassOf)(void* Ctx, vh_handle Handle, vh_arena* Arena, vh_value* OutClassName);

	/* --- reference table: declared in v2.0, not yet supplied ---
	 *
	 * The design and the measurements behind it are in docs/abi-v2-design.md; what is missing is
	 * the implementation on both sides, which is the rest of R-TYPE-1. Until then the consumer
	 * leaves these null and the host never reaches a value that would need them -- no vh_value
	 * carries VH_TYPE_REF yet. They are declared now because roadmap 1.1 asks the v2 header to
	 * anticipate the whole spec: a reference type added later must not need a second calling
	 * convention, and reserving the shape is what guarantees that.
	 */

	/* Drops the host's claim on a reference id. After this the id may be reused, so the host must
	 * not name it again. Called when the Verse value wrapping it is collected. */
	void (*ReleaseRef)(void* Ctx, int64_t Ref);

	/* A second, independent claim on an id the host already holds -- for copying a Verse value
	 * that wraps one. Answers the id back for convenience. */
	int64_t (*RetainRef)(void* Ctx, int64_t Ref);

	/* Mints an empty Array or Dictionary (or a packed array) and returns its id, already claimed
	 * by the host. 0 if Tag is not a reference type. */
	int64_t (*NewRef)(void* Ctx, int32_t VariantTag);

	/* Element access on an Array, a Dictionary or a packed array. Key is an int index for the
	 * sequence types and any value for a Dictionary. RefGet reports VH_CALL_NO_SUCH_MEMBER for a
	 * key that is absent or an index out of range, which the host turns into an ordinary Verse
	 * failure rather than a raise -- a missing key is not a bug. */
	int32_t (*RefGet)(void* Ctx, int64_t Ref, const vh_value* Key, vh_arena* Arena, vh_value* OutValue);
	int32_t (*RefSet)(void* Ctx, int64_t Ref, const vh_value* Key, const vh_value* Value);
	int32_t (*RefSize)(void* Ctx, int64_t Ref, int64_t* OutSize);

	/* The whole container at once, for the bulk converters that let a script reach Verse's own
	 * map and array idioms. OutValue is a VH_TYPE_ARRAY of elements, or a VH_TYPE_MAP of pairs. */
	int32_t (*RefContents)(void* Ctx, int64_t Ref, vh_arena* Arena, vh_value* OutValue);

	/* Invokes a Callable. The one thing that makes a callback-taking engine API reachable. */
	int32_t (*InvokeCallable)(void* Ctx, int64_t Ref, const vh_value* Args, int32_t ArgCount, vh_arena* Arena, vh_value* OutValue);

	/* Godot's 114 static methods and its 114 `@GlobalScope` utility functions, which share one
	 * problem: neither has an object to ride a call on. Two by-name calls that carry no handle
	 * (R-SCN-3).
	 *
	 * Answers vh_call_status. VH_CALL_NO_SUCH_MEMBER for a name this build of Godot does not
	 * have, which is the mirror and the engine having drifted apart. */
	int32_t (*CallStatic)(void* Ctx, const char* ClassUtf8, int32_t ClassLen, const char* NameUtf8, int32_t NameLen, const vh_value* Args, int32_t ArgCount, vh_arena* Arena, vh_value* OutValue);
	int32_t (*CallUtility)(void* Ctx, const char* NameUtf8, int32_t NameLen, const vh_value* Args, int32_t ArgCount, vh_arena* Arena, vh_value* OutValue);

	/* A Godot Callable that calls back into the host, as a reference id in the same table an Array
	 * or a Dictionary rides in. CallbackId is what vh_callback_invoke will be handed; OwnerHandle
	 * is the Godot object the Verse function is bound to, which the Callable reports as its
	 * `get_object()` -- so a freed node drops its connections with no work from the host, and
	 * `is_valid()` is ObjectDB's answer rather than a guess.
	 *
	 * The consumer releases the id through vh_callback_release when the Callable is destroyed.
	 * 0 if one could not be made. */
	int64_t (*MakeCallable)(void* Ctx, int64_t CallbackId, vh_handle OwnerHandle);

	/* --- signals: declared in v2.0, implemented in roadmap Phase 4 (spec 5.3) --- */

	int32_t (*EmitSignal)(void* Ctx, vh_handle Handle, const char* NameUtf8, int32_t NameLen, const vh_value* Args, int32_t ArgCount);
	/* Flags is Godot's own Object::ConnectFlags; VH_CONNECT_ONE_SHOT is the only one the host
	 * passes, for the single connection one `Await` needs. */
	int32_t (*ConnectSignal)(void* Ctx, vh_handle Handle, const char* NameUtf8, int32_t NameLen, const vh_value* Target, int32_t Flags);
	int32_t (*DisconnectSignal)(void* Ctx, vh_handle Handle, const char* NameUtf8, int32_t NameLen, const vh_value* Target);

	/* --- v6.0: what a Signal *value* names -------------------------------------------------- */

	/* Splits a Signal reference id into the object that declares it and its name, which is what
	 * connecting to one needs and what a reference id alone does not say.
	 *
	 * This is how a signal the mirror has no accessor for is reached -- one a GDScript or C#
	 * script declared, or one made with add_user_signal (R-INT-1). The name is written into
	 * *OutNameUtf8 as a NUL-terminated string the consumer owns until its next call, in the same
	 * bargain every other string this header hands back makes.
	 *
	 * VH_CALL_OK, or VH_CALL_BAD_VALUE for an id that is not a Signal. */
	int32_t (*SignalTarget)(void* Ctx, int64_t Ref, vh_handle* OutHandle, const char** OutNameUtf8);

	/* The inverse: a Signal value naming one signal of one object, as a reference id already
	 * claimed by the host -- Godot's own `Signal(object, "name")`, which has no other spelling on
	 * this wire.
	 *
	 * The direct analogue of MakeCallable, and needed for the same reason: without it a script can
	 * receive a Signal but never name one, so a signal the mirror has no accessor for would be
	 * unreachable however good the machinery behind it was. 0 for a handle Godot has freed. */
	int64_t (*MakeSignalRef)(void* Ctx, vh_handle Handle, const char* NameUtf8, int32_t NameLen);

	/* --- v8.1: the debugger (R-DIAG-4) ------------------------------------------------------ */

	/* Both null in a consumer that does not debug, and the host then never breaks. Called from
	 * inside the interpreter's handshake, on the vh_init thread, with a Verse op in flight -- so
	 * the consumer must do nothing here that re-enters the host except the vh_debug_* reads, and
	 * must do that only from inside DebugBreak.
	 *
	 * The division is: the host owns *which frame*, because that is the one thing only Notify can
	 * see; the consumer owns *which line*, because the breakpoint list and the step state are
	 * Godot's (phase-6-design.md D3). */

	/* Is (PathUtf8, Line) a place to stop? The host asks once per distinct location, never once
	 * per op -- per-op would cross this boundary millions of times a second, and the dedup that
	 * makes it affordable is Epic's own (VVMSocketDebugger.cpp UpdatePrevLocation).
	 *
	 * FrameRelation is a vh_debug_frame_relation: how the frame about to execute relates to the
	 * frame the last stop happened in. It is what makes step-over and step-out implementable --
	 * the bridge sees no Verse call and so cannot keep Godot's depth counter, but it does see
	 * frame ancestry, which is the mechanism Epic's own debugger steps with. */
	vh_bool (*DebugShouldBreak)(void* Ctx, const char* PathUtf8, int32_t PathLen, int32_t Line, int32_t FrameRelation);

	/* Stop. Returns when the user continues. The consumer is expected to block here -- Godot's
	 * debug loop runs on this thread and calls back through the vh_debug_* reads while it does.
	 *
	 * No reason is passed, because the consumer is the side that decided one: DebugShouldBreak
	 * answered from its own breakpoint list and its own step state, and _debug_get_error is its
	 * question to answer. */
	void (*DebugBreak)(void* Ctx);

	/* --- v8.3: an object that is not a node (R-NODE-3) -------------------------------------- */

	/* ClassDB::instantiate, by Godot class name: the peer a Verse `helper{}` gets. Answers the new
	 * object's instance id, or 0 for a class Godot will not instantiate -- an abstract one, a
	 * singleton, or a name this build of Godot does not have.
	 *
	 * Ownership is split the way Godot splits it, and the consumer is the side that knows which
	 * half applies. A RefCounted-derived peer is born with one reference and the consumer holds
	 * it *for the host*, so the object survives being handed back as a bare instance id; an
	 * Object-derived peer is owned by nobody, exactly as `Object.new()` is in GDScript. The host
	 * never has to ask which it got: it calls ReleaseObject for every peer it minted and the
	 * consumer does the right thing for each.
	 *
	 * Reached from Verse's own construction path, so it must not re-enter the host. */
	vh_handle (*InstantiateClass)(void* Ctx, const char* ClassUtf8, int32_t ClassLen);

	/* The host is done with a peer it minted with InstantiateClass -- the Verse value holding it
	 * was collected, or the transaction that made it aborted.
	 *
	 * Drops the reference the consumer took for the host. A RefCounted peer nothing else holds
	 * dies here; one Godot still holds does not. An Object-derived peer is left alone, because
	 * Godot's rule for it is that the author frees it -- a Verse-minted Object that is never freed
	 * leaks exactly as GDScript's does, and Godot's orphan-object report at exit names both the
	 * same way.
	 *
	 * bDiscard says the mint is being *undone* rather than dropped: the transaction that created
	 * the peer aborted, so nothing outside it can ever have seen the object and an Object-derived
	 * one is freed rather than leaked. The rollback is not a place to leak on. */
	void (*ReleaseObject)(void* Ctx, vh_handle Handle, vh_bool bDiscard);

	/* --- v8.7: a method on a reference rather than on an object ----------------------------- */

	/* Calls a method of the *builtin type* a reference id names -- `Signal.emit`, `Callable.bind`,
	 * `Callable.call_deferred`, and every other method of an Array, a Dictionary, a Callable or a
	 * Signal that the mirror does not carry as a wrapper method.
	 *
	 * CallValue cannot reach these: it takes a vh_handle, which names a Godot *Object*, and none
	 * of the builtin types is one. The dispatch is Godot's own `Variant::callp`, so a method this
	 * build of Godot does not have answers VH_CALL_NO_SUCH_MEMBER rather than crashing.
	 *
	 * OutValue may be left VH_TYPE_VOID for a method that answers nothing. */
	int32_t (*RefCall)(void* Ctx, int64_t Ref, const char* NameUtf8, int32_t NameLen, const vh_value* Args, int32_t ArgCount, vh_arena* Arena, vh_value* OutValue);
} vh_godot_api;

/* How the frame about to execute relates to the frame the debugger last stopped in.
 *
 * Godot encodes a step as a depth: -1 step-in, 0 step-over, 1 step-out (remote_debugger.cpp's
 * step/next/out commands). GDScript keeps that counter honest by pushing and popping around every
 * call; this bridge has no such hook, so the counter would never move. These three are the
 * equivalent question asked of the stack instead, and they are Epic's two tests spelled out:
 * step-over stops on anything that is not DEEPER (`!IsProperAncestorOf`), step-out on OTHER alone
 * (`!IsAncestorOf`). */
typedef enum vh_debug_frame_relation
{
	VH_DEBUG_FRAME_SAME = 0,  /* the very frame the last stop was in */
	VH_DEBUG_FRAME_DEEPER,    /* inside a call made from it */
	VH_DEBUG_FRAME_OTHER      /* neither -- it returned, or this is unrelated work */
} vh_debug_frame_relation;

/* ----------------------------------------------------------- diagnostics -- */

typedef enum vh_severity
{
	VH_SEVERITY_INFO = 0,
	VH_SEVERITY_WARNING,
	VH_SEVERITY_ERROR
} vh_severity;

typedef struct vh_diagnostic
{
	int32_t Severity; /* vh_severity */
	const char* MessageUtf8;
	int32_t MessageLen;
	const char* FilePathUtf8;
	int32_t FilePathLen;
	/* 1-based, 0 when the diagnostic carries no location. */
	int32_t Line;
	int32_t Column;
	int32_t EndLine;
	int32_t EndColumn;
	int32_t ReferenceCode;

	/* The type the subject of this diagnostic was looked for in, spelled as Verse source, or empty
	 * -- `node2d` for "Unknown member `GetPosition` in `node2d`.".
	 *
	 * The message already says it, and parsing it back out of English is what this replaces: uLang
	 * formats both that wording and the bare "Unknown identifier `X`." under one reference code
	 * (3506, ErrSemantic_UnknownIdentifier), so the code cannot tell the two apart and only the
	 * prose could. Non-empty *is* the distinction now: a member access resolved its receiver, and a
	 * bare identifier had none to resolve.
	 *
	 * Filled from the AST after the analysis, not at the moment the glitch is raised -- the error
	 * node the analyzer leaves behind keeps the receiver as its child, with its type intact. Empty
	 * whenever that lookup finds nothing, which a consumer must treat as "not said" rather than as
	 * "no receiver". */
	const char* SubjectTypeUtf8;
	int32_t SubjectTypeLen;
} vh_diagnostic;

typedef void (*vh_diagnostic_fn)(void* Ctx, const vh_diagnostic* Diagnostic);

/* One Verse call frame, for R-DIAG-2. */
typedef struct vh_stack_frame
{
	/* The function's Verse name, undecorated. Empty for a frame with no name to give. */
	const char* FunctionUtf8;
	int32_t FunctionLen;
	/* The .verse file, as an absolute path the consumer can turn into a res:// path and make
	 * clickable. Empty for a frame in a package the project has no source for -- the generated
	 * Godot API, Verse's own library -- which can be shown but not jumped to. */
	const char* PathUtf8;
	int32_t PathLen;
	/* 1-based, 0 when the frame carries no location. */
	int32_t Line;
	int32_t Column;
} vh_stack_frame;

/* A Verse runtime error: a failed unrecoverable expression, a stale object access, a division by
 * zero. Distinct from vh_diagnostic, which describes source the compiler read; this describes code
 * that ran.
 *
 * Reported through its own callback rather than as an error severity diagnostic because the
 * consumer does different things with it -- a compile error annotates the script editor's gutter,
 * a runtime error goes to the output and errors panel with a stack the user can click through. */
typedef struct vh_runtime_error
{
	const char* MessageUtf8;
	int32_t MessageLen;
	/* Innermost frame first. Empty when the VM could not produce a stack, which is not an error:
	 * the message still names what happened. */
	const vh_stack_frame* Frames;
	int32_t FrameCount;
} vh_runtime_error;

typedef void (*vh_runtime_error_fn)(void* Ctx, const vh_runtime_error* Error);

/* ------------------------------------------------------------ entry points -- */

typedef struct vh_init_desc
{
	int32_t StructSize;
	int32_t AbiVersion; /* VH_ABI_VERSION */

	/* Absolute utf8 path to the UE Engine directory the host was built from. NULL uses the
	 * directory the DLL was loaded from. */
	const char* EngineDirUtf8;

	vh_godot_api Godot;

	vh_diagnostic_fn OnDiagnostic;
	void* DiagnosticCtx;

	/* R-DIAG-2. NULL folds runtime errors into OnDiagnostic as errors without a location, which
	 * is what v1 did and is strictly worse. */
	vh_runtime_error_fn OnRuntimeError;
	void* RuntimeErrorCtx;

	vh_bool EnableDebugger;

	/* Added at ABI v8.2, read only when StructSize covers it.
	 *
	 * Absolute utf8 path to the directory holding the cooked Verse packages and the class
	 * sidecar that an exported game ships in place of its sources. NULL means there is no
	 * cooked project: an editor host expects that, and a runtime host answers VH_ERR_INIT,
	 * because with no compiler a project it was not handed is one it can never have. */
	const char* CookedDirUtf8;
} vh_init_desc;

#if defined(_WIN32)
#	define VH_EXPORT __declspec(dllexport)
#else
#	define VH_EXPORT __attribute__((visibility("default")))
#endif

#ifdef VERSE_HOST_IMPLEMENTATION
#	define VH_API VH_EXPORT
#else
#	define VH_API
#endif

/* The host is compiled by the AutoRTFM clang driver, which requires these entry points to carry
 * AUTORTFM_DISABLE on their first declaration. The host defines VH_ATTR before including this. */
#ifndef VH_ATTR
#	define VH_ATTR
#endif

VH_API int32_t vh_abi_version(void);

/* Which of the three hosts this is. Answerable before vh_init, like vh_abi_version, and for
 * the same reason: the consumer has to be able to refuse the wrong one with a sentence rather
 * than fail inside a boot.
 *
 * There is no RequiredHostKind in vh_init_desc. The consumer already knows which kind it
 * wants -- editor while Engine::is_editor_hint(), runtime otherwise -- and writes the better
 * message itself, which is not worth a StructSize dance to earn a worse one. */
/* The same three values as macros. A target selects its kind with a -D and the preprocessor
 * cannot see an enum, so host/ needs these to compile CookMain.cpp into the cooker alone.
 * The enumerators below are defined in terms of them, so the two cannot drift. */
#define VH_HOST_KIND_EDITOR 1
#define VH_HOST_KIND_RUNTIME 2
#define VH_HOST_KIND_COOKER 3

/* Unnamed, unlike every other enum here: a typedef named vh_host_kind and a function named
 * vh_host_kind are the same identifier in C++, and nothing in the ABI takes one of these as a
 * parameter -- vh_host_kind() answers int32_t, as every entry point does. */
enum
{
	/* verse_host.dll: compiles, analyses and runs. What the Godot editor loads. */
	VH_HOST_EDITOR = VH_HOST_KIND_EDITOR,
	/* verse_host_runtime.dll: runs cooked packages, and only those. WITH_VERSE_COMPILER=0, so
	 * the eleven compiler-side entry points answer VH_ERR_UNSUPPORTED. Ships with the game. */
	VH_HOST_RUNTIME = VH_HOST_KIND_RUNTIME,
	/* verse_cook.exe: compiles a project and saves it as cooked packages. Never loaded as a
	 * library -- it is an executable the export plugin runs -- so nothing calls this through
	 * the ABI; it is here so that the three kinds are one enum. */
	VH_HOST_COOKER = VH_HOST_KIND_COOKER
};

/* A host older than 8.2 does not export this at all, and its absence means VH_HOST_EDITOR,
 * which is what every host before 8.2 was. */
VH_API int32_t vh_host_kind(void);

VH_ATTR VH_API int32_t vh_init(const vh_init_desc* Desc);
VH_ATTR VH_API void vh_shutdown(void);

/* What one vh_tick did, for R-ASYNC-6. A budget nothing can see the effect of is a number nobody
 * can set: this is what makes "the pump ran out of time" observable rather than inferred from a
 * frame that got slower. */
typedef struct vh_tick_stats
{
	int32_t StructSize;
	/* Queued jobs the pump ran. */
	int32_t JobsRun;
	/* Queued jobs still waiting when it stopped. Non-zero *and* Overran means the budget cut it
	 * short; non-zero without it means something queued more work while the pump was running. */
	int32_t JobsPending;
	/* Tasks suspended in `Sleep`, waiting for their deadline. Not queue depth: a sleeper is not
	 * work the budget could have got to sooner. */
	int32_t Sleeping;
	/* Seconds the pump spent, garbage collection included -- that is also what the budget covers,
	 * and hiding it would make an over-budget frame look like an idle one. */
	double ElapsedSeconds;
	/* The budget stopped the pump with work still queued. */
	vh_bool Overran;

	/* Added at ABI v6.1: a consumer whose StructSize stops above these reads neither, and the host
	 * writes neither. */

	/* How many times the calling thread blocked waiting for a background analysis to finish since
	 * the previous vh_tick, and the seconds it spent blocked.
	 *
	 * This is the editor's per-keystroke stall, and it is nowhere in the pump's own numbers: the
	 * analysis runs on a thread the host owns, but ~20 entry points must wait it out before they
	 * may touch the VM or the semantic program, so a completion or a hover asked mid-analysis pays
	 * the rest of it. Zero on a frame where nothing had to wait, which is the common case. */
	int32_t AnalysisWaits;
	double AnalysisWaitSeconds;

	/* Added at ABI v8.1, and the same rule applies: a consumer whose StructSize stops above this
	 * does not read it and the host does not write it. */

	/* The largest number of live tasks any one script instance's task scope holds (R-ASYNC-4's
	 * scopes, counted through VTaskGroup::GetNumActive).
	 *
	 * OQ-13 chose observability over a cap, and this is the observation: a `spawn` in `_Process`
	 * makes sixty tasks a second on one instance, and nothing else in these numbers separates that
	 * from sixty instances with one task each. JobsPending cannot -- a suspended task is not
	 * queued work. */
	int32_t PeakInstanceTasks;
} vh_tick_stats;

/* Runs queued Verse work for at most BudgetSeconds. Call once per frame. OutStats may be NULL.
 *
 * **What the budget governs is the queue, and only the queue** (phase-5-design.md D4): `Sleep`
 * resumptions, `Main`, and anything else with no Godot event behind it. A task awaiting a Godot
 * signal is not here at all -- it resumes *inside* the emission, synchronously, which is where
 * GDScript resumes a coroutine too and is therefore as unbudgeted as GDScript's is.
 *
 * Also where collection is driven: requesting a Verse collection cycle from inside running Verse
 * code deadlocks the process (docs/abi-v2-design.md §1a), so the reference table's entries are
 * only ever released from here.
 *
 * It is no longer where Verse is restarted after a script raises. Until ABI v6 a runtime error
 * stopped every script in the process until the next tick and this was the frame boundary that
 * cleared it; now a raise terminates only the raising instance's task scope, and that instance is
 * given a fresh one at its next call. A consumer that never ticks still never runs queued work.
 *
 * OutStats is filled up to its own StructSize: a consumer built against a lower minor gets the
 * prefix it reserved and nothing past it. One smaller than the v6.0 struct is not a prefix of
 * anything and is left untouched. */
VH_ATTR VH_API void vh_tick(double BudgetSeconds, vh_tick_stats* OutStats);

/* Added at ABI v8.3. Runs a collection to completion, rather than the incremental slice vh_tick
 * takes when the object array is running short.
 *
 * It exists because collection is the only moment at which "Verse has dropped this" becomes
 * knowable, and nothing else can make that moment happen: a reference table entry and a Godot
 * object a script minted are both released from a UObject's BeginDestroy, so until a cycle runs
 * neither release is observable and neither can be asserted. The GDExtension does not call it --
 * vh_tick's incremental collection is what a running game wants -- and host_smoke does, which is
 * the only place the lifetime half of R-NODE-3 can be a test rather than a claim.
 *
 * Call from the vh_init thread and never from inside running Verse: requesting a cycle from Verse
 * code deadlocks the process (docs/abi-v2-design.md 1a), which is why vh_tick is where the
 * incremental half lives. Does nothing before vh_init. */
VH_ATTR VH_API void vh_collect_garbage(void);

/* One .verse file, and where in the project's module tree it belongs. */
typedef struct vh_source_file
{
	/* Absolute path to the file on disk. */
	const char* PathUtf8;

	/* The module the file's definitions go into: a '/'-separated path of Verse identifiers,
	 * relative to the project's root module. NULL or "" puts the file in the root module, which
	 * is where every file in a project with no modules lives.
	 *
	 * The consumer decides this, not the host -- which directories are modules is a question
	 * about res://, and the host never learns what res:// means. */
	const char* ModulePathUtf8;
} vh_source_file;

/* Compiles every listed .verse file as ONE Verse program, publishing a new generation of it.
 *
 * Verse's compilation unit is the package, not the file, so every script the host will ever run
 * has to be in the list: a build is always of the whole project.
 *
 * Callable as often as the consumer likes. Each call publishes a package this process has not
 * published before -- publishing marks a package's exports LoaderImport and republishing that
 * same package asserts on the flag -- and writes the new generation's number through
 * OutGeneration, which counts from 1. Instances made against an earlier generation keep their
 * own class and go on running; nothing adopts the new one. The previous generation's package is
 * retained rather than reclaimed, which costs memory per generation (spec R-ITER-6).
 *
 * A failed build publishes nothing: the previous generation keeps running and OutGeneration is
 * left alone. Diagnostics are reported through the init callback.
 *
 * Everything that *describes* a class answers about this build the moment it returns: the snapshot
 * is taken from the build's own semantic analysis. What a build does not leave is an AST, because
 * generating code puts it out of reach -- so the three entry points that resolve a position answer
 * VH_ERR_STATE until the consumer asks for an analysis. A consumer with an editor in it should ask
 * for one after a successful build; one without an editor has nothing to ask for.
 *
 * Class names in this ABI are qualified by module from here on: `player` for a file in the root
 * module, `gameplay/player` for one in the `gameplay` module. */
VH_ATTR VH_API int32_t vh_compile_project(const vh_source_file* Files, int32_t Count, int32_t* OutGeneration);

/* Re-runs semantic analysis over the already-compiled project with one file's text replaced,
 * reporting diagnostics through the init callback. Generates no code, so the running program is
 * unchanged and this is safe to call repeatedly -- unlike vh_compile_project, which publishes a
 * generation every time. Returns VH_OK when the project still analyses clean.
 *
 * Blocks for the length of a whole-project analysis -- ~750 ms once the mirror is read from its
 * digest, ~1.4-1.8 s before the project's first successful build, where it is still source. Either
 * is a visible stall if called from an editor's UI thread. Prefer the _begin/_poll pair below. */
VH_ATTR VH_API int32_t vh_check_project(const char* PathUtf8, const char* SourceUtf8);

/* The same analysis, started on a background thread so the caller's UI thread keeps running.
 *
 * Returns VH_OK once started, or VH_ERR_STATE when an analysis is already in flight -- only one
 * runs at a time. Diagnostics are NOT reported from the worker: they are buffered and handed to
 * the init callback from vh_check_project_poll, so the callback still only ever runs on the
 * vh_init thread.
 *
 * While an analysis is in flight the host will not execute Verse: vh_tick becomes a no-op, and
 * every entry point that runs Verse -- vh_instantiate, vh_instance_call, vh_callback_invoke,
 * vh_run_main, the instance field accessors, vh_compile_project -- blocks until the analysis
 * finishes. That is enforced here rather than asked of the caller because getting it wrong is not
 * recoverable: VerseVM blocks execution for the duration of a build, and ticking anyway trips
 * `ensure(!bBlockAllExecution)` and then takes the process down.
 *
 * The entry points that merely *describe* a class do not block. They answer from a snapshot taken
 * at the end of each analysis and made current by vh_check_project_poll, so what they describe is
 * the last analysis that landed rather than the one in flight. The three that resolve a *position*
 * are the exception -- vh_lookup_symbol, vh_complete_symbol and vh_signature_at answer against the
 * AST the worker is rebuilding, which no snapshot describes, so they answer VH_ERR_STATE while one
 * runs rather than waiting for it.
 *
 * An analysis begun here must be polled to completion: nothing else reaps one, and until it is
 * reaped the next vh_check_project_begin is refused and vh_tick stays a no-op. */
VH_ATTR VH_API int32_t vh_check_project_begin(const char* PathUtf8, const char* SourceUtf8);

/* Reaps a vh_check_project_begin. Call from the vh_init thread, e.g. once per frame.
 *
 * Sets *OutFinished to 1 and reports the analysis' buffered diagnostics through the init callback
 * when one had been started and has now completed; sets it to 0 while one is still running, or
 * when none was started. The return value is the finished analysis' result -- VH_OK when the
 * project analysed clean -- and VH_OK when there was nothing to reap. */
VH_ATTR VH_API int32_t vh_check_project_poll(vh_bool* OutFinished);

/* Whether an analysis started by vh_check_project_begin is still running. */
VH_ATTR VH_API vh_bool vh_check_project_busy(void);

/* Calls the compiled project's suspending Main(:[]string, :[string]string) and pumps until it
 * completes. A Godot scene has no use for it; it is how a project is run as a plain program. */
VH_ATTR VH_API int32_t vh_run_main(const char* const* Args, int32_t ArgCount, int64_t* OutExitCode);

/* ------------------------------------------------------- class instances -- */

/* One live Verse object: a script's `class(node2d)` bound to one Godot object.
 *
 * A script is a top-level class named after its own file. The host instantiates that class
 * against the node the script is attached to and calls its methods. */
typedef struct vh_instance vh_instance;

/* Whether the compiled project defines a top-level class of that name deriving from `object`,
 * which is what makes a .verse file usable as a script at all. */
VH_ATTR VH_API vh_bool vh_has_class(const char* ClassNameUtf8);

/* Instantiates the top-level Verse class ClassNameUtf8 (undecorated) and binds it to Handle.
 * The class must derive from `object`. */
VH_ATTR VH_API int32_t vh_instantiate(const char* ClassNameUtf8, vh_handle Handle, vh_instance** OutInstance);
VH_ATTR VH_API void vh_release_instance(vh_instance* Instance);

/* ----------------------------------------------------------- dispatch (v2) -- */

/* One parameter of a script method. */
typedef struct vh_param_desc
{
	const char* NameUtf8; /* the parameter's own name, for an editor's argument hint */
	int32_t NameLen;
	int32_t Type;       /* vh_type -- how an argument of this parameter is laid out */
	int32_t VariantTag; /* vh_variant_tag -- what Godot type the consumer should convert from */
	vh_bool HasDefault; /* a `?Named:t = default` parameter, which the caller may omit */
} vh_param_desc;

/* One method a script's class defines, for R-NODE-9 and for binding Godot's virtuals.
 *
 * v1 reported the intersection of the script with a hardcoded three-name array, and dispatched
 * over exactly two call shapes. This describes what the class actually declares. */
typedef struct vh_method_desc
{
	/* The Verse name, undecorated -- `Fire`. This is the name Godot sees, verbatim: a script
	 * method is not transformed on its way out, which is what makes `mover.Fire()` the spelling in
	 * GDScript and matches how a property already reads there. */
	const char* NameUtf8;
	int32_t NameLen;

	/* The decorated name vh_instance_call takes -- `(/user@localhost/mover:)Fire(:float)`. The
	 * mangling is the VM's, and looking a method up by anything else does not fail politely:
	 * UVerseClass::PeekField asserts on a field the shape does not have. */
	const char* DecoratedUtf8;
	int32_t DecoratedLen;

	const vh_param_desc* Params;
	int32_t ParamCount;
	/* How many leading parameters have no default, and so must be supplied. */
	int32_t RequiredParamCount;

	int32_t ResultType;       /* vh_type; VH_TYPE_VOID for a method returning nothing */
	int32_t ResultVariantTag; /* vh_variant_tag */

	/* The method can fail -- declared <decides>. A failing call answers VH_CALL_NO_SUCH_MEMBER
	 * rather than raising, and the consumer turns that into whatever its own language spells
	 * absence as. */
	vh_bool CanFail;
	/* The method suspends -- declared <suspends>. Calling one starts a task rather than running
	 * it to completion, so a consumer expecting a return value must not call it. */
	vh_bool Suspends;

	/* Godot's own name for the virtual this method overrides -- `_ready`, `_unhandled_input` --
	 * or empty for a method that is not one. Filled from the mirrored class the script derives
	 * from, which the generator built from extension_api.json's `is_virtual` methods; the script
	 * declares it with <override> and Verse's own redeclaration rules supply the error when it
	 * does not. That is the general mechanism R-NODE-7 asks for: a virtual added by a future Godot
	 * version arrives by regenerating the mirror, with no code change here. */
	const char* GodotVirtualUtf8;
	int32_t GodotVirtualLen;

	/* Where the method is declared: zero-based row, utf8 byte column, as everywhere else. Both
	 * -1 when the definition has no source location. */
	int32_t Line;
	int32_t Column;
} vh_method_desc;

/* Why a signal a script declared cannot reach Godot.
 *
 * The same bargain vh_export_reject makes, for the same reason: a rejected signal is still listed,
 * because the consumer needs somewhere to say why. A signal that silently does not exist is the
 * worst outcome -- the author sees neither the connection nor a reason, and finds out at the first
 * emission, at runtime, if at all.
 *
 * All five are decidable from the declaration alone, which is the point: each was a runtime
 * surprise before it was a reject code. */
typedef enum vh_signal_reject
{
	VH_SIGNAL_OK = 0,

	/* Declared `var`. A signal is an identity, not a value to reassign: the binding is minted once
	 * against the object the member was constructed on, so a later write leaves the name pointing
	 * at a row nothing emits through. */
	VH_SIGNAL_IS_VAR,

	/* Not `<public>`. Godot registers signals per script class and connects by name from outside
	 * the class entirely, so a member the rest of the program cannot see has no one to connect it. */
	VH_SIGNAL_NOT_PUBLIC,

	/* The declaring class has no Godot object to register on -- it does not derive from `object`,
	 * so nothing ever hands it a handle. Unlike GDScript, where every class extends Object and so
	 * every instance carries a signal table of its own, a plain Verse class is a VM object with no
	 * Godot counterpart: there is nothing to emit on and nothing to connect to. */
	VH_SIGNAL_NO_GODOT_OWNER,

	/* A payload argument with no Godot wire lane. RejectDetail names the argument. */
	VH_SIGNAL_PAYLOAD_UNSUPPORTED,

	/* A struct payload with a field that is itself a struct the bridge does not mirror. A struct
	 * payload decomposes one level, into one Godot argument per top-level field -- which is what
	 * buys the connect dialog real names -- and there is no second level to decompose into:
	 * Godot has no argument shape for "a struct". RejectDetail names the field. */
	VH_SIGNAL_PAYLOAD_NESTED_STRUCT
} vh_signal_reject;

/* One signal a script's class declares (R-SIG-1).
 *
 * There is no `@signal` attribute: the member's *type* is the declaration -- `Hit:signal(t)`
 * -- and the host reads declared types out of the semantic program already. A bridge attribute
 * exists where the text is the only source, which is why `@global_class` needs one and this does
 * not.
 *
 * The name is the **Verse spelling verbatim** -- `Hit`, `MobSpawned` -- following C# (which
 * registers `Hit`, not `hit`) and following what a Verse method already does in every scene
 * connection this repository carries. */
typedef struct vh_signal_desc
{
	const char* NameUtf8;
	int32_t NameLen;

	/* One per Godot argument. A `tuple()` payload has none; `tuple(int, string)` has two, named
	 * `Arg0` and `Arg1` because Verse tuples cannot name their elements; a *struct* payload has one
	 * per top-level field, each named by the field, which is what buys the connect dialog and
	 * _make_function real names; anything else has one, named for its type.
	 *
	 * Mapping is one level only -- a `vector2` payload is one Vector2 argument, not two floats, and
	 * a struct field that is itself a struct is VH_SIGNAL_PAYLOAD_NESTED_STRUCT rather than a
	 * second round of flattening. */
	const vh_param_desc* Args;
	int32_t ArgCount;

	/* Where the member is declared: zero-based row, utf8 byte column. */
	int32_t Line;
	int32_t Column;

	/* vh_signal_reject. Anything but VH_SIGNAL_OK means the signal is listed so it can be
	 * explained and must not be registered with Godot: the consumer drops it from the script's
	 * signal list and reports RejectDetail at Line/Column. */
	int32_t Reject;

	/* The argument or field the rejection is about, for the two payload rejections; empty for the
	 * three that are about the member itself. Not null terminated. */
	const char* RejectDetailUtf8;
	int32_t RejectDetailLen;
} vh_signal_desc;

/* Why a method's `@rpc` cannot reach Godot. Same bargain vh_signal_reject makes: a rejected config
 * is still listed, because the consumer needs somewhere to say why. */
typedef enum vh_rpc_reject
{
	VH_RPC_OK = 0,

	/* An argument that is not one of Godot's seven words. GDScript's own message lists them, and
	 * RejectDetail carries the word that was written. */
	VH_RPC_UNKNOWN_ARGUMENT,

	/* Two arguments from one category -- two of "call_local"/"call_remote", two of
	 * "any_peer"/"authority", or two transfer modes. RejectDetail names the category. */
	VH_RPC_DUPLICATE_CATEGORY,

	/* An argument in a position that wants a different kind of value: the channel is the fourth and
	 * must be an integer, and the first three must be strings. */
	VH_RPC_BAD_ARGUMENT_TYPE
} vh_rpc_reject;

/* One method's `@rpc` configuration (R-EXP-9), as Godot's own `rpc_config` Dictionary wants it.
 *
 * The four fields are Godot's four and carry its own numbering, so the consumer copies rather than
 * translates: RpcMode is MultiplayerAPI::RPCMode, TransferMode is MultiplayerPeer::TransferMode.
 * A method with no `@rpc` is not listed at all -- absence is how "not an RPC" is spelled, and it is
 * what Godot's own empty config means.
 *
 * The defaults are GDScript's, which are also SceneRPCInterface::_parse_rpc_config's: authority,
 * not call-local, reliable, channel 0. They are filled in here rather than left to the consumer so
 * that the two sides cannot drift about what `@rpc("any_peer")` alone means. */
typedef struct vh_rpc_desc
{
	/* The Verse method name, which is the name Godot dispatches by -- or Godot's own name for the
	 * virtual it overrides, for the same reason vh_method_desc carries one. */
	const char* NameUtf8;
	int32_t NameLen;

	int32_t RpcMode;      /* MultiplayerAPI::RPCMode: 0 disabled, 1 any peer, 2 authority */
	vh_bool CallLocal;
	int32_t TransferMode; /* MultiplayerPeer::TransferMode: 0 unreliable, 1 ordered, 2 reliable */
	int32_t Channel;

	int32_t Line;
	int32_t Column;

	/* vh_rpc_reject. Anything but VH_RPC_OK means the method must not be registered as an RPC and
	 * the consumer reports RejectDetail at Line/Column. */
	int32_t Reject;
	const char* RejectDetailUtf8;
	int32_t RejectDetailLen;
} vh_rpc_desc;

/* One member of a class's statics module (R-NODE-4).
 *
 * Verse has no `static` keyword, and an inline module is what it has instead -- a script can
 * already write `PlayerStatics.MaxSpeed` with nothing from the bridge. What crosses here is the
 * *link*, so Godot can be told: `_get_constants` and `_has_static_method` are the two questions it
 * asks a script class about, and neither has an instance to ask through. */
typedef struct vh_static_desc
{
	const char* NameUtf8;
	int32_t NameLen;

	/* A function rather than a constant. A constant carries Value; a function does not, and is
	 * reported so that _has_static_method can answer. */
	vh_bool IsFunction;

	/* The constant's value, for a member that is not a function. Points at host storage that lives
	 * until the next call to vh_class_static_list. */
	vh_value Value;

	int32_t Line;
	int32_t Column;
} vh_static_desc;

/* The members of the module that declares itself this class's statics -- `@statics("player")` over
 * `player_statics`.
 *
 * The association is *declared* rather than inferred from a name, and that was the point: a naming
 * convention produces a silently empty statics module when it is mistyped, where a module naming a
 * class that does not exist can be a diagnostic. A class with no statics module answers VH_OK with
 * a count of 0; VH_ERR_NOT_FOUND means there is no such class.
 *
 * The descriptors are the host's and live until the next call. */
VH_ATTR VH_API int32_t vh_class_static_list(const char* ClassNameUtf8, const vh_static_desc** OutStatics, int32_t* OutCount);

/* Whether the class is declared `class<abstract>` (R-NODE-5), so Godot stops offering to
 * instantiate a base script that was never meant to be attached. */
VH_ATTR VH_API vh_bool vh_class_is_abstract(const char* ClassNameUtf8);

/* Added at ABI v8.4. The Godot class a script of this class attaches to -- `Node2D` for a
 * `class(node2d)`, `Resource` for a `class(resource)`, and the nearest *mirrored* ancestor for a
 * script that extends another script. Written into *OutUtf8 as a NUL-terminated string the host
 * owns until the next call to this entry point.
 *
 * The consumer normally answers this from the source text, which is both faster and available long
 * before anything is built. What it cannot answer from is a source that is not there: an exported
 * game ships every `.verse` as a one-byte stub, so an export had no superclass to read and answered
 * nothing. This is the other side of the same question, and the only side an export has.
 *
 * It resolves the class in the VM, so it is subject to the usual two rules -- call it from the
 * vh_init thread, and not while an analysis is running, which answers VH_ERR_STATE. Neither binds
 * in practice: the consumer reaches this only where the text is gone, and a host with no text is a
 * runtime host, which never analyses.
 *
 * VH_ERR_NOT_FOUND for a class this program does not carry. */
VH_ATTR VH_API int32_t vh_class_base_type(const char* ClassNameUtf8, const char** OutUtf8);

/* Every signal ClassNameUtf8 declares, its base script classes' included -- signals inherit.
 *
 * Read out of the semantic program the last analysis left behind, like vh_class_method_list and
 * vh_class_export_list, so it refreshes per keystroke rather than per build. A signal declared in
 * a script that has never been built appears after the next Build, which is the same bargain an
 * `@export` *default* already makes.
 *
 * A signal the bridge cannot carry is listed with a Reject rather than dropped -- see
 * vh_signal_reject for why -- so a consumer registering signals with Godot must filter on Reject.
 *
 * The descriptors are the host's and live until the next call to this function.
 * Returns VH_ERR_NOT_FOUND when the class does not exist in the analysed program. */
VH_ATTR VH_API int32_t vh_class_signal_list(const char* ClassNameUtf8, const vh_signal_desc** OutSignals, int32_t* OutCount);

/* v8.7: every method of ClassNameUtf8 that carries an `@rpc` attribute (R-EXP-9).
 *
 * A method with no `@rpc` is absent rather than listed with a disabled mode, because that is what
 * Godot's own config means -- SceneRPCInterface walks the keys it is given and nothing else.
 *
 * Read from the same snapshot the method and export lists are, so it refreshes per keystroke; an
 * `@rpc` added to a method is live before the next Build, the way a signal declaration is.
 *
 * A configuration the bridge cannot carry is listed with a Reject rather than dropped, for the
 * reason vh_signal_reject gives, so a consumer building Godot's Dictionary must filter on Reject.
 *
 * The descriptors are the host's and live until the next call to this function.
 * Returns VH_ERR_NOT_FOUND when the class does not exist in the analysed program. */
VH_ATTR VH_API int32_t vh_class_rpc_list(const char* ClassNameUtf8, const vh_rpc_desc** OutRpcs, int32_t* OutCount);

/* Every method ClassNameUtf8 declares, including the ones that override a Godot virtual.
 *
 * Read out of the semantic program the last analysis left behind, like vh_class_export_list and
 * for the same reason: analysis re-runs as often as the editor types while code generation runs
 * only when a generation is built, so this is the method list that refreshes per keystroke.
 *
 * OutMethods points into storage owned by the host, valid until the next call to this function.
 * Returns VH_ERR_NOT_FOUND when the class does not exist in the analysed program. */
VH_ATTR VH_API int32_t vh_class_method_list(const char* ClassNameUtf8, const vh_method_desc** OutMethods, int32_t* OutCount);

/* Whether the instance implements this method itself, rather than inheriting an empty body from
 * the mirrored class it derives from. DecoratedName is vh_method_desc::DecoratedUtf8.
 *
 * The distinction decides whether Godot puts the node in the per-frame process list at all: every
 * mirrored virtual resolves on every instance, so a plain "does it resolve" test is true for
 * everything. */
VH_ATTR VH_API vh_bool vh_instance_has_function(vh_instance* Instance, const char* DecoratedName);

/* Calls any method the script defines, with any arguments, and answers its return value.
 *
 * This replaces v1's vh_instance_call_void and vh_instance_call_void_float, which is why v2 is a
 * break rather than an addition: there is one calling convention and it is this one.
 *
 * Answers vh_status, not vh_call_status -- the two enums are numbered independently and mixing
 * them is how a failed call reads as a successful one.
 *
 * Args are converted to the parameter types vh_class_method_list reported. Supplying the wrong
 * number, or an argument with no Verse spelling, answers VH_ERR_ARGUMENT and runs nothing;
 * neither is a runtime error, because neither is the script's fault. An unknown method is
 * VH_ERR_NOT_FOUND, also having run nothing.
 *
 * A <decides> method that runs and declines answers VH_ERR_FAILED with no value written -- which
 * is not the same as there being no such method. A method that raises answers VH_ERR_RUNTIME,
 * having reported the error with its stack through OnRuntimeError; the enclosing transaction is
 * rolled back, so queued Godot writes are discarded.
 *
 * OutResult is written into Arena and is valid until the caller releases it. Arena may be NULL
 * for a method whose result the caller does not want, which is not the same as a void method --
 * the call still runs.
 *
 * Calling into the instance seals it: see vh_instance_set_field. */
VH_ATTR VH_API int32_t vh_instance_call(vh_instance* Instance,
										const char* DecoratedName,
										const vh_value* Args,
										int32_t ArgCount,
										vh_arena* Arena,
										vh_value* OutResult);

/* Invokes the Verse function a Callable was made from (R-TYPE-3's other direction, R-INT-4).
 *
 * CallbackId is what vh_godot_api::MakeCallable was given. Args are converted to the declared
 * parameter types of the Verse method, exactly as vh_instance_call converts them, so the same
 * answers apply: VH_ERR_ARGUMENT for the wrong shape, VH_ERR_FAILED for a <decides> method that
 * declined, VH_ERR_RUNTIME for one that raised. VH_ERR_NOT_FOUND for a callback id that has been
 * released, or whose object Godot has freed -- which the consumer should already have caught
 * through is_valid(). */
VH_ATTR VH_API int32_t vh_callback_invoke(int64_t CallbackId,
										  const vh_value* Args,
										  int32_t ArgCount,
										  vh_arena* Arena,
										  vh_value* OutResult);

/* Drops the host's record of a callback. Called when the Callable holding the id is destroyed,
 * which is the only moment "Godot has finished with this" is knowable. Releasing an id twice, or
 * one that was never minted, does nothing. */
VH_ATTR VH_API void vh_callback_release(int64_t CallbackId);

/* ---------------------------------------------------------- class exports -- */

/* The inspector hint a member's declaration implies. Spelled as what the host knows -- "this is a
 * range" -- rather than as Godot's own PropertyHint numbering, which the host has no business
 * carrying: it has no ClassDB to check the answer against. */
typedef enum vh_export_hint
{
	VH_EXPORT_HINT_NONE = 0,
	/* A constrained int or float: the bounds are in the Range fields rather than the hint string,
	 * because turning them into a hint is Godot's business and needs Godot's editor settings. */
	VH_EXPORT_HINT_RANGE,
	/* The enumerators of the declared enum, comma separated, in declaration order. */
	VH_EXPORT_HINT_ENUM,
	/* The Verse name of a mirrored class, which the consumer turns into Godot's own and sorts
	 * into node or resource itself. */
	VH_EXPORT_HINT_CLASS,
	/* The Verse name of a class the project itself declares, carrying `@global_class`. Kept apart
	 * from VH_EXPORT_HINT_CLASS because the two resolve through different tables: a mirrored name
	 * is looked up in the generated API, while this one is a script's class, whose Godot name is
	 * the PascalCase spelling of the name given here -- the same one the script registered. */
	VH_EXPORT_HINT_SCRIPT_CLASS,

	/* --- v8.8: the hints a declared type cannot imply (R-EXP-1) -----------------------------
	 *
	 * Everything above is read off the member's *type*, which is the rule R-EXP-1 states:
	 * type-driven where the Verse type can say it, an attribute only where it cannot. These five
	 * are the set where it cannot -- the declared type is `string` or `int` and says nothing
	 * about what the value is for -- so each is an attribute the author writes.
	 *
	 * An older consumer that does not know one of these draws a plain field, which is exactly
	 * what it drew before the attribute existed. */

	/* A file path. HintString is the filter, as Godot spells it: `*.png` or `*.png,*.jpg`, and
	 * `*` for any file. There is no "no filter" spelling, because an attribute with no argument
	 * would have to be the attribute class itself and the class is what the constructor builds. */
	VH_EXPORT_HINT_FILE,
	/* A directory path. No hint string. */
	VH_EXPORT_HINT_DIR,
	/* A paragraph rather than a line: Godot's multi-line text editor. No hint string. */
	VH_EXPORT_HINT_MULTILINE,
	/* A bitmask over named bits, which Verse has no flag enum for. HintString is the names,
	 * comma separated, which is Godot's own spelling for this hint and passes straight through. */
	VH_EXPORT_HINT_FLAGS,
	/* A NodePath, which is a `string` on this wire. HintString is the Godot class a picked node
	 * must be -- `Node2D`, or `Node` for any. */
	VH_EXPORT_HINT_NODE_PATH
} vh_export_hint;

/* The inspector section a member opens. Godot's three nesting depths, and Verse's three
 * attributes for them: a category is a heading, a group folds under it, a subgroup under that. */
typedef enum vh_export_group
{
	VH_EXPORT_GROUP_NONE = 0,
	VH_EXPORT_GROUP_CATEGORY,
	VH_EXPORT_GROUP_GROUP,
	VH_EXPORT_GROUP_SUBGROUP
} vh_export_group;

/* Why a member the author asked to export cannot reach the inspector.
 *
 * A rejected member is still listed rather than dropped: the consumer needs somewhere to say why,
 * and a member that silently vanishes from the inspector is the worst of the three outcomes --
 * the author sees neither the property nor a reason. */
typedef enum vh_export_reject
{
	VH_EXPORT_OK = 0,

	/* No Godot type to rebuild the value as. */
	VH_EXPORT_UNSUPPORTED_TYPE,

	/* A Godot reference declared without an `option` around it. Nothing can force a value into an
	 * inspector slot, so a member that cannot hold the empty case is one whose declared type the
	 * scene can always violate -- and the Verse spelling that compiles, `node2d{}`, is a handle
	 * of 0: a reference that is dead from birth and indistinguishable from one freed later. */
	VH_EXPORT_OBJECT_NOT_OPTIONAL,

	/* An `option` around something that is not a Godot reference. The inspector has no empty slot
	 * for a number, so there is nothing for the option to mean. */
	VH_EXPORT_OPTION_NOT_OBJECT,

	/* A reference to a class the project declares, which did not register that class with Godot.
	 * Verse will let a member be typed as any class in the project, but the inspector filters a
	 * slot by a *Godot* class name, and an unregistered class has none -- so there is nothing to
	 * filter by and nothing to put in the scene. `@global_class` on the class being referred to is
	 * the whole of the fix. */
	VH_EXPORT_SCRIPT_CLASS_NOT_GLOBAL,

	/* v8.8: one of the five hint attributes on a member whose declared type it cannot describe --
	 * `@export_file` on an int, `@export_flags` on a string. The attribute exists precisely because
	 * the type does not say what the value is for, so the pairing is the author's to get right and
	 * this is the only place it can be checked.
	 *
	 * Hint still carries the attribute the author wrote, which is what the consumer's message
	 * names -- vh_export_desc has no reject detail and adding one would be a layout change. */
	VH_EXPORT_HINT_WRONG_TYPE
} vh_export_reject;

/* One data member of a script's class carrying `@export`. */
typedef struct vh_export_desc
{
	const char* NameUtf8; /* not null terminated */
	int32_t NameLen;
	vh_type Type;

	/* Which Godot type to rebuild the value as, when vh_type alone cannot say -- an object rather
	 * than the int its handle is. 0 (VH_VARIANT_NIL) means "infer from Type", as it does in
	 * vh_value. */
	int32_t VariantTag;

	/* VH_TYPE_ARRAY only, and only where VariantTag is VH_VARIANT_ARRAY: the Godot type of an
	 * element. Godot's packed arrays say what they hold in their own tag, but a plain Array does not
	 * -- it needs the element type spelled out beside it or the inspector offers the author an
	 * editor that can add anything. 0 where there is nothing to say. */
	int32_t ElementVariantTag;

	vh_bool IsVar; /* a `var` member; anything else can only be written before the instance seals */

	int32_t Hint; /* vh_export_hint */

	/* VH_EXPORT_HINT_ENUM and the two class hints only; a range speaks through the fields below. */
	const char* HintStringUtf8;
	int32_t HintStringLen;

	/* The two class hints only: the mirrored class whose Godot counterpart decides how the slot is
	 * drawn -- a node is picked out of the scene and a resource off disk, and the consumer has to ask
	 * ClassDB which this is. For VH_EXPORT_HINT_CLASS that is the class in HintStringUtf8 itself; for
	 * VH_EXPORT_HINT_SCRIPT_CLASS it is the nearest mirrored class the referenced class derives from,
	 * because ClassDB has never heard of a name a script registered and cannot place it. Empty when
	 * the chain reaches `object` without passing a mirrored class, which is a reference to something
	 * that is neither a node nor a resource. */
	const char* NativeClassUtf8;
	int32_t NativeClassLen;

	/* VH_EXPORT_HINT_RANGE: the bounds the declared type carries, exactly as it carries them. A
	 * bound the type does not constrain is absent rather than infinite, which is not the same
	 * thing to an inspector -- one end open is a spinbox that clamps on one side only.
	 *
	 * A strict inequality arrives already normalised: `_X < 500.0` is the double immediately below
	 * 500.0, and `0 < _X` on an int is simply 1. So there is nothing here to say which inequality
	 * was written, and nothing that needs saying -- the consumer rounds each bound inward to its
	 * own step, which lands on the last reachable value satisfying the constraint either way. */
	double RangeMin;
	double RangeMax;
	vh_bool HasRangeMin;
	vh_bool HasRangeMax;

	/* The inspector section this member opens, which every member listed after it joins until one
	 * opens another. VH_EXPORT_GROUP_NONE for a member that opens none; a section is closed by
	 * opening an empty-named one, since what is in force is whatever the last member named. */
	int32_t GroupKind; /* vh_export_group */
	const char* GroupNameUtf8;
	int32_t GroupNameLen;

	/* Where the member is declared, for a consumer with something to say about it: zero-based row,
	 * and a column counted in utf8 bytes the way the compiler counts. As everywhere else, this is
	 * where the *definition* starts, which is its first attribute rather than its name. Both are
	 * -1 when the definition has no source location. */
	int32_t Line;
	int32_t Column;

	/* vh_export_reject. VH_EXPORT_OK is a promise as well as an answer: the consumer can build a
	 * Godot value of Type/VariantTag for this member, so it is the whole of the filter -- nothing
	 * downstream has to second-guess the type. */
	int32_t Reject;
} vh_export_desc;

/* Lists the exported data members of a top-level class, in the order the class declares them.
 *
 * Read out of the semantic program the last analysis pass left behind, NOT out of the running
 * bytecode -- so this answers for the source as vh_check_project last saw it, and a member added
 * to a file since then shows up here while being absent from every live instance. That is the
 * point: analysis may re-run as often as the editor types, while vh_compile_project runs only
 * when a generation is built, so this is the export list that refreshes per keystroke.
 *
 * OutExports points into storage owned by the host, valid until the next call to this function.
 * Returns VH_ERR_NOT_FOUND when the class does not exist in the analysed program. */
VH_ATTR VH_API int32_t vh_class_export_list(const char* ClassNameUtf8, const vh_export_desc** OutExports, int32_t* OutCount);

/* Reads one data member off a live instance.
 *
 * Unlike the export list this does go through the VM, because a value only exists there.
 *
 * A reference reads as VH_TYPE_REF, and a reference holding nothing as the empty option tagged the
 * same way -- which is the one read that has to be told the declared type to answer: Verse spells
 * an empty option and `logic` false with the same cell, so the value alone cannot say which the
 * author wrote.
 *
 * OutValue points into storage owned by the host, valid until the next call to this function.
 * Returns VH_ERR_NOT_FOUND for a field the instance's shape does not carry. */
VH_ATTR VH_API int32_t vh_instance_get_field(vh_instance* Instance, const char* NameUtf8, const vh_value** OutValue);

/* The same read against the class default object, whose Verse constructor has already run --
 * which is the only place a member's declared default can be got.
 *
 * Every `@export` of every class is read into the analysis snapshot, so this is a lookup and never
 * waits for an analysis. A member that is not an export is read on the spot, and answers
 * VH_ERR_NOT_FOUND rather than blocking while an analysis is in flight.
 *
 * OutValue points into storage owned by the host, valid until the next call to *this* function --
 * vh_instance_get_field has its own buffer and no longer invalidates this one. */
VH_ATTR VH_API int32_t vh_class_default_field(const char* ClassNameUtf8, const char* NameUtf8, const vh_value** OutValue);

/* Writes one data member on a live instance.
 *
 * A reference member is written with the handle of the object it should hold, tagged
 * VH_VARIANT_OBJECT; a handle of 0 clears it to the empty option. The host builds the Verse
 * wrapper for that handle itself, which it can only do for a mirrored class -- a member typed as
 * one of the project's own classes is written through vh_instance_set_field_instance instead,
 * because the object it should hold already exists.
 *
 * An instance is *unsealed* from vh_instantiate until the first vh_instance_call, and sealing is
 * one-way. While unsealed, any exported member may be written: that is initialization, and it is
 * how a scene's stored values reach the object. Once sealed, only a `var` may be written -- a
 * non-var is immutable by the script author's own declaration, and no Verse code has yet run that
 * could have observed the value being replaced.
 *
 * Returns VH_ERR_NOT_FOUND for a member that is absent, stored as a shape constant, or a non-var
 * on a sealed instance. */
VH_ATTR VH_API int32_t vh_instance_set_field(vh_instance* Instance, const char* NameUtf8, const vh_value* Value);

/* Writes a reference member with an instance rather than a handle, which is the only way to give
 * one node's script a reference to another node's.
 *
 * A handle is not enough there. The object a `?mover` member should hold is the one that node's own
 * instance already is, and building a second from the handle would give one node two Verse objects:
 * two sets of members, two identities, and whichever the author reached through would be the wrong
 * one half the time. So the instance itself crosses.
 *
 * Value may be NULL, which clears the member to the empty option. Returns VH_ERR_NOT_FOUND for a
 * member that is absent or unwritable, as vh_instance_set_field does, and for one whose declared
 * class Value is not an instance of -- a slot holding a value of the wrong class is a fault the VM
 * does not notice until compiled code reads it, so it is refused here rather than written. */
VH_ATTR VH_API int32_t vh_instance_set_field_instance(vh_instance* Instance, const char* NameUtf8, vh_instance* Value);

/* What the script chose to call itself, for Godot's `to_string` -- R-NODE-10's first hook.
 *
 * Its own entry point rather than a vh_instance_call, because what it calls is not a method and
 * never can be. Verse spells this as an **extension method**:
 *
 *     (X:my_class).ToString<public>()<transacts>:string = "..."
 *
 * which is a module-level `operator'.ToString'(:my_class, :tuple())` -- the receiver is an ordinary
 * first argument and the call's own arguments are a tuple in the second. A class *member* named
 * ToString cannot be written at all: it is ambiguous with /Verse.org/Verse's own ToString, which is
 * itself reachable as an extension method. So vh_class_method_list never carries one and
 * vh_instance_call has nothing to resolve.
 *
 * A receiver written for a *base* class serves every script deriving from it.
 *
 * OutValue points into storage owned by the host, valid until the next call to this function.
 * Answers VH_ERR_NOT_FOUND when the class declares no such method, which is the common case and
 * means the consumer should leave Godot its own representation rather than show an empty string. */
VH_ATTR VH_API int32_t vh_instance_to_string(vh_instance* Instance, const vh_value** OutValue);

/* ---------------------------------------------------------- symbol lookup -- */

/* What an identifier resolved to. The consumer needs the var/non-var split to say which of
 * Godot's two local lookup results to report; the rest is for the text it shows. */
typedef enum vh_lookup_kind
{
	VH_LOOKUP_UNKNOWN = 0,
	VH_LOOKUP_DATA,
	VH_LOOKUP_FUNCTION,
	VH_LOOKUP_CLASS,
	VH_LOOKUP_MODULE,
	VH_LOOKUP_TYPE_ALIAS,
	VH_LOOKUP_ENUM
} vh_lookup_kind;

/* Where an identifier's definition is, and what it is. All strings are utf8 and none is null
 * terminated. */
typedef struct vh_lookup_desc
{
	const char* NameUtf8;
	int32_t NameLen;

	/* The file the definition was read from. Empty for a definition with no file behind it --
	 * the generated Godot API and Verse's own standard library are compiled from packages the
	 * project never names -- which is a definition that can be described but not jumped to. */
	const char* PathUtf8;
	int32_t PathLen;

	/* Where the definition starts. Zero-based row; the column is a byte offset into that row's
	 * utf8, matching how the compiler counts and NOT how Godot counts. Both are -1 when there is
	 * no source location.
	 *
	 * "Starts" includes any attributes applied to it, because that is where the compiler puts
	 * the definition's source range -- so a member behind four lines of @editable and friends
	 * reports the first of those rather than the row its name is on. */
	int32_t Line;
	int32_t Column;

	/* The definition's type, spelled as Verse source. Empty when it has none to spell. */
	const char* TypeUtf8;
	int32_t TypeLen;

	/* The name of the scope the definition was declared in -- for a method, the class that
	 * declares it, which is the one an inherited call resolves to rather than the one it was
	 * called through. Empty at the top level. This is what lets a consumer recognise a
	 * definition as belonging to a mirrored Godot class and name the Godot original. */
	const char* OwnerUtf8;
	int32_t OwnerLen;

	int32_t Kind;  /* vh_lookup_kind */
	vh_bool IsVar; /* declared with `var`, so assignable after the instance seals */

	/* A parameter of the function that declares it, rather than a member or a local. Worth telling
	 * apart because a parameter has no documentation of its own: its source line is the line its
	 * whole function is declared on, so the comment block "above" it is the function's. */
	vh_bool IsParameter;

	/* The cursor was on the definition itself rather than on a reference to it. A consumer that
	 * wants to jump somewhere useful needs this: at a declaration, the definition's own location
	 * is where the cursor already is. */
	vh_bool IsDefinition;

	/* The definition this one overrides, laid out like the fields above. Empty when it overrides
	 * nothing, and only ever filled when IsDefinition is set -- the immediate parent, not the
	 * root of the chain. An override cannot rename, so the name is NameUtf8 above.
	 *
	 * This is what lets a consumer describe `Ready<override>()` with the parent's documentation
	 * when the override itself carries no comment, and send a click to the parent rather than to
	 * the line the cursor is already on. */
	const char* OverriddenOwnerUtf8;
	int32_t OverriddenOwnerLen;
	const char* OverriddenPathUtf8;
	int32_t OverriddenPathLen;
	int32_t OverriddenLine;
	int32_t OverriddenColumn;
} vh_lookup_desc;

/* Resolves the identifier at Line/Column of PathUtf8 to the definition it refers to.
 *
 * Line and Column are zero-based and Column is a byte offset into the line, as above.
 *
 * Answered from the semantic program the last analysis left behind, so the caller must only ask
 * about a buffer that analysis actually saw: nothing here can tell that the file has been edited
 * since, and every locus below an insertion would be off by the rows it added. The caller holds
 * that contract -- it is the one that knows what the editor's buffer says.
 *
 * Only an analysis-only program can answer this. Code generation hangs an IR package off every
 * module and the accessors this walks assert rather than degrade when they find one -- so the
 * host tracks which kind of build produced the program it holds and answers VH_ERR_STATE rather
 * than trusting the caller to have asked at a safe moment. That is the same "ask again once an
 * analysis has landed" this answers while one is in flight, and it is what vh_compile_project
 * leaves behind: a build no longer runs an analysis of its own to put an AST back, so a caller
 * that wants one after a build asks for it.
 *
 * Answers VH_ERR_STATE while a vh_check_project_begin analysis is in flight. A position resolves
 * against the AST, which the worker is rebuilding and which no snapshot describes, so this is the
 * one read that can neither answer nor be made to wait cheaply -- declining costs an underline for
 * a frame.
 *
 * OutResult points into storage owned by the host, valid until the next call to this function.
 * Returns VH_ERR_NOT_FOUND when no identifier at that position resolves to a definition, which
 * includes the ordinary cases of hovering whitespace, a keyword or a comment. */
VH_ATTR VH_API int32_t vh_lookup_symbol(const char* PathUtf8, int32_t Line, int32_t Column, const vh_lookup_desc** OutResult);

/* ------------------------------------------------------------- completion -- */

/* Which question vh_complete_symbol is being asked. */
typedef enum vh_complete_mode
{
	/* What the expression at Line/Column has members of -- the answer to a `.`. The position is
	 * the *receiver's* last byte, not the cursor: the member being typed does not exist yet, and
	 * asking about it would resolve nothing. */
	VH_COMPLETE_MEMBERS = 0,

	/* What an identifier written at Line/Column could name: the locals ahead of it, the enclosing
	 * class' members and its superclasses', and every scope the file has brought into view. */
	VH_COMPLETE_SCOPE = 1,

	/* The same scopes, narrowed to what may follow an `@`: an attribute class, and the
	 * <constructor> function that builds one where the attribute takes an argument --
	 * `@editable` names the class, `@clamp_min("0.0")` the constructor beside it. The position is
	 * where the name would be written, past the `@`, exactly as VH_COMPLETE_SCOPE takes it.
	 *
	 * Not filtered by where the attribute may be applied: `@attribscope_data` says `editable`
	 * belongs on a data member, and this offers it anywhere an attribute can be written. What the
	 * attribute is about to be attached to is not written yet at the moment the question is
	 * asked. */
	VH_COMPLETE_ATTRIBUTES = 2,

	/* The same, narrowed to what may follow a `<` instead: `public`, `override`, `transacts`.
	 *
	 * A separate question from VH_COMPLETE_ATTRIBUTES because Verse separates the two positions
	 * and refuses the wrong one -- an attribute class tagged `@attribscope_specifier` "can only be
	 * used as a <specifier>", one tagged `@attribscope_attribute` "can only be used as an
	 * @attribute", and a user-defined attribute carrying neither tag is accepted in both
	 * (SemanticAnalyzer.cpp, ErrSemantic_InvalidAttributeScope). So the two modes are overlapping
	 * sets rather than one set asked for twice. */
	VH_COMPLETE_SPECIFIERS = 3,

	/* The same scopes as VH_COMPLETE_SCOPE, narrowed to what may stand where a type is expected --
	 * past the `:` of `Speed<public>:` or of a parameter. Classes, interfaces, enums, type aliases
	 * and the modules that qualify one; never a function or a data definition, which is what the
	 * unnarrowed answer offered there (730 functions and 41 members of 2774 options, so `Sp` in a
	 * type position offered the `Speed` member). */
	VH_COMPLETE_TYPES = 4,

	/* Narrower still: what may stand in a class header's parentheses, `X := class(...)`. Only a
	 * class or an interface can, so this drops the enums and aliases VH_COMPLETE_TYPES keeps --
	 * two thirds of that position's answer, since the mirror carries 758 enums. */
	VH_COMPLETE_SUPERTYPES = 5,

	/* What a `set` may assign to: a `var` data definition, local or member, and nothing else. The
	 * position is where the name would be written, past the `set `. By far the tightest of these
	 * -- a statement position offers ~2770 names and a `set` target around 40. */
	VH_COMPLETE_ASSIGNABLE = 6,

	/* The fields an archetype body may give a value: `vector2{` offers `X` and `Y`. Data members
	 * only -- a method cannot be written in an archetype body -- and inherited ones included,
	 * since a subclass' archetype may set a base's field.
	 *
	 * Positioned like VH_COMPLETE_MEMBERS rather than like the modes above: the answer is about
	 * the class named before the `{`, so Line/Column is that identifier's last byte. */
	VH_COMPLETE_ARCHETYPE_FIELDS = 7
} vh_complete_mode;

/* One name completion could insert. Laid out like vh_lookup_desc's first few fields, and read the
 * same way: utf8, none null terminated. */
typedef struct vh_complete_item
{
	const char* NameUtf8;
	int32_t NameLen;

	/* Spelled as Verse source -- a function's whole signature, a member's type. Empty when the
	 * definition has no type to spell. */
	const char* TypeUtf8;
	int32_t TypeLen;

	/* The scope that declares it, which is how the consumer recognises a name as belonging to a
	 * mirrored Godot class. Empty at the top level. */
	const char* OwnerUtf8;
	int32_t OwnerLen;

	/* Where it was declared, laid out like vh_lookup_desc's Path/Line and read the same way: the
	 * path is empty and the line -1 for a definition with no source behind it. Here so that a
	 * consumer can find the comment block above a name it is about to offer or document. */
	const char* PathUtf8;
	int32_t PathLen;
	int32_t Line;

	int32_t Kind;  /* vh_lookup_kind */
	vh_bool IsVar;

	/* How many parameters it declares, or -1 for anything that is not a function. An editor needs
	 * this to decide where to leave the caret after inserting a call. */
	int32_t ParamCount;

	/* Everything a function's declaration spells after its name, as Verse source: the parameter
	 * list with the parameters' own names, the effect specifiers, and the result type --
	 * "(Delta:float)<transacts>:void". Empty for anything that is not a function.
	 *
	 * Separate from TypeUtf8 because that is a function *type*, which drops the parameter names:
	 * the same definition reads there as "float->void". An editor writing a declaration needs the
	 * names, and only the compiler has them. */
	const char* SignatureUtf8;
	int32_t SignatureLen;

	/* Whether a subclass could declare this with <override>: a class member that is neither
	 * <final> nor a class var's accessor, the three things the analyzer refuses an override for.
	 * False for a free function, and for anything that is not a function.
	 *
	 * Says only that the compiler would take the declaration. Whether overriding it *does*
	 * anything is the consumer's question: a Verse method the bridge generated to forward into
	 * Godot answers here, and overriding one changes nothing about what Godot dispatches --
	 * which is why a script that does so is reported (docs/abi-v2-design.md §4, collision 3). */
	vh_bool IsOverridable;

	/* Hops from the class or scope the completion was asked about to the one that declares this
	 * item: 0 for that class itself, 1 for its immediate superclass, and so on outwards. -1 when
	 * the item was reached through a `using` rather than by inheritance or by an enclosing scope.
	 *
	 * The -1 is not "far away", it is a different kind of distance: a script reaches the whole
	 * mirrored Godot API through `using { /Godot.org/Godot }` written on its own module, so by hops
	 * the 9597 mirrored methods would sit a step or two from the cursor and outrank the class's own
	 * members. Only the chain a name is inherited or enclosed by is counted here; what an import
	 * costs is the consumer's to decide.
	 *
	 * Here so an editor can rank a class's own members above its parent's above Object's, which
	 * Godot's own LOCATION_PARENT_MASK is exactly the shape of. */
	int32_t OwnerDistance;

	/* Whether this is a *named* parameter -- declared `?ExactMatch:logic = false` -- which decides
	 * how a call site may pass it: `?ExactMatch := true`, never positionally. False for every name
	 * that is not a parameter.
	 *
	 * The compiler does not carry this on the parameter's definition. AnalyzeParam gives the
	 * definition the *value* type and only then wraps that in a CNamedType for the signature, so
	 * `?ExactMatch:logic` arrives as a definition named ExactMatch of type logic and the `?` is
	 * recoverable from the function type's parallel parameter list alone -- which the consumer
	 * does not have. Hence a flag rather than a spelling the consumer could parse.
	 *
	 * Added at ABI v9.0, and it is a major bump for the reason every addition to this struct is:
	 * the items are handed back as an array, so a field at the end changes the stride an older
	 * consumer indexes by and would be read as silent corruption rather than as a refusal. */
	vh_bool IsNamed;
} vh_complete_item;

/* Lists what could be written at Line/Column of PathUtf8, as the program describes that file
 * *now*. SourceUtf8 is not analysed: it is the buffer the caller is asking about, and this
 * checks it against the text the last analysis left the program holding.
 *
 * Runs no analysis and waits for none (ABI v7; until v6 it did both, and a first `.` cost the
 * editor a synchronous whole-project analysis). Answers VH_ERR_STATE when it cannot describe
 * SourceUtf8 -- either an analysis is in flight, or the program was built from different text --
 * and the caller's recourse is vh_check_project_begin on this very buffer, then ask again once
 * vh_check_project_poll reaps it. The position is answered against the AST, which the worker
 * rebuilds and which no snapshot describes, so this is the same refusal vh_lookup_symbol makes
 * and for the same reason.
 *
 * The buffer does not have to analyse cleanly, which is the point: `Position.` is a syntax error
 * and still answers, because uLang keeps the analysed sub-expressions of an expression it could
 * not analyse. The caller is expected to hand the completion buffer to vh_check_project_begin and
 * throw that analysis' diagnostics away -- they describe a line the author is halfway through
 * writing.
 *
 * An analysis of a completion buffer leaves the program holding it as that file's text, exactly
 * as any other does, so a vh_lookup_symbol afterwards answers for the completion buffer rather
 * than the editor's. That is the caller's to track; nothing here hides it.
 *
 * OutItems points into storage owned by the host, valid until the next call to this function.
 * Returns VH_ERR_NOT_FOUND when nothing at that position has members, or when the position is
 * not inside any scope the project owns. */
VH_ATTR VH_API int32_t vh_complete_symbol(const char* PathUtf8,
										  const char* SourceUtf8,
										  int32_t Line,
										  int32_t Column,
										  int32_t Mode,
										  const vh_complete_item** OutItems,
										  int32_t* OutCount);

/* One module of the project, by the path a `using` would name it with. */
typedef struct vh_module_ref
{
	/* '/'-separated and relative to the project's root module -- "gameplay", "gameplay/ai" -- so
	 * the import the consumer writes is `using { /user@localhost/<this> }`. Never empty: the root
	 * module is in scope everywhere and so is never an answer to this question. */
	const char* PathUtf8;
	int32_t PathLen;
} vh_module_ref;

/* Which of the project's modules declare a top-level NameUtf8.
 *
 * The question behind R-TOOL-12: analysis has just reported an unknown identifier, and this says
 * whether an import would fix it and which one. Zero answers means the name is simply not in the
 * project; more than one means the consumer must not guess, because two modules declaring one
 * name is legal and only the author knows which was meant.
 *
 * Read out of the semantic program the last analysis left behind, so it answers for the editor's
 * buffer rather than for the last build.
 *
 * OutModules points into storage owned by the host, valid until the next call to this function. */
VH_ATTR VH_API int32_t vh_resolve_unknown_name(const char* NameUtf8,
											   const vh_module_ref** OutModules,
											   int32_t* OutCount);

/* Every member ClassNameUtf8 declares itself -- not what it inherits, which each superclass
 * answers for on its own. Read out of the semantic program the last analysis left, exactly like
 * vh_class_export_list, and with the same consequence: it describes the source as last analysed
 * rather than the running program, which is what lets it refresh without a restart.
 *
 * Unlike vh_class_export_list this is not restricted to `@editable` members and does include
 * methods -- it exists to be turned into documentation, where a member the inspector ignores is
 * still a member the author wrote.
 *
 * OutItems points into storage owned by the host, valid until the next call to this function.
 * Returns VH_ERR_NOT_FOUND when the class does not exist in the analysed program. */
VH_ATTR VH_API int32_t vh_class_members(const char* ClassNameUtf8, const vh_complete_item** OutItems, int32_t* OutCount);

/* Every member ClassNameUtf8 inherits that a subclass could still declare with <override> -- the
 * other half of vh_class_members, and the half the class did not write.
 *
 * Each item is what vh_complete_symbol would hand back for the same name in VH_COMPLETE_SCOPE with
 * the cursor at a member declaration in that class: same signature spelling, same owner, same
 * IsOverridable. It is the same extraction, so a consumer may format an item from either call the
 * same way and the full list replaces the partial one without anything moving.
 *
 * What the class already declares is absent, inherited or not. vh_complete_symbol answers such a
 * name owned by the class that redeclared it -- the nearer scope wins -- and a declaration that is
 * already written is not one to offer; here the name is simply not in the list.
 *
 * Answered from the snapshot the last analysis left, so it never waits and never analyses: this is
 * the call an editor makes on the keystroke that opens completion, before the buffer in front of
 * the author has been analysed at all. It therefore describes the last analysed text, which for a
 * class the author is adding a method to is the text without that method.
 *
 * OutItems points into storage owned by the host, valid until the next call to this function.
 * Returns VH_ERR_NOT_FOUND when the class does not exist in the analysed program. */
VH_ATTR VH_API int32_t vh_class_override_candidates(const char* ClassNameUtf8, const vh_complete_item** OutItems, int32_t* OutCount);

/* ---------------------------------------------------------- call signature -- */

/* The parameters of the function being called at a position, for an editor's argument hint. */
typedef struct vh_signature_desc
{
	const char* NameUtf8;
	int32_t NameLen;

	/* The return type, spelled as Verse source. Empty when there is none to spell. */
	const char* ResultUtf8;
	int32_t ResultLen;

	/* One per declared parameter, in order, each carrying the parameter's own name and type.
	 * Points into storage owned by the host with the same lifetime as this descriptor.
	 *
	 * This is the one place vh_complete_item::IsNamed answers anything: a hint that spells a
	 * named parameter as `ExactMatch:logic` names a call no author can write. */
	const vh_complete_item* Params;
	int32_t ParamCount;
} vh_signature_desc;

/* Resolves the function called at Line/Column of PathUtf8, with that file's text replaced by
 * SourceUtf8, and reports its parameters.
 *
 * Line and Column name the *callee's* last byte -- the `t` of `GetChild(` -- for the same reason
 * vh_complete_symbol takes the receiver's: the argument list under construction does not analyse,
 * and there is nothing at the cursor to resolve.
 *
 * Runs no analysis and waits for none, and answers VH_ERR_STATE when it cannot describe
 * SourceUtf8 -- the whole of vh_complete_symbol's contract above, for the same reasons.
 *
 * OutResult points into storage owned by the host, valid until the next call to this function.
 * Returns VH_ERR_NOT_FOUND when nothing at that position is a function. */
VH_ATTR VH_API int32_t vh_signature_at(const char* PathUtf8,
									   const char* SourceUtf8,
									   int32_t Line,
									   int32_t Column,
									   const vh_signature_desc** OutResult);

/* ------------------------------------------------- the debugger (v8.1, R-DIAG-4) -- */

/* Installs or removes the Verse debugger. While installed, `Notify` fires on every bytecode op
 * and the computation watchdog is suspended -- the latter is what makes sitting on a breakpoint
 * for a minute legal rather than an ErrRuntime_ComputationLimitExceeded.
 *
 * VH_ERR_STATE when Epic's own socket debugger owns the VM's single debugger slot, which is what
 * vh_init_desc::EnableDebugger asks for. The two cannot both be installed. */
VH_ATTR VH_API int32_t vh_debug_set_enabled(vh_bool Enabled);

/* One Verse call frame, while stopped. Native frames are included -- Epic's stack walk emits them
 * with a name and no location -- which is what makes a stop inside a mirrored method legible. */
typedef struct vh_debug_frame
{
	int32_t StructSize;
	const char* PathUtf8;   int32_t PathLen;    /* empty for a native frame */
	const char* NameUtf8;   int32_t NameLen;
	int32_t Line;                               /* 0 for a native frame */
} vh_debug_frame;

/* One named value in a stopped frame. */
typedef struct vh_debug_value
{
	int32_t StructSize;
	const char* NameUtf8;   int32_t NameLen;
	/* Exactly one is populated. Value when the bridge carries the type, Rendered otherwise --
	 * a Verse local can be a tuple, an option, a map or a class instance and vh_value describes
	 * none of them (phase-6-design.md D7). */
	const vh_value* Value;
	const char* RenderedUtf8; int32_t RenderedLen;
} vh_debug_value;

/* Which set vh_debug_stack_values answers. */
typedef enum vh_debug_value_kind
{
	/* Every register the frame names except `Self`. */
	VH_DEBUG_LOCALS = 0,
	/* `Self` and its fields. This is the only place a script instance's state appears: Godot calls
	 * a C++ virtual on whatever _debug_get_stack_level_instance returns, and a GDExtension script
	 * instance is not a ScriptInstance, so that virtual must answer null forever (D8). */
	VH_DEBUG_MEMBERS
} vh_debug_value_kind;

/* The three reads that describe the stopped stack. All answer VH_ERR_STATE when nothing is
 * stopped. They are not the only entry points legal while it is -- S-3 found nested entry safe,
 * so an ordinary call or property read from inside the debug loop works too.
 *
 * They join the "answer from what is stashed" group rather than the "wait for an analysis" group
 * or the "execute Verse" group: Notify's four arguments are valid only for the duration of the
 * call, so the host keeps them for exactly as long as DebugBreak is on the stack.
 *
 * What they hand back points into storage owned by the host, valid until the next call to the
 * same function. */
VH_ATTR VH_API int32_t vh_debug_stack_count(int32_t* OutCount);
VH_ATTR VH_API int32_t vh_debug_stack_frame(int32_t Level, const vh_debug_frame** OutFrame);
VH_ATTR VH_API int32_t vh_debug_stack_values(int32_t Level, int32_t Kind, const vh_debug_value** OutValues, int32_t* OutCount);

/* ------------------------------------------------- the profiler (v8.1, R-DIAG-5) -- */

/* One row of Godot's ScriptLanguageExtensionProfilingInfo, filled from what the bridge knows
 * exactly: every crossing it makes, plus whatever the author asked for with a `profile{}` block.
 *
 * Verse offers no per-call hook, so a Verse function called from another Verse function has no row
 * of its own unless it is wrapped in one of those blocks. A sampler would have produced rows with
 * no call count, which is worse than fewer rows (phase-6-design.md D13). */
typedef struct vh_profile_row
{
	int32_t StructSize;
	/* GDScript's three-part shape, `path::start_line::Class.func`, because the editor's profiler
	 * splits on it -- matching it is what makes a Verse row read like every other row
	 * (gdscript_compiler.cpp builds the same string). */
	const char* SignatureUtf8; int32_t SignatureLen;
	int64_t CallCount;
	double TotalSeconds;
	/* TotalSeconds less the time spent in nested boundary crossings, which is what makes a Verse
	 * method that emits a signal that calls another Verse method attribute correctly. */
	double SelfSeconds;
} vh_profile_row;

/* Off until Godot turns it on. With it off a boundary crossing pays one relaxed load and a
 * predicted branch, and a `profile{}` block costs a delegate that is not bound. */
VH_ATTR VH_API int32_t vh_profiling_set_enabled(vh_bool Enabled);

/* The accumulated rows, or this frame's alone -- Godot asks both questions and they are
 * _profiling_get_accumulated_data and _profiling_get_frame_data. FrameOnly resets the per-frame
 * accumulator as it reads it, which is what makes the next frame's numbers that frame's.
 *
 * OutRows points into storage owned by the host, valid until the next call. */
VH_ATTR VH_API int32_t vh_profiling_read(vh_bool FrameOnly, const vh_profile_row** OutRows, int32_t* OutCount);

/* Signatures for GetProcAddress on the consumer side. */
typedef int32_t (*vh_abi_version_fn)(void);
typedef int32_t (*vh_host_kind_fn)(void);
typedef int32_t (*vh_init_fn)(const vh_init_desc*);
typedef void (*vh_shutdown_fn)(void);
typedef void (*vh_tick_fn)(double, vh_tick_stats*);
typedef void (*vh_collect_garbage_fn)(void);
typedef int32_t (*vh_compile_project_fn)(const vh_source_file*, int32_t, int32_t*);
typedef int32_t (*vh_resolve_unknown_name_fn)(const char*, const vh_module_ref**, int32_t*);
typedef int32_t (*vh_check_project_fn)(const char*, const char*);
typedef int32_t (*vh_check_project_begin_fn)(const char*, const char*);
typedef int32_t (*vh_check_project_poll_fn)(vh_bool*);
typedef vh_bool (*vh_check_project_busy_fn)(void);
typedef int32_t (*vh_run_main_fn)(const char* const*, int32_t, int64_t*);
typedef vh_bool (*vh_has_class_fn)(const char*);
typedef int32_t (*vh_instantiate_fn)(const char*, vh_handle, vh_instance**);
typedef void (*vh_release_instance_fn)(vh_instance*);
typedef int32_t (*vh_class_method_list_fn)(const char*, const vh_method_desc**, int32_t*);
typedef int32_t (*vh_class_signal_list_fn)(const char*, const vh_signal_desc**, int32_t*);
typedef int32_t (*vh_class_rpc_list_fn)(const char*, const vh_rpc_desc**, int32_t*);
typedef int32_t (*vh_class_static_list_fn)(const char*, const vh_static_desc**, int32_t*);
typedef vh_bool (*vh_class_is_abstract_fn)(const char*);
typedef int32_t (*vh_class_base_type_fn)(const char*, const char**);
typedef vh_bool (*vh_instance_has_function_fn)(vh_instance*, const char*);
typedef int32_t (*vh_instance_call_fn)(vh_instance*, const char*, const vh_value*, int32_t, vh_arena*, vh_value*);
typedef int32_t (*vh_callback_invoke_fn)(int64_t, const vh_value*, int32_t, vh_arena*, vh_value*);
typedef void (*vh_callback_release_fn)(int64_t);
typedef int32_t (*vh_class_export_list_fn)(const char*, const vh_export_desc**, int32_t*);
typedef int32_t (*vh_instance_get_field_fn)(vh_instance*, const char*, const vh_value**);
typedef int32_t (*vh_class_default_field_fn)(const char*, const char*, const vh_value**);
typedef int32_t (*vh_instance_set_field_fn)(vh_instance*, const char*, const vh_value*);
typedef int32_t (*vh_instance_set_field_instance_fn)(vh_instance*, const char*, vh_instance*);
typedef int32_t (*vh_instance_to_string_fn)(vh_instance*, const vh_value**);
typedef int32_t (*vh_lookup_symbol_fn)(const char*, int32_t, int32_t, const vh_lookup_desc**);
typedef int32_t (*vh_complete_symbol_fn)(const char*, const char*, int32_t, int32_t, int32_t, const vh_complete_item**, int32_t*);
typedef int32_t (*vh_class_members_fn)(const char*, const vh_complete_item**, int32_t*);
typedef int32_t (*vh_class_override_candidates_fn)(const char*, const vh_complete_item**, int32_t*);
typedef int32_t (*vh_signature_at_fn)(const char*, const char*, int32_t, int32_t, const vh_signature_desc**);
typedef int32_t (*vh_debug_set_enabled_fn)(vh_bool);
typedef int32_t (*vh_debug_stack_count_fn)(int32_t*);
typedef int32_t (*vh_debug_stack_frame_fn)(int32_t, const vh_debug_frame**);
typedef int32_t (*vh_debug_stack_values_fn)(int32_t, int32_t, const vh_debug_value**, int32_t*);
typedef int32_t (*vh_profiling_set_enabled_fn)(vh_bool);
typedef int32_t (*vh_profiling_read_fn)(vh_bool, const vh_profile_row**, int32_t*);

#ifdef __cplusplus
}
#endif

#endif /* VERSE_HOST_ABI_H */
