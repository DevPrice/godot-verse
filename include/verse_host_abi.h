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

#define VH_ABI_VERSION 1

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

struct vh_value
{
	int32_t Type; /* vh_type */
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

VH_API int32_t vh_abi_version(void);

VH_API int32_t vh_init(const vh_init_desc* Desc);
VH_API void vh_shutdown(void);

/* Runs queued Verse work for at most BudgetSeconds. Call once per frame. */
VH_API void vh_tick(double BudgetSeconds);

/* Compiles one .verse file as a standalone snippet. Diagnostics go to the init callback. */
VH_API int32_t vh_compile_file(const char* PathUtf8, vh_script** OutScript);
VH_API void vh_release_script(vh_script* Script);

VH_API vh_bool vh_script_has_function(vh_script* Script, const char* DecoratedName);

/* Calls a suspending Main(:[]string, :[string]string) and pumps until it completes. */
VH_API int32_t vh_run_main(vh_script* Script, const char* const* Args, int32_t ArgCount, int64_t* OutExitCode);

VH_API int32_t vh_call_void(vh_script* Script, const char* DecoratedName);
VH_API int32_t vh_call_void_float(vh_script* Script, const char* DecoratedName, double Arg);

/* Signatures for GetProcAddress on the consumer side. */
typedef int32_t (*vh_abi_version_fn)(void);
typedef int32_t (*vh_init_fn)(const vh_init_desc*);
typedef void (*vh_shutdown_fn)(void);
typedef void (*vh_tick_fn)(double);
typedef int32_t (*vh_compile_file_fn)(const char*, vh_script**);
typedef void (*vh_release_script_fn)(vh_script*);
typedef vh_bool (*vh_script_has_function_fn)(vh_script*, const char*);
typedef int32_t (*vh_run_main_fn)(vh_script*, const char* const*, int32_t, int64_t*);
typedef int32_t (*vh_call_void_fn)(vh_script*, const char*);
typedef int32_t (*vh_call_void_float_fn)(vh_script*, const char*, double);

#ifdef __cplusplus
}
#endif

#endif /* VERSE_HOST_ABI_H */
