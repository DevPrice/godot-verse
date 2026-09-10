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
 * into a vh_arena are owned by the arena and valid until the call that supplied it returns.
 */
#ifndef VERSE_HOST_ABI_H
#define VERSE_HOST_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VH_ABI_VERSION 14

typedef int32_t vh_bool;

typedef enum vh_status
{
	VH_OK = 0,
	VH_ERR_ABI,       /* version mismatch or malformed descriptor */
	VH_ERR_STATE,     /* called before vh_init, or twice */
	VH_ERR_INIT,      /* engine boot failed */
	VH_ERR_COMPILE,   /* Verse source did not compile; diagnostics were reported */
	VH_ERR_NOT_FOUND, /* no such function / file */
	VH_ERR_RUNTIME    /* Verse raised a runtime error */
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
	VH_CALL_BAD_VALUE       /* an argument or the result has no representation on this wire */
} vh_call_status;

/* ---------------------------------------------------------------- values -- */

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
	VH_TYPE_OPTION /* Option == NULL is Verse's false */
} vh_type;

typedef struct vh_value vh_value;
typedef struct vh_pair vh_pair;

/* Godot's Variant::Type, as the wire carries it. vh_type says how the payload is laid out;
 * this says which Godot type to rebuild from it, which vh_type alone cannot express -- a
 * two-float tuple is equally a Vector2, a Vector2i or a plain array.
 *
 * 0 (Godot's TYPE_NIL) means "infer from vh_type", which is what every pre-v3 caller wants.
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
	VH_VARIANT_PACKED_VECTOR4_ARRAY = 38
} vh_variant_tag;

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

typedef struct vh_godot_api
{
	int32_t StructSize;
	void* Ctx;

	void (*Print)(void* Ctx, const char* Utf8, int32_t Len);
	vh_handle (*GetNode)(void* Ctx, const char* PathUtf8, int32_t PathLen); /* 0 if absent */
	vh_bool (*IsValid)(void* Ctx, vh_handle Handle);

	/* All three answer vh_call_status. A dead handle must be reported as VH_CALL_DEAD_OBJECT
	 * rather than folded into a missing member: the host raises on the former and fails on the
	 * latter. */
	int32_t (*GetProperty)(void* Ctx, vh_handle Handle, const char* NameUtf8, int32_t NameLen, vh_arena* Arena, vh_value* OutValue);
	int32_t (*SetProperty)(void* Ctx, vh_handle Handle, const char* NameUtf8, int32_t NameLen, const vh_value* Value);
	int32_t (*CallMethod)(void* Ctx, vh_handle Handle, const char* NameUtf8, int32_t NameLen, const vh_value* Args, int32_t ArgCount, vh_arena* Arena, vh_value* OutValue);

	int32_t (*GetChildCount)(void* Ctx, vh_handle Handle);
	vh_handle (*GetChild)(void* Ctx, vh_handle Handle, int32_t Index);

	/* Writes a VH_TYPE_MAP of string->string describing the object (name, class, path). */
	vh_bool (*GetMeta)(void* Ctx, vh_handle Handle, vh_arena* Arena, vh_value* OutValue);

	/* ClassDB::instantiate. 0 if the class is unknown or not instantiable. The caller owns the
	 * result: a Node that is never added to a tree leaks unless it is freed. */
	vh_handle (*Instantiate)(void* Ctx, const char* ClassNameUtf8, int32_t ClassNameLen);

	/* Engine::get_singleton, for Input, Time, and the rest of Godot's global objects. 0 if
	 * there is no such singleton. */
	vh_handle (*GetSingleton)(void* Ctx, const char* NameUtf8, int32_t NameLen);
} vh_godot_api;

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
} vh_diagnostic;

typedef void (*vh_diagnostic_fn)(void* Ctx, const vh_diagnostic* Diagnostic);

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

	vh_bool EnableDebugger;
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

VH_ATTR VH_API int32_t vh_init(const vh_init_desc* Desc);
VH_ATTR VH_API void vh_shutdown(void);

