/* C ABI between godot_verse.dll (MSVC, godot-cpp) and verse_host.dll (UBT, EpicClang/AutoRTFM).
 *
 * The two DLLs cannot share a C++ ABI: verse_host is built by Unreal Build Tool with the
 * AutoRTFM clang driver and a monolithic UE runtime, godot_verse by SCons with MSVC. Everything
 * that crosses the boundary is plain C.
 *
 * Threading: every entry point must be called from the thread that called vh_init (Godot's main
 * thread). UE binds its game thread there.
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

#define VH_ABI_VERSION 3

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

	vh_bool (*GetProperty)(void* Ctx, vh_handle Handle, const char* NameUtf8, int32_t NameLen, vh_arena* Arena, vh_value* OutValue);
	vh_bool (*SetProperty)(void* Ctx, vh_handle Handle, const char* NameUtf8, int32_t NameLen, const vh_value* Value);
	vh_bool (*CallMethod)(void* Ctx, vh_handle Handle, const char* NameUtf8, int32_t NameLen, const vh_value* Args, int32_t ArgCount, vh_arena* Arena, vh_value* OutValue);

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

typedef struct vh_script vh_script;

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

/* Compiles one .verse file as a standalone snippet. Diagnostics go to the init callback. */
VH_ATTR VH_API int32_t vh_compile_file(const char* PathUtf8, vh_script** OutScript);

/* Compiles every listed .verse file as ONE Verse program.
 *
 * Verse's compilation unit is the package, not the file, and this is not a preference: a second
 * build in the same process re-notifies already-loaded native Verse packages and aborts inside
 * the async loader. So this may be called once per process, and every script the host will ever
 * run has to be in the list.
 *
 * All the files share one flat scope, so each must wrap its definitions in a module named after
 * its own file stem or its definitions collide with every other script's. */
VH_ATTR VH_API int32_t vh_compile_project(const char* const* PathsUtf8, int32_t Count);

/* Resolves a handle for one file of an already-compiled project. Does not compile. */
VH_ATTR VH_API int32_t vh_open_script(const char* PathUtf8, vh_script** OutScript);

VH_ATTR VH_API void vh_release_script(vh_script* Script);

VH_ATTR VH_API vh_bool vh_script_has_function(vh_script* Script, const char* DecoratedName);

/* Calls a suspending Main(:[]string, :[string]string) and pumps until it completes. */
VH_ATTR VH_API int32_t vh_run_main(vh_script* Script, const char* const* Args, int32_t ArgCount, int64_t* OutExitCode);

VH_ATTR VH_API int32_t vh_call_void(vh_script* Script, const char* DecoratedName);
VH_ATTR VH_API int32_t vh_call_void_float(vh_script* Script, const char* DecoratedName, double Arg);

/* ------------------------------------------------------- class instances -- */

/* One live Verse object: a script's `class(godot_node2d)` bound to one Godot object.
 *
 * The class-per-script shape supersedes the module-per-file one: a script may instead define a
 * top-level class named after its file, in which case the host instantiates that class and calls
 * its methods rather than the module's free functions. Both shapes are supported. */
typedef struct vh_instance vh_instance;

/* Whether the compiled project defines a top-level class of that name deriving from
 * `godot_object`. This is how a class-shaped script is told from a module-shaped one. */
VH_ATTR VH_API vh_bool vh_has_class(const char* ClassNameUtf8);

/* Instantiates the top-level Verse class ClassNameUtf8 (undecorated) and binds it to Handle.
 * The class must derive from `godot_object`. */
VH_ATTR VH_API int32_t vh_instantiate(const char* ClassNameUtf8, vh_handle Handle, vh_instance** OutInstance);
VH_ATTR VH_API void vh_release_instance(vh_instance* Instance);

VH_ATTR VH_API vh_bool vh_instance_has_function(vh_instance* Instance, const char* DecoratedName);
VH_ATTR VH_API int32_t vh_instance_call_void(vh_instance* Instance, const char* DecoratedName);
VH_ATTR VH_API int32_t vh_instance_call_void_float(vh_instance* Instance, const char* DecoratedName, double Arg);

/* Signatures for GetProcAddress on the consumer side. */
typedef int32_t (*vh_abi_version_fn)(void);
typedef int32_t (*vh_init_fn)(const vh_init_desc*);
typedef void (*vh_shutdown_fn)(void);
typedef void (*vh_tick_fn)(double);
typedef int32_t (*vh_compile_file_fn)(const char*, vh_script**);
typedef int32_t (*vh_compile_project_fn)(const char* const*, int32_t);
typedef int32_t (*vh_open_script_fn)(const char*, vh_script**);
typedef void (*vh_release_script_fn)(vh_script*);
typedef vh_bool (*vh_script_has_function_fn)(vh_script*, const char*);
typedef int32_t (*vh_run_main_fn)(vh_script*, const char* const*, int32_t, int64_t*);
typedef int32_t (*vh_call_void_fn)(vh_script*, const char*);
typedef int32_t (*vh_call_void_float_fn)(vh_script*, const char*, double);
typedef vh_bool (*vh_has_class_fn)(const char*);
typedef int32_t (*vh_instantiate_fn)(const char*, vh_handle, vh_instance**);
typedef void (*vh_release_instance_fn)(vh_instance*);
typedef vh_bool (*vh_instance_has_function_fn)(vh_instance*, const char*);
typedef int32_t (*vh_instance_call_void_fn)(vh_instance*, const char*);
typedef int32_t (*vh_instance_call_void_float_fn)(vh_instance*, const char*, double);

#ifdef __cplusplus
}
#endif

#endif /* VERSE_HOST_ABI_H */