/* Runs queued Verse work for at most BudgetSeconds. Call once per frame. */
VH_ATTR VH_API void vh_tick(double BudgetSeconds);

/* Compiles every listed .verse file as ONE Verse program.
 *
 * Verse's compilation unit is the package, not the file, and this is not a preference: a second
 * build in the same process re-notifies already-loaded native Verse packages and aborts inside
 * the async loader. So this may be called once per process, and every script the host will ever
 * run has to be in the list.
 *
 * All the files share one flat scope, so each must name its class after its own file stem or its
 * definitions collide with every other script's. */
VH_ATTR VH_API int32_t vh_compile_project(const char* const* PathsUtf8, int32_t Count);

/* Re-runs semantic analysis over the already-compiled project with one file's text replaced,
 * reporting diagnostics through the init callback. Generates no code, so the running program is
 * unchanged and this is safe to call repeatedly -- unlike vh_compile_project, which may run once
 * per process. Returns VH_OK when the project still analyses clean.
 *
 * Blocks for the length of a whole-project analysis (~100ms on a three-file project), which is a
 * visible stall if called from an editor's UI thread. Prefer the _begin/_poll pair below. */
VH_ATTR VH_API int32_t vh_check_project(const char* PathUtf8, const char* SourceUtf8);

/* The same analysis, started on a background thread so the caller's UI thread keeps running.
 *
 * Returns VH_OK once started, or VH_ERR_STATE when an analysis is already in flight -- only one
 * runs at a time. Diagnostics are NOT reported from the worker: they are buffered and handed to
 * the init callback from vh_check_project_poll, so the callback still only ever runs on the
 * vh_init thread.
 *
 * While an analysis is in flight the host will not execute Verse. vh_tick becomes a no-op and
 * returns VH_ERR_STATE, and every entry point that reads the semantic program blocks until the
 * analysis finishes. Both are enforced here rather than asked of the caller because getting it
 * wrong is not recoverable: VerseVM blocks execution for the duration of a build, and ticking
 * anyway trips `ensure(!bBlockAllExecution)` and then takes the process down. */
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

VH_ATTR VH_API vh_bool vh_instance_has_function(vh_instance* Instance, const char* DecoratedName);
VH_ATTR VH_API int32_t vh_instance_call_void(vh_instance* Instance, const char* DecoratedName);
VH_ATTR VH_API int32_t vh_instance_call_void_float(vh_instance* Instance, const char* DecoratedName, double Arg);

/* ---------------------------------------------------------- class exports -- */

/* One data member of a script's class carrying `@godot_export`. */
typedef struct vh_export_desc
{
	const char* NameUtf8; /* not null terminated */
	int32_t NameLen;
	vh_type Type;
	vh_bool IsVar; /* a `var` member; anything else can only be written before the instance seals */

	/* Inspector hints, from the metadata attributes a member also carries. Each is empty when the
	 * attribute is absent. These arrive as the strings Verse spelled them with -- `@clamp_min`
	 * and friends take a string argument, not a number -- so the consumer parses them and a typo
	 * is a bad hint rather than a compile error. */
	const char* ClampMinUtf8;
	int32_t ClampMinLen;
	const char* ClampMaxUtf8;
	int32_t ClampMaxLen;
	const char* CategoryUtf8;
	int32_t CategoryLen;
} vh_export_desc;

/* Lists the `@godot_export` data members of a top-level class.
 *
 * Read out of the semantic program the last analysis pass left behind, NOT out of the running
 * bytecode -- so this answers for the source as vh_check_project last saw it, and a member added
 * to a file since then shows up here while being absent from every live instance. That is the
 * point: analysis may re-run as often as the editor types, while vh_compile_project may not run
 * twice in a process, so this is the only export list that can refresh without a restart.
 *
 * OutExports points into storage owned by the host, valid until the next call to this function.
 * Returns VH_ERR_NOT_FOUND when the class does not exist in the analysed program. */
VH_ATTR VH_API int32_t vh_class_export_list(const char* ClassNameUtf8, const vh_export_desc** OutExports, int32_t* OutCount);

/* Reads one data member off a live instance.
 *
 * Unlike the export list this does go through the VM, because a value only exists there. Only
 * logic, int, float and string come across; a member of any other Verse type reports
 * VH_ERR_NOT_FOUND rather than a half-converted value.
 *
 * OutValue points into storage owned by the host, valid until the next call to either field
 * reader. Returns VH_ERR_NOT_FOUND for a field the instance's shape does not carry. */
VH_ATTR VH_API int32_t vh_instance_get_field(vh_instance* Instance, const char* NameUtf8, const vh_value** OutValue);

/* The same read against the class default object, whose Verse constructor has already run --
 * which is the only place a member's declared default can be got. Same storage lifetime. */
VH_ATTR VH_API int32_t vh_class_default_field(const char* ClassNameUtf8, const char* NameUtf8, const vh_value** OutValue);

/* Writes one data member on a live instance. Same four types the reader covers.
 *
 * An instance is *unsealed* from vh_instantiate until the first vh_instance_call_*, and sealing is
 * one-way. While unsealed, any exported member may be written: that is initialization, and it is
 * how a scene's stored values reach the object. Once sealed, only a `var` may be written -- a
 * non-var is immutable by the script author's own declaration, and no Verse code has yet run that
 * could have observed the value being replaced.
 *
 * Returns VH_ERR_NOT_FOUND for a member that is absent, stored as a shape constant, or a non-var
 * on a sealed instance. */
VH_ATTR VH_API int32_t vh_instance_set_field(vh_instance* Instance, const char* NameUtf8, const vh_value* Value);

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
 * host tracks which kind of build produced the program it holds and answers VH_ERR_NOT_FOUND
 * rather than trusting the caller to have asked at a safe moment. vh_compile_project leaves the
 * program analysable for this reason.
 *
 * OutResult points into storage owned by the host, valid until the next call to this function.
 * Returns VH_ERR_NOT_FOUND when no identifier at that position resolves to a definition, which
 * includes the ordinary cases of hovering whitespace, a keyword or a comment. */
VH_ATTR VH_API int32_t vh_lookup_symbol(const char* PathUtf8, int32_t Line, int32_t Column, const vh_lookup_desc** OutResult);

/* Signatures for GetProcAddress on the consumer side. */
typedef int32_t (*vh_abi_version_fn)(void);
typedef int32_t (*vh_init_fn)(const vh_init_desc*);
typedef void (*vh_shutdown_fn)(void);
typedef void (*vh_tick_fn)(double);
typedef int32_t (*vh_compile_project_fn)(const char* const*, int32_t);
typedef int32_t (*vh_check_project_fn)(const char*, const char*);
typedef int32_t (*vh_check_project_begin_fn)(const char*, const char*);
typedef int32_t (*vh_check_project_poll_fn)(vh_bool*);
typedef vh_bool (*vh_check_project_busy_fn)(void);
typedef int32_t (*vh_run_main_fn)(const char* const*, int32_t, int64_t*);
typedef vh_bool (*vh_has_class_fn)(const char*);
typedef int32_t (*vh_instantiate_fn)(const char*, vh_handle, vh_instance**);
typedef void (*vh_release_instance_fn)(vh_instance*);
typedef vh_bool (*vh_instance_has_function_fn)(vh_instance*, const char*);
typedef int32_t (*vh_instance_call_void_fn)(vh_instance*, const char*);
typedef int32_t (*vh_instance_call_void_float_fn)(vh_instance*, const char*, double);
typedef int32_t (*vh_class_export_list_fn)(const char*, const vh_export_desc**, int32_t*);
typedef int32_t (*vh_instance_get_field_fn)(vh_instance*, const char*, const vh_value**);
typedef int32_t (*vh_class_default_field_fn)(const char*, const char*, const vh_value**);
typedef int32_t (*vh_instance_set_field_fn)(vh_instance*, const char*, const vh_value*);
typedef int32_t (*vh_lookup_symbol_fn)(const char*, int32_t, int32_t, const vh_lookup_desc**);

#ifdef __cplusplus
}
#endif

#endif /* VERSE_HOST_ABI_H */
