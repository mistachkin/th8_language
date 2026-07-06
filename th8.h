/*
 * th8.h -- Public interface to the TH8 interpreter.
 *
 * TH8 is a minimal, embeddable Tcl-compatible scripting language
 * implementing the Tcl Language Standard (Draft 0.1) with Tcl 8.4
 * surface syntax, strict UTF-8 strings, a non-recursive evaluation
 * engine, first-class value/token unification, and a platform
 * abstraction layer that eliminates all direct C runtime dependencies.
 *
 * This header defines the complete public API for embedding TH8.
 * It is the only header an embedder needs to include.
 *
 * Key design principles:
 *
 *   1. Platform abstraction -- TH8 never calls malloc, memcpy, printf,
 *      or any other C standard library function directly.  All such
 *      operations are routed through the Th8_Platform callback table,
 *      making TH8 portable to freestanding environments (UEFI, kernel,
 *      bare-metal) without #ifdef proliferation.
 *
 *   2. No direct CRT calls in the core -- The core interpreter (th8_core.c)
 *      and the language module (th8_lang.c) use only the xMalloc,
 *      xMemcpy, xMemset, etc. callbacks.  Higher-level CRT functions
 *      (strlen, strcmp, qsort, vsnprintf) are optional and only needed
 *      by auxiliary modules such as the regex engine.
 *
 *   3. NRE trampoline -- Evaluation uses a non-recursive engine (NRE)
 *      to avoid C stack overflow on deeply nested scripts.  Commands
 *      that evaluate sub-scripts schedule continuations via
 *      Th8_NRAddCallback and return to the trampoline loop rather
 *      than recursing in C.  The recursive Th8_Eval entry point is
 *      provided for convenience; it drives the trampoline internally.
 *
 *   4. Strict UTF-8 -- All strings are UTF-8.  The interpreter
 *      validates input and all internal operations preserve UTF-8
 *      well-formedness.  A taint bit in the length field tracks
 *      strings originating from untrusted sources.
 *
 *   5. Value/token unification -- Th8_Value serves as both a runtime
 *      value and a parse-tree token, reducing allocations and
 *      simplifying the representation.
 *
 *   6. Safe by construction -- TH8 has no built-in commands that
 *      directly access the host system.  There is no [open], [exec],
 *      [socket], [file], or any other command that bypasses the
 *      platform abstraction.  ALL external interaction (I/O, file
 *      access, time, process info, entropy) is mediated by
 *      Th8_Platform callbacks that the embedder controls.  This
 *      means EVERY command is safe in the sense that it cannot
 *      escape the interpreter sandbox.  The security boundary is
 *      the Th8_Platform itself, not a command whitelist.
 *
 *      Commands like [gets], [puts], and [source] operate only on
 *      channels and data stores provided by the platform.  Since
 *      TH8 has no [open], scripts cannot obtain new channel handles
 *      or file descriptors.  The embedder decides what (if anything)
 *      each callback does: a NULL callback disables the feature, a
 *      restricted callback limits access, and a custom callback can
 *      implement virtual file systems, sandboxed I/O, or logging.
 *
 *      For resource limits, the embedder may set:
 *        Th8_SetStepLimit     -- bounds total computation
 *        Th8_SetResultLimit   -- bounds string/result size
 *        Th8_SetAllocLimit    -- bounds total memory usage
 *        Th8_SetOverflowCheck -- makes integer overflow an error
 *
 * Dedicated to the memory of Miguel Sofer (d. 2016).
 *
 * Copyright (c) 2026 by Joe Mistachkin.  All rights reserved.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#ifndef TH8_H
#define TH8_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Feature test macros are defined in th8_meta_defs.h (the
 * internal meta-header).  They are NOT defined here because
 * th8.h is the public API header distributed to embedders --
 * it must be self-contained and not pull in internal build
 * infrastructure.  For TH8 source files, the meta-headers
 * are always included BEFORE th8.h.
 *
 * For the amalgamation, the build system prepends the feature
 * test macros at the top of the generated th8.c file.
 */

/*
 * Standard headers needed by the public API.
 *
 * These are the ONLY system headers th8.h includes.  They
 * provide the types used in public API signatures:
 *
 *   <stddef.h>  -- size_t, NULL (used in every API function)
 *   <stdarg.h>  -- va_list (used by xVsnprintf callback typedef)
 */

#include <stddef.h>
#include <stdarg.h>

/*
 *----------------------------------------------------------------------
 *
 * Export decoration --
 *
 *	TH8_API marks public API functions.  On Windows it expands
 *	to __declspec(dllexport) when building the TH8 library itself
 *	(TH8_BUILD_DLL is defined) and to __declspec(dllimport) when
 *	consuming the library.  On GCC/Clang it uses the visibility
 *	attribute.  The header user may also predefine TH8_API to
 *	override this logic (e.g. for static linking).
 *
 *----------------------------------------------------------------------
 */

#if !defined(TH8_API)
#  if defined(_WIN32) || defined(__CYGWIN__)
#    if defined(TH8_BUILD_STATIC)
#      define TH8_API
#    elif defined(TH8_BUILD_DLL)
#      define TH8_API __declspec(dllexport)
#    else
#      define TH8_API __declspec(dllimport)
#    endif
#  elif defined(__GNUC__) && __GNUC__ >= 4
#    define TH8_API __attribute__((visibility("default")))
#  else
#    define TH8_API
#  endif
#endif

/*
 *----------------------------------------------------------------------
 *
 * Platform selection macros --
 *
 *	TH8_PLATFORM_POSIX   POSIX platform (Linux, BSD, macOS).
 *	TH8_PLATFORM_WIN32   Windows platform (Win32 API).
 *	TH8_PLATFORM_MACOS   macOS-specific extensions (on top of POSIX).
 *	TH8_PLATFORM_LIBC    C runtime bridge (mem, I/O wrappers).
 *	TH8_PLATFORM_NULLIO  Null/restricted I/O platform.
 *
 *	Auto-detected from compiler-defined macros when not explicitly
 *	set.  The user may predefine these to force a specific platform
 *	(e.g. in an amalgamation build that includes all platform files).
 *
 *----------------------------------------------------------------------
 */

#if !defined(TH8_PLATFORM_POSIX) && !defined(TH8_PLATFORM_WIN32)
#  if defined(_WIN32) || defined(WIN32)
#    define TH8_PLATFORM_WIN32 1
#  else
#    define TH8_PLATFORM_POSIX 1
#  endif
#endif

#if !defined(TH8_PLATFORM_MACOS) && defined(__APPLE__)
#  if defined(TH8_PLATFORM_POSIX)
#    define TH8_PLATFORM_MACOS 1
#  endif
#endif

/*
 * iOS auto-derivation: when building against an Apple SDK,
 * TARGET_OS_IPHONE is defined to 1 in <TargetConditionals.h> for
 * iOS / iPadOS / tvOS / watchOS / visionOS targets and to 0 for
 * macOS targets.  We pull in TargetConditionals only when __APPLE__
 * is set so non-Apple builds are unaffected.  TH8_PLATFORM_IOS
 * implies TH8_PLATFORM_MACOS (the iOS layer extends the macOS
 * layer with its own deltas; see th8_ios.c).
 */
#if !defined(TH8_PLATFORM_IOS) && defined(__APPLE__)
#  include <TargetConditionals.h>
#  if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
#    define TH8_PLATFORM_IOS 1
#    if !defined(TH8_PLATFORM_MACOS)
#      define TH8_PLATFORM_MACOS 1
#    endif
#  endif
#endif

/*
 * Android auto-derivation: __ANDROID__ is unconditionally defined
 * by every Android NDK toolchain (clang / gcc), regardless of API
 * level or ABI.  Bionic libc supplies all POSIX surface so
 * TH8_PLATFORM_POSIX stays defined; the Android layer adds Bionic-
 * specific deltas on top (see th8_android.c).
 */
#if !defined(TH8_PLATFORM_ANDROID) && defined(__ANDROID__)
#  define TH8_PLATFORM_ANDROID 1
#endif

#if !defined(TH8_PLATFORM_LIBC)
#  define TH8_PLATFORM_LIBC 1
#endif

#if !defined(TH8_PLATFORM_NULLIO)
#  define TH8_PLATFORM_NULLIO 1
#endif

#if !defined(TH8_PLATFORM_CURL) && defined(TH8_ENABLE_LIBCURL)
#  define TH8_PLATFORM_CURL 1
#endif

/*
 *----------------------------------------------------------------------
 *
 * Defensive coding macros --
 *
 *	ALWAYS(X) surrounds boolean expressions that are intended to
 *	always be true.  NEVER(X) surrounds expressions that are
 *	intended to always be false.  These guard against unexpected
 *	behavior (e.g. a malloc that "can't fail" returning NULL)
 *	and make the code self-healing rather than brittle.
 *
 *	In coverage testing, ALWAYS/NEVER are hard-coded to 1/0
 *	so the unreachable defensive branches are not counted as
 *	untested code.  In debug builds, they assert on violation.
 *	In release builds, they pass the expression through.
 *
 *	Adapted from the SQLite source tree.
 *
 *----------------------------------------------------------------------
 */

#if defined(TH8_OMIT_AUXILIARY_SAFETY_CHECKS)
#  define ALWAYS(X) (1)
#  define NEVER(X)  (0)
#elif defined(TH8_DEBUG)
#  include <assert.h>
#  define ALWAYS(X) ((X) ? 1 : (assert(0), 0))
#  define NEVER(X)  ((X) ? (assert(0), 1) : 0)
#else
#  define ALWAYS(X) (X)
#  define NEVER(X)  (X)
#endif

/*
 *----------------------------------------------------------------------
 *
 * Integer types --
 *
 *	th8_int64_t:  signed 64-bit integer.
 *	th8_uint64_t: unsigned 64-bit integer.
 *
 *	These are TH8's own typedefs to avoid depending on C99's
 *	<stdint.h> or on `long long` being exactly 64 bits.
 *
 *----------------------------------------------------------------------
 */

#if defined(_MSC_VER)
typedef __int64 th8_int64_t;
typedef unsigned __int64 th8_uint64_t;
#elif defined(__GNUC__) || defined(__clang__)
typedef long long th8_int64_t;
typedef unsigned long long th8_uint64_t;
#else
typedef long long th8_int64_t;
typedef unsigned long long th8_uint64_t;
#endif

/*
 * 64-bit integer limits.  Defined here rather than relying on
 * <limits.h> LLONG_MIN/MAX which may not exist on all compilers.
 */

#define TH8_INT64_MAX  ((th8_int64_t)0x7fffffffffffffff)
#define TH8_INT64_MIN  ((th8_int64_t)(-TH8_INT64_MAX - 1))
#define TH8_UINT64_MAX ((th8_uint64_t)0xffffffffffffffff)

/*
 * Pointer-width integer conversion macros.
 *
 * NRE callback slots (pData[0..3]) are void* pointers.  When storing
 * integer values in these slots, the cast must go through an integer
 * type that matches the pointer width to avoid truncation warnings on
 * 32-bit platforms.  These macros use size_t (unsigned, pointer-width)
 * as the intermediate type.
 */

#define TH8_INT2PTR(v) ((void *)(size_t)(v))
#define TH8_PTR2INT(p) ((th8_int64_t)(size_t)(p))

/*
 * Platform-portable path utilities.
 *
 * TH8_IS_SEP(c) -- test whether a character is a path separator.
 *    On Win32: both '/' and '\\' are separators.
 *    On POSIX/Cosmopolitan: only '/' is a separator.
 *
 * TH8_PATH_CMP(a, b, n) -- compare two path strings of length n.
 *    On Win32: case-insensitive (_strnicmp).
 *    On POSIX/Cosmopolitan: case-sensitive (memcmp).
 */

#if (defined(_WIN32) || defined(WIN32)) && !defined(__COSMOPOLITAN__)
#  define TH8_IS_SEP(c)         ((c) == '/' || (c) == '\\')
#  define TH8_PATH_CMP(a, b, n) _strnicmp((a), (b), (n))
#else
#  define TH8_IS_SEP(c)         ((c) == '/')
/*
 * AUDIT-OK[direct-libc-memcmp]: low-level path-byte compare.
 * Defined here as a parallel macro to the case-insensitive
 * _strnicmp branch above; the macro IS the platform-neutral
 * abstraction over byte equality, so calling memcmp inside the
 * macro body is the canonical implementation, not a bypass.
 */
#  define TH8_PATH_CMP(a, b, n) memcmp((a), (b), (n))
#endif

/*
 *----------------------------------------------------------------------
 *
 * Taint system macros and string length limits --
 *
 *	TH8 limits strings to TH8_MX_STRLEN bytes (100 MB).  The
 *	top bit is the sign bit.  The next three bits are reserved;
 *	one of those (the 0x10000000 bit) marks tainted strings.
 *
 *	TH8_LEN_MASK --
 *		Bitmask for extracting the raw byte length from a
 *		size_t that may have taint/reserved bits set.  This
 *		is always 0x0FFFFFFF (the low 28 bits).
 *
 *	TH8_MX_STRLEN --
 *		Maximum byte length of any string value.  Also the
 *		default result size limit.  Any operation that would
 *		produce a longer string returns TH8_ERROR.
 *		Currently set to 100 MB (104,857,600 bytes).
 *
 *	TH8_TAINT_BIT --
 *		Bit mask stored in the high bits of a size_t length
 *		field.  When set, the string is "tainted" (originated
 *		from an untrusted source).  Taint propagates through
 *		concatenation, substitution, and most string operations.
 *
 *	TH8_NOLEN --
 *		Sentinel value ((size_t)-1) passed as a length parameter
 *		to indicate "compute the length from the NUL terminator".
 *		All public API functions accepting a size_t length
 *		recognize this sentinel and call Th8_Strlen() internally.
 *		The caller must ensure the string is NUL-terminated when
 *		passing TH8_NOLEN.
 *
 *	TH8_LEN(X) --
 *		Extracts the raw byte length from a size_t that may have
 *		taint bits set, masking off the taint and reserved bits.
 *
 *	TH8_TAINTED(X) --
 *		Returns non-zero if the taint bit is set in X.
 *
 *	TH8_RM_TAINT(X) --
 *		Returns X with the taint bit cleared.
 *
 *	TH8_ADD_TAINT(X) --
 *		Returns X with the taint bit set.
 *
 *	TH8_XFER_TAINT(A,B) --
 *		Propagates taint from B into A.  If B is tainted, A
 *		becomes tainted; otherwise A is unchanged.  This is
 *		the standard taint-propagation idiom: the output
 *		inherits taint from the input.
 *
 *	TH8_SIZECHECK(I,N) --
 *		Guard macro.  If N exceeds TH8_MX_STRLEN, calls
 *		th8OversizeString(I) to signal the oversize condition.
 *		Typically used at the entry point of functions that
 *		allocate or manipulate string buffers.
 *
 *----------------------------------------------------------------------
 */

#define TH8_LEN_MASK          (0x0fffffff)
#define TH8_MX_STRLEN         (100 * 1024 * 1024)
#define TH8_MX_ALLOC          (256 * 1024 * 1024)
#define TH8_MAX_LIST_ELEMENTS (1000000)
#define TH8_TAINT_BIT         (0x10000000)
#define TH8_NOLEN             ((size_t)-1)

#define TH8_LEN(X) ((size_t)((X) & TH8_LEN_MASK))

#define TH8_TAINTED(X)       (((X) & TH8_TAINT_BIT) != 0)
#define TH8_RM_TAINT(X)      ((X) & ~TH8_TAINT_BIT)
#define TH8_ADD_TAINT(X)     ((X) | TH8_TAINT_BIT)
#define TH8_XFER_TAINT(A, B) (A) |= (TH8_TAINT_BIT & (B))

#define TH8_SIZECHECK(I, N)                                                  \
    if ((N) > TH8_MX_STRLEN) {                                               \
	th8OversizeString((I));                                              \
    }

/*
 *----------------------------------------------------------------------
 *
 * Value / Token type codes --
 *
 *	Every value in TH8 is a Th8_Value.  When eType is
 *	TH8_VALUE_STRING (0), the value is a plain runtime string
 *	with no source provenance.  When eType is a TH8_TOKEN_*
 *	code, the value carries source position information and may
 *	have children forming a parse tree.
 *
 *	TH8_VALUE_NONE    -- There is no data value.
 *	TH8_VALUE_STRING  -- Plain string value (no source info).
 *	TH8_TOKEN_WORD    -- One word of a command; children are the
 *	                     constituent parts (text, backslash subs,
 *	                     command subs, variable subs).
 *	TH8_TOKEN_SIMPLE_WORD -- A word consisting of a single TEXT
 *	                     token (no substitutions required).
 *	TH8_TOKEN_TEXT    -- Literal text fragment within a word.
 *	TH8_TOKEN_BS      -- Backslash substitution sequence.
 *	TH8_TOKEN_COMMAND -- Command substitution [cmd ...].
 *	TH8_TOKEN_VARIABLE -- Variable substitution $name or
 *	                     ${name} or $name(index).
 *	TH8_TOKEN_SUB_EXPR -- Sub-expression within an expr parse.
 *	TH8_TOKEN_OPERATOR -- Operator within an expr parse.
 *	TH8_TOKEN_FUNCTION -- Function call within an expr parse.
 *	TH8_TOKEN_COMMENT  -- Comment preceding a command.
 *
 *----------------------------------------------------------------------
 */

#define TH8_VALUE_NONE        (0) /* Reserved (zero = uninitialized). */
#define TH8_VALUE_STRING      (1) /* Plain runtime string value. */
#define TH8_TOKEN_WORD        (2) /* Composite word (needs substitution). */
#define TH8_TOKEN_SIMPLE_WORD (3) /* Literal word (no substitution). */
#define TH8_TOKEN_TEXT        (4) /* Literal text fragment. */
#define TH8_TOKEN_BS          (5) /* Backslash-substituted sequence. */
#define TH8_TOKEN_COMMAND     (6) /* [command] substitution. */
#define TH8_TOKEN_VARIABLE    (7) /* $variable substitution. */
#define TH8_TOKEN_SUB_EXPR    (8) /* Sub-expression in expr parse. */
#define TH8_TOKEN_OPERATOR    (9) /* Operator in expr parse. */
#define TH8_TOKEN_FUNCTION    (10) /* Function call in expr parse. */
#define TH8_TOKEN_COMMENT     (11) /* Comment preceding a command. */

/*
 *----------------------------------------------------------------------
 *
 * TH8_CACHE_* --
 *
 *	Cache type codes for the internal-representation cache.
 *	Each code identifies the kind of conversion that was cached
 *	so the same string can have independent cache entries for
 *	different uses (e.g. as a number vs. as a list).
 *
 *----------------------------------------------------------------------
 */

#define TH8_CACHE_INT     (1) /* Plain int conversion. */
#define TH8_CACHE_WIDE    (2) /* 64-bit integer conversion. */
#define TH8_CACHE_DOUBLE  (3) /* Double conversion. */
#define TH8_CACHE_BIGINT  (4) /* Arbitrary-precision integer. */
#define TH8_CACHE_BOOL    (5) /* Boolean conversion. */
#define TH8_CACHE_SUBEXPR (6) /* Sub-expression (reserved). */
#define TH8_CACHE_STRING  (7) /* List-to-string joining. */
#define TH8_CACHE_LIST    (8) /* List-from-string splitting. */
#define TH8_CACHE_COMMAND (9) /* Command name resolution. */
#define TH8_CACHE_DICT    (10) /* Dict-from-string splitting. */
#define TH8_CACHE_BUFFER  (11) /* Mutable byte buffer. */

/*
 *----------------------------------------------------------------------
 *
 * Th8_Value --
 *
 *	Unified value / token structure.  The "Tcl value" and the
 *	"Tcl token" are the same struct.  Every value carries optional
 *	source provenance; every token is simultaneously a value.
 *
 *	The common fields (eType, zData, nData) are always present.
 *	The union provides type-specific storage without inflating
 *	the struct for sub-types that don't need all fields:
 *
 *	  token     - source position and child array (parse tree)
 *	  integer   - cached int representation
 *	  wide      - cached 64-bit integer representation
 *	  real      - cached double representation
 *	  bigint    - cached arbitrary-precision integer (opaque)
 *	  list      - cached Th8_Value element array
 *	  splitlist - cached Th8_SplitList output arrays
 *	  command   - cached resolved command pointer
 *
 *	Only one union member is active at a time; the iValid flag
 *	(present in every member except token) indicates whether
 *	the cached representation is current.
 *
 *----------------------------------------------------------------------
 */

/*
 * Th8_Bigint --
 *
 *	Opaque type for an arbitrary-precision integer.  Defined
 *	internally as a wrapper around libtommath's mp_int when
 *	TH8_ENABLE_BIGINT is set.  Extensions see only the opaque
 *	pointer and use th8Bigint* functions to operate on it.
 */
typedef struct Th8_Bigint Th8_Bigint;

typedef struct Th8_Value Th8_Value;
struct Th8_Value {
    th8_int64_t nVersion; /* Struct version (must be first). */
    int eType;   /* TH8_VALUE_STRING or TH8_TOKEN_* */
    const char *zData;  /* String value or source text ptr */
    size_t nData;  /* Byte length (taint in high bits) */
    union {
	struct {
	    int nLine;  /* 1-based source line */
	    int nCol;  /* 0-based byte column */
	    int nChild;  /* Number of child values */
	    Th8_Value *aChild; /* Array of children */
	} token;
	struct {
	    int iValue;  /* Cached int value. */
	    int iValid;  /* Non-zero if cache is current. */
	} integer;
	struct {
	    th8_int64_t iValue; /* Cached 64-bit integer value. */
	    int iValid;  /* Non-zero if cache is current. */
	} wide;
	struct {
	    double rValue; /* Cached double value. */
	    int iValid;  /* Non-zero if cache is current. */
	} real;
	struct {
	    int bValue;  /* Cached boolean (0 or 1). */
	    int iValid;  /* Non-zero if cache is current. */
	} boolean;
	struct {
	    Th8_Bigint *pBigint;/* Bigint handle (cache-owned). */
	    int iValid;  /* Non-zero if cache is current. */
	} bigint;
	struct {
	    Th8_Value **apElem; /* Cached element array. */
	    int nElem;  /* Number of elements. */
	    int iValid;  /* Non-zero if cache is current. */
	} list;
	struct {
	    char **azElem; /* Element string pointers. */
	    size_t *anElem; /* Element byte lengths. */
	    int nElem;  /* Number of elements. */
	    int iValid;  /* Non-zero if cache is current. */
	} splitlist;
	struct {
	    void *pCommand; /* Resolved Th8_Command pointer. */
	    int iValid;  /* Non-zero if cache is current. */
	} command;
	struct {
	    void *pBuffer; /* Pointer to start of buffer. */
	    size_t nUsed; /* Total bytes actually used. */
	    size_t nCapacity; /* Total bytes of capacity. */
	} buffer;
    } u;
};

/*
 *----------------------------------------------------------------------
 *
 * Th8_Parse --
 *
 *	Represents one parsed command.  Produced incrementally by
 *	th8ParseCommand().
 *
 *----------------------------------------------------------------------
 */

typedef struct Th8_Parse Th8_Parse;
struct Th8_Parse {
    th8_int64_t nVersion; /* Struct version (must be first). */
    const char *zCommand; /* Start of the parsed command */
    size_t nCommand;  /* Bytes in command (incl. term.) */
    int nWord;   /* Number of words in the command */
    Th8_Value *aWord;  /* Array of TH8_TOKEN_WORD values */
    int nLine;   /* Line where command begins */
    const char *zAfter;  /* First byte after parsed command */
    size_t nComment;  /* Bytes in leading comment (0=none) */
    const char *zComment; /* Comment text (NULL if none) */
};

/*
 *----------------------------------------------------------------------
 *
 * Th8_Mutex --
 *
 *	Dual-mode mutex type for the platform callback signatures.
 *
 *	Implementation files (th8_core.c, th8_plat.c, etc.) include
 *	th8_plat.h BEFORE th8.h.  This defines TH8_PLAT_H and
 *	provides the concrete Th8_PlatformMutex typedef (which is
 *	pthread_mutex_t on POSIX, an inline CRITICAL_SECTION layout
 *	on Win32, etc.).  In this mode, Th8_Mutex resolves to
 *	Th8_PlatformMutex, giving the implementation full type safety.
 *
 *	Embedders and the amalgamation include ONLY th8.h (without
 *	th8_plat.h).  In this mode, Th8_Mutex resolves to void,
 *	making the mutex callbacks accept void* pointers.  This
 *	avoids pulling <pthread.h> or <windows.h> into the public
 *	header, keeping th8.h free of platform-specific system
 *	headers.
 *
 *	The callbacks (xMutexNew, xMutexFree, xMutexEnter,
 *	xMutexLeave) operate on Th8_Mutex* regardless of mode.
 *	The embedder casts void* to their concrete mutex type
 *	inside their callback implementations.
 *
 *----------------------------------------------------------------------
 */

#if defined(TH8_PLAT_H)
#  define Th8_Mutex Th8_PlatformMutex
#else
#  define Th8_Mutex void
#endif

/*
 * TH8_TRACE_ERR -- Debug-only trace macro.  Emits file, line, message,
 * and the platform error code via Th8_EmitTrace + Th8_GetLastError.
 * Uses only public API functions.  Expands to nothing in release builds.
 */

#ifdef TH8_DEBUG
#  define TH8_TRACE_ERR(interp, msg)                                         \
      Th8_EmitTrace(                                                         \
	  (Th8_Interp *)(interp), "%s:%d: %s: %s (os_error=%d)\n", __FILE__, \
	  __LINE__, __func__, (msg), Th8_GetLastError((void *)(interp)))
#else
#  define TH8_TRACE_ERR(interp, msg) ((void)(interp), (void)(msg))
#endif

/*
 *----------------------------------------------------------------------
 *
 * Th8_Platform --
 *
 *	Platform abstraction table.  All interactions between TH8 and
 *	the operating system or host environment go through this struct.
 *	TH8 calls no C standard library functions directly; every
 *	external dependency is mediated by a callback slot here.
 *
 *	The embedder fills in this struct and passes it to
 *	Th8_CreateInterp().  NULL slots disable the corresponding
 *	functionality (unless otherwise noted below).  Helper
 *	functions Th8_GetPosixPlatform(), Th8_GetWin32Platform(),
 *	and Th8_GetLibcPlatform() return pre-filled tables for
 *	common environments; use Th8_MergePlatform() to combine
 *	partial tables.
 *
 *----------------------------------------------------------------------
 */

/* Forward declaration for use in callback signatures. */
typedef struct Th8_Interp Th8_Interp;
typedef struct Th8_Platform Th8_Platform;

/*
 * Th8_DnsResult --
 *	DNSSEC-validated DNS lookup answer.  Public shape so
 *	xDnsResolve / xDnsResolveFree platform callbacks and
 *	the curl integration can share a common type.  pData
 *	is an array of `nRecord` answer-record byte arrays;
 *	pLen is the matching array of lengths (in bytes).  For
 *	A records the length is always 4; for AAAA always 16.
 *	bogus is non-zero iff DNSSEC validation rejected the
 *	answer as untrustworthy.
 */
typedef struct Th8_DnsResult {
    int bogus;
    int nRecord;
    const unsigned char **pData;
    const size_t *pLen;
} Th8_DnsResult;

#define TH8_DNS_TYPE_A    (1)
#define TH8_DNS_TYPE_AAAA (28)

struct Th8_Platform {
    th8_int64_t nVersion; /* Struct version (must be first). */

    /*
     *------------------------------------------------------------------
     * Lifecycle callbacks
     *------------------------------------------------------------------
     */

    /*
     * xInitialize --
     *	Called once before the platform is used.  The platform
     *	should create any internal state it needs (mutexes, heaps,
     *	etc.).  Returns TH8_OK on success, TH8_ERROR on failure.
     *	If NULL, no initialization is performed.
     */
    int (*xInitialize)(Th8_Interp *interp, void *pCtx);

    /*
     * xFinalize --
     *	Called once when the platform is no longer needed.  The
     *	platform should destroy any internal state created by
     *	xInitialize.  If NULL, no finalization is performed.
     */
    void (*xFinalize)(Th8_Interp *interp, void *pCtx);

    /*
     * xPreDeleteInterp --
     *	Called by Th8_DeleteInterp at the START of cleanup, while the
     *	interpreter is still fully functional (global namespace,
     *	commands, math funcs, package registry, etc. are all intact).
     *	Use this to invoke library _Unload entry points so that
     *	libraries can clean up their interpreter-resident state via
     *	Th8_Eval, Th8_DeleteMathFunc, namespace deletion, etc., BEFORE
     *	the core teardown frees those resources.  Do NOT release the
     *	platform-level handle (e.g. dlclose) here -- the namespace
     *	cleanup that follows may still call xDel functions that live
     *	in the library's text segment.  If NULL, no action is taken.
     */
    void (*xPreDeleteInterp)(Th8_Interp *interp, void *pCtx);

    /*
     * xDeleteInterp --
     *	Called by Th8_DeleteInterp at the END of cleanup, just before
     *	the interpreter struct is freed.  At this point the namespace
     *	tree, packages, math funcs, and most other interp state have
     *	been freed; any library xDel callbacks have run.  Use this to
     *	release platform-level handles (e.g. dlclose loaded libraries)
     *	and free platform-specific per-interpreter state.  If NULL,
     *	no action is taken.
     */
    void (*xDeleteInterp)(Th8_Interp *interp, void *pCtx);

    /*
     *------------------------------------------------------------------
     * Memory allocation callbacks
     *------------------------------------------------------------------
     */

    /*
     * xMalloc --
     *	Allocate nByte bytes of memory.  The returned memory MUST be
     *	zero-filled (like calloc).  Returns NULL on allocation failure,
     *	which causes the interpreter to return TH8_ERROR.  Must not be
     *	NULL; the interpreter cannot function without a memory allocator.
     */
    void *(*xMalloc)(Th8_Interp *interp, void *pCtx, size_t nByte);

    /*
     * xRealloc --
     *	Resize a previously allocated block to nByte bytes.  May move the
     *	block.  If p is NULL, behaves like xMalloc.  Returns NULL on
     *	failure.  Must not be NULL.
     */
    void *(*xRealloc)(Th8_Interp *interp, void *pCtx, void *p, size_t nByte);

    /*
     * xFree --
     *	Free a block previously returned by xMalloc or xRealloc.  If p
     *	is NULL, does nothing.  Must not be NULL.
     */
    void (*xFree)(Th8_Interp *interp, void *pCtx, void *p);

    /*
     * xMemorySize --
     *	Return the usable size of an allocated block.  Used by
     *	Th8_Free to accurately track memory usage (decrementing
     *	nAllocBytes).  Must not be NULL.
     *
     *	Implemented via malloc_size (macOS), malloc_usable_size
     *	(Linux/BSD), or _msize (Windows).
     */
    size_t (*xMemorySize)(Th8_Interp *interp, void *pCtx, void *p);

    /*
     * xNeedMemory --
     *	Second-chance allocator.  Called when xMalloc returns NULL
     *	and the interpreter is in a recoverable state.  The embedder
     *	may free caches, compact memory, or try an alternative
     *	allocator.  If a valid block is returned, it MUST be at
     *	least nByte bytes and MUST be freeable via xFree.  May
     *	return NULL (allocation truly failed).  Optional: if NULL,
     *	normal OOM handling applies.
     */
    void *(*xNeedMemory)(Th8_Interp *interp, size_t nByte);

    /*
     *------------------------------------------------------------------
     * Byte operations
     *
     *	These provide the low-level byte operations the interpreter
     *	needs internally.  They have the same semantics as their C
     *	standard library counterparts.  Must not be NULL.
     *------------------------------------------------------------------
     */

    /*
     * xMemcpy --
     *	Copy n bytes from src to dst.  The source and destination must
     *	not overlap (use xMemmove for overlapping regions).  Called
     *	extensively by the evaluator and string operations.
     */
    void *(*xMemcpy)(
        Th8_Interp *interp,
        void *pCtx,
        void *dst,
        const void *src,
        size_t n);

    /*
     * xMemmove --
     *	Copy n bytes from src to dst.  Unlike xMemcpy, the source and
     *	destination may overlap.  Called during in-place string
     *	manipulations (e.g., list element removal).
     */
    void *(*xMemmove)(
        Th8_Interp *interp,
        void *pCtx,
        void *dst,
        const void *src,
        size_t n);

    /*
     * xMemset --
     *	Fill n bytes of dst with the byte value c.  Called to
     *	zero-initialize structures and clear buffers.
     */
    void *(
        *xMemset)(Th8_Interp *interp, void *pCtx, void *dst, int c, size_t n);

    /*
     * xMemcmp --
     *	Compare n bytes of a and b.  Returns <0, 0, or >0 as in
     *	memcmp().  Called by string comparison, hash lookup, and
     *	variable resolution.
     */
    int (*xMemcmp)(
        Th8_Interp *interp,
        void *pCtx,
        const void *a,
        const void *b,
        size_t n);

    /*
     *------------------------------------------------------------------
     * String / utility callbacks
     *
     *	These are higher-level CRT functions used by auxiliary modules
     *	such as the regex engine.  They are NOT required by the core
     *	interpreter.  If NULL, the corresponding operations are
     *	unavailable and modules requiring them will return errors.
     *
     *	Embedders who want full CRT routing through the platform
     *	layer should fill these in; otherwise they can be left NULL.
     *------------------------------------------------------------------
     */

    /*
     * xStrlen --
     *	Return the length of the NUL-terminated string s, as strlen().
     *	If NULL, regex and other modules needing strlen are unavailable.
     */
    size_t (*xStrlen)(Th8_Interp *interp, void *pCtx, const char *s);

    /*
     * xStrcmp --
     *	Compare two NUL-terminated strings, as strcmp().
     *	If NULL, operations requiring strcmp are unavailable.
     */
    int (*xStrcmp)(
        Th8_Interp *interp,
        void *pCtx,
        const char *s1,
        const char *s2);

    /*
     * xStrchr --
     *	Locate the first occurrence of byte c in NUL-terminated string s,
     *	as strchr().  If NULL, operations requiring strchr are unavailable.
     */
    char *(*xStrchr)(Th8_Interp *interp, void *pCtx, const char *s, int c);

    /*
     * xAtoi --
     *	Convert a NUL-terminated decimal string to int, as atoi().
     *	If NULL, operations requiring atoi are unavailable.
     */
    int (*xAtoi)(Th8_Interp *interp, void *pCtx, const char *s);

    /*
     * xQsort --
     *	Sort an array in place, as qsort().  Used by [lsort] when a
     *	fast native sort is available.
     *	If NULL, [lsort] falls back to the interpreter's built-in sort.
     */
    void (*xQsort)(
        Th8_Interp *interp,
        void *pCtx,
        void *base,
        size_t nmemb,
        size_t size,
        int (*cmp)(const void *, const void *));

    /*
     * xVsnprintf --
     *	Formatted output into a buffer, as vsnprintf().  Used for
     *	[format]-style operations and diagnostic messages.
     *	If NULL, [format] and similar commands are unavailable.
     */
    int (*xVsnprintf)(
        Th8_Interp *interp,
        void *pCtx,
        char *buf,
        size_t size,
        const char *fmt,
        va_list ap);

    /*
     *------------------------------------------------------------------
     * Threading callbacks
     *------------------------------------------------------------------
     *
     * These callbacks manage an in-place Th8_Mutex.  They are used
     * by Th8_Initialize/Th8_Finalize and by platform code to protect
     * process-global state (e.g., the loaded-library handle list).
     *
     * xMutexInit initializes a pre-allocated Th8_Mutex in-place
     * (like InitializeCriticalSection / pthread_mutex_init).
     * xMutexFinal destroys a mutex.
     * xMutexEnter acquires the mutex (blocking).
     * xMutexLeave releases the mutex.
     *
     * If all four are NULL, TH8 operates without locking
     * (single-threaded mode).
     */
    void (*xMutexInit)(Th8_Interp *interp, void *pCtx, Th8_Mutex *pMutex);
    void (*xMutexFinal)(Th8_Interp *interp, void *pCtx, Th8_Mutex *pMutex);
    void (*xMutexEnter)(Th8_Interp *interp, void *pCtx, Th8_Mutex *pMutex);
    void (*xMutexLeave)(Th8_Interp *interp, void *pCtx, Th8_Mutex *pMutex);

    /*
     * xIntCmpXchg --
     *	Atomic integer compare-and-exchange.  Atomically compares
     *	*pTarget with iComparand; if equal, stores iExchange in
     *	*pTarget.  Returns the original value of *pTarget.
     *	Used to protect platform initialization flags.
     *	If NULL, no atomic operation is performed (single-threaded
     *	assumption).
     */
    int (*xIntCmpXchg)(
        Th8_Interp *interp,
        void *pCtx,
        volatile int *pTarget,
        int iExchange,
        int iComparand);

    /*
     * xMemBarrier --
     *	Full memory barrier / fence.  Ensures that all memory
     *	writes issued before the barrier are visible to other
     *	threads before any reads or writes issued after the
     *	barrier.  Used after volatile flag writes (e.g.,
     *	bCanceled, th8HashSeeded) to guarantee cross-thread
     *	visibility.  On Win32 this maps to MemoryBarrier();
     *	on GCC/Clang to __sync_synchronize().  If NULL,
     *	no barrier is performed (single-threaded assumption).
     */
    void (*xMemBarrier)(Th8_Interp *interp, void *pCtx);

    /*
     * xEventCreate / xEventDestroy / xEventSet / xEventReset /
     * xEventWait --
     *
     *	Manual-reset event handle, used by the per-interp event
     *	queue (Th8_QueueEvent) and the script-level [update] /
     *	[vwait] commands.  An event handle has two states:
     *	signaled and nonsignaled.  Set transitions to signaled
     *	(all subsequent waits return immediately); Reset
     *	transitions to nonsignaled (subsequent waits block);
     *	Wait blocks until signaled, the timeout expires, or an
     *	early-wake condition fires.
     *
     *	Win32 implementation: CreateEvent (manual-reset) /
     *	SetEvent / ResetEvent / WaitForSingleObjectEx.  The
     *	wait MUST be alertable (bAlertable=TRUE) so that
     *	QueueUserAPC fires while [vwait] is sleeping.
     *
     *	POSIX implementation: pthread_mutex + pthread_cond +
     *	a "signaled" int flag in the platform-allocated
     *	struct.  The wait returns early on signal delivery
     *	(EINTR / spurious wake from pthread_kill) -- this is
     *	the practical analog of Win32 APC delivery.
     *
     *	xEventWait return values use the TH8_WAIT_* constants:
     *	   TH8_WAIT_OK       (0)  signal observed
     *	   TH8_WAIT_RETRY   (-1)  early wake (WAIT_IO_COMPLETION on
     *	                          Win32; signal delivery on POSIX) --
     *	                          caller treats as "re-poll"
     *	   TH8_WAIT_ERROR    (1)  wait failed (handle invalid, etc.)
     *	   TH8_WAIT_TIMEOUT  (2)  timeout expired
     *
     *	Negative nTimeoutMs means infinite wait.
     *
     *	All five callbacks must be supplied for the event queue
     *	to function.  Th8_CreateAsyncState validates the full
     *	set up front and fails the create call if any required
     *	callback is missing -- there is no fallback path.
     */
    void *(*xEventCreate)(Th8_Interp *interp, void *pCtx);
    void (*xEventDestroy)(Th8_Interp *interp, void *pCtx, void *pEvent);
    void (*xEventSet)(Th8_Interp *interp, void *pCtx, void *pEvent);
    void (*xEventReset)(Th8_Interp *interp, void *pCtx, void *pEvent);
    int (*xEventWait)(
        Th8_Interp *interp,
        void *pCtx,
        void *pEvent,
        int nTimeoutMs);

    /*
     *------------------------------------------------------------------
     * I/O core callbacks
     *------------------------------------------------------------------
     */

    /*
     * xInput --
     *	Read input from the host (e.g., stdin).  On success, sets
     *	*pzOut and *pnOut to a newly-allocated buffer containing the
     *	available data and returns TH8_OK.  Returns TH8_ERROR on
     *	EOF or failure.  This is the inverse of xOutput.  pCtx is
     *	the platform's pCtx field.
     *	If NULL, the [gets] command and similar input operations
     *	are unavailable.
     */
    int (*xInput)(
        Th8_Interp *interp,
        void *pCtx,
        char **pzOut,
        size_t *pnOut,
        void *pChannel);

    /*
     * xOutput --
     *	Write z (n bytes) to the host output channel identified by
     *	pChannel.  pChannel is NULL for the default stdout channel.
     *	Returns TH8_OK on success, TH8_ERROR on failure.
     *	If NULL, [puts] and similar output operations are unavailable.
     */
    int (*xOutput)(
        Th8_Interp *interp,
        void *pCtx,
        const char *z,
        size_t n,
        void *pChannel);

    /*
     * xOutputError --
     *	Write z (n bytes) to the host error channel identified by
     *	pChannel.  pChannel is NULL for the default stderr channel.
     *	Same contract as xOutput but targets the error stream.
     *	If NULL, error output is silently discarded.
     */
    int (*xOutputError)(
        Th8_Interp *interp,
        void *pCtx,
        const char *z,
        size_t n,
        void *pChannel);

    /*
     *------------------------------------------------------------------
     * I/O channel redirection
     *------------------------------------------------------------------
     *
     * These callbacks get and set the opaque channel pointers used
     * by xInput, xOutput, and xOutputError.  NULL means "use the
     * default channel" (stdin, stdout, stderr respectively).
     *
     * The interp pointer is provided so that implementations can
     * maintain per-interpreter channel state if desired.
     */

    /*
     * xGetInput / xSetInput --
     *	Get or set the input channel for xInput (e.g., FILE* on
     *	POSIX, HANDLE on Win32).  NULL resets to default (stdin).
     */
    int (*xGetInput)(Th8_Interp *interp, void *pCtx, void **pChannel);
    int (*xSetInput)(Th8_Interp *interp, void *pCtx, void *pChannel);

    /*
     * xGetOutput / xSetOutput --
     *	Get or set the output channel for xOutput.  NULL resets
     *	to default (stdout).
     */
    int (*xGetOutput)(Th8_Interp *interp, void *pCtx, void **pChannel);
    int (*xSetOutput)(Th8_Interp *interp, void *pCtx, void *pChannel);

    /*
     * xGetErrorOutput / xSetErrorOutput --
     *	Get or set the error output channel for xOutputError.
     *	NULL resets to default (stderr).
     */
    int (*xGetErrorOutput)(Th8_Interp *interp, void *pCtx, void **pChannel);
    int (*xSetErrorOutput)(Th8_Interp *interp, void *pCtx, void *pChannel);

    /*
     *------------------------------------------------------------------
     * Channel / temporary I/O callbacks
     *------------------------------------------------------------------
     */

    /*
     * xChannelControl --
     *	Perform I/O operations on a channel handle returned by
     *	xGetTemporaryData.  The `op` parameter selects the
     *	operation (TH8_CHANCTL_*):
     *
     *	  SEEK  (0) -- nArg1=offset, nArg2=whence (0/1/2).
     *	  TELL  (1) -- *pnResult = current position.
     *	  FLUSH (2) -- flush pending writes.
     *	  CLOSE (3) -- close the handle.
     *	  WRITE (4) -- write pBuf[0..nArg1-1] to channel.
     *	  READ  (5) -- read up to nArg1 bytes into pBuf.
     *	               *pnResult = bytes actually read.
     *	  OPEN  (6) -- open file at path pBuf (nArg1 bytes).
     *	               nArg2 = mode (0=read, 1=write-create).
     *	               *pnResult = opaque handle (use
     *	               TH8_INT2PTR to recover as void*).
     *
     *	pBuf is used by WRITE, READ, OPEN; NULL for other ops.
     *	If NULL, channel operations are unavailable.
     */
    int (*xChannelControl)(
        Th8_Interp *interp,
        void *pCtx,
        void *pChannel,
        int op,
        th8_int64_t nArg1,
        int nArg2,
        th8_int64_t *pnResult,
        void *pBuf);

    /*
     * xGetTemporaryData --
     *	Request a temporary file of at least `nSize` bytes.  The
     *	platform creates and pre-allocates the file (contents
     *	zero-filled), opens it for read/write, and returns:
     *	  - The OS file path in *pzOut (Th8_Malloc'd; caller frees)
     *	  - An opaque channel handle in *ppChannel, suitable for
     *	    passing to xInput/xOutput as the pChannel argument.
     *
     *	The platform may impose limits on total files, max size,
     *	or directory.
     *
     *	pCtx1 is the Th8_Interp* (for Th8_Malloc).
     *	pCtx2 is the platform's pCtx.
     *
     *	Returns TH8_OK on success, TH8_ERROR on failure.
     *	If NULL, [file tempname] is unavailable.
     */
    int (*xGetTemporaryData)(
        Th8_Interp *interp,
        void *pCtx,
        size_t nSize,
        char **pzOut,
        size_t *pnOut,
        void **ppChannel);

    /*
     * xDeleteTemporaryData --
     *	Delete a temporary file previously created by
     *	xGetTemporaryData.  Called during channel close
     *	and interpreter destruction.
     *
     *	pCtx1 is the Th8_Interp*.
     *	pCtx2 is the platform's pCtx.
     *
     *	Returns TH8_OK on success, TH8_ERROR on failure.
     *	If NULL, temporary files are not cleaned up.
     */
    int (*xDeleteTemporaryData)(
        Th8_Interp *interp,
        void *pCtx,
        const char *zPath,
        size_t nPath);

    /*
     * xSetTemporaryData --
     *	Called immediately after a [puts] write to a temporary
     *	channel is flushed to the file system.  Allows the
     *	platform to validate, transform, or reject the written
     *	data.  If this callback returns non-TH8_OK, the [puts]
     *	command fails with an error (even though the data was
     *	physically written).
     *
     *	zName/nName is the channel name (e.g., "./tmp/foo.tmp").
     *	nOffset is the byte offset where the write started.
     *	nLength is the number of bytes written.
     *
     *	pCtx1 is the Th8_Interp*.
     *	pCtx2 is the platform's pCtx.
     *
     *	If NULL, no post-write notification occurs.
     */
    int (*xSetTemporaryData)(
        Th8_Interp *interp,
        void *pCtx,
        const char *zName,
        size_t nName,
        th8_uint64_t nOffset,
        th8_uint64_t nLength);

    /*
     * xCloseTemporaryData --
     *	Called when a script explicitly closes a temporary channel
     *	via [close].  The platform can veto the close by returning
     *	TH8_ERROR (e.g., if the channel is still needed by the
     *	host).  On TH8_OK, the channel handle is closed via
     *	xChannelControl CLOSE and the channel is removed from
     *	the registry.
     *
     *	zName/nName is the channel name.
     *	pChannel is the opaque channel handle.
     *
     *	pCtx1 is the Th8_Interp*.
     *	pCtx2 is the platform's pCtx.
     *
     *	If NULL, explicit close is always allowed.
     */
    int (*xCloseTemporaryData)(
        Th8_Interp *interp,
        void *pCtx,
        const char *zName,
        size_t nName,
        void *pChannel);

    /*
     *------------------------------------------------------------------
     * Filesystem callbacks
     *------------------------------------------------------------------
     */

    /*
     * xNormalizePath --
     *	Return the canonical absolute path for zPath as a
     *	NUL-terminated UTF-8 string.  The result is allocated
     *	via the platform's xMalloc; the caller frees it with
     *	xFree.  Returns NULL on failure.
     *
     *	On POSIX this is implemented via realpath(3) for
     *	existing paths, or getcwd + manual "."/".." resolution
     *	for paths that do not yet exist.  On Windows it maps
     *	to GetFullPathNameW.
     *
     *	If NULL, [file normalize] falls back to pure string
     *	"."/".." resolution without making the path absolute.
     */
    char *(*xNormalizePath)(
        Th8_Interp *interp,
        void *pCtx,
        const char *zPath,
        size_t nPath);

    /*
     * xGetCwd --
     *	Return the current working directory as a NUL-terminated
     *	UTF-8 string allocated via Th8_Malloc (using the interp
     *	parameter).  The caller frees the result via Th8_Free.
     *	Returns NULL on failure.  If NULL, the current working
     *	directory is not available.
     */
    char *(*xGetCwd)(Th8_Interp *interp, void *pCtx);

    /*
     * xSetCwd --
     *	Change the current working directory.  zPath is a
     *	NUL-terminated UTF-8 string.  Returns TH8_OK on success,
     *	TH8_ERROR on failure.  If NULL, the [cd] command returns
     *	an error indicating that file system access is unavailable.
     *
     *	The security policy is that xSetCwd should only accept
     *	"." (the base directory) and reject all other paths.
     */
    int (*xSetCwd)(
        Th8_Interp *interp,
        void *pCtx,
        const char *zPath,
        size_t nPath);

    /*
     * xGetExePath --
     *	Return the full path to the currently running executable as
     *	a NUL-terminated UTF-8 string allocated via Th8_Malloc
     *	(using the interp parameter).  The caller frees the result
     *	via Th8_Free.  Returns NULL on failure.
     *
     *	Used by [info nameofexecutable] when the host has not set
     *	the ::th8_nameofexecutable variable.
     *
     *	Implementation guidance by platform:
     *	  Linux:   readlink("/proc/self/exe", ...)
     *	  macOS:   _NSGetExecutablePath() + realpath()
     *	  FreeBSD: sysctl(CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1)
     *	  Win32:   GetModuleFileNameA(NULL, ...) + resolve
     *
     *	If NULL, [info nameofexecutable] falls back to the
     *	::th8_nameofexecutable variable (which may be empty).
     */
    char *(*xGetExePath)(Th8_Interp *interp, void *pCtx);

    /*
     * xGetRealPath --
     *	Resolve a path to its canonical absolute form using the
     *	operating system's native path resolution (realpath on
     *	POSIX, GetFullPathNameA on Win32).
     *
     *	Unlike xNormalizePath, this does NOT apply base-path
     *	security restrictions.  It operates at the raw OS level
     *	and is intended for embedder use before or during
     *	initialization (e.g., resolving script file arguments
     *	before Th8_Initialize changes the working directory).
     *
     *	On success, writes the NUL-terminated absolute path into
     *	zBuf (up to nBuf bytes including NUL) and returns TH8_OK.
     *	Returns TH8_ERROR if the path cannot be resolved.
     *
     *	If NULL, path resolution is unavailable.
     */
    int (*xGetRealPath)(
        Th8_Interp *interp,
        void *pCtx,
        const char *zPath,
        size_t nPath,
        char *zBuf,
        size_t nBuf);

    /*
     * xGetRootPath --
     *	Return the filesystem root (mount point) for a given path.
     *	On POSIX this returns "/".  On Win32 this returns the
     *	volume root (e.g., "C:\\") via GetVolumePathNameA.
     *
     *	On success, writes the NUL-terminated root path into zBuf
     *	(up to nBuf bytes including NUL) and returns TH8_OK.
     *	Returns TH8_ERROR if the root cannot be determined.
     *
     *	If NULL, root path detection is unavailable.
     */
    int (*xGetRootPath)(
        Th8_Interp *interp,
        void *pCtx,
        const char *zPath,
        size_t nPath,
        char *zBuf,
        size_t nBuf);

    /*
     * xSameFile --
     *	Test whether two paths refer to the same physical file.
     *	On POSIX: stat both and compare st_dev + st_ino.
     *	On Win32: open both and compare volume serial number +
     *	file index.
     *
     *	Returns non-zero if both paths refer to the same file,
     *	zero otherwise (including when either path does not exist).
     *
     *	If NULL, same-file detection is unavailable.
     */
    int (*xSameFile)(
        Th8_Interp *interp,
        void *pCtx,
        const char *zName1,
        size_t nName1,
        const char *zName2,
        size_t nName2);

    /*
     *------------------------------------------------------------------
     * Data retrieval / binary loading callbacks
     *------------------------------------------------------------------
     */

    /*
     * xGetData --
     *	Retrieve named data from the host environment.  zName/nName
     *	identify the data (semantics are host-defined; e.g., a filename,
     *	a resource key).  On success, sets *pzOut and *pnOut to a
     *	buffer allocated via Th8_Malloc and returns TH8_OK.  Returns
     *	TH8_ERROR if the data is not found or cannot be read.
     *
     *	pCtx1 is the Th8_Interp* (cast to void*).  The callback
     *	should use Th8_Malloc(interp, n) to allocate the output
     *	buffer so it can be freed with Th8_Free by the caller.
     *
     *	pCtx2 is the platform's pCtx field, for host-specific
     *	state (e.g., a virtual filesystem handle, access control
     *	policy, etc.).
     *
     *	If NULL, [source] and similar commands are unavailable.
     */
    int (*xGetData)(
        Th8_Interp *interp,
        void *pCtx,
        const char *zName,
        size_t nName,
        char **pzOut,
        size_t *pnOut);

    /*
     * xDataExists --
     *	Test whether named data exists without retrieving it.  Returns
     *	non-zero if the data exists, zero otherwise.
     *
     *	pAttrs is an optional output pointer.  When non-NULL, the
     *	callback stores the file-type attribute (one of the
     *	TH8_FILE_ATTR_* constants).  When NULL, the callback
     *	performs a plain existence check only.
     *
     *	If NULL, existence checks always report "not found".
     */
    int (*xDataExists)(
        Th8_Interp *interp,
        void *pCtx,
        const char *zName,
        size_t nName,
        int *pAttrs);

    /*
     * xLoad --
     *	Load binary code (e.g., a shared library / DLL) into the
     *	host process.  zName/nName identify the binary to load
     *	(semantics are host-defined; e.g., a file path, a resource
     *	name, or a package identifier).  zProc/nProc, if non-NULL,
     *	is the name of the initialization entry point to call after
     *	loading (e.g., "Foo_Init").
     *
     *	pCtx1 is the Th8_Interp* (cast to void*).  The entry point
     *	is called with the interpreter so it can register commands
     *	via Th8_CreateCommand.
     *
     *	pCtx2 is the platform's pCtx field, for host-specific
     *	state (e.g., a load policy, allowed path whitelist, etc.).
     *
     *	Returns TH8_OK on success, TH8_ERROR on failure.
     *
     *	If NULL, binary loading is unavailable.
     *
     *	SECURITY NOTE: This callback can execute arbitrary native
     *	code.  It is the host's responsibility to implement
     *	appropriate access control (e.g., code signing validation,
     *	path whitelisting, sandboxing).  For safe interpreters,
     *	set this to NULL.
     */
    int (*xLoad)(
        Th8_Interp *interp,
        void *pCtx,
        const char *zName,
        size_t nName,
        const char *zProc,
        size_t nProc);

    /*
     * xUnload --
     *	Unload (or finalize) a previously loaded binary.
     *	zName/nName is the same name passed to xLoad.
     *	zProc/nProc is the unload entry point name, or
     *	NULL/0 to derive it from zName (appending "_Unload").
     *	bClose controls whether the library is actually closed:
     *	  0 = keeplibrary (call _Unload entry point only)
     *	  1 = nokeeplibrary (call _Unload AND dlclose/FreeLibrary)
     *	Returns TH8_OK on success, TH8_ERROR on failure.
     *	If NULL, the [unload] command is unavailable.
     */
    int (*xUnload)(
        Th8_Interp *interp,
        void *pCtx,
        const char *zName,
        size_t nName,
        const char *zProc,
        size_t nProc,
        int bClose);

    /*
     *------------------------------------------------------------------
     * Time callbacks
     *------------------------------------------------------------------
     */

    /*
     * xTimeMs --
     *	Store the current wall-clock time in milliseconds since the
     *	Unix epoch into *pMs.  Returns TH8_OK on success, TH8_ERROR
     *	if a clock is unavailable.  Used by [clock milliseconds] and
     *	similar commands.
     *	If NULL, time-related commands return TH8_ERROR.
     */
    int (*xTimeMs)(Th8_Interp *interp, void *pCtx, th8_int64_t *pMs);

    /*
     * xTimeUs --
     *	Store the current monotonic time in microseconds into *pUs.
     *	Used by [time] for high-precision benchmarking.  Unlike
     *	xTimeMs (which returns wall-clock time), this callback
     *	SHOULD use a monotonic clock (CLOCK_MONOTONIC,
     *	QueryPerformanceCounter) so that measurements are not
     *	affected by clock adjustments.
     *	If NULL, Th8_GetTimeUs falls back to xTimeMs * 1000.
     */
    int (*xTimeUs)(Th8_Interp *interp, void *pCtx, th8_int64_t *pUs);

    /*
     * xSleep --
     *	Sleep for the specified number of milliseconds.  The
     *	implementation SHOULD use a platform sleep that yields
     *	the CPU (nanosleep, Sleep, etc.) rather than busy-waiting.
     *	If NULL, Th8_Sleep is a no-op.
     */
    void (*xSleep)(Th8_Interp *interp, void *pCtx, int nMs);

    /*
     *------------------------------------------------------------------
     * Process / host information callbacks
     *------------------------------------------------------------------
     */

    /*
     * xGetPid --
     *	Return the current process ID (or equivalent host identifier).
     *	pCtx is the platform's pCtx field.  Returns 0 if the concept
     *	is not applicable.
     *	If NULL, [pid] returns 0.
     */
    int (*xGetPid)(Th8_Interp *interp, void *pCtx);

    /*
     * xGetUserName --
     *	Return the current user's name in zBuf (up to nBuf bytes
     *	including NUL).  Returns TH8_OK on success.  If NULL,
     *	the user name is reported as empty.
     */
    int (*xGetUserName)(
        Th8_Interp *interp,
        void *pCtx,
        char *zBuf,
        size_t nBuf);

    /*
     * xGetHostName --
     *	Return the machine hostname in zBuf (up to nBuf bytes
     *	including NUL).  Returns TH8_OK on success.  If NULL,
     *	the hostname is reported as empty.
     */
    int (*xGetHostName)(
        Th8_Interp *interp,
        void *pCtx,
        char *zBuf,
        size_t nBuf);

    /*
     * xGetEnv --
     *	Return the value of the named environment variable as a
     *	NUL-terminated UTF-8 string allocated via the platform's
     *	xMalloc (using the interp parameter when non-NULL).  The
     *	caller frees the result via Th8_Free (or xFree when no
     *	interp is available).  Returns NULL if the variable is
     *	not set or if allocation fails.
     *
     *	On Win32, this wraps GetEnvironmentVariableW with
     *	UTF-16 to UTF-8 conversion.  On POSIX, it copies the
     *	result of getenv() into an allocated buffer.
     *
     *	If NULL, environment variable queries return NULL.
     */
    char *(*xGetEnv)(Th8_Interp *interp, void *pCtx, const char *zName);

    /*
     * xKeyValue --
     *	Perform a key-value store operation.  The `op` parameter
     *	selects the operation (TH8_KV_*):
     *
     *	  NONE     (0) -- No operation (reserved).
     *	  EXISTS   (1) -- Check if zName exists (literal).
     *	  LIST     (2) -- List keys matching zName (glob).
     *	  GET      (3) -- Get value for zName (literal).
     *	  SET      (4) -- Set zName to zValue (both literal).
     *	  UNSET    (5) -- Remove zName (literal).
     *	  EXISTS2  (6) -- Check if any key matches zName and/or
     *	                  zValue globs.
     *	  LIST2    (7) -- List keys matching zName and/or zValue
     *	                  globs.
     *	  GET2     (8) -- Return dict of key-value pairs matching
     *	                  zName and/or zValue globs.
     *	  SET2     (9) -- Set all keys matching zName glob to the
     *	                  literal zValue (NULL zName = all keys).
     *	  UNSET2  (10) -- Remove and return dict of key-value pairs
     *	                  matching zName and/or zValue globs.
     *
     *	For original ops (1-5): zName is a literal key name
     *	(except LIST where it is a glob).  zValue is unused
     *	except for SET.
     *
     *	For *2 ops (6-10): NULL zName matches all keys.  NULL
     *	zValue matches all values.  Non-NULL values are glob
     *	patterns (except SET2 where zValue is a literal).
     *
     *	Returns TH8_OK on success, TH8_ERROR on failure.
     *	If NULL, key-value operations are unavailable.
     */
    int (*xKeyValue)(
        Th8_Interp *interp,
        void *pCtx,
        int op,
        const char *zName,
        size_t nName,
        const char *zValue,
        size_t nValue);

    /*
     * xGetStackBounds --
     *	Query the native C stack bounds.  Sets *ppBase to the stack
     *	base address and *pnSize to the total stack size in bytes.
     *	Called once at interpreter creation.  pCtx is the platform's
     *	pCtx field.
     *
     *	If NULL, native stack checking is disabled and only the
     *	counter-based nEvalDepth limit is enforced.
     *
     *	Implementation guidance by platform:
     *	  Win32:      VirtualQuery on a local variable, or read TEB.
     *	  POSIX/Linux: pthread_attr_getstack or getrlimit(RLIMIT_STACK).
     *	  macOS/BSD:  pthread_get_stackaddr_np / pthread_get_stacksize_np.
     */
    int (*xGetStackBounds)(
        Th8_Interp *interp,
        void *pCtx,
        void **ppBase,
        size_t *pnSize);

    /*
     * xGetParentPid --
     *	Return the parent process ID (or equivalent host identifier).
     *	Returns 0 if the concept is not applicable.
     *	If NULL, parent PID queries return 0.
     */
    int (*xGetParentPid)(Th8_Interp *interp, void *pCtx);

    /*
     * xGetThreadId --
     *	Return the current thread ID as a 64-bit unsigned integer.
     *	Returns 0 if threading is not supported.
     *	If NULL, thread ID queries return 0.
     */
    th8_uint64_t (*xGetThreadId)(Th8_Interp *interp, void *pCtx);

    /*
     *------------------------------------------------------------------
     * Error / diagnostics callbacks
     *------------------------------------------------------------------
     */

    /*
     * xGetLastError --
     *	Return the most recent OS error code.  On Win32 this is
     *	GetLastError(); on POSIX it is errno.  Returns the
     *	platform-specific integer error code.  If NULL, returns 0.
     */
    int (*xGetLastError)(Th8_Interp *interp, void *pCtx);

    /*
     * xSetLastError --
     *	Set (or clear) the OS error code.  Typically called with 0
     *	before an operation whose error status needs checking.
     *	On Win32 this is SetLastError(); on POSIX it sets errno.
     *	If NULL, no action is taken.
     */
    void (*xSetLastError)(Th8_Interp *interp, void *pCtx, int nErr);

    /*
     * xEmitTrace --
     *	Emit a diagnostic trace message.  Used to report platform
     *	errors (e.g., failures in xInitialize).  On Win32 this
     *	calls OutputDebugStringA; on POSIX it calls syslog().
     *	zMsg is a NUL-terminated UTF-8 string.  If NULL, tracing
     *	is disabled.
     */
    void (*xEmitTrace)(Th8_Interp *interp, void *pCtx, const char *zMsg);

    /*
     * xPanic --
     *	Called on unrecoverable internal errors (e.g., failed assertion,
     *	allocation failure in a critical path).  zMsg/nMsg describe the
     *	error.  If NULL, the interpreter has no panic handler and an
     *	unrecoverable error could lead to undefined behavior; however,
     *	this will not corrupt any state outside of the current TH8
     *	interpreter in use.
     */
    void (*xPanic)(
        Th8_Interp *interp,
        void *pCtx,
        const char *zMsg,
        size_t nMsg);

    /*
     *------------------------------------------------------------------
     * Math / entropy callbacks
     *------------------------------------------------------------------
     */

    /*
     * xMathFunc --
     *	Evaluate a transcendental math operation identified by op
     *	(one of the TH8_MATH_* codes).  Stores the result in *pResult.
     *	For unary operations (sin, cos, sqrt, etc.), only parameter 'a'
     *	is used.  For binary operations (atan2, fmod, pow, hypot), both
     *	'a' and 'b' are used.  pCtx is the platform's pCtx field.
     *
     *	Returns TH8_OK on success, TH8_ERROR on domain error (e.g.,
     *	sqrt of a negative number, log of zero).
     *
     *	If NULL, all transcendental math functions in [expr] are
     *	unavailable and return TH8_ERROR.
     */
    int (*xMathFunc)(
        Th8_Interp *interp,
        void *pCtx,
        int op,
        double *pResult,
        double a,
        double b);

    /*
     * xRandomBytes --
     *	Fill pBuf with nByte cryptographically random bytes.  Returns
     *	TH8_OK on success, TH8_ERROR if randomness is unavailable.
     *	Used by [expr rand()].
     *	If NULL, random number generation is unavailable.
     */
    int (*xRandomBytes)(
        Th8_Interp *interp,
        void *pCtx,
        void *pBuf,
        size_t nByte);

    /*
     * xDnsResolve / xDnsResolveFree --
     *	DNSSEC-validating DNS resolver, used by the libcurl
     *	integration to pin IP addresses returned by a trusted
     *	(DNSSEC-validated) DNS lookup.  The default POSIX
     *	implementation delegates to libunbound; testlib and
     *	other diagnostic platforms can stub these with a
     *	fixed response for path coverage of unusual server
     *	responses (e.g. malformed A-record lengths) that no
     *	real DNS server would return.
     *
     *	xDnsResolve:  fills *ppResult with a heap-allocated
     *	  Th8_DnsResult that describes the answer.  The result
     *	  MUST be freed by a matching xDnsResolveFree call.
     *	  zName/nName: hostname to resolve.
     *	  eType: TH8_DNS_TYPE_A or TH8_DNS_TYPE_AAAA.
     *	  Returns TH8_OK on success (even if the answer was
     *	  bogus -- check pResult->bogus); TH8_ERROR if the
     *	  resolver could not be invoked at all.
     *
     *	xDnsResolveFree: release a result returned by
     *	  xDnsResolve.  Safe to call with NULL.
     *
     *	If xDnsResolve is NULL, callers must treat the lookup
     *	as failed (TH8_ERROR) and fall back to non-pinned
     *	resolution behaviour.
     */
    int (*xDnsResolve)(
        Th8_Interp *interp,
        void *pCtx,
        const char *zName,
        size_t nName,
        int eType,
        Th8_DnsResult **ppResult);
    void (*xDnsResolveFree)(
        Th8_Interp *interp,
        void *pCtx,
        Th8_DnsResult *pResult);

    /*
     *------------------------------------------------------------------
     * Host context pointer
     *------------------------------------------------------------------
     */

    /*
     * pCtx --
     *	Opaque pointer passed to all platform callbacks.  Every
     *	callback receives both pCtx and an interp pointer (as
     *	void*) for consistent access to host context and
     *	interpreter services.  The interpreter never dereferences
     *	pCtx.
     *
     *	NOTE: Th8_MergePlatform does NOT merge pCtx.  The
     *	destination platform's pCtx is always preserved unchanged.
     */
    void *pCtx;
};

/*
 * Opaque interpreter handle (forward-declared before Th8_Platform).
 */

/*
 *----------------------------------------------------------------------
 *
 * Expansion callback --
 *
 *	Registered via Th8_RegisterExpansion.  Called when the parser
 *	encounters {tag}word in a command.  The callback receives the
 *	substituted word value and must produce a list of elements to
 *	splice into the argument vector.
 *
 *	The callback must allocate azOut and anOut via Th8_Malloc;
 *	the caller will free them.  Return TH8_OK on success or
 *	TH8_ERROR (with an error message in the interp result).
 *
 *----------------------------------------------------------------------
 */

typedef int (*Th8_ExpansionProc)(
    Th8_Interp *interp,  /* Interpreter. */
    const char *zInput,  /* Substituted word value. */
    size_t nInput,  /* Byte length of zInput. */
    char ***pazOut,  /* OUT: array of expanded elements. */
    size_t **panOut,  /* OUT: array of element lengths. */
    int *pnCount,  /* OUT: number of elements. */
    void *pCtx   /* User context from registration. */
);

/*
 *----------------------------------------------------------------------
 *
 * Th8_MathFuncProc --
 *
 *	Callback for expression math functions (abs, sin, int, etc.).
 *	Registered via Th8_CreateMathFunc.  Called during expression
 *	evaluation when the function name is encountered.
 *
 *	The callback receives 0, 1, or 2 string arguments (as parsed
 *	by the expression evaluator) and must set the interpreter
 *	result to the computed value.
 *
 *----------------------------------------------------------------------
 */

typedef int (*Th8_MathFuncProc)(
    Th8_Interp *interp,  /* Interpreter. */
    void *pCtx,   /* User context from registration. */
    const char *zArg1,  /* First argument (or NULL). */
    size_t nArg1,  /* Byte length of first argument. */
    const char *zArg2,  /* Second argument (or NULL). */
    size_t nArg2  /* Byte length of second argument. */
);

/*
 *----------------------------------------------------------------------
 *
 * Exit codes --
 *
 *	TH8_EXIT_SUCCESS -- The process had zero errors.
 *
 *	TH8_EXIT_FAILURE -- The process had one or more errors.
 *
 *	TH8_EXIT_DEMAND --  The process interpreter (somehow) invoked the
 *	                    [exit] script command and this status was not
 *	                    reset.
 *
 *	TH8_EXIT_EXCEPTION -- An exception was caught.  Uncaught exceptions,
 *	                      like segfault, may have a different exit code.
 *
 *	TH8_EXIT_PLATFORM -- An important call into a platform subsystem has
 *	                     failed.
 *
 *	TH8_EXIT_SECURITY -- An important call into a security subsystem has
 *	                     failed.
 *
 *	TH8_EXIT_DEBUGGER -- The script debugger or the interactive user has
 *	                     explicitly requested to exit.
 *
 *----------------------------------------------------------------------
 */

#define TH8_EXIT_SUCCESS             (0)
#define TH8_EXIT_FAILURE             (1)
#define TH8_EXIT_DEMAND              (2)
#define TH8_EXIT_EXCEPTION           (3)
#define TH8_EXIT_END_OF_TRANSMISSION (4)
#define TH8_EXIT_PLATFORM            (5)
#define TH8_EXIT_SECURITY            (6)
#define TH8_EXIT_DEBUGGER            (7)

/*
 *----------------------------------------------------------------------
 *
 * Return codes --
 *
 *	Every public API function that returns int uses one of these
 *	codes.  They mirror the classic Tcl return codes.
 *
 *	TH8_OK       -- Success.  The operation completed normally.
 *	                The interpreter result (if any) holds the
 *	                return value.
 *
 *	TH8_ERROR    -- Error.  The operation failed.  The interpreter
 *	                result holds an error message.  Th8_GetErrorLine()
 *	                gives the source line of the error.
 *
 *	TH8_RETURN   -- The [return] command was invoked.  The result
 *	                holds the return value.  Propagates up through
 *	                one level of procedure call.
 *
 *	TH8_BREAK    -- The [break] command was invoked.  Propagates
 *	                up to the enclosing loop.
 *
 *	TH8_CONTINUE -- The [continue] command was invoked.  Propagates
 *	                up to the enclosing loop.
 *
 *	TH8_RETURN2  -- A [return -code return] was invoked, causing
 *	                a double-level return that unwinds through
 *	                two procedure frames.
 *
 *	TH8_SUSPEND  -- The interpreter has been frozen via Th8_Freeze.
 *	                The NRE callback chain is preserved on the heap
 *	                so Th8_Thaw can resume evaluation later.
 *
 *----------------------------------------------------------------------
 */

#define TH8_OK       (0)
#define TH8_ERROR    (1)
#define TH8_RETURN   (2)
#define TH8_BREAK    (3)
#define TH8_CONTINUE (4)
#define TH8_RETURN2  (5)
#define TH8_SUSPEND  (6)
#define TH8_YIELD    (7)
#define TH8_CLEANUP  (8)

/*
 *----------------------------------------------------------------------
 *
 * Th8_Platform.xEventWait return codes.
 *
 *	xEventWait reports one of four distinct outcomes; each has
 *	a distinct caller recovery path.  These constants are the
 *	contract -- never use the bare integers at call sites.
 *
 *	TH8_WAIT_OK       Signal observed; the wait succeeded.
 *	TH8_WAIT_RETRY    Early wake (WAIT_IO_COMPLETION on Win32;
 *	                  EINTR / spurious wake on POSIX).  The
 *	                  caller should re-check its predicates
 *	                  and call wait again if still pending.
 *	TH8_WAIT_ERROR    The wait failed (e.g. handle invalid).
 *	                  Distinct from timeout: this means the
 *	                  primitive is broken and the caller
 *	                  should bail out rather than retry.
 *	TH8_WAIT_TIMEOUT  The configured timeout elapsed without
 *	                  the signal firing.  The caller's
 *	                  contract decides whether this is an
 *	                  error or a normal completion.
 *
 *----------------------------------------------------------------------
 */

#define TH8_WAIT_OK      (0)
#define TH8_WAIT_RETRY   (-1)
#define TH8_WAIT_ERROR   (1)
#define TH8_WAIT_TIMEOUT (2)

/*
 * Phase constants for the unified policy callback.
 * Timing (PRE/POST) and operation (READ/EVAL) are
 * orthogonal bits combined via bitwise OR.
 */
#define TH8_PHASE_PRE  0x01
#define TH8_PHASE_POST 0x02
#define TH8_PHASE_READ 0x04
#define TH8_PHASE_EVAL 0x08

/*
 *----------------------------------------------------------------------
 *
 * Callback types --
 *
 *	Th8_PolicyProc --
 *		Unified policy callback invoked before and after both
 *		script evaluations and data reads.  The `phase` argument
 *		is a bitmask combining a timing bit (TH8_PHASE_PRE or
 *		TH8_PHASE_POST) with an operation bit (TH8_PHASE_EVAL or
 *		TH8_PHASE_READ).  In PRE phases, returning non-TH8_OK
 *		aborts the operation.  In POST phases, the return value
 *		is ignored.  Registered with Th8_SetPolicyCallback.
 *
 *	Th8_CommandProc --
 *		Implementation function for a TH8 command.  Receives the
 *		interpreter, the context pointer from Th8_CreateCommand,
 *		and the argument vector (argc/argv/argl).  argv[0] is the
 *		command name.  Each argv[i] is a UTF-8 string of argl[i]
 *		bytes.  Returns a TH8_* return code.
 *
 *	Th8_CallbackProc --
 *		NRE continuation callback.  Receives the interpreter, four
 *		opaque client-data pointers (pData[0..3]), and the return
 *		code from the preceding evaluation step.  Returns a TH8_*
 *		return code that becomes the input to the next continuation.
 *
 *	Th8_SubCommand --
 *		Entry in a sub-command dispatch table.  zName is the
 *		sub-command name (NUL-terminated) and xProc is its
 *		implementation.  Pass an array of these to
 *		Th8_CallSubCommand for ensemble-style dispatch.
 *
 *----------------------------------------------------------------------
 */

typedef int (*Th8_PolicyProc)(
    Th8_Interp *interp,
    int phase,
    const char *zName,
    size_t nName,
    const char *zData,
    size_t nData,
    int flags,
    int rc,
    void *pCtx);

typedef int (*Th8_PreLoadProc)(
    Th8_Interp *interp,
    const char *zName,
    size_t nName,
    void *pCtx);

typedef int (*Th8_CommandProc)(
    Th8_Interp *interp,
    void *pCtx,
    int argc,
    const char **argv,
    size_t *argl);

typedef int (*Th8_CallbackProc)(Th8_Interp *interp, void *pData[4], int rc);

typedef struct Th8_SubCommand Th8_SubCommand;
struct Th8_SubCommand {
    th8_int64_t nVersion; /* Struct version (must be first). */
    const char *zName;  /* Sub-command name. */
    Th8_CommandProc xProc; /* Implementation function. */
};

/*
 *----------------------------------------------------------------------
 *
 * Debug event types and callback (for Th8_SetDebugCallback).
 */

#define TH8_DEBUG_STEP       (1) /* Command boundary */
#define TH8_DEBUG_BREAKPOINT (2) /* Breakpoint hit */
#define TH8_DEBUG_EXCEPTION  (3) /* Uncaught error */
#define TH8_DEBUG_ENTER      (4) /* Entering proc */
#define TH8_DEBUG_LEAVE      (5) /* Leaving proc */

/*
 * Step modes (for Th8_SetStepMode).
 */

#define TH8_STEP_NONE (0)
#define TH8_STEP_INTO (1)
#define TH8_STEP_OVER (2)
#define TH8_STEP_OUT  (3)

/*
 * Debug callback.  Fired at command boundaries when debugging is
 * active.  Return TH8_OK to continue, TH8_BREAK to suspend the
 * interpreter (freeze), or TH8_ERROR to abort.
 */

typedef int (*Th8_DebugProc)(
    Th8_Interp *interp,
    int event,   /* TH8_DEBUG_* event type. */
    const char *zScript, /* Script name (or NULL). */
    size_t nScript,  /* Length of zScript. */
    int nLine,   /* 1-based line number. */
    int nDepth,   /* Call frame depth. */
    void *pCtx);  /* Client data. */

/*
 *----------------------------------------------------------------------
 *
 * Channel control operations (for xChannelControl).
 */

#define TH8_CHANCTL_SEEK  (0)
#define TH8_CHANCTL_TELL  (1)
#define TH8_CHANCTL_FLUSH (2)
#define TH8_CHANCTL_CLOSE (3)
#define TH8_CHANCTL_WRITE (4)  /* nArg1 = length; pBuf = data */
#define TH8_CHANCTL_READ                                                     \
    (5)  /* nArg1 = max length; pBuf = buffer; *pnResult = bytes read */
#define TH8_CHANCTL_OPEN                                                     \
    (6) /* nArg1 = path length; nArg2 = mode (0=rd,1=wr); pBuf = path; *pnResult = handle */

/*
 * TH8_KV_* -- Operation codes for the xKeyValue platform callback.
 */
#define TH8_KV_NONE    (0)
#define TH8_KV_EXISTS  (1)
#define TH8_KV_LIST    (2)
#define TH8_KV_GET     (3)
#define TH8_KV_SET     (4)
#define TH8_KV_UNSET   (5)
#define TH8_KV_EXISTS2 (6)
#define TH8_KV_LIST2   (7)
#define TH8_KV_GET2    (8)
#define TH8_KV_SET2    (9)
#define TH8_KV_UNSET2  (10)

/*
 * File attribute type codes for the xDataExists pAttrs output --
 *
 *	When xDataExists receives a non-NULL pAttrs pointer and the
 *	data exists, it stores one of these values to indicate the
 *	type of the filesystem entry.
 *
 *----------------------------------------------------------------------
 */

#define TH8_FILE_ATTR_NONE        (0) /* Nothing. */
#define TH8_FILE_ATTR_FILE        (1) /* Regular file. */
#define TH8_FILE_ATTR_DIRECTORY   (2) /* Directory. */
#define TH8_FILE_ATTR_UNSUPPORTED (3) /* Not file, dir, or symlink. */
#define TH8_FILE_ATTR_SYMLINK     (4) /* Flag: ORed with FILE or DIR. */
#define TH8_FILE_ATTR_BROKEN      (8) /* Broken link, etc. */

/*
 * Script cancellation flags --
 *
 *	TH8_CANCEL_UNWIND --
 *		Prevents [catch] from intercepting the cancellation
 *		error.  The error propagates through all [catch]
 *		frames to the outermost Th8_Eval caller.
 *
 *	TH8_CANCEL_SIGNAL --
 *		The zMsg string is static (or otherwise long-lived)
 *		and must NOT be copied or freed.  When this flag is
 *		set, Th8_CancelEval stores the pointer directly.
 *		This makes the call async-signal-safe (no malloc).
 *		Without this flag, zMsg is copied via Th8_Malloc.
 *
 *----------------------------------------------------------------------
 */

#define TH8_CANCEL_UNWIND (0x01)
#define TH8_CANCEL_SIGNAL (0x02)

/*
 *----------------------------------------------------------------------
 *
 * Math function op codes for xMathFunc --
 *
 *	These codes identify the operation to perform in the
 *	xMathFunc platform callback.  They are grouped as follows:
 *
 *	Unary  double -> double:
 *	  ACOS, ASIN, ATAN, CEIL, COS, COSH, EXP, FLOOR, LOG,
 *	  LOG10, SIN, SINH, SQRT, TAN, TANH
 *
 *	Binary (double, double) -> double:
 *	  ATAN2, FMOD, HYPOT, POW
 *
 *	Special:
 *	  RAND   -- returns a random double in [0,1); 'a' and 'b'
 *	            are unused.
 *	  SRAND  -- seeds the random generator with (int)a; *pResult
 *	            is unused on output.
 *
 *----------------------------------------------------------------------
 */

#define TH8_MATH_NONE  (0) /* Reserved (zero = invalid). */
#define TH8_MATH_ACOS  (1)
#define TH8_MATH_ASIN  (2)
#define TH8_MATH_ATAN  (3)
#define TH8_MATH_ATAN2 (4)
#define TH8_MATH_CEIL  (5)
#define TH8_MATH_COS   (6)
#define TH8_MATH_COSH  (7)
#define TH8_MATH_EXP   (8)
#define TH8_MATH_FLOOR (9)
#define TH8_MATH_FMOD  (10)
#define TH8_MATH_HYPOT (11)
#define TH8_MATH_LOG   (12)
#define TH8_MATH_LOG10 (13)
#define TH8_MATH_POW   (14)
#define TH8_MATH_RAND  (15)
#define TH8_MATH_SIN   (16)
#define TH8_MATH_SINH  (17)
#define TH8_MATH_SQRT  (18)
#define TH8_MATH_SRAND (19)
#define TH8_MATH_TAN   (20)
#define TH8_MATH_TANH  (21)

/*
 * TIP #745: C99 math functions.
 */

#define TH8_MATH_ACOSH     (22)
#define TH8_MATH_ASINH     (23)
#define TH8_MATH_ATANH     (24)
#define TH8_MATH_CBRT      (25)
#define TH8_MATH_COPYSIGN  (26) /* 2 args */
#define TH8_MATH_ERF       (27)
#define TH8_MATH_ERFC      (28)
#define TH8_MATH_EXP2      (29)
#define TH8_MATH_EXPM1     (30)
#define TH8_MATH_FDIM      (31) /* 2 args */
#define TH8_MATH_FMA       (32) /* 3 args: a*b+c (a,b via args, c via b2) */
#define TH8_MATH_LGAMMA    (33)
#define TH8_MATH_LOG1P     (34)
#define TH8_MATH_LOG2      (35)
#define TH8_MATH_LOGB      (36)
#define TH8_MATH_NEXTAFTER (37) /* 2 args */
#define TH8_MATH_REMAINDER (38) /* 2 args */
#define TH8_MATH_TGAMMA    (39)
#define TH8_MATH_TRUNC     (40)
#define TH8_MATH_LDEXP     (41) /* double, int */
#define TH8_MATH_SIGNBIT   (42)

/*
 * TIP #521: Float classification (not routed through xMathFunc).
 * These are handled directly as math function callbacks using
 * C99 <math.h> classification macros.
 */

#define TH8_MATH_ISFINITE    (50)
#define TH8_MATH_ISINF       (51)
#define TH8_MATH_ISNAN       (52)
#define TH8_MATH_ISNORMAL    (53)
#define TH8_MATH_ISSUBNORMAL (54)
#define TH8_MATH_ISUNORDERED (55)
#define TH8_MATH_FPCLASSIFY  (56)


/* ====================================================================
 * Section: Library Lifecycle
 * ==================================================================== */

/*
 *----------------------------------------------------------------------
 *
 * Platform merging --
 *
 * Th8_MergePlatform --
 *
 *	Merge pSrc into pDst: for each callback slot, if pDst's
 *	pointer is NULL, copy from pSrc.  This allows composing
 *	multiple platform modules (e.g. OS + math).
 *
 *	IMPORTANT: the pCtx field is NOT merged.  The destination's
 *	pCtx is always preserved unchanged.  This is intentional:
 *	pCtx is host-specific context and should be set explicitly
 *	by the embedder after merging.
 *
 *----------------------------------------------------------------------
 */

TH8_API int Th8_MergePlatform(Th8_Platform *pDst, const Th8_Platform *pSrc);

/*
 * Th8_ClonePlatform --
 *	Allocate a new mutable copy of a const platform table.
 *	The returned platform must be freed by the caller with
 *	Th8_FreePlatform when no longer needed.  Returns NULL
 *	on allocation failure.
 */
TH8_API Th8_Platform *Th8_ClonePlatform(const Th8_Platform *pSrc);

/*
 * Th8_FreePlatform --
 *	Free a platform table allocated by Th8_ClonePlatform.
 */
TH8_API void Th8_FreePlatform(Th8_Platform *pPlatform);

/*
 * Th8_MergePlatformInterp --
 *	Merge the callbacks from pSrc into the interpreter's current
 *	platform.  Clones the current platform, merges pSrc into the
 *	clone (NULL slots in the clone are filled from pSrc), and
 *	replaces the interpreter's platform pointer.  The clone is
 *	freed automatically when the interpreter is deleted or when
 *	this function is called again.  Returns TH8_OK or TH8_ERROR.
 */
TH8_API int
Th8_MergePlatformInterp(Th8_Interp *interp, const Th8_Platform *pSrc);

/*
 * Th8_Initialize --
 *	Initialize the TH8 library for use in the current process.
 *	Must be called exactly once before any other TH8 API call.
 *	The pPlatform argument provides the mutex callbacks needed
 *	for protecting process-global state.  If pPlatform is NULL
 *	or the mutex callbacks are NULL, TH8 operates in single-
 *	threaded mode with no locking.
 *
 *	Returns TH8_OK on success, TH8_ERROR on failure.
 *	After Th8_Finalize(), Th8_Initialize() may be called again.
 */

TH8_API int Th8_Initialize(Th8_Platform *pPlatform);

/*
 * Th8_Finalize --
 *	Finalize the TH8 library, releasing all process-global
 *	resources.  After this call, no TH8 API may be used until
 *	Th8_Initialize() is called again.
 *
 *	Returns TH8_OK on success, TH8_ERROR on failure.
 */
TH8_API int Th8_Finalize(Th8_Platform *pPlatform);

/*
 * Th8_GetStubs --
 *	Return a pointer to the interpreter's stubs table (opaque).
 *	Used internally by Th8_InitStubs in the stubs library.
 */
TH8_API const void *Th8_GetStubs(Th8_Interp *interp);

/*
 * Th8_GetInternalStubs --
 *	Return a pointer to the TH8 internal stubs table (opaque).
 *	The returned pointer must be cast to
 *	`const Th8InternalStubsTable *` (declared in
 *	th8InternalDecls.h, NOT in this header) before use.
 *
 *	Designed for binary plugins compiled against
 *	libth8stub.a (e.g. src/test/th8_testlib.c) that need to
 *	invoke specific TH8_INTERNAL functions whose symbols are
 *	hidden from the shared library's export table.  Callers
 *	must validate the table's magic and version fields before
 *	dereferencing function pointers.
 *
 *	General-purpose extensions SHOULD NOT use this API -- it
 *	is part of the test / diagnostic surface and its function
 *	set will change over time as MC/DC needs dictate.
 *
 *	Never returns NULL.
 */
TH8_API const void *Th8_GetInternalStubs(void);

/* ====================================================================
 * Section: Threading & Diagnostics
 * ==================================================================== */

/*
 * Th8_IntCmpXchg --
 *	Atomic integer compare-and-exchange via the global platform's
 *	xIntCmpXchg callback.  Compares *pTarget with iComparand; if
 *	equal, stores iExchange in *pTarget.  Returns the original
 *	value of *pTarget.  Falls back to non-atomic operation if
 *	the callback is NULL (single-threaded assumption).
 *	Uses the global platform directly (not through an interp),
 *	since it is needed during initialization before an interpreter
 *	exists.
 */
TH8_API int Th8_IntCmpXchg(
    Th8_Interp *interp,
    volatile int *pTarget,
    int iExchange,
    int iComparand);

/* ====================================================================
 * Section: Interpreter Lifecycle
 * ==================================================================== */

/*
 * Th8_CreateInterp --
 *	Create a new TH8 interpreter.  pPlatform provides all external
 *	dependencies (memory, I/O, etc.).
 *
 *	IMPORTANT: The interpreter stores a POINTER to the platform
 *	struct --- it does NOT copy it.  The Th8_Platform struct must
 *	remain valid (and at a stable address) for the entire lifetime
 *	of the interpreter.  Stack-allocated platforms are safe only if
 *	the interpreter is created and destroyed within the same stack
 *	frame.  For interpreters that outlive the creating function,
 *	use a static, global, or heap-allocated Th8_Platform.
 *
 *	Returns NULL if the initial memory allocation fails.  The
 *	returned interpreter must eventually be destroyed with
 *	Th8_DeleteInterp().  Th8_Initialize() must have been called
 *	before this function.
 */
TH8_API Th8_Interp *Th8_CreateInterp(Th8_Platform *pPlatform);

/*
 *----------------------------------------------------------------------
 *
 * Interpreter restoration flags --
 *
 *	Used with Th8_RestoreInterp to selectively re-create
 *	built-in commands and/or standard global variables after
 *	a [namespace delete ::] or similar destructive operation.
 *
 *----------------------------------------------------------------------
 */

#define TH8_RESTORE_NONE      (0x00)
#define TH8_RESTORE_COMMANDS  (0x01)
#define TH8_RESTORE_VARIABLES (0x02)
#define TH8_RESTORE_ALL       (TH8_RESTORE_COMMANDS | TH8_RESTORE_VARIABLES)

/*
 * Th8_RestoreInterp --
 *	Re-create built-in state that was destroyed by namespace
 *	deletion.  flags is a bitmask of TH8_RESTORE_* values.
 *
 *	TH8_RESTORE_COMMANDS  -- re-register all built-in commands
 *	                         (Th8_RegisterLanguage + regex).
 *	TH8_RESTORE_VARIABLES -- re-create standard global variables
 *	                         (::tcl_platform, ::argv, ::errorInfo, etc.).
 *	TH8_RESTORE_NONE      -- do nothing (no-op).
 *	TH8_RESTORE_ALL       -- both commands and variables.
 *
 *	Returns TH8_OK.
 */
TH8_API int Th8_RestoreInterp(Th8_Interp *interp, int flags);

/*
 * Th8_DeleteInterp --
 *	Destroy the interpreter and free all associated resources
 *	(variables, commands, internal buffers).  After this call,
 *	the interp pointer is invalid.  Any Th8_TakeResult buffers
 *	previously obtained remain valid (caller owns them).
 */
TH8_API void Th8_DeleteInterp(Th8_Interp *interp);

/* ====================================================================
 * Section: Interpreter Readiness
 * ==================================================================== */

/*
 * Th8_Ready --
 *	Check whether the interpreter is ready to execute.  Returns
 *	TH8_OK if the interpreter is in a usable state (not cancelled,
 *	step limit not exceeded).  Returns TH8_ERROR otherwise.
 */
TH8_API int Th8_Ready(Th8_Interp *interp);

/* ====================================================================
 * Section: Cancellation and event queue (the only thread-safe TH8 APIs)
 *
 * `Th8_CancelEval` and `Th8_QueueEvent` are the ONLY public TH8 APIs
 * documented as callable from any thread.  Every other Th8_* API is
 * single-threaded per interpreter and MUST be invoked on the
 * interpreter's owning thread.
 * ==================================================================== */

/*
 * Th8_CancelEval --
 *	Request cancellation of the currently executing script.  zMsg/nMsg
 *	provide an error message (pass TH8_NOLEN if zMsg is NUL-terminated).
 *	flags may include TH8_CANCEL_UNWIND to prevent [catch] from
 *	intercepting the cancellation, ensuring it propagates to the
 *	outermost Th8_Eval caller.  Safe to call from a signal handler
 *	or another thread.  Returns TH8_OK.
 */
TH8_API int
Th8_CancelEval(Th8_Interp *interp, const char *zMsg, size_t nMsg, int flags);

/*
 * Th8_ThreadInit / Th8_ThreadDone --
 *	Register / unregister the calling thread with TH8's allocator.
 *
 *	Any worker thread that will call a thread-safe TH8 API which
 *	allocates memory (e.g. Th8_QueueEvent) MUST call Th8_ThreadInit
 *	once at thread entry, before its first allocating call, and
 *	Th8_ThreadDone once at thread exit, after its last call.  Both
 *	are idempotent and safe to call from any thread.
 *
 *	This matters because some allocator backends (notably mimalloc
 *	in TH8_USE_MIMALLOC builds) require per-thread initialization
 *	to avoid allocator-internal assertion failures or undefined
 *	behaviour.  On builds whose allocator does not need per-thread
 *	state, both functions are no-ops.  The contract is uniform so
 *	embedder code is portable across allocator configurations.
 *
 *	The main interp thread is registered automatically by the
 *	first Th8_CreateInterp call; only ADDITIONAL threads need
 *	these calls.
 */
TH8_API void Th8_ThreadInit(void);
TH8_API void Th8_ThreadDone(void);

/*
 * Th8_CreateAsyncState --
 *	Allocate an opaque "async state" object that an embedder
 *	passes to Th8_QueueEvent from any thread.  The pState
 *	carries (a) a back-pointer to the interp, (b) the
 *	embedder's own pCtx, and (c) an atomic nDeleted flag
 *	that lets cross-thread callers detect a torn-down interp
 *	without dereferencing freed memory.
 *
 *	MUST be called on the interp's owning thread.  The
 *	returned pState is the embedder's to keep -- TH8 does NOT
 *	free it.  When the interp is deleted, TH8 atomically
 *	marks every registered pState as "deleted" (its first
 *	field, nDeleted, transitions from 0 to non-zero) so
 *	subsequent Th8_QueueEvent calls fail cleanly with
 *	TH8_ERROR.  The embedder MUST eventually call
 *	Th8_FinalizeAsyncState on every pState it created
 *	(typically after all worker threads using it have
 *	stopped).
 *
 *	Returns the pState on success, NULL if the platform does
 *	not have the required threading primitives wired up
 *	(xMutex* + xEvent*) or on allocation failure.
 *
 *	This function is thread-confined to the interp's owning
 *	thread; it is NOT thread-safe.
 */
TH8_API void *Th8_CreateAsyncState(Th8_Interp *interp, void *pCtx);

/*
 * Th8_FinalizeAsyncState --
 *	Free an async-state object previously returned by
 *	Th8_CreateAsyncState.  Safe to call on a pState whose
 *	owning interp has already been deleted (the interp
 *	pointer is NULL'd and nDeleted is set during
 *	Th8_DeleteInterp).
 *
 *	NOT thread-safe: the embedder MUST ensure no worker is
 *	concurrently inside Th8_QueueEvent on this pState.
 *	Typical pattern: signal workers to stop, join them, then
 *	call Th8_FinalizeAsyncState.
 */
TH8_API int Th8_FinalizeAsyncState(void *pState);

/*
 * Th8_QueueEvent --
 *	Enqueue an event for later drain by [update] / [vwait]
 *	or by an explicit call to Th8_DrainQueueEvents.  Thread-
 *	safe: callable from any thread.
 *
 *	pState must be a non-NULL value previously returned by
 *	Th8_CreateAsyncState.  Each pState owns its own queue,
 *	signal handle, and serialization mutex; queueing on one
 *	pState never contends with queueing on another (even on
 *	the same interp).  The callback is invoked LATER, on
 *	the owning interp's thread, with (interp, pCtx) where
 *	pCtx is the value passed to Th8_CreateAsyncState.
 *
 *	The callback may do anything a normal interp call can do,
 *	including evaluate scripts and modify variables.
 *
 *	Returns TH8_OK on success.  Returns TH8_ERROR if pState
 *	is NULL, the interp has been deleted (atomic nDeleted
 *	check fails), or on allocation failure.
 */
TH8_API int Th8_QueueEvent(
    void *pState,
    int (*xCallback)(Th8_Interp *interp, void *pCtx));

/*
 * Th8_DrainQueueEvents --
 *	Drain and invoke all pending events across every pState
 *	registered with the interp.  Single-threaded: must be
 *	called on the interp's owning thread.  This is the
 *	high-level operation [update] performs and the supported
 *	way for embedders or test code to flush queued work
 *	without scripts.  Walks every live (non-finalized) pState
 *	on the interp; from each, drains every queued event in
 *	FIFO order.  Returns TH8_OK if every callback returned
 *	TH8_OK; returns the first non-OK return otherwise (still
 *	stops at that point -- remaining events stay queued).
 */
TH8_API int Th8_DrainQueueEvents(Th8_Interp *interp);

/*
 * Th8_IsCanceled --
 *	Check whether the interpreter has a pending cancellation.
 *	Returns TH8_ERROR if cancelled (and sets the interpreter result
 *	to the cancellation message), TH8_OK otherwise.  flags may
 *	include TH8_CANCEL_UNWIND to also check for unwind requests.
 */
TH8_API int Th8_IsCanceled(Th8_Interp *interp, int flags);

/*
 * Th8_IsBeingUnwound --
 *	Check whether an unwind-style cancellation is in progress.
 *	Returns non-zero if TH8_CANCEL_UNWIND was set, zero otherwise.
 */
TH8_API int Th8_IsBeingUnwound(Th8_Interp *interp);

/*
 * Th8_ResetCancel --
 *	Clear the interpreter's cancellation state (bCanceled flag,
 *	cancel message, and cancel flags).  Called internally by
 *	[catch] when intercepting a non-unwind cancellation, and
 *	available for host code to programmatically reset cancel state.
 */
TH8_API void Th8_ResetCancel(Th8_Interp *interp);

#define TH8_CANCEL_SAVE_SIZE 64

/* ====================================================================
 * Section: Exit
 * ==================================================================== */

/*
 * Th8_Exit --
 *	Set the interpreter's exit flag.  Th8_Ready will return
 *	TH8_ERROR on every subsequent call, unwinding the stack.
 *	The flag can only be cleared by Th8_ResetExit.
 */
TH8_API void Th8_Exit(Th8_Interp *interp);

/*
 * Th8_IsExited --
 *	Check whether the interpreter's exit flag is set.  Returns
 *	non-zero if [exit] has been called.  The flag is sticky and
 *	can only be cleared by Th8_ResetExit.
 */
TH8_API int Th8_IsExited(Th8_Interp *interp);

/*
 * Th8_ResetExit --
 *	Clear the exit flag so the interpreter can resume execution.
 *	This is the only way to clear the exit state set by [exit].
 */
TH8_API void Th8_ResetExit(Th8_Interp *interp);

/* ====================================================================
 * Section: Suspension (Freeze / Thaw)
 * ==================================================================== */

/*
 * Th8_Freeze --
 *	Suspend the interpreter at the next command boundary.
 *	The NRE state is preserved on the heap for later resumption.
 *	Safe to call from any thread.
 */
TH8_API int Th8_Freeze(Th8_Interp *interp);

/*
 * Th8_Thaw --
 *	Resume a frozen interpreter by clearing the suspension flag.
 *	After this call, Th8_Ready will return TH8_OK again.  If
 *	NRE callbacks were preserved, the caller should re-enter the
 *	trampoline via th8EvalTrampoline at the appropriate level.
 */
TH8_API int Th8_Thaw(Th8_Interp *interp);

/*
 * Th8_IsSuspended --
 *	Return non-zero if the interpreter has a pending suspension
 *	(Th8_Freeze was called but Th8_Thaw has not yet cleared it).
 */
TH8_API int Th8_IsSuspended(Th8_Interp *interp);

/* ====================================================================
 * Section: Script Debugging
 * ==================================================================== */

/*
 * Th8_SetDebugCallback --
 *	Install or remove a debug callback.  The callback fires at
 *	every command boundary when the interpreter is being debugged
 *	(step mode is active or breakpoints are set).  Pass NULL to
 *	remove the callback and disable debugging.
 */
TH8_API int
Th8_SetDebugCallback(Th8_Interp *interp, Th8_DebugProc xDebug, void *pCtx);

/*
 * Th8_SetBreakpoint --
 *	Set a breakpoint at the given script name and line number.
 *	Returns the breakpoint ID in *pBreakpointId (if non-NULL).
 */
TH8_API int Th8_SetBreakpoint(
    Th8_Interp *interp,
    const char *zScript,
    size_t nScript,
    int nLine,
    int *pBreakpointId);

/*
 * Th8_ClearBreakpoint --
 *	Remove a breakpoint by its ID.
 */
TH8_API int Th8_ClearBreakpoint(Th8_Interp *interp, int breakpointId);

/*
 * Th8_ClearAllBreakpoints --
 *	Remove all breakpoints from the interpreter.
 */
TH8_API int Th8_ClearAllBreakpoints(Th8_Interp *interp);

/*
 * Th8_SetStepMode / Th8_GetStepMode --
 *	Control the debugger step mode (TH8_STEP_*).
 *	STEP_INTO: break at every command.
 *	STEP_OVER: break when frame depth <= starting depth.
 *	STEP_OUT:  break when frame depth < starting depth.
 *	STEP_NONE: free-run (break only on breakpoints).
 */
TH8_API int Th8_SetStepMode(Th8_Interp *interp, int mode);
TH8_API int Th8_GetStepMode(Th8_Interp *interp);

/*
 * Th8_GetFrameCount --
 *	Return the number of call frames on the stack.
 */
TH8_API int Th8_GetFrameCount(Th8_Interp *interp);

/*
 * Th8_GetFrameInfo --
 *	Retrieve information about a specific call frame.
 *	frameIndex 0 is the innermost (current) frame.
 *	Returns TH8_OK on success, TH8_ERROR if out of range.
 */
TH8_API int Th8_GetFrameInfo(
    Th8_Interp *interp,
    int frameIndex,
    const char **pzProc,
    size_t *pnProc,
    const char **pzScript,
    size_t *pnScript,
    int *pnLine);

/*
 * Th8_EvalAtFrame --
 *	Evaluate a script in the context of a specific call frame.
 *	frameIndex 0 is the innermost (current) frame.
 *	Useful for evaluating watch expressions during a debug pause.
 */
TH8_API int Th8_EvalAtFrame(
    Th8_Interp *interp,
    int frameIndex,
    const char *zScript,
    size_t nScript);

/* ====================================================================
 * Section: Coroutines
 * ==================================================================== */

/*
 * Th8_CoroCreate --
 *	Create a coroutine named zName.  Evaluates zBody; if the body
 *	calls [yield], the coroutine is suspended and the yield value
 *	becomes the result.  A command named zName is created that
 *	resumes the coroutine when called.
 */
TH8_API int Th8_CoroCreate(
    Th8_Interp *interp,
    const char *zName,
    size_t nName,
    const char *zBody,
    size_t nBody);

/*
 * Th8_CoroYield --
 *	Suspend the current coroutine and return zValue to the
 *	coroutine's caller.  Can only be called from within a
 *	coroutine body.  Returns TH8_OK (via Th8_Freeze internally).
 */
TH8_API int
Th8_CoroYield(Th8_Interp *interp, const char *zValue, size_t nValue);

/* ====================================================================
 * Section: Script Evaluation
 * ==================================================================== */

/*
 * Th8_Eval --
 *	Evaluate a Tcl script synchronously (blocking).  This is the
 *	primary evaluation entry point.  Internally drives the NRE
 *	trampoline to completion before returning.
 *
 *	iFrame selects the call frame for variable resolution:
 *	  0  -- the current (topmost) frame.
 *	  N>0 -- absolute frame level N (1 = global).
 *	  N<0 -- relative: -1 = caller's frame, -2 = caller's caller, etc.
 *	Most callers pass 0.
 *
 *	zProg/nProg is the script text.  Pass TH8_NOLEN for nProg if
 *	zProg is NUL-terminated.
 *
 *	zName/nName is the origin name (e.g. a file or bucket name)
 *	describing where the script came from.  Pass NULL/0 if the
 *	origin is unknown.  This is forwarded to the policy callback.
 *
 *	Returns a TH8_* return code; the interpreter result holds the
 *	script's result or error message.
 */
TH8_API int Th8_Eval(
    Th8_Interp *interp,
    int iFrame,
    const char *zProg,
    size_t nProg,
    const char *zName,
    size_t nName);

/*
 * Th8_EvalTrusted --
 *
 *	Evaluate a script with an explicit trust assertion from the
 *	embedder.  Identical to Th8_Eval except that the interpreter's
 *	TH8_EVAL_TRUSTED flag is set before the evaluation.  This
 *	flag allows the signed-only policy callback to return TH8_OK
 *	when zName/nName are both NULL/0 (i.e., the script has no
 *	file origin).
 *
 *	This function is the ONLY way for an embedder to authorize
 *	interactive or programmatic script input when signed-only
 *	mode is active.  Every call site is a security-relevant
 *	trust decision point.
 *
 *	If zName/nName are non-NULL, the normal verification path
 *	applies regardless of the trusted flag.
 */
TH8_API int Th8_EvalTrusted(
    Th8_Interp *interp,
    int iFrame,
    const char *zProg,
    size_t nProg,
    const char *zName,
    size_t nName);

/*
 * Th8_EvalDownlevel --
 *	Evaluate a script in the call frame that was active just
 *	prior to the most recent uplevel (frame switch).  Per Eagle
 *	semantics, [downlevel] undoes the scope change of [uplevel],
 *	running the script back in the frame the uplevel departed
 *	from.
 */
TH8_API int Th8_EvalDownlevel(
    Th8_Interp *interp,
    const char *zProg,
    size_t nProg,
    const char *zName,
    size_t nName);

/*
 * Th8_Expr --
 *	Evaluate a mathematical/logical expression.  zExpr/nExpr is the
 *	expression text (pass TH8_NOLEN if NUL-terminated).  zName/nName
 *	is the script origin (NULL/0 if unknown).  The result is left in
 *	the interpreter result.  Returns TH8_OK or TH8_ERROR.
 */
TH8_API int Th8_Expr(
    Th8_Interp *interp,
    const char *zExpr,
    size_t nExpr,
    const char *zName,
    size_t nName);

/*
 * Th8_Complete --
 *	Test whether zScript (nScript bytes) is a "complete" Tcl script,
 *	meaning all braces, brackets, quotes, and backslash-newline
 *	sequences are balanced and terminated.  Returns non-zero if
 *	complete, zero if the script needs more input (e.g., an
 *	unterminated brace).  This is used by interactive shells to
 *	decide whether to prompt for continuation input.  Does not
 *	require an interpreter.
 */
TH8_API int Th8_Complete(const char *zScript, size_t nScript);

/*
 * Th8_EvalFile --
 *	Evaluate the contents of a file.  Retrieves the file data
 *	via the platform's xGetData callback, pushes the file name
 *	for [info script], evaluates the script, and cleans up.
 *	Returns TH8_OK on success or TH8_ERROR on failure.
 */
TH8_API int Th8_EvalFile(Th8_Interp *interp, const char *zName, size_t nName);

/*
 * Th8_EvalFileAsData --
 *	Load binary data by evaluating a signed script in an isolated
 *	child interpreter.  The child inherits the parent's platform
 *	and signed-only policy.  The script's result is base64-decoded
 *	and returned via pzData/pnData.  The child is always deleted.
 *	Caller must free *pzData with Th8_Free.  pCtx is reserved
 *	and must be NULL.
 */
TH8_API int Th8_EvalFileAsData(
    Th8_Interp *interp,
    const char *zName,
    size_t nName,
    const unsigned char **pzData,
    size_t *pnData,
    void *pCtx);

/* ====================================================================
 * Section: NRE (Non-Recursive Engine)
 * ==================================================================== */

/*
 * Th8_NREval --
 *	Schedule a script for evaluation via the NRE trampoline.  This
 *	is the NRE-aware counterpart of Th8_Eval.  The script runs in
 *	the current frame (iFrame=0).
 *
 *	zName/nName is the script origin (NULL/0 if unknown).  This is
 *	forwarded to the policy callback.
 *
 *	IMPORTANT: This function does NOT drive the trampoline itself.
 *	The caller must return to the trampoline loop (by returning from
 *	the current Th8_CommandProc or Th8_CallbackProc).  Use Th8_Eval
 *	if you need synchronous evaluation.
 *
 *	When to use Th8_NREval vs Th8_Eval:
 *	  - In a Th8_CommandProc or Th8_CallbackProc that needs to
 *	    evaluate a sub-script: use Th8_NREval + Th8_NRAddCallback.
 *	  - From top-level host code outside the trampoline: use Th8_Eval.
 */
TH8_API int Th8_NREval(
    Th8_Interp *interp,
    const char *zProg,
    size_t nProg,
    const char *zName,
    size_t nName);

/*
 * Th8_NRAddCallback --
 *	Push a continuation onto the NRE callback stack.  xProc will
 *	be called after the next scheduled evaluation completes,
 *	receiving the four opaque pointers p0..p3 in pData[0..3] and
 *	the return code from the evaluation in rc.
 *
 *	Callbacks are invoked in LIFO order (the last one pushed runs
 *	first after the evaluation completes).  Multiple callbacks may
 *	be pushed before returning to the trampoline; they form a chain.
 */
TH8_API int Th8_NRAddCallback(
    Th8_Interp *interp,
    Th8_CallbackProc xProc,
    void *p0,
    void *p1,
    void *p2,
    void *p3);

/*
 * Th8_NREvalInFrame --
 *	Schedule a script for evaluation in the specified frame using
 *	the NRE trampoline.  Unlike Th8_Eval, this does NOT drive the
 *	trampoline; the caller must return to the trampoline loop (or
 *	call th8EvalTrampoline) for evaluation to proceed.  Use this
 *	from within Th8_CommandProc or Th8_CallbackProc implementations
 *	to avoid C stack growth.
 *
 *	iFrame semantics are the same as Th8_Eval.  zName/nName is
 *	the script origin (NULL/0 if unknown).
 */
TH8_API int Th8_NREvalInFrame(
    Th8_Interp *interp,
    int iFrame,
    const char *zProg,
    size_t nProg,
    const char *zName,
    size_t nName);

/* ====================================================================
 * Section: Variables
 * ==================================================================== */

/*
 * Th8_ExistsVar --
 *	Test whether the variable zVar (nVar bytes, or TH8_NOLEN) exists
 *	in the current frame.  Returns non-zero if it exists, zero
 *	otherwise.  Does not modify the interpreter result.
 */
TH8_API int Th8_ExistsVar(Th8_Interp *interp, const char *zVar, size_t nVar);

/*
 * Th8_ExistsArrayVar --
 *	Test whether zVar names an array variable (as opposed to a scalar).
 *	Returns non-zero if it is an array, zero otherwise.
 */
TH8_API int
Th8_ExistsArrayVar(Th8_Interp *interp, const char *zVar, size_t nVar);

/*
 * Th8_GetVar --
 *	Read the value of variable zVar (nVar bytes, or TH8_NOLEN) in
 *	the current frame.  On success, the interpreter result is set
 *	to the variable's value and TH8_OK is returned.  Returns
 *	TH8_ERROR if the variable does not exist.
 */
TH8_API int Th8_GetVar(Th8_Interp *interp, const char *zVar, size_t nVar);

/*
 * Th8_SetVar --
 *	Set the variable zVar (nVar bytes) to the value zVal (nVal bytes)
 *	in the current frame.  Creates the variable if it does not exist.
 *	Pass TH8_NOLEN for nVar or nVal if the string is NUL-terminated.
 *	Returns TH8_OK on success, TH8_ERROR on failure (e.g., out of
 *	memory).
 */
TH8_API int Th8_SetVar(
    Th8_Interp *interp,
    const char *zVar,
    size_t nVar,
    const char *zVal,
    size_t nVal);

/*
 * Th8_LinkVar --
 *	Create a variable link: the local variable zLocal (nLocal bytes)
 *	in the current frame becomes an alias for the variable zRemote
 *	(nRemote bytes) in frame iFrame (same semantics as Th8_Eval's
 *	iFrame parameter).  This is the implementation of [upvar].
 *	Returns TH8_OK on success, TH8_ERROR on failure.
 */
TH8_API int Th8_LinkVar(
    Th8_Interp *interp,
    const char *zLocal,
    size_t nLocal,
    int iFrame,
    const char *zRemote,
    size_t nRemote);

/*
 * Th8_UnsetVar --
 *	Remove the variable zVar (nVar bytes, or TH8_NOLEN) from the
 *	current frame.  Returns TH8_OK on success, TH8_ERROR if the
 *	variable does not exist.
 */
TH8_API int Th8_UnsetVar(Th8_Interp *interp, const char *zVar, size_t nVar);

/* ====================================================================
 * Section: Commands
 * ==================================================================== */

/*
 * Th8_CreateCommand --
 *	Register a new command named zName (NUL-terminated) in the
 *	interpreter.  xProc is called when the command is invoked.
 *	pContext is passed through to xProc as its pCtx argument.
 *	xDel, if non-NULL, is called when the command is deleted or
 *	the interpreter is destroyed, receiving (interp, pContext).
 *	If a command with the same name already exists, it is replaced
 *	(and its xDel is called).
 *
 *	If pToken is non-NULL, a unique command token is assigned and
 *	written to *pToken.  The token survives rename and can be used
 *	with Th8_DeleteCommand to remove the command regardless of
 *	its current name.
 *
 *	Returns TH8_OK on success.
 */
TH8_API int Th8_CreateCommand(
    Th8_Interp *interp,
    const char *zName,
    Th8_CommandProc xProc,
    void *pContext,
    void (*xDel)(Th8_Interp *, void *),
    th8_uint64_t *pToken);

/*
 * Th8_DeleteCommand --
 *	Delete a command by its unique token.  The command is found
 *	regardless of any renames that may have occurred since creation.
 *	Returns TH8_OK if the command was found and deleted, TH8_ERROR
 *	if no command with the given token exists (it may have already
 *	been deleted or replaced).
 */
TH8_API int Th8_DeleteCommand(Th8_Interp *interp, th8_uint64_t token);

/*
 * Th8_GetCommandInfo --
 *	Look up the command named zName (nName bytes, or TH8_NOLEN)
 *	and return its procedure pointer and context.  The lookup
 *	follows the same rules as command dispatch: try the current
 *	namespace first, then fall back to the global namespace.
 *	On success, *pxProc and *ppContext are filled in and TH8_OK
 *	is returned.  If the command is not found, TH8_ERROR is
 *	returned and the interpreter result is set.
 */
TH8_API int Th8_GetCommandInfo(
    Th8_Interp *interp,
    const char *zName,
    size_t nName,
    Th8_CommandProc *pxProc,
    void **ppContext);

/*
 * Th8_SetCommandCopy --
 *	Set a deep-copy callback on a command.  Used by proc/nproc
 *	to enable safe namespace import (copies the ProcDefn instead
 *	of sharing a pointer).  xCopy receives (interp, pContext) and
 *	returns a new context owned by the imported command.
 */
TH8_API void Th8_SetCommandCopy(
    Th8_Interp *interp,
    const char *zName,
    void *(*xCopy)(Th8_Interp *, void *));

/*
 * Th8_RenameCommand --
 *	Rename the command zOld (nOld bytes) to zNew (nNew bytes).
 *	If zNew is empty (nNew==0), the command is deleted.  Returns
 *	TH8_OK on success, TH8_ERROR if the source command does not
 *	exist.
 */
TH8_API int Th8_RenameCommand(
    Th8_Interp *interp,
    const char *zOld,
    size_t nOld,
    const char *zNew,
    size_t nNew);

/* ====================================================================
 * Section: Expansion Operators
 * ==================================================================== */

/*
 * Th8_RegisterExpansion --
 *	Register an expansion operator for the given tag in the
 *	current namespace.  When the parser encounters {tag}word,
 *	xProc is called with the substituted word value.
 *
 *	The built-in {*} operator (Tcl 8.5 list expansion) is
 *	registered automatically by Th8_RegisterLanguage.  It can
 *	be replaced or unregistered like any other tag.
 *
 *	If an operator with the same tag already exists in the
 *	namespace, it is replaced.  Returns TH8_OK on success.
 */
TH8_API int Th8_RegisterExpansion(
    Th8_Interp *interp,
    const char *zTag,
    size_t nTag,
    Th8_ExpansionProc xProc,
    void *pCtx);

/*
 * Th8_UnregisterExpansion --
 *	Remove the expansion operator for the given tag from the
 *	current namespace.  Returns TH8_OK if found and removed,
 *	TH8_ERROR if not found.
 */
TH8_API int
Th8_UnregisterExpansion(Th8_Interp *interp, const char *zTag, size_t nTag);

/*
 * Th8_FindExpansion --
 *	Look up an expansion operator by tag.  Searches the current
 *	namespace first, then walks up to the global namespace.
 *	Returns TH8_OK if found (*pxProc and *ppCtx are filled in),
 *	TH8_ERROR if not found.
 */
TH8_API int Th8_FindExpansion(
    Th8_Interp *interp,
    const char *zTag,
    size_t nTag,
    Th8_ExpansionProc *pxProc,
    void **ppCtx);

/*
 * Th8_ListAppendExpansions --
 *	Append registered expansion operator tag names to a list
 *	string, optionally filtered by a glob pattern.  Iterates
 *	the current namespace's paExpansion hash.  Used by
 *	[info expansions ?pattern?].
 */
TH8_API void Th8_ListAppendExpansions(
    Th8_Interp *interp,
    char **pzList,
    size_t *pnList,
    const char *zPat,
    size_t nPat);

/*
 * Th8_ListAppendBreakpoints --
 *	Append breakpoint descriptions to a list string.  Each
 *	breakpoint is appended as three elements: id, scriptName,
 *	lineNumber.  Used by [info breakpoints].
 */
TH8_API void
Th8_ListAppendBreakpoints(Th8_Interp *interp, char **pzList, size_t *pnList);

/*
 * Th8_CreateMathFunc --
 *	Register a math function for use in [expr].  The function is
 *	stored per-interpreter and looked up by name during expression
 *	evaluation.
 *
 *	nArg is the expected argument count (0, 1, or 2).  If a
 *	function with the same name exists, it is replaced.
 *
 *	Built-in functions (abs, int, double, sin, cos, etc.) are
 *	registered automatically by Th8_RegisterLanguage from a
 *	static table.  They can be replaced by user-registered
 *	functions with the same name.
 */
TH8_API int Th8_CreateMathFunc(
    Th8_Interp *interp,
    const char *zName,
    size_t nName,
    int nArg,
    Th8_MathFuncProc xProc,
    void *pCtx);

/*
 * Th8_DeleteMathFunc --
 *	Remove a math function from the interpreter.
 */
TH8_API int
Th8_DeleteMathFunc(Th8_Interp *interp, const char *zName, size_t nName);

/*
 * Th8_FindMathFunc --
 *	Look up a math function by name.
 *	Returns TH8_OK if found, TH8_ERROR if not.
 */
TH8_API int Th8_FindMathFunc(
    Th8_Interp *interp,
    const char *zName,
    size_t nName,
    int *pnArg,
    Th8_MathFuncProc *pxProc,
    void **ppCtx);

/*
 * Th8_ListAppendMathFunctions --
 *	Append registered math function names to a list string,
 *	optionally filtered by a glob pattern.  Used by
 *	[info functions ?pattern?].
 */
TH8_API void Th8_ListAppendMathFunctions(
    Th8_Interp *interp,
    char **pzList,
    size_t *pnList,
    const char *zPat,
    size_t nPat);

/*
 * Th8_GlobMatch --
 *	Glob-style pattern matching.  Matches zStr against zPat
 *	using *, ?, [...], and \ metacharacters.  Returns 1 on
 *	match, 0 on mismatch.  If interp is non-NULL, Th8_Ready
 *	is called periodically for cancellation.
 */
TH8_API int Th8_GlobMatch(
    Th8_Interp *interp,
    const char *zPat,
    size_t nPat,
    const char *zStr,
    size_t nStr);

/*
 *----------------------------------------------------------------------
 *
 * Th8_Subst --
 *
 *	Perform Tcl-style substitutions on the string z (n bytes).
 *	The result is stored in the interpreter result.
 *
 *	The flags argument is a bitmask of TH8_SUBST_* constants
 *	controlling which substitution types are performed.
 *
 *	Command substitutions that return TH8_BREAK stop processing
 *	(result is everything up to the break).  TH8_CONTINUE replaces
 *	the command result with the empty string.  TH8_RETURN uses
 *	the returned value.  TH8_ERROR propagates out.
 *
 *----------------------------------------------------------------------
 */

#define TH8_SUBST_BACKSLASHES (1)
#define TH8_SUBST_COMMANDS    (2)
#define TH8_SUBST_VARIABLES   (4)
#define TH8_SUBST_ALL                                                        \
    (TH8_SUBST_BACKSLASHES | TH8_SUBST_COMMANDS | TH8_SUBST_VARIABLES)

TH8_API int Th8_Subst(Th8_Interp *interp, const char *z, size_t n, int flags);

/*
 *----------------------------------------------------------------------
 *
 * RSA Key Loading (SNK / CAPI format)
 *
 *	Parse Microsoft CAPI / .NET Strong Name Key blobs and extract
 *	RSA key components.  Gated by TH8_ENABLE_CRYPTOGRAPHY.
 *
 *----------------------------------------------------------------------
 */

#if defined(TH8_ENABLE_CRYPTOGRAPHY)

typedef struct Th8_RsaKey Th8_RsaKey;

TH8_API int Th8_RsaKeyLoad(
    Th8_Interp *interp,
    const unsigned char *zData,
    size_t nData,
    Th8_RsaKey **ppKey);

TH8_API void Th8_RsaKeyFree(Th8_Interp *interp, Th8_RsaKey *pKey);

TH8_API int Th8_RsaKeyBitLen(const Th8_RsaKey *pKey);
TH8_API int Th8_RsaKeyHasPrivate(const Th8_RsaKey *pKey);
TH8_API unsigned int Th8_RsaKeyPubExp(const Th8_RsaKey *pKey);

TH8_API const unsigned char *
Th8_RsaKeyModulus(const Th8_RsaKey *pKey, size_t *pn);
TH8_API const unsigned char *
Th8_RsaKeyPrivExp(const Th8_RsaKey *pKey, size_t *pn);
TH8_API const unsigned char *
Th8_RsaKeyPrime1(const Th8_RsaKey *pKey, size_t *pn);
TH8_API const unsigned char *
Th8_RsaKeyPrime2(const Th8_RsaKey *pKey, size_t *pn);

/*
 * Th8_RsaKeyToken --
 *
 *	Compute the .NET public key token for an RSA key.
 *	The token is the last 8 bytes of the SHA-1 hash of the
 *	CAPI public key blob, byte-reversed (little-endian).
 *	Writes exactly 8 bytes to zOut.  Returns TH8_OK.
 */
TH8_API int Th8_RsaKeyToken(
    Th8_Interp *interp,
    const Th8_RsaKey *pKey,
    unsigned char zOut[8]);

/*
 * Th8_RsaKeyTokenHex --
 *
 *	Like Th8_RsaKeyToken but returns the token as a 16-character
 *	lowercase hex string (NUL-terminated, 17 bytes).
 */
TH8_API int
Th8_RsaKeyTokenHex(Th8_Interp *interp, const Th8_RsaKey *pKey, char zOut[17]);

/*
 * Th8_HarpySigLoad --
 *
 *	Parse a Harpy .b64sig raw signature file.  Decodes the
 *	base64 body and extracts the public key token from the
 *	header.  The caller must free *ppSig and *ppToken with
 *	Th8_Free.
 */
TH8_API int Th8_HarpySigLoad(
    Th8_Interp *interp,
    const char *zData,
    size_t nData,
    unsigned char **ppSig,
    size_t *pnSig,
    char **ppToken);

/*
 * Th8_RsaVerify --
 *
 *	Verify an RSA signature (PKCS#1 v1.5, SHA-512) over zData/nData
 *	using the public key in pKey.  Returns TH8_OK if the signature
 *	is valid, TH8_ERROR otherwise.
 */
TH8_API int Th8_RsaVerify(
    Th8_Interp *interp,
    const Th8_RsaKey *pKey,
    const unsigned char *zData,
    size_t nData,
    const unsigned char *zSig,
    size_t nSig);

/*
 * Th8_RsaSign --
 *
 *	Sign data with RSA PKCS#1 v1.5 using SHA-512.  The key MUST
 *	contain a private key.  On success, *ppSig receives the raw
 *	signature (caller frees with Th8_Free).
 *
 *	Hardening: rejects keys < 2048 bits, validates all private
 *	components, performs Bellcore self-verification after signing,
 *	securely zeroes the signature on any failure path.
 */
TH8_API int Th8_RsaSign(
    Th8_Interp *interp,
    const Th8_RsaKey *pKey,
    const unsigned char *zData,
    size_t nData,
    unsigned char **ppSig,
    size_t *pnSig);

/*
 * Th8_Sha512Hex --
 *
 *	Compute the SHA-512 hash of zData/nData and write the
 *	128-character lowercase hex digest to zOut (at least 129 bytes).
 */
TH8_API int Th8_Sha512Hex(
    Th8_Interp *interp,
    const unsigned char *zData,
    size_t nData,
    char zOut[129]);

/*
 * Th8_RsaExtractHash --
 *
 *	Recover the SHA-512 hash embedded in an RSA PKCS#1 v1.5
 *	signature by performing the public-key operation and
 *	stripping the DigestInfo wrapper.  Writes the 128-character
 *	hex hash to zOut (at least 129 bytes).
 */
TH8_API int Th8_RsaExtractHash(
    Th8_Interp *interp,
    const Th8_RsaKey *pKey,
    const unsigned char *zSig,
    size_t nSig,
    char zOut[129]);

/*
 * Th8_InstallSignedPolicy --
 *
 *	Install the signed-only policy callback.  When the
 *	signed-only gate is enabled (Th8_EnableSignedOnly), every
 *	script must have a companion .b64sig file whose RSA signature
 *	is verified before evaluation proceeds.  The public key is
 *	fetched from a well-known registry keyed by the token in the
 *	signature header.  Call Th8_RemoveSignedPolicy to uninstall.
 *	If ppCtx is non-NULL, the opaque context pointer is written
 *	there for later use with Th8_RemoveSignedPolicy.
 */
TH8_API int Th8_InstallSignedPolicy(Th8_Interp *interp, void **ppCtx);

/*
 * Th8_RemoveSignedPolicy --
 *
 *	Remove the signed-only policy callback and free the
 *	context.  pCtx is the opaque context pointer obtained from
 *	the interpreter's callback state; pass NULL for a no-op.
 */
TH8_API void Th8_RemoveSignedPolicy(Th8_Interp *interp, void *pCtx);

/*
 * Th8_PolicyPreloadKey --
 *
 *	Pre-load an RSA key into the signed-only policy cache so
 *	that signature verification can proceed without fetching
 *	the key from the network.  The policy takes ownership of
 *	pKey.  pCtx is the opaque context from Th8_InstallSignedPolicy.
 */
TH8_API int
Th8_PolicyPreloadKey(Th8_Interp *interp, void *pCtx, Th8_RsaKey *pKey);

/*
 * Th8_PolicyGetKeyTokens --
 *
 *	Return a Tcl list of all public key tokens currently loaded
 *	in the policy's key cache.  The result is set in the
 *	interpreter.  Returns TH8_OK on success (even if empty).
 */
TH8_API int Th8_PolicyGetKeyTokens(Th8_Interp *interp, void *pCtx);

/*
 * Th8_PolicyFindKey --
 *
 *	Look up an RSA key by its 16-character hex public key token
 *	in the signed-only policy's key cache.  Returns a borrowed
 *	pointer to the Th8_RsaKey (owned by the policy cache -- the
 *	caller must NOT free it).  Returns NULL if not found.
 *
 *	pCtx is the opaque policy context from Th8_InstallSignedPolicy
 *	or Th8_EnableSignedPolicy.
 */
TH8_API const Th8_RsaKey *Th8_PolicyFindKey(
    Th8_Interp *interp,
    void *pCtx,
    const char *zToken,
    size_t nToken);

/*
 * Th8_EnableSignedPolicy --
 *
 *	One-call convenience: install the signed-only policy, preload
 *	all embedded keys (keyRoot and key0), and enable the gate.
 *	On success, *ppCtx is set to the opaque context for later use
 *	with Th8_RemoveSignedPolicy.  On failure, any partial state
 *	is cleaned up and TH8_ERROR is returned.
 */
TH8_API int
Th8_EnableSignedPolicy(Th8_Interp *interp, void **ppCtx, int bEnable);

/*
 * Th8_EvalFileAndRsaKeyLoad --
 *
 *	Load an RSA public key by evaluating a signed script file in
 *	a child interpreter.  Combines Th8_EvalFileAsData (base64
 *	decode) and Th8_RsaKeyLoad (SNK/CAPI parse).  If bPreload
 *	is non-zero, the key is preloaded into the policy cache via
 *	Th8_PolicyPreloadKey (pCtx must be the policy context).
 */
TH8_API int Th8_EvalFileAndRsaKeyLoad(
    Th8_Interp *interp,
    const char *zName,
    size_t nName,
    void *pCtx,
    int bPreload);

/*
 * Th8_GetEmbeddedKeyRoot --
 *
 *	Returns a pointer to the embedded root key data and optionally
 *	its size.  Pass NULL for pnData if the size is not needed.
 */
TH8_API const unsigned char *Th8_GetEmbeddedKeyRoot(size_t *pnData);

/*
 * Th8_GetEmbeddedKey0 --
 *
 *	Returns a pointer to the embedded key0 data (the built-in
 *	public key) and optionally its size.  Pass NULL for pnData
 *	if the size is not needed.
 */
TH8_API const unsigned char *Th8_GetEmbeddedKey0(size_t *pnData);

/*
 * Th8_GetPublicKeyZero --
 *
 *	Load the embedded key0 as a parsed Th8_RsaKey.  The returned
 *	key is cached in the policy context and must NOT be freed by
 *	the caller.  Returns NULL on parse failure.
 */
TH8_API Th8_RsaKey *Th8_GetPublicKeyZero(Th8_Interp *interp);

/*
 * Th8_GetPublicKeyZeroToken --
 *
 *	Returns the 16-character hex public key token for the
 *	embedded key0.  Writes to zOut (must be at least 17 bytes).
 *	Returns TH8_OK on success.
 */
TH8_API int Th8_GetPublicKeyZeroToken(Th8_Interp *interp, char zOut[17]);

/*
 * Th8_GetPublicKeyRoot --
 *
 *	Load the embedded keyRoot as a parsed Th8_RsaKey.  The
 *	returned key is cached and must NOT be freed by the caller.
 *	Returns NULL on parse failure.
 */
TH8_API Th8_RsaKey *Th8_GetPublicKeyRoot(Th8_Interp *interp);

/*
 * Th8_GetPublicKeyRootToken --
 *
 *	Returns the 16-character hex public key token for the
 *	embedded keyRoot.  Writes to zOut (must be at least 17 bytes).
 *	Returns TH8_OK on success.
 */
TH8_API int Th8_GetPublicKeyRootToken(Th8_Interp *interp, char zOut[17]);

/*
 * Th8_GetEmbeddedKeyTime --
 *	Returns a pointer to the embedded keyTime data (the time
 *	server signing key) and optionally its size.
 */
TH8_API const unsigned char *Th8_GetEmbeddedKeyTime(size_t *pnData);

/*
 * Th8_KeyringEntry --
 *	Single entry in the embedded trusted-root keyring.  Each entry
 *	is a raw SNK public key blob (the same format consumed by
 *	Th8_RsaKeyLoad and produced by Th8_GetEmbeddedKey0 / KeyRoot /
 *	KeyTime / KeyTest).
 *
 *	The keyring is a per-embedder collection of trust anchors.
 *	The canonical TH8 build links an empty stub
 *	(`src/crypto/th8_keyring_stub.c`).  Embedders that need real
 *	trust anchors generate their own keyring source file using:
 *
 *	    tclsh tools/mkkey.tcl --keyring NAME key1.snk key2.snk ...
 *
 *	and either replace the stub in the link or define
 *	`TH8_OMIT_KEYRING_STUB` at compile time and link the generated
 *	file alongside the amalgamation.
 */
typedef struct Th8_KeyringEntry Th8_KeyringEntry;
struct Th8_KeyringEntry {
    const unsigned char *zData; /* Raw SNK key blob bytes. */
    size_t nData; /* Blob size in bytes. */
};

/*
 * Th8_GetEmbeddedKeyring --
 *	Return the array of trusted-root keys embedded in this build.
 *	The canonical stub returns NULL with *pnEntries=0; embedder-
 *	supplied keyrings return their populated array.
 *
 *	Calling sequence:
 *	    size_t nEntries = 0;
 *	    const Th8_KeyringEntry *ring = Th8_GetEmbeddedKeyring(&nEntries);
 *	    for (size_t i = 0; i < nEntries; i++) {
 *	        ... use ring[i].zData / ring[i].nData ...
 *	    }
 *
 *	pnEntries must not be NULL.
 */
TH8_API const Th8_KeyringEntry *Th8_GetEmbeddedKeyring(size_t *pnEntries);

/*
 * Th8_GetEmbeddedKeyTest --
 *	Returns a pointer to the embedded 2048-bit test signing key
 *	data.  This key is only available when TH8_ENABLE_TEST_KEY
 *	is defined at compile time.  It is used for development and
 *	testing of the signed-only script policy.
 *	This key must NEVER be used in production.
 */
#  if defined(TH8_ENABLE_TEST_KEY)
/*
 * Th8_GetEmbeddedKeyTest --
 *	Returns a pointer to the embedded 2048-bit test signing key
 *	data.  Only available when TH8_ENABLE_TEST_KEY is defined.
 *	This key must NEVER be used in production.
 */
TH8_API const unsigned char *Th8_GetEmbeddedKeyTest(size_t *pnData);

/*
 * Th8_GetPublicKeyTest --
 *	Load the embedded test key as a parsed Th8_RsaKey.  The
 *	returned key is cached and must NOT be freed by the caller.
 *	Returns NULL on parse failure.
 *	Only available when TH8_ENABLE_TEST_KEY is defined.
 */
TH8_API Th8_RsaKey *Th8_GetPublicKeyTest(Th8_Interp *interp);

/*
 * Th8_GetPublicKeyTestToken --
 *	Returns the 16-character hex public key token for the
 *	embedded test key.  Writes to zOut (must be at least 17
 *	bytes).  Returns TH8_OK on success.
 *	Only available when TH8_ENABLE_TEST_KEY is defined.
 */
TH8_API int Th8_GetPublicKeyTestToken(Th8_Interp *interp, char zOut[17]);
#  endif

#endif /* TH8_ENABLE_CRYPTOGRAPHY */


/*
 *----------------------------------------------------------------------
 *
 * OpenBSD Security (pledge / unveil)
 *
 *	On OpenBSD, these functions restrict the process's system call
 *	and filesystem access.  On other platforms they are no-ops that
 *	return TH8_OK.
 *
 *----------------------------------------------------------------------
 */

/*
 * Th8_Pledge --
 *
 *	Restrict the process to the given set of pledge promises.
 *	Returns TH8_OK on success, TH8_ERROR on failure.
 *	On non-OpenBSD platforms, always returns TH8_OK.
 */
TH8_API int Th8_Pledge(
    Th8_Interp *interp,
    const char *zPromises,
    const char *zExecPromises);

/*
 * Th8_Unveil --
 *
 *	Reveal a filesystem path with the given permissions.
 *	Pass NULL, NULL to lock the unveil set.
 *	Returns TH8_OK on success, TH8_ERROR on failure.
 *	On non-OpenBSD platforms, always returns TH8_OK.
 */
TH8_API int
Th8_Unveil(Th8_Interp *interp, const char *zPath, const char *zPermissions);

/* Th8_GetMathFuncHash -- declared below, after Th8_Hash is defined. */

/* ====================================================================
 * Section: Result Management
 * ==================================================================== */

/*
 * Th8_SetResult --
 *	Set the interpreter result to a copy of z (n bytes, or
 *	TH8_NOLEN).  The interpreter takes ownership of the copy.
 *	Returns TH8_OK.
 */
TH8_API int Th8_SetResult(Th8_Interp *interp, const char *z, size_t n);

/*
 * Th8_SetResultStatic --
 *	Set the interpreter result to a static string literal.  The
 *	string is NOT copied and must outlive the interpreter.  This
 *	function never allocates memory and cannot fail.  Use for
 *	error messages in out-of-memory paths.
 */
TH8_API void Th8_SetResultStatic(Th8_Interp *interp, const char *z, size_t n);

/*
 * Th8_ClearResult --
 *	Reset the interpreter result to NULL (no result).  Frees any
 *	previously allocated result string.  After this call,
 *	Th8_GetResult returns "" with length 0, but internally the
 *	result pointer is NULL -- semantically distinct from an empty
 *	string set by Th8_SetResult(interp, "", 0).
 *
 *	This is the idiomatic way for commands that produce no
 *	meaningful return value (e.g., DOM mutations) to leave the
 *	interpreter in a clean state.
 */
TH8_API void Th8_ClearResult(Th8_Interp *interp);

/*
 * Th8_GetResult --
 *	Return a pointer to the current interpreter result string.
 *	If pN is non-NULL, *pN is set to the byte length.  The returned
 *	pointer is valid until the next operation that modifies the
 *	result.  The caller must NOT free the returned pointer.
 */
TH8_API const char *Th8_GetResult(Th8_Interp *interp, size_t *pN);

/*
 * Th8_TakeResult --
 *	Transfer ownership of the interpreter result buffer to the
 *	caller.  The interpreter result is cleared.  The caller is
 *	responsible for freeing the returned buffer via Th8_Free.
 *	If pN is non-NULL, *pN is set to the byte length.  Returns
 *	NULL if the result is empty.
 *
 *	REFUSED for sensitive results: if Th8_IsResultSensitive(interp)
 *	is true, this function returns NULL and sets the interpreter
 *	result to an error.  Sensitive results MUST be consumed
 *	in-place via Th8_GetResult; ownership transfer would copy
 *	plaintext out of the protected backing region.
 */
TH8_API char *Th8_TakeResult(Th8_Interp *interp, size_t *pN);

/*
 * Th8_IsResultSensitive --
 *	Return non-zero if the interpreter result is currently marked
 *	sensitive (set via Th8_MarkResultSensitive or
 *	Th8_SetResultSensitive on crypto-enabled builds).  Used by
 *	callers that must avoid copying sensitive plaintext out of
 *	the interpreter.  Always available; returns 0 on non-crypto
 *	builds where sensitive results cannot be produced.
 */
TH8_API int Th8_IsResultSensitive(Th8_Interp *interp);

#if defined(TH8_ENABLE_CRYPTOGRAPHY)
/*
 * Th8_MarkResultSensitive --
 *	Mark the current interpreter result as containing sensitive
 *	plaintext (e.g. decrypted secret material).  When the result
 *	is later overwritten or cleared, the old buffer SHALL be
 *	securely zeroed before being freed.  Idempotent.
 *
 *	This call only sets a flag; it does not move the result into
 *	protected memory.  Use Th8_SetResultSensitive for full
 *	protection (mlock + guard pages + in-place enforcement).
 *	Available only when TH8 is built with cryptography support.
 */
TH8_API void Th8_MarkResultSensitive(Th8_Interp *interp);

/*
 * Th8_SetResultSensitive --
 *	Set the interpreter result to z (n bytes), copying the bytes
 *	into a per-interpreter mlock'd, guard-paged backing region
 *	(an internal Th8_ProtectedRegion).  The result is automatically
 *	marked sensitive: subsequent overwrites or clears SHALL
 *	securely zero the protected region's data area in place
 *	rather than freeing it (the region is reused for the lifetime
 *	of the interpreter and freed only when the interpreter is
 *	destroyed).
 *
 *	The protected region's data page is one OS page minus an
 *	8-byte canary; n MUST fit (typically n <= 4088 on 4 KB-page
 *	systems).  If n exceeds the available capacity, this function
 *	returns TH8_ERROR with a descriptive error result.  If the
 *	protected region cannot be allocated (mlock failure, no
 *	memory), this function also returns TH8_ERROR.
 *
 *	Available only when TH8 is built with cryptography support
 *	(TH8_ENABLE_CRYPTOGRAPHY).
 *
 *	Returns TH8_OK on success, TH8_ERROR on failure.
 */
TH8_API int
Th8_SetResultSensitive(Th8_Interp *interp, const char *z, size_t n);
#endif /* TH8_ENABLE_CRYPTOGRAPHY */

/*
 * Th8_ErrorMessage --
 *	Set the interpreter result to an error message composed of
 *	the NUL-terminated prefix zPre followed by z (n bytes, or
 *	TH8_NOLEN).  Convenience wrapper for common error formatting.
 *	Returns TH8_ERROR, so callers can write:
 *	  return Th8_ErrorMessage(interp, "bad value: ", z, n);
 */
TH8_API int Th8_ErrorMessage(
    Th8_Interp *interp,
    const char *zPre,
    const char *z,
    size_t n);

/*
 * Th8_SetResultInt --
 *	Set the interpreter result to the decimal string representation
 *	of iVal.  Returns TH8_OK.
 */
TH8_API int Th8_SetResultInt(Th8_Interp *interp, int iVal);

/*
 * Th8_SetResultWideInt --
 *	Set the interpreter result to the decimal string representation
 *	of the 64-bit integer wVal.  Returns TH8_OK.
 */
TH8_API int Th8_SetResultWideInt(Th8_Interp *interp, th8_int64_t wVal);

/*
 * Th8_SetResultDouble --
 *	Set the interpreter result to the string representation of the
 *	double-precision floating-point value rVal.  Returns TH8_OK.
 */
TH8_API int Th8_SetResultDouble(Th8_Interp *interp, double rVal);

/* ====================================================================
 * Section: Error Tracking
 * ==================================================================== */

/*
 * Th8_GetErrorLine --
 *	Return the 1-based source line number where the most recent
 *	error was raised.  Returns 0 if no error has occurred.
 */
TH8_API int Th8_GetErrorLine(Th8_Interp *interp);

/*
 * Th8_GetEvalDepth --
 *	Return the current script evaluation nesting depth.
 *	0 = no eval is active; 1 = outermost eval; >1 = nested.
 */
TH8_API int Th8_GetEvalDepth(Th8_Interp *interp);

/*
 * Th8_SetErrorLine --
 *	Set the error line number.  Normally called by the evaluator
 *	internals; host code may call this when synthesizing errors.
 */
TH8_API void Th8_SetErrorLine(Th8_Interp *interp, int nLine);

/*
 * Th8_GetLine --
 *	Return the 1-based source line number currently being evaluated.
 */
TH8_API int Th8_GetLine(Th8_Interp *interp);

/* ====================================================================
 * Section: Resource Limits
 * ==================================================================== */

/*
 * Th8_SetStepLimit --
 *	Set the maximum number of steps allowed.  Pass 0 to disable
 *	step counting (no limit).
 */
TH8_API void Th8_SetStepLimit(Th8_Interp *interp, th8_int64_t nLimit);

/*
 * Th8_GetStepLimit --
 *	Return the current step limit.  Returns 0 if step counting
 *	is disabled.
 */
TH8_API th8_int64_t Th8_GetStepLimit(Th8_Interp *interp);

/*
 * Th8_GetStepCount --
 *	Return the number of steps executed since the last reset.
 */
TH8_API th8_int64_t Th8_GetStepCount(Th8_Interp *interp);

/*
 * Th8_ResetStepCount --
 *	Reset the step counter to zero without changing the limit.
 */
TH8_API void Th8_ResetStepCount(Th8_Interp *interp);

/*
 * Th8_SetStepCount --
 *	Set the step counter to an explicit value, without changing
 *	the limit.  Useful for embedders that want to stage the counter
 *	at a specific boundary (e.g. just-below-limit) so a subsequent
 *	step exhaustion fires deterministically.  Pass any value in
 *	[0, INT64_MAX]; negative values are clamped to 0.
 */
TH8_API void Th8_SetStepCount(Th8_Interp *interp, th8_int64_t nCount);

/*
 * Th8_SetResultLimit --
 *	Set the maximum byte size of any interpreter result.  Pass 0
 *	to disable (the default limit is TH8_MX_STRLEN).
 */
TH8_API void Th8_SetResultLimit(Th8_Interp *interp, size_t nLimit);

/*
 * Th8_GetResultLimit --
 *	Return the current result size limit.  Returns 0 if disabled.
 */
TH8_API size_t Th8_GetResultLimit(Th8_Interp *interp);

#if defined(TH8_ENABLE_BIGINT)
/*
 * Th8_EnableBigint --
 *	Enable or disable arbitrary precision integer support.
 *
 *	When enabled (bEnable non-zero), integer overflow in [expr]
 *	promotes to bigint via libtommath.  When disabled (bEnable
 *	zero), overflow either errors (if overflow check is on) or
 *	wraps (if overflow check is off).
 *
 *	Uses a random token gate so that a single-bit corruption
 *	cannot enable the feature.  Only the C embedder can toggle
 *	this; scripts cannot.
 */
TH8_API int Th8_EnableBigint(Th8_Interp *interp, int bEnable);

/*
 * Th8_IsBigintEnabled --
 *	Returns 1 if bigint is enabled for this interpreter, 0 if
 *	disabled or not compiled in.
 */
TH8_API int Th8_IsBigintEnabled(Th8_Interp *interp);
#endif


/* ====================================================================
 * Section: Expression-grammar features (opt-in extensions)
 *
 * TH8 in default configuration accepts EXACTLY the Tcl 8.6 expr(n)
 * grammar (https://www.tcl-lang.org/man/tcl8.6/TclCmd/expr.htm).
 * The following API enables opt-in extensions that go beyond strict
 * compliance.  Every extension is OFF by default; the embedder must
 * opt in by calling Th8_SetExprFeatures.  This API is NOT exposed as
 * a script-level command -- the strict-vs-extended decision belongs
 * to the embedder, not the script author.
 *
 * Flag bit values are STABLE across releases: once shipped, a name
 * always maps to the same bit value, so embedder code referencing
 * TH8_EXPR_TOP_COMMA continues to work after upgrades.
 * ==================================================================== */

/*
 * TH8_EXPR_NONE --
 *	No extensions enabled.  Strict Tcl 8.6 expr(n) compliance.
 *	This is the default state for a freshly created interpreter.
 */
#define TH8_EXPR_NONE 0x0000

/*
 * TH8_EXPR_TOP_COMMA --
 *	Top-level-only `,` sequence operator.  When set, an
 *	expression of the form "a, b, c" is evaluated left-to-right
 *	and yields the value of the LAST sub-expression.  The comma
 *	is a SEPARATOR at the TOP of the expression only -- it is
 *	NOT a binary operator and may NOT appear inside parentheses,
 *	ternary operands, or any other scope without parens, where
 *	it remains a syntax error.  Function-call argument commas
 *	(`pow(x, y)`) are unaffected by this flag.
 *
 *	Diverges from expr(n).  Documented as a TH8 extension.
 */
#define TH8_EXPR_TOP_COMMA 0x0001

/*
 * TH8_EXPR_VAR_ASSIGN --
 *	`:=` variable-assignment operator with QUOTED-STRING LHS.
 *	When set, an expression like `"x" := 5` (or `{x} := 5`,
 *	`$nameVar := 5`, `[set name x] := 5`) sets the variable
 *	whose name is the LHS's substituted value to the RHS value
 *	and yields the assigned value.  Right-associative, so
 *	`"a" := "b" := 1` sets both `a` and `b` to 1.
 *
 *	Bare-word LHS (e.g. `x := 5`) is NOT supported and remains
 *	a parse error -- the bareword-rejection security envelope
 *	is preserved.  The LHS must be a regular expr(n) operand.
 *
 *	Diverges from expr(n).  Documented as a TH8 extension.
 */
#define TH8_EXPR_VAR_ASSIGN 0x0002

/*
 * TH8_EXPR_ALL --
 *	All defined expression-feature flags.  Reserved for future
 *	expansion -- adding a new flag automatically extends this
 *	mask.  Useful for embedders that want every available
 *	extension and accept that the set may grow over time.
 */
#define TH8_EXPR_ALL (TH8_EXPR_TOP_COMMA | TH8_EXPR_VAR_ASSIGN)

/*
 * Th8_GetExprFeatures --
 *	Return the current expression-feature flag set for the
 *	given interpreter.  A return value of TH8_EXPR_NONE (0)
 *	means strict Tcl 8.6 expr(n) compliance.
 */
TH8_API int Th8_GetExprFeatures(Th8_Interp *interp);

/*
 * Th8_SetExprFeatures --
 *	Set the expression-feature flag set for an interpreter.
 *	`flags` is a bitwise-OR of TH8_EXPR_* constants describing
 *	what the embedder wants enabled.  Passing TH8_EXPR_NONE (0)
 *	disables all features (returning to strict expr(n)).
 *
 *	Returns the PREVIOUS flag set, so callers can implement
 *	scoped enable/disable as save-and-restore around a region:
 *
 *		int saved = Th8_SetExprFeatures(interp,
 *		    TH8_EXPR_TOP_COMMA | TH8_EXPR_VAR_ASSIGN);
 *		Th8_Eval(interp, 0, trusted_script, ...);
 *		Th8_SetExprFeatures(interp, saved);
 *
 *	Unrecognised bits in `flags` (bits not corresponding to any
 *	defined TH8_EXPR_* constant) are silently masked off so
 *	that future-released embedder code calling an older TH8
 *	does not accidentally enable a feature the older library
 *	does not understand.
 */
TH8_API int Th8_SetExprFeatures(Th8_Interp *interp, int flags);


/*
 * Th8_SetOverflowCheck --
 *	Enable or disable integer overflow checking in [expr].
 *	bEnable=1 (default): overflow returns TH8_ERROR.
 *	bEnable=0: overflow wraps silently (Tcl 8.x compatible).
 */
TH8_API void Th8_SetOverflowCheck(Th8_Interp *interp, int bEnable);

/*
 * Th8_GetOverflowCheck --
 *	Returns 1 if overflow checking is enabled, 0 if disabled.
 */
TH8_API int Th8_GetOverflowCheck(Th8_Interp *interp);

/*
 * Th8_SetAllocLimit --
 *	Set a per-interpreter memory allocation ceiling in bytes.
 *	When the limit is reached, Th8_Malloc calls xPanic.
 *	Set to 0 for unlimited (default).
 *
 *	For sandbox use, a reasonable limit is 16-64 MB.
 */
TH8_API void Th8_SetAllocLimit(Th8_Interp *interp, size_t nLimit);

/*
 * Th8_GetAllocLimit --
 *	Returns the current allocation limit (0 = unlimited).
 */
TH8_API size_t Th8_GetAllocLimit(Th8_Interp *interp);

/*
 * Th8_GetAllocBytes --
 *	Returns the current total bytes allocated by this interp.
 */
TH8_API size_t Th8_GetAllocBytes(Th8_Interp *interp);

/* ====================================================================
 * Section: Binary Loading
 * ==================================================================== */

#if defined(TH8_ENABLE_LOAD)
/*
 * Th8_EnableLoad --
 *	Enable or disable the xLoad binary loading callback.
 *
 *	bEnable non-zero: generates a fresh 64-bit random token
 *	from xRandomBytes and stores it as both nLoadToken and
 *	nLoadOk.  The token is regenerated on every enable call
 *	so that a previously observed token cannot be replayed.
 *	Values 0, 1, and all-ones are rejected and retried.
 *
 *	bEnable zero: clears both nLoadOk and nLoadToken to 0,
 *	fully disabling xLoad.  The old token is destroyed so
 *	it cannot be reused if loading is re-enabled later.
 *
 *	Returns TH8_OK on success.  Returns TH8_ERROR if
 *	xRandomBytes is NULL (a secure entropy source is
 *	required for the load gate -- loading cannot be enabled
 *	without one).
 *
 *	Security properties:
 *	  - A single-bit memory corruption cannot enable loading
 *	    (the full 64-bit random token must match).
 *	  - nLoadToken and nLoadOk are stored in separate,
 *	    distant regions of the Th8_Interp struct so a
 *	    linear buffer overflow cannot corrupt both.
 *	  - Only the C embedder can call this; scripts cannot
 *	    enable loading by themselves.
 */
TH8_API int Th8_EnableLoad(Th8_Interp *interp, int bEnable);
#endif


#if defined(TH8_ENABLE_LOAD)
/*
 * Th8_IsLoadEnabled --
 *	Returns non-zero if binary loading is currently enabled
 *	(nLoadOk is non-zero and matches nLoadToken).
 */
TH8_API int Th8_IsLoadEnabled(Th8_Interp *interp);
#endif


/*
 * Th8_EnableSignedOnly --
 *	Enable or disable the signed-only script evaluation mode.
 *	When enabled, the policy callback should reject scripts
 *	that are not signed with a trusted key.
 *
 *	Uses the same random-token gate pattern as Th8_EnableLoad.
 */
TH8_API int Th8_EnableSignedOnly(Th8_Interp *interp, int bEnable);

/*
 * Th8_IsSignedOnlyEnabled --
 *	Returns non-zero if signed-only mode is enabled.
 */
TH8_API int Th8_IsSignedOnlyEnabled(Th8_Interp *interp);

/*
 * Th8_SaveSignedOnly / Th8_RestoreSignedOnly --
 *	Save and restore the full signed-only policy state: gate
 *	tokens and policy callback.  Used to temporarily bypass
 *	policy enforcement (e.g., when reading auxiliary files
 *	that are not themselves signed) or to install a temporary
 *	policy context without clobbering the outer one.
 *	pSaved is an opaque buffer of at least TH8_SIGNED_SAVE_SIZE
 *	bytes provided by the caller.
 */
#define TH8_SIGNED_SAVE_SIZE 48
TH8_API void Th8_SaveSignedOnly(Th8_Interp *interp, void *pSaved);
TH8_API void Th8_RestoreSignedOnly(Th8_Interp *interp, const void *pSaved);

/*
 *----------------------------------------------------------------------
 *
 * System Variables
 *
 *	System variables are read-only from scripts.  The C API
 *	can read and write them freely.  Script commands (set,
 *	unset, append, incr, lappend, array set) check the guard
 *	and return an error for system variables.
 *
 *----------------------------------------------------------------------
 */

/*
 * Th8_DeclareSystemVar --
 *	Mark a variable name as a system variable.  For arrays,
 *	declare the base name (e.g. "th8_security") and all
 *	elements are automatically protected.
 */
TH8_API int
Th8_DeclareSystemVar(Th8_Interp *interp, const char *zName, size_t nName);

/*
 * Th8_IsSystemVar --
 *	Returns non-zero if the variable name (or its array base
 *	name) is a system variable.
 */
TH8_API int
Th8_IsSystemVar(Th8_Interp *interp, const char *zName, size_t nName);

#if defined(TH8_ENABLE_LOAD)
/*
 * Th8_Load --
 *	Load a binary into the interpreter.  Checks the load gate,
 *	calls xLoad, and records the library in the tracking list.
 */
TH8_API int Th8_Load(
    Th8_Interp *interp,
    const char *zName,
    size_t nName,
    const char *zProc,
    size_t nProc);
#endif


/*
 * Unload gate flags for Th8_EnableUnload --
 *
 *	TH8_UNLOAD_OK enables the [unload] command itself.
 *	TH8_UNLOAD_DANGEROUS enables actual library closing
 *	(dlclose/FreeLibrary) via the -nokeeplibrary option.
 *	The default [unload] behavior (-keeplibrary) only
 *	calls the _Unload entry point without closing the
 *	library, which is always safe.
 */
#define TH8_UNLOAD_OK        (1)
#define TH8_UNLOAD_DANGEROUS (2)

/*
 * Callback flags for the Pkg_Unload entry point --
 *
 *	These flags are passed to the extension's _Unload function
 *	to indicate the reason for unloading.  They follow the same
 *	bit layout as the Tcl equivalents for source compatibility,
 *	with TH8-specific extensions in the upper bits.
 *
 *	TH8_UNLOAD_DETACH_FROM_INTERPRETER (1<<0)
 *	  The extension is being detached from this interpreter
 *	  only (-keeplibrary).  The library remains mapped in
 *	  the process; another interpreter may still use it.
 *	  The extension should remove its commands and free
 *	  per-interpreter state.
 *
 *	TH8_UNLOAD_DETACH_FROM_PROCESS (1<<1)
 *	  The extension is being detached from the entire process
 *	  (-nokeeplibrary).  The library will be dlclose'd /
 *	  FreeLibrary'd after this call returns.  The extension
 *	  should free ALL state, including process-global state.
 *
 *	TH8_UNLOAD_FROM_INIT (1<<2)
 *	  The _Unload function is being called from the _Init
 *	  function itself to clean up after a partial initialization
 *	  failure.  The extension should undo whatever _Init did
 *	  before the failure.
 *
 *	TH8_UNLOAD_FROM_CMD_DELETE (1<<3)
 *	  The _Unload function is being called because the
 *	  extension's primary command (or its containing
 *	  interpreter) is being deleted.  The extension should
 *	  clean up as if detaching from the interpreter.
 */
#define TH8_UNLOAD_DETACH_FROM_INTERPRETER (1 << 0)
#define TH8_UNLOAD_DETACH_FROM_PROCESS     (1 << 1)
#define TH8_UNLOAD_FROM_INIT               (1 << 2)
#define TH8_UNLOAD_FROM_CMD_DELETE         (1 << 3)

#if defined(TH8_ENABLE_LOAD)
/*
 * Th8_EnableUnload --
 *	Enable or disable the [unload] command.  The flags
 *	argument is a bitmask of TH8_UNLOAD_OK and/or
 *	TH8_UNLOAD_DANGEROUS.  Passing 0 disables unloading
 *	entirely.  Each call regenerates the security token.
 *	Returns TH8_OK on success, TH8_ERROR if the entropy
 *	source (xRandomBytes) is unavailable.
 */
TH8_API int Th8_EnableUnload(Th8_Interp *interp, int flags);
#endif


#if defined(TH8_ENABLE_LOAD)
/*
 * Th8_Unload --
 *	Unload a previously loaded binary.  Checks the unload gate,
 *	calls xUnload, and removes from the tracking list.
 *	bClose=0 for -keeplibrary, bClose=1 for -nokeeplibrary.
 */
TH8_API int Th8_Unload(
    Th8_Interp *interp,
    const char *zName,
    size_t nName,
    const char *zProc,
    size_t nProc,
    int bClose);
#endif


/* ====================================================================
 * Section: Callbacks
 * ==================================================================== */

/*
 * Th8_SetPolicyCallback --
 *	Register or remove the unified policy callback that is
 *	invoked before and after both script evaluations and data
 *	reads.  The callback receives a phase bitmask combining
 *	a timing bit (TH8_PHASE_PRE or TH8_PHASE_POST) with an
 *	operation bit (TH8_PHASE_EVAL or TH8_PHASE_READ).  Only
 *	one callback may be active at a time; calling this again
 *	replaces the previous callback.  Pass xProc==NULL to
 *	remove the callback.  pCtx is passed through unchanged.
 */
TH8_API void
Th8_SetPolicyCallback(Th8_Interp *interp, Th8_PolicyProc xProc, void *pCtx);

#if defined(TH8_ENABLE_LOAD)
/*
 * Th8_SetPreLoadCallback --
 *	Register a callback that is invoked before each [load]
 *	operation.  The callback receives the library name and
 *	can veto the load by returning TH8_ERROR.  This provides
 *	fine-grained control over which libraries may be loaded,
 *	complementing the binary Th8_EnableLoad gate.
 *	Pass xProc==NULL to remove the callback.
 */
TH8_API void
Th8_SetPreLoadCallback(Th8_Interp *interp, Th8_PreLoadProc xProc, void *pCtx);
#endif

/*
 * Th8_GetPolicyCallback --
 *	Retrieve the current policy callback and context.
 *	Either output pointer may be NULL if not needed.
 */
TH8_API void Th8_GetPolicyCallback(
    Th8_Interp *interp,
    Th8_PolicyProc *pxProc,
    void **ppCtx);

/* ====================================================================
 * Section: Frames
 * ==================================================================== */

/*
 * Th8_GetFrameObjv --
 *	Retrieve the argument vector for frame level iLevel.  On success,
 *	sets *pArgc, *pArgv, and *pArgl to the values stored by
 *	th8SetFrameObjv for that frame.  Returns TH8_OK on success,
 *	TH8_ERROR if the frame level is invalid or has no stored objv.
 */
TH8_API int Th8_GetFrameObjv(
    Th8_Interp *interp,
    int iLevel,
    int *pArgc,
    const char ***pArgv,
    size_t **pArgl);

/* ====================================================================
 * Section: Strings & Lists
 * ==================================================================== */

/*
 * Th8_StringAppend --
 *	Append zApp (nApp bytes, or TH8_NOLEN) to the string at
 *	*pzStr / *pnStr.  The buffer is grown via xRealloc as needed.
 *	Taint is propagated from zApp to the result.  Returns TH8_OK
 *	on success, TH8_ERROR on failure (e.g., result size limit
 *	exceeded).
 */
TH8_API int Th8_StringAppend(
    Th8_Interp *interp,
    char **pzStr,
    size_t *pnStr,
    const char *zApp,
    size_t nApp);

/*
 * Th8_ListAppend --
 *	Append a single element zElem (nElem bytes, or TH8_NOLEN) to the
 *	list string at *pzList / *pnList.  The list is grown as needed via
 *	xRealloc.  Proper Tcl list quoting is applied.  Returns TH8_OK
 *	on success, TH8_ERROR on failure.
 */
TH8_API int Th8_ListAppend(
    Th8_Interp *interp,
    char **pzList,
    size_t *pnList,
    const char *zElem,
    size_t nElem);

/*
 * List split flags for Th8_SplitList.
 *
 *	TH8_LIST_NONE      -- Default behavior (use IR cache).
 *	TH8_LIST_NO_CACHE  -- Skip the IR cache entirely.  Use this
 *	                      when the input string is temporary and
 *	                      will be freed after the split; caching
 *	                      it would leave dangling element pointers
 *	                      in the cache.
 */

#define TH8_LIST_NONE     (0)
#define TH8_LIST_NO_CACHE (1)

/*
 * Th8_SplitList --
 *	Parse the string zList (nList bytes, or TH8_NOLEN) as a Tcl
 *	list and split it into elements.  On success, sets *pazElem to
 *	a newly-allocated array of element strings, *panElem to a
 *	parallel array of element lengths, and *pnCount to the number
 *	of elements.  Returns TH8_OK on success, TH8_ERROR on malformed
 *	list syntax.  The caller must free *pazElem (and the strings it
 *	points to) and *panElem with Th8_Free when done.
 *
 *	Pass TH8_LIST_NONE for flags in the common case.  Pass
 *	TH8_LIST_NO_CACHE when zList points to memory that will be
 *	freed after the call returns.
 */
TH8_API int Th8_SplitList(
    Th8_Interp *interp,
    const char *zList,
    size_t nList,
    char ***pazElem,
    size_t **panElem,
    int *pnCount,
    int flags);

/* ====================================================================
 * Section: Type Conversion
 * ==================================================================== */

/*
 * Th8_ToInt --
 *	Parse z (n bytes, or TH8_NOLEN) as a decimal, hexadecimal (0x),
 *	octal (0o), or binary (0b) integer and store the result in
 *	*piVal.  Returns TH8_OK on success, TH8_ERROR if the string
 *	is not a valid integer.
 */
TH8_API int
Th8_ToInt(Th8_Interp *interp, const char *z, size_t n, int *piVal);

/*
 * Th8_ToBoolean --
 *	Convert a string to a boolean (0 or 1).  Accepts integers
 *	(0 = false, nonzero = true) and the strings true/false,
 *	yes/no, on/off (case-insensitive).
 */
TH8_API int
Th8_ToBoolean(Th8_Interp *interp, const char *z, size_t n, int *pbVal);

/*
 * Th8_ToWideInt --
 *	Like Th8_ToInt but stores the result in a 64-bit integer *pwVal.
 *	Returns TH8_OK on success, TH8_ERROR on parse failure or overflow.
 */
TH8_API int Th8_ToWideInt(
    Th8_Interp *interp,
    const char *z,
    size_t n,
    th8_int64_t *pwVal);

/*
 * Th8_ToDouble --
 *	Parse z (n bytes, or TH8_NOLEN) as a floating-point number and
 *	store the result in *prVal.  Returns TH8_OK on success, TH8_ERROR
 *	if the string is not a valid double.
 */
TH8_API int
Th8_ToDouble(Th8_Interp *interp, const char *z, size_t n, double *prVal);

/* ====================================================================
 * Section: Introspection
 * ==================================================================== */

/*
 * Th8_ListAppendCommands --
 *	Append all command names in the interpreter to the list at
 *	*pz / *pn.  Returns TH8_OK.
 */
TH8_API int Th8_ListAppendCommands(Th8_Interp *interp, char **pz, size_t *pn);

/*
 * Th8_ListAppendCommandsMatching --
 *	Like Th8_ListAppendCommands, but only appends commands whose
 *	implementation function is xMatch1 or xMatch2.  This allows
 *	filtering by command type (e.g., listing only built-in commands
 *	vs. user-defined procs).
 */
TH8_API int Th8_ListAppendCommandsMatching(
    Th8_Interp *interp,
    char **pz,
    size_t *pn,
    Th8_CommandProc xMatch1,
    Th8_CommandProc xMatch2);

/*
 * Th8_ListAppendVariables --
 *	Append all variable names in the current frame to the list at
 *	*pz / *pn.  Returns TH8_OK.
 */
TH8_API int
Th8_ListAppendVariables(Th8_Interp *interp, char **pz, size_t *pn);

/*
 * Th8_ListAppendNsVariables --
 *	Append all variable names in the named namespace to the list.
 *	If zNs is NULL or empty, uses the current namespace.
 */
TH8_API int Th8_ListAppendNsVariables(
    Th8_Interp *interp,
    const char *zNs,
    size_t nNs,
    char **pz,
    size_t *pn);

/*
 * Th8_ListAppendGlobalVariables --
 *	Like Th8_ListAppendVariables but iterates the global frame.
 */
TH8_API int
Th8_ListAppendGlobalVariables(Th8_Interp *interp, char **pz, size_t *pn);

/*
 * Th8_ListAppendVarLinks --
 *	Append the names of all linked variables (upvar/global)
 *	in the current frame to the list at *pz / *pn.  A variable
 *	is considered linked if its reference count is greater than 1.
 *	Returns TH8_OK.
 */
TH8_API int Th8_ListAppendVarLinks(Th8_Interp *interp, char **pz, size_t *pn);

/*
 * Th8_ListAppendArray --
 *	Append all element names of the array variable zArr (nArr bytes)
 *	to the list at *pz / *pn.  Returns TH8_OK on success, TH8_ERROR
 *	if zArr is not an array.
 */
TH8_API int Th8_ListAppendArray(
    Th8_Interp *interp,
    const char *zArr,
    size_t nArr,
    char **pz,
    size_t *pn);

/*
 * Th8_WrongNumArgs --
 *	Set the interpreter result to "wrong # args: should be \"zMsg\""
 *	and return TH8_ERROR.  Standard error reporting for argument
 *	count mismatches in command implementations.
 */
TH8_API int Th8_WrongNumArgs(Th8_Interp *interp, const char *zMsg);

/*
 * Th8_CallSubCommand --
 *	Dispatch to a sub-command.  argv[1] is matched against the zName
 *	fields of the aSub array (NULL-terminated).  If found, the
 *	corresponding xProc is called with the same arguments.  If not
 *	found, an error listing the valid sub-commands is generated.
 *	Returns the return code from the dispatched sub-command, or
 *	TH8_ERROR on mismatch.
 */
TH8_API int Th8_CallSubCommand(
    Th8_Interp *interp,
    void *ctx,
    int argc,
    const char **argv,
    size_t *argl,
    const Th8_SubCommand *aSub);

/*
 * Th8_ReportTaint --
 *	If zStr (nStr bytes) is tainted (TH8_TAINT_BIT set in nStr),
 *	emit a diagnostic message through xOutputError identifying the
 *	tainted string and the context described by zTitle.  Returns
 *	TH8_OK if the string is not tainted, TH8_ERROR if it is tainted
 *	(after reporting).
 */
TH8_API int Th8_ReportTaint(
    Th8_Interp *interp,
    const char *zTitle,
    const char *zStr,
    size_t nStr);

/* ====================================================================
 * Section: I/O & File System
 * ==================================================================== */

/*
 * Line-ending translation flag for Th8_Input, Th8_Output,
 * and Th8_GetData.  When set, Th8_Input and Th8_GetData
 * convert \r\n to \n in the returned data; Th8_Output
 * converts \n to \r\n.  This should be used when reading
 * script data (which may have DOS line endings) and when
 * writing to a text-mode channel on Windows.
 */
#define TH8_TRANSLATE_NONE (0)
#define TH8_TRANSLATE_EOL  (1)

/*
 * Th8_Input --
 *	Read input via the platform's xInput callback.  On success,
 *	*pzOut and *pnOut receive the data.  If flags includes
 *	TH8_TRANSLATE_EOL, \r\n sequences are converted to \n.
 *	Returns TH8_OK or TH8_ERROR.
 */
TH8_API int
Th8_Input(Th8_Interp *interp, char **pzOut, size_t *pnOut, int flags);

/*
 * Th8_Output --
 *	Write z (n bytes) to the standard output via the platform's
 *	xOutput callback.  If flags includes TH8_TRANSLATE_EOL,
 *	\n is converted to \r\n before writing.
 *	Returns TH8_OK or TH8_ERROR.
 */
TH8_API int
Th8_Output(Th8_Interp *interp, const char *z, size_t n, int flags);

/*
 * Th8_OutputError --
 *	Write z (n bytes) to the error output via the platform's
 *	xOutputError callback.  Returns TH8_OK or TH8_ERROR.
 */
TH8_API int Th8_OutputError(Th8_Interp *interp, const char *z, size_t n);

#if defined(TH8_PLUGIN_IO)
/*
 * Th8_GetInput --
 *	Retrieve the current input channel for the interpreter.
 *	Returns the opaque channel pointer via *pChannel.  NULL
 *	means the default input (e.g., stdin).
 */
TH8_API int Th8_GetInput(Th8_Interp *interp, void **pChannel);

/*
 * Th8_RedirectInput --
 *	Set the input channel for the interpreter.  Subsequent calls
 *	to Th8_Input (and [gets]) will use this channel.  Pass NULL
 *	to reset to the default input.
 */
TH8_API int Th8_RedirectInput(Th8_Interp *interp, void *channel);

/*
 * Th8_GetOutput --
 *	Retrieve the current output channel for the interpreter.
 *	NULL means the default output (e.g., stdout).
 */
TH8_API int Th8_GetOutput(Th8_Interp *interp, void **pChannel);

/*
 * Th8_RedirectOutput --
 *	Set the output channel for the interpreter.  Subsequent calls
 *	to Th8_Output (and [puts]) will use this channel.  Pass NULL
 *	to reset to the default output.
 */
TH8_API int Th8_RedirectOutput(Th8_Interp *interp, void *channel);

/*
 * Th8_GetErrorOutput --
 *	Retrieve the current error output channel for the interpreter.
 *	NULL means the default error output (e.g., stderr).
 */
TH8_API int Th8_GetErrorOutput(Th8_Interp *interp, void **pChannel);

/*
 * Th8_RedirectErrorOutput --
 *	Set the error output channel for the interpreter.  Subsequent
 *	calls to Th8_OutputError (and [puts stderr]) will use this
 *	channel.  Pass NULL to reset to the default error output.
 */
TH8_API int Th8_RedirectErrorOutput(Th8_Interp *interp, void *channel);
#endif /* TH8_PLUGIN_IO */

/*
 * Th8_GetData --
 *	Retrieve named data via the platform's xGetData callback.
 *	On success, *pzOut and *pnOut receive the data.  If flags
 *	includes TH8_TRANSLATE_EOL, \r\n sequences are converted
 *	to \n (for reading script files with DOS line endings).
 *	Returns TH8_OK or TH8_ERROR.
 */
TH8_API int Th8_GetData(
    Th8_Interp *interp,
    const char *zName,
    size_t nName,
    char **pzOut,
    size_t *pnOut,
    int flags);

/*
 * Th8_DataExists --
 *	Test whether named data exists via the platform's xDataExists
 *	callback.  Returns 1 if the data exists, 0 otherwise.
 *	If xDataExists is NULL, always returns 0.
 *	pAttrs is an optional output pointer for file type attributes
 *	(TH8_FILE_ATTR_*).  Pass NULL for a plain existence check.
 */
TH8_API int Th8_DataExists(
    Th8_Interp *interp,
    const char *zName,
    size_t nName,
    int *pAttrs);

/*
 * Th8_NormalizePath --
 *	Normalize a file path via the platform's xNormalizePath
 *	callback.  Returns a malloc'd NUL-terminated string the
 *	caller must free with Th8_Free, or NULL if the callback
 *	is not available or fails.
 */
TH8_API char *
Th8_NormalizePath(Th8_Interp *interp, const char *zPath, size_t nPath);

/*
 * Th8_GetCwd --
 *	Return the current working directory via the platform's
 *	xGetCwd callback.  Returns a malloc'd NUL-terminated string
 *	the caller must free with Th8_Free, or NULL if the callback
 *	is not available or fails.
 */
TH8_API char *Th8_GetCwd(Th8_Interp *interp);

/*
 * Th8_SetCwd --
 *	Change the current working directory via the platform's
 *	xSetCwd callback.  Returns TH8_OK on success or TH8_ERROR
 *	if the callback is NULL or rejects the path.
 */
TH8_API int Th8_SetCwd(Th8_Interp *interp, const char *zPath, size_t nPath);

/*
 * Th8_GetRealPath --
 *	Resolve a path to its canonical absolute form via the
 *	platform's xGetRealPath callback.  On success, writes the
 *	result into zBuf (up to nBuf bytes) and returns TH8_OK.
 *	Returns TH8_ERROR if the callback is NULL or resolution fails.
 */
TH8_API int Th8_GetRealPath(
    Th8_Interp *interp,
    const char *zPath,
    size_t nPath,
    char *zBuf,
    size_t nBuf);

/*
 * Th8_GetRootPath --
 *	Return the filesystem root (mount point) for a given path via
 *	the platform's xGetRootPath callback.  On success, writes the
 *	result into zBuf (up to nBuf bytes) and returns TH8_OK.
 *	Returns TH8_ERROR if the callback is NULL or detection fails.
 */
TH8_API int Th8_GetRootPath(
    Th8_Interp *interp,
    const char *zPath,
    size_t nPath,
    char *zBuf,
    size_t nBuf);

/*
 * Th8_SameFile --
 *	Test whether two paths refer to the same physical file via
 *	the platform's xSameFile callback.  Returns non-zero if
 *	both paths refer to the same file, zero otherwise.
 */
TH8_API int Th8_SameFile(
    Th8_Interp *interp,
    const char *zName1,
    size_t nName1,
    const char *zName2,
    size_t nName2);

/*
 * Th8_SetBasePath --
 *	Explicitly set the base path for the TH8 platform layer.
 *	This overrides the automatic dladdr-based detection.
 *	Must be called BEFORE Th8_Initialize.  The path is copied
 *	internally.  Pass "." to use the current working directory.
 */
TH8_API int Th8_SetBasePath(const char *zPath, size_t nPath);

/*
 * Th8_GetBasePath --
 *
 *	Return the current base path as a NUL-terminated string,
 *	or NULL if no base path has been established.  The returned
 *	pointer is owned by the platform layer and must not be freed
 *	by the caller.  Valid until the next call to Th8_SetBasePath
 *	or Th8_Finalize.
 */
TH8_API const char *Th8_GetBasePath(void);

/* ====================================================================
 * Section: Packages
 * ==================================================================== */

/*
 *----------------------------------------------------------------------
 *
 * Hash table --
 *
 *	A simple chaining hash table used internally for command and
 *	variable lookup, and exposed for use by extension modules
 *	(e.g., the package registry).
 *
 *----------------------------------------------------------------------
 */

/*
 * Hash table types and API.
 *
 * When th8_hash.h has already been included (e.g., in the multi-file
 * build), TH8_HASH_H is defined and we skip the inline definitions.
 * When th8.h is the only header (amalgamation or embedder), we
 * define the types and API here.
 */

#ifndef TH8_HASH_H
#  define TH8_HASH_H

#  define TH8_HASH_SIZE 257

typedef struct Th8_Hash Th8_Hash;
typedef struct Th8_HashEntry Th8_HashEntry;

struct Th8_Hash {
    Th8_HashEntry *aBucket[TH8_HASH_SIZE];
    int nNextOrder; /* Next insertion-order counter. */
};

struct Th8_HashEntry {
    th8_int64_t nVersion; /* Struct version (must be first). */
    void *pData; /* User data pointer. */
    char *zKey; /* Key string (owned). */
    size_t nKey; /* Byte length of key. */
    Th8_HashEntry *pNext; /* Internal use only. */
    int nInsertOrder; /* Insertion sequence number. */
};

TH8_API Th8_Hash *Th8_HashNew(Th8_Interp *interp);
TH8_API void Th8_HashDelete(Th8_Interp *interp, Th8_Hash *pHash);
TH8_API void Th8_HashIterate(
    Th8_Interp *interp,
    Th8_Hash *pHash,
    int (*xCallback)(Th8_HashEntry *, void *),
    void *pCtx);
TH8_API void Th8_HashIterateOrdered(
    Th8_Interp *interp,
    Th8_Hash *pHash,
    int (*xCallback)(Th8_HashEntry *, void *),
    void *pCtx);
TH8_API Th8_HashEntry *Th8_HashFind(
    Th8_Interp *interp,
    Th8_Hash *pHash,
    const char *zKey,
    size_t nKey,
    int op);
TH8_API void Th8_HashRemove(
    Th8_Interp *interp,
    Th8_Hash *pHash,
    const char *zKey,
    size_t nKey);
TH8_API void th8SeedHash(Th8_Platform *pPlatform);

#endif /* TH8_HASH_H */

/*
 * Th8_GetPackageHash --
 *	Return the interpreter's package registry hash table.  Used by
 *	[package] command implementations to record loaded packages.
 */
TH8_API struct Th8_Hash *Th8_GetPackageHash(Th8_Interp *interp);

/*
 * Th8_GetMathFuncHash --
 *	Return the interpreter's math function registry hash.
 *	Lazily creates the hash on first call.
 */
TH8_API Th8_Hash *Th8_GetMathFuncHash(Th8_Interp *interp);

/*
 * Th8_IterateArraySearches --
 *	Diagnostic enumeration of pending [array startsearch] state
 *	in the interpreter.  For each open search, invokes xCallback
 *	with (zArray, nArray, zSid, nSid, pCtx).  Returning anything
 *	other than TH8_OK from the callback aborts the iteration and
 *	is propagated as the function's return value.
 *
 *	Used by leak-detection helpers (e.g.
 *	[::th8testlib::array_searches]) and embedder diagnostics.
 *	Iteration order is unspecified; sort the collected results
 *	if a stable ordering is needed.  The strings passed to the
 *	callback are borrowed and valid only for the duration of the
 *	call.
 *
 *	Returns TH8_OK if iteration completes.
 */
TH8_API int Th8_IterateArraySearches(
    Th8_Interp *interp,
    int (*xCallback)(
        const char *zArray,
        size_t nArray,
        const char *zSid,
        size_t nSid,
        void *pCtx),
    void *pCtx);

/*
 * Th8_GetPackageUnknown --
 *	Return the name of the "package unknown" handler command, or
 *	NULL if none is set.  The handler is invoked when [package require]
 *	cannot find a package.
 */
TH8_API const char *Th8_GetPackageUnknown(Th8_Interp *interp);

/*
 * Th8_SetPackageUnknown --
 *	Set the "package unknown" handler command to zCmd (nCmd bytes,
 *	or TH8_NOLEN).  Pass NULL/0 to clear the handler.
 */
TH8_API void
Th8_SetPackageUnknown(Th8_Interp *interp, const char *zCmd, size_t nCmd);

/*
 * Th8_AutoPathSearch --
 *	Search ::auto_path for pkgIndex.th8 files and source them
 *	to populate the package registry.  Call this once after
 *	language commands are registered.
 */
TH8_API int Th8_AutoPathSearch(Th8_Interp *interp, char *zAuto, size_t nAuto);

/* ====================================================================
 * Section: Namespaces
 * ==================================================================== */

/*
 * Th8_GetCurrentNamespace --
 *	Return the name of the current namespace (NUL-terminated).
 *	Returns "::" for the global namespace.
 */
TH8_API const char *Th8_GetCurrentNamespace(Th8_Interp *interp);

/*
 * Th8_FindNamespace --
 *	Look up the namespace zName (nName bytes, or TH8_NOLEN).  If
 *	bCreate is non-zero and the namespace does not exist, create it.
 *	Returns TH8_OK if found (or created), TH8_ERROR if not found
 *	and bCreate is zero.
 */
TH8_API int Th8_FindNamespace(
    Th8_Interp *interp,
    const char *zName,
    size_t nName,
    int bCreate);

/*
 * Th8_DeleteNamespace --
 *	Delete the namespace zName (nName bytes) and all its contents
 *	(variables, commands, child namespaces).  Returns TH8_OK on
 *	success, TH8_ERROR if the namespace does not exist.
 */
TH8_API int
Th8_DeleteNamespace(Th8_Interp *interp, const char *zName, size_t nName);

/*
 * Th8_NsEval --
 *	Evaluate zScript (nScript bytes) in the context of namespace zNs
 *	(nNs bytes).  Equivalent to [namespace eval $ns $script].  Returns
 *	a TH8_* return code.
 */
TH8_API int Th8_NsEval(
    Th8_Interp *interp,
    const char *zNs,
    size_t nNs,
    const char *zScript,
    size_t nScript);

/*
 * Th8_ListAppendNsChildren --
 *	Append the names of all child namespaces of zNs (nNs bytes) to
 *	the list at *pz / *pn.  Returns TH8_OK.
 */
TH8_API int Th8_ListAppendNsChildren(
    Th8_Interp *interp,
    const char *zNs,
    size_t nNs,
    char **pz,
    size_t *pn);

/*
 * Th8_NsExport --
 *	Add export glob patterns to the named namespace.  If the
 *	namespace does not exist, TH8_ERROR is returned.
 */
TH8_API int Th8_NsExport(
    Th8_Interp *interp,
    const char *zNs,
    size_t nNs,
    const char *zPattern,
    size_t nPattern);

/*
 * Th8_NsImport --
 *	Import commands matching zPattern from the namespace named
 *	by the qualified pattern (e.g., "::foo::*").  Commands are
 *	copied into the current namespace.  If bForce is true,
 *	existing commands in the target namespace are overwritten.
 */
TH8_API int Th8_NsImport(
    Th8_Interp *interp,
    const char *zPattern,
    size_t nPattern,
    int bForce);

/* ====================================================================
 * Section: Source Name Stack
 * ==================================================================== */

/*
 * Th8_PushSourceName / Th8_PopSourceName / Th8_GetSourceName --
 *	Manage the [source] script name stack for [info script].
 *	Push before evaluating a sourced file; pop after.
 *	GetSourceName returns the current innermost name, or ""
 *	if no source is active.
 */
TH8_API void
Th8_PushSourceName(Th8_Interp *interp, const char *zName, size_t nName);
TH8_API void Th8_PopSourceName(Th8_Interp *interp);
TH8_API const char *Th8_GetSourceName(Th8_Interp *interp, size_t *pnName);

/* ====================================================================
 * Section: Time & Process
 * ==================================================================== */

/*
 * Th8_GetTimeMs --
 *	Get the current time in milliseconds via the platform's xTimeMs
 *	callback.  Stores the result in *pMs.  Returns TH8_OK or TH8_ERROR.
 */
TH8_API int Th8_GetTimeMs(Th8_Interp *interp, th8_int64_t *pMs);

/*
 * Th8_GetTimeUs --
 *	Get the current monotonic time in microseconds via the
 *	platform's xTimeUs callback.  If xTimeUs is NULL, falls
 *	back to xTimeMs * 1000.  Used by [time] for high-precision
 *	benchmarking.
 */
TH8_API int Th8_GetTimeUs(Th8_Interp *interp, th8_int64_t *pUs);

/*
 * Th8_Sleep --
 *	Sleep for nMs milliseconds via the platform's xSleep callback.
 *	No-op if xSleep is NULL.
 */
TH8_API void Th8_Sleep(Th8_Interp *interp, int nMs);

/*
 * Th8_GetPid --
 *	Return the process ID via the platform's xGetPid callback.
 *	Returns 0 if the callback is NULL or the host does not support it.
 */
TH8_API int Th8_GetPid(Th8_Interp *interp);

/*
 * Th8_GetParentPid --
 *	Return the parent process ID via the platform's xGetParentPid
 *	callback.  Returns 0 if the callback is NULL or the host does
 *	not support it.
 */
TH8_API int Th8_GetParentPid(Th8_Interp *interp);

/*
 * Th8_GetThreadId --
 *	Return the current thread ID via the platform's xGetThreadId
 *	callback.  Returns 0 if the callback is NULL or the host does
 *	not support it.
 */
TH8_API th8_uint64_t Th8_GetThreadId(Th8_Interp *interp);

/*
 * Th8_GetEnv --
 *	Return the value of the named environment variable as a
 *	NUL-terminated UTF-8 string.  The result is allocated via
 *	Th8_Malloc and the caller MUST free it with Th8_Free.
 *	Returns NULL if the variable is not set, the callback is
 *	unavailable, or allocation fails.
 *	Supports NULL interp (falls back to th8GlobalPlatform).
 */
TH8_API char *Th8_GetEnv(Th8_Interp *interp, const char *zName);

/*
 * Th8_KeyValue --
 *	Dispatch a key-value operation (TH8_KV_*) to the platform's
 *	xKeyValue callback.  For GET and LIST, the result is set via
 *	Th8_SetResult.  Returns TH8_OK on success, TH8_ERROR on failure
 *	or if no xKeyValue callback is available.
 */
TH8_API int Th8_KeyValue(
    Th8_Interp *interp,
    int op,
    const char *zName,
    size_t nName,
    const char *zValue,
    size_t nValue);

/*
 * Th8_PlatformFunc -- generic function pointer type used as the
 * callback identity in Th8_SetPlatformContext / Th8_GetPlatformContext.
 * Callers cast their actual platform callback function pointer to
 * this type; ISO C permits function-pointer-to-function-pointer
 * casts but forbids function-pointer-to-object-pointer casts, so
 * this typedef avoids -Wpedantic warnings at every call site.
 */
typedef void (*Th8_PlatformFunc)(void);

/*
 * Th8_SetPlatformContext --
 *	Associate a per-callback context pointer with a specific
 *	platform callback function.  When the callback is dispatched,
 *	it receives this context instead of the platform's default pCtx.
 *	If pCtx is NULL, the per-callback override is removed and the
 *	callback reverts to using the platform's default pCtx.
 */
TH8_API int Th8_SetPlatformContext(
    Th8_Interp *interp,
    Th8_PlatformFunc xCallback,
    void *pCtx);

/*
 * Th8_GetPlatformContext --
 *	Retrieve the context pointer associated with a specific
 *	platform callback function.  If a per-callback override has
 *	been set via Th8_SetPlatformContext, that value is returned.
 *	Otherwise, the platform's default pCtx is returned.
 */
TH8_API int Th8_GetPlatformContext(
    Th8_Interp *interp,
    Th8_PlatformFunc xCallback,
    void **ppCtx);

/*
 * Th8_RandomBytes --
 *	Fill pBuf with nByte random bytes from the platform's
 *	entropy source.  Returns TH8_OK or TH8_ERROR.
 */
TH8_API int Th8_RandomBytes(Th8_Interp *interp, void *pBuf, size_t nByte);

/*
 * Th8_DnsResolve --
 *	DNSSEC-validating DNS lookup.  Wraps the platform's
 *	xDnsResolve callback.  On TH8_OK, *ppResult is set to a
 *	heap-allocated Th8_DnsResult that the caller must
 *	release with Th8_DnsResolveFree.  Check pResult->bogus
 *	for DNSSEC validation failure.  Returns TH8_ERROR when
 *	the platform has no resolver.
 */
TH8_API int Th8_DnsResolve(
    Th8_Interp *interp,
    const char *zName,
    size_t nName,
    int eType,
    Th8_DnsResult **ppResult);

/*
 * Th8_DnsResolveFree --
 *	Release a Th8_DnsResult returned by Th8_DnsResolve.
 *	Safe to call with NULL.
 */
TH8_API void Th8_DnsResolveFree(Th8_Interp *interp, Th8_DnsResult *pResult);

/*
 * Th8_GetExePath --
 *	Return the path to the current executable.  Caller must free
 *	with Th8_Free.  Returns NULL if unavailable.
 */
TH8_API char *Th8_GetExePath(Th8_Interp *interp);

/*
 * Th8_GetPlatform --
 *	Return a pointer to the interpreter's Th8_Platform table.  The
 *	returned pointer is valid for the lifetime of the interpreter.
 *	This allows command implementations to call platform callbacks
 *	directly (e.g., xOutput, xMathFunc).
 */
TH8_API const Th8_Platform *Th8_GetPlatform(Th8_Interp *interp);

/* ====================================================================
 * Section: String Utilities
 * ==================================================================== */

/*
 * Th8_Strlen --
 *	Return the byte length of NUL-terminated string z.  Uses the
 *	platform's xStrlen if available, otherwise a built-in loop.
 */
TH8_API size_t Th8_Strlen(Th8_Interp *interp, const char *z);

/*
 * Th8_Strdup --
 *	Allocate a copy of z (n bytes) using the interpreter's xMalloc.
 *	The returned string is NUL-terminated.  Returns NULL on
 *	allocation failure.
 */
TH8_API char *Th8_Strdup(Th8_Interp *interp, const char *z, size_t n);

/*
 * Th8_Memcmp --
 *	Compare n bytes of a and b using the platform's xMemcmp.
 *	Convenience wrapper that routes through the interpreter's
 *	platform table.
 */
TH8_API int
Th8_Memcmp(Th8_Interp *interp, const void *a, const void *b, size_t n);

/*
 * Th8_Memcpy --
 *	Copy n bytes from src to dst using the platform's xMemcpy.
 *	Convenience wrapper.  Returns dst.
 */
TH8_API void *
Th8_Memcpy(Th8_Interp *interp, void *dst, const void *src, size_t n);

/*
 * Th8_Memset --
 *	Fill n bytes of dst with the byte value c using the
 *	platform's xMemset.  When c is zero, uses secure zeroing
 *	semantics (volatile writes or SecureZeroMemory) to prevent
 *	the compiler from optimizing away the operation.
 *	Returns dst.
 */
TH8_API void *Th8_Memset(Th8_Interp *interp, void *dst, int c, size_t n);

/* ====================================================================
 * Section: UTF-8
 * ==================================================================== */

/*
 * Th8_Utf8Decode --
 *	Decode one UTF-8 codepoint from z (at most n bytes).  Returns
 *	the Unicode codepoint.  If pnByte is non-NULL, *pnByte is set
 *	to the number of bytes consumed (1-4).  Returns U+FFFD for
 *	invalid sequences.
 */
TH8_API int Th8_Utf8Decode(const char *z, size_t n, int *pnByte);

/*
 * Th8_Utf8Encode --
 *	Encode codepoint as UTF-8 into the buffer z (which must have
 *	room for at least 4 bytes).  Returns the number of bytes
 *	written (1-4).
 */
TH8_API int Th8_Utf8Encode(int codepoint, char *z);

/*
 * Th8_Utf8Len --
 *	Return the number of Unicode codepoints in z (n bytes).
 */
TH8_API int Th8_Utf8Len(const char *z, size_t n);

/*
 * Th8_Utf8Advance --
 *	Advance nChar codepoints from z (within n bytes).  Returns a
 *	pointer to the first byte after the nChar-th codepoint, or
 *	z+n if there are fewer than nChar codepoints remaining.
 */
TH8_API const char *Th8_Utf8Advance(const char *z, size_t n, int nChar);

/*
 * Th8_Utf8Index --
 *	Return a pointer to the first byte of the iChar-th codepoint
 *	(0-based) in z (n bytes).  Returns NULL if iChar is out of
 *	range.
 */
TH8_API const char *Th8_Utf8Index(const char *z, size_t n, int iChar);

/*
 * Th8_Utf8Validate --
 *	Validate that z (n bytes) is well-formed UTF-8.  Returns
 *	non-zero if valid.  If invalid and piOffset is non-NULL,
 *	*piOffset is set to the byte offset of the first invalid
 *	sequence.
 */
TH8_API int Th8_Utf8Validate(const char *z, size_t n, int *piOffset);

/*
 * Th8_ByteToUtf16Col --
 *	Convert a byte offset nByte within the line zLine (nLine bytes)
 *	to a UTF-16 column index.  Useful for LSP (Language Server
 *	Protocol) integration where columns are measured in UTF-16
 *	code units.
 */
TH8_API int Th8_ByteToUtf16Col(const char *zLine, size_t nLine, int nByte);

/* ====================================================================
 * Section: Platform Providers
 * ==================================================================== */

/*
 * Th8_GetPosixPlatform / Th8_GetWin32Platform --
 *	Return a Th8_Platform pre-filled with the OS-native
 *	implementations of all callbacks (memory, I/O, time, stack
 *	bounds, etc.).  Th8_GetPosixPlatform is available on
 *	Unix/Linux/macOS; Th8_GetWin32Platform on Windows.
 */
#if !defined(_WIN32) && !defined(WIN32)
TH8_API const Th8_Platform *Th8_GetPosixPlatform(void);
#else
TH8_API const Th8_Platform *Th8_GetWin32Platform(void);
#endif

#if defined(__APPLE__)
/*
 * Th8_GetMacOSPlatform --
 *	Return a Th8_Platform with macOS-specific memory management
 *	using a private malloc zone (malloc_create_zone).  Reduces
 *	fragmentation and provides accurate memory tracking via
 *	malloc_zone_size.  Merge with Th8_GetPosixPlatform and
 *	Th8_GetLibcPlatform for the complete platform.
 */
TH8_API const Th8_Platform *Th8_GetMacOSPlatform(void);
#endif

#if defined(TH8_PLATFORM_IOS)
/*
 * Th8_GetIosPlatform --
 *	Return a Th8_Platform with iOS-specific overrides.  iOS shares
 *	all UNIX/POSIX surface and the Apple malloc-zone APIs with
 *	macOS, so this layer overrides only the iOS deltas: trace and
 *	panic routed through the unified logging system (os_log) so
 *	they reach Console.app and crash reports, and entropy via
 *	arc4random_buf to avoid a /dev/urandom fd open inside the
 *	iOS sandbox.  Merge with Th8_GetMacOSPlatform,
 *	Th8_GetPosixPlatform, and Th8_GetLibcPlatform (in that order)
 *	for the complete platform.
 */
TH8_API const Th8_Platform *Th8_GetIosPlatform(void);
#endif

#if defined(TH8_PLATFORM_ANDROID)
/*
 * Th8_GetAndroidPlatform --
 *	Return a Th8_Platform with Android-specific overrides.  Android
 *	is Linux+Bionic; this layer overrides trace and panic to route
 *	through __android_log_print (logcat), supplies xMemorySize
 *	via Bionic's malloc_usable_size, and on API 28+ supplies
 *	xRandomBytes via getrandom(2).  Merge with Th8_GetPosixPlatform
 *	and Th8_GetLibcPlatform for the complete platform.  Do NOT
 *	merge Th8_GetMacOSPlatform -- the Apple malloc zone APIs are
 *	not available on Bionic.
 */
TH8_API const Th8_Platform *Th8_GetAndroidPlatform(void);
#endif

#if defined(TH8_ENABLE_LIBCURL)
/*
 * Th8_GetCurlPlatform --
 *	Return a Th8_Platform with xGetData backed by libcurl.
 *	Fetches data from HTTP/HTTPS URIs.  Only xGetData is
 *	provided; all other callbacks are NULL.  Merge with
 *	Th8_GetNullIoPlatform and Th8_GetLibcPlatform for the
 *	remaining capabilities.
 *
 *	Security: only http:// and https:// schemes are allowed.
 *	TLS certificate verification is enabled.  Downloads are
 *	limited to 1 MB.  Redirects are followed (max 5).
 *	Timeout is 30 seconds.
 */
TH8_API const Th8_Platform *Th8_GetCurlPlatform(void);
#endif

/*
 * Th8_GetNullIoPlatform --
 *	Return a Th8_Platform with null I/O: all I/O callbacks are
 *	implemented but produce no side effects.  [source] returns
 *	empty data, [puts]/[gets] succeed silently, [load] is
 *	forbidden, [file exists] returns 0, [file normalize] returns
 *	the path verbatim.  Merge with Th8_GetLibcPlatform() for
 *	memory and CRT support to get a fully sandboxed interpreter.
 */
TH8_API const Th8_Platform *Th8_GetNullIoPlatform(void);

/*
 * Th8_GetLibcPlatform --
 *	Return a Th8_Platform using only standard C library functions
 *	(malloc, free, memcpy, etc.) with no OS-specific calls.
 *	Portable but may lack stack bounds and some optional features.
 */
TH8_API const Th8_Platform *Th8_GetLibcPlatform(void);

/*
 * Th8_GetEnvPlatform --
 *	Return a Th8_Platform with xKeyValue backed by the host
 *	process environment variables.  All other callbacks are NULL.
 *	Merge with Th8_GetPosixPlatform/Th8_GetWin32Platform and
 *	Th8_GetLibcPlatform for a complete platform.
 */
TH8_API const Th8_Platform *Th8_GetEnvPlatform(void);

#if defined(TH8_USE_MIMALLOC)
/*
 * Th8_GetMimallocPlatform --
 *	Return a Th8_Platform backed by Microsoft's mimalloc allocator.
 *	Provides xMalloc, xRealloc, xFree, xMemorySize, xInitialize,
 *	and xFinalize only.  All other callbacks are NULL and must be
 *	filled by merging with other layers.
 *
 *	A dedicated mimalloc heap is created during xInitialize and
 *	destroyed during xFinalize (O(1) bulk free of all allocations).
 *
 *	Typical composition:
 *	  Th8_Platform *p = Th8_ClonePlatform(Th8_GetPosixPlatform());
 *	  Th8_MergePlatform(p, Th8_GetMimallocPlatform());
 *	  Th8_MergePlatform(p, Th8_GetLibcPlatform());
 */
TH8_API const Th8_Platform *Th8_GetMimallocPlatform(void);
#endif

#if defined(TH8_PLATFORM_COSMOPOLITAN)
/*
 * Th8_GetCosmopolitanPlatform --
 *	Return a Th8_Platform for Cosmopolitan Libc (Actually Portable
 *	Executables).  Provides xGetExePath via GetProgramExecutableName,
 *	xMemorySize via malloc_usable_size, and xMemset with
 *	explicit_bzero for secure zeroing on all platforms.
 *
 *	Merge with Th8_GetPosixPlatform() and Th8_GetLibcPlatform()
 *	to get a fully functional cross-platform interpreter.
 *
 *	The resulting th8sh.com binary runs natively on Linux, macOS,
 *	Windows, FreeBSD, OpenBSD, and NetBSD.
 */
TH8_API const Th8_Platform *Th8_GetCosmopolitanPlatform(void);
#endif

/*
 * Th8_GetMemPlatform --
 *	Platform layer providing only xNeedMemory.  When allocation
 *	fails, clears the interpreter's IR cache and retries via
 *	the interpreter's current xMalloc.  Merge with other
 *	platform layers via Th8_MergePlatform.
 */
TH8_API const Th8_Platform *Th8_GetMemPlatform(void);

/*
 * Th8_UseDefaultPlatform --
 *
 *	Populate a caller-supplied Th8_Platform struct with the default
 *	platform configuration for the current operating system.  This
 *	is the same layering that th8sh uses:
 *
 *	  macOS:         MacOS (private zone) + POSIX + libc
 *	  Linux/BSD:     POSIX + libc
 *	  Windows:       Win32 + libc
 *	  Cosmopolitan:  Cosmopolitan + POSIX + libc
 *	  mimalloc:      mimalloc + OS layer + libc
 *
 *	The platform struct is zero-initialized first, then the
 *	appropriate layers are merged in priority order.
 *
 *	NOTE: The Th8_Platform struct passed to Th8_CreateInterp must
 *	outlive the interpreter (see Th8_CreateInterp docs).  For
 *	long-lived interpreters, declare the platform as static or
 *	global --- not as a local variable in a function that returns.
 *
 *	Returns TH8_OK on success, TH8_ERROR if a merge fails
 *	(version mismatch).
 */
TH8_API int Th8_UseDefaultPlatform(Th8_Platform *pPlatform);

/* ====================================================================
 * Section: Fault Injection (TH8_ENABLE_FAULT_INJECTION)
 * ==================================================================== */

#if defined(TH8_ENABLE_FAULT_INJECTION)

/*
 * Th8_FaultFilter --
 *	One entry in the per-site allocation-failure filter list.
 *	An allocation matches a filter iff:
 *	  - zFile == NULL                OR  the call-site filename
 *	    matches via strcmp;
 *	  - nLineFrom == 0               OR  call-site line is in
 *	    [nLineFrom, nLineTo].  When nLineTo == 0 and nLineFrom
 *	    is non-zero, the match is single-line (== nLineFrom).
 *
 *	The fault layer only counts an allocation toward
 *	Th8_FaultConfig.nAllocFailAfter when it matches at least
 *	one filter (or the filter list is empty, preserving the
 *	original count-only behavior).
 */
typedef struct Th8_FaultFilter Th8_FaultFilter;
struct Th8_FaultFilter {
    const char *zFile; /* NULL = match any file (caller-owned). */
    int nLineFrom; /* 0 = any line; else inclusive lower bound. */
    int nLineTo; /* 0 + nLineFrom!=0 = single-line; else
			 * inclusive upper bound. */
    int nHit; /* Per-filter hit counter, bumped each time
			 * this filter matches.  Updated by the fault
			 * layer; reserved for later surfacing. */
};

/*
 * Th8_FaultConfig --
 *	Configuration and running counters for the fault-injection
 *	layer.  The caller initializes this with Th8_FaultConfigInit,
 *	sets the desired fault parameters, and passes it to
 *	Th8_FaultInstall.  Counters are updated in place during
 *	execution.
 */
typedef struct Th8_FaultConfig Th8_FaultConfig;
struct Th8_FaultConfig {
    /* Allocation faults. */
    th8_int64_t nAllocFailAfter; /* Fail after N allocs (0=never). */
    th8_int64_t nAllocFailInterval; /* Then every Mth (0=once only). */

    /* Blanket failure flags. */
    int bFailGetData; /* Always fail xGetData. */
    int bFailDataExists; /* Always fail xDataExists. */
    int bFailRandomBytes; /* Always fail xRandomBytes. */
    int bFailMathFunc; /* Always fail xMathFunc. */
    int bFailGetCwd; /* Always fail xGetCwd (returns NULL). */
    int bFailTimeMs; /* Always fail xTimeMs (returns TH8_ERROR). */
    int bFailGetEnv; /* Always fail xGetEnv (returns NULL). */

    /* Channel-I/O failure flags (F3 fault-injection extension).
     * Each fails the corresponding TH8_CHANCTL_* op when passed
     * through pt_xChannelControl; bFailChannelEOF instead
     * synthesises a 0-byte READ result (premature EOF) without
     * an error code. */
    int bFailChannelRead; /* Fail TH8_CHANCTL_READ. */
    int bFailChannelEOF; /* Synth 0-byte read (no error). */
    int bFailChannelWrite; /* Fail TH8_CHANCTL_WRITE. */
    int bFailChannelOpen; /* Fail TH8_CHANCTL_OPEN. */

    /* Per-cacheType lookup-failure mask.  When bit
     * (1u << cacheType) is set, Th8_FindInCache returns NULL
     * for that cacheType -- bypassing the allocation-site
     * filter entirely so MC/DC tests can deterministically
     * drive Bug-28-family `pCached == NULL` vectors without
     * matching a specific allocation index in the cache.c
     * allocator window.  Only honored when this fault config
     * is the active one (Th8_FaultInstall sets it; Uninstall
     * clears).  Default 0 (no per-type forced misses). */
    unsigned int nFailCacheLookupMask;

    /* Counter: number of times the per-cacheType
     * lookup-failure hook returned NULL while this config
     * was active.  Read after the fault eval to verify the
     * hook actually fired. */
    th8_int64_t nCacheHookFires;

    /* Skip-then-fail counter for the per-cacheType lookup
     * hook (Bug 28 second-call targeting).  When
     * `nFailCacheLookupMask` matches the cacheType:
     *   - if `nCacheLookupSkip > 0`, decrement it and
     *     pass through (real cache lookup proceeds).
     *   - else fail the lookup (return NULL).
     * Lets tests target the SECOND call to
     * Th8_FindInCache(<same cacheType>) in a store path
     * without having to count allocator hits.  Set
     * `nCacheLookupSkip = N` to fail the (N+1)th lookup
     * of the masked cacheType.  Default 0 (fail on first
     * matching lookup, matching the pre-extension
     * behavior). */
    int nCacheLookupSkip;

    /* One-shot xRandomBytes byte forcing.  When
     * nForceRandomBytesCount > 0 the fault wrapper copies
     * aForceRandomBytes (truncated/zero-padded to the
     * caller's nByte) into the caller's buffer, decrements
     * the counter, and returns TH8_OK -- bypassing the real
     * platform RNG.  Drives reserved-token retry vectors at
     * th8_load.c L106/L622 et al. */
    unsigned char aForceRandomBytes[8];
    int nForceRandomBytesCount;

    /* Per-site allocation filter (caller-owned, optional).
     * If aFilter == NULL or nFilter == 0, no site filter is
     * applied (back-compatible with count-only mode).  Otherwise
     * an allocation must match at least one Th8_FaultFilter
     * entry before it is counted toward nAllocFailAfter. */
    Th8_FaultFilter *aFilter;
    int nFilter;

    /* Per-allocation site state, set by Th8_Safe* immediately
     * before invoking the platform's xMalloc.  Read by the
     * fault layer's filter check.  Callers should NOT touch
     * these; they are runtime-internal. */
    const char *zCurFile;
    int nCurLine;

    /* Null-callback overrides (caller-owned, optional).  Each
     * entry is the name of a Th8_Platform callback slot
     * ("xMutexEnter", "xPanic", ...).  After the fault wrapper
     * is built, every named slot is forced to NULL on the
     * fault platform so consuming code sees NULL and takes the
     * false-vector branch on `if (pPlat->xXxx)` checks.  Used
     * to drive MC/DC coverage of platform-callback compounds.
     *
     * Unknown slot names are reported as a TH8_ERROR from
     * Th8_FaultInstall.  Nulling lifecycle-critical slots
     * (xMalloc, xMutexEnter, xPanic) may make the interpreter
     * unusable for general work; callers must scope the
     * eval'd script accordingly. */
    const char **azNullCallbacks;
    int nNullCallbacks;

    /* Counters (updated by fault layer). */
    th8_int64_t nAllocCount;
    th8_int64_t nAllocFailCount;
    th8_int64_t nGetDataCount;
    th8_int64_t nGetDataFailCount;
};

/*
 * Th8_FaultCtx --
 *	Opaque context for the fault-injection layer.  Stores the
 *	fault-wrapping platform and the saved original platform.
 *	Must remain valid between Install and Uninstall.
 */
typedef struct Th8_FaultCtx Th8_FaultCtx;

/*
 * Th8_FaultConfigInit --
 *	Initialize a fault config to safe defaults (no faults).
 */
TH8_API void Th8_FaultConfigInit(Th8_FaultConfig *pCfg);

/*
 * Th8_FaultInstall --
 *	Dynamically install the fault-injection layer on a running
 *	interpreter.  Saves the current platform, builds the fault-
 *	wrapping platform, and swaps the interpreter's platform
 *	pointer.  pConfig and pCtx must remain valid until
 *	Th8_FaultUninstall is called.
 */
TH8_API int Th8_FaultInstall(
    Th8_Interp *interp,
    Th8_FaultConfig *pConfig,
    Th8_FaultCtx *pCtx);

/*
 * Th8_FaultUninstall --
 *	Remove the fault-injection layer from a running interpreter.
 *	Restores the original platform.  After this call, pCtx may
 *	be freed or go out of scope.
 */
TH8_API int Th8_FaultUninstall(Th8_Interp *interp, Th8_FaultCtx *pCtx);

/*
 * Th8_FaultCtxSize --
 *	Return sizeof(Th8_FaultCtx) so callers can stack-allocate
 *	without including the internal header.
 */
TH8_API size_t Th8_FaultCtxSize(void);

#endif /* TH8_ENABLE_FAULT_INJECTION */

/* ====================================================================
 * Section: Registration
 * ==================================================================== */

/*
 * Th8_RegisterLanguage --
 *	Register the built-in TH8 language commands ([if], [while],
 *	[proc], [set], [expr], etc.) with the interpreter.  Must be
 *	called once after Th8_CreateInterp to make the interpreter
 *	usable as a Tcl-like scripting engine.  Returns TH8_OK.
 */
TH8_API int Th8_RegisterLanguage(Th8_Interp *interp);

/*
 * Th8_ResetSecurityArray --
 *	Set all elements of the ::th8_security array to "none".
 */
#if defined(TH8_ENABLE_VARIABLES)
TH8_API void Th8_ResetSecurityArray(Th8_Interp *interp);
#endif

/*
 * Th8_SaveSystemVar / Th8_RestoreSystemVar --
 *
 *	Save and restore all elements of a system array variable.
 *	Save snapshots the current values of all array elements
 *	into an opaque handle (*ppSaved).  Restore copies the
 *	saved values back and frees the handle.  Each save/restore
 *	pair uses its own handle, so calls stack naturally.
 *
 *	Neither function modifies the interpreter result.
 */
TH8_API int Th8_SaveSystemVar(
    Th8_Interp *interp,
    const char *zArr,
    size_t nArr,
    void **ppSaved);
TH8_API int Th8_RestoreSystemVar(
    Th8_Interp *interp,
    const char *zArr,
    size_t nArr,
    void *pSaved);

/* ====================================================================
 * Section: Attribute Flags (Harpy-compatible)
 * ==================================================================== */

/*
 * Th8_AfMap -- map of key-to-flagset pairs for attribute flags.
 * Stack-allocatable.  Th8_AttrFlagsParse/Format/Have/Change
 * operate on this type.
 */

#define TH8_AF_MAX_KEYS 16

typedef struct {
    unsigned char present[128]; /* 1 = flag char present (O(1) lookup) */
    char order[128]; /* Insertion-order sequence of flag chars */
    int nOrder; /* Number of entries in order[] */
} Th8_FlagSet;

typedef struct {
    th8_int64_t key;
    Th8_FlagSet flags;
} Th8_AfKeyEntry;

typedef struct Th8_AfMap {
    Th8_AfKeyEntry a[TH8_AF_MAX_KEYS];
    int n;
} Th8_AfMap;

TH8_API int Th8_AttrFlagsParse(
    Th8_Interp *interp,
    const char *zText,
    size_t nText,
    int bComplex,
    int bSpace,
    Th8_AfMap *pMap);
TH8_API int Th8_AttrFlagsFormat(
    Th8_Interp *interp,
    const Th8_AfMap *pMap,
    int bLegacy,
    int bCompact,
    int bSpace,
    int bSort,
    char **pzOut,
    size_t *pnOut);
TH8_API int Th8_AttrFlagsHave(
    const Th8_AfMap *pMap,
    th8_int64_t key,
    const char *zHave,
    size_t nHave,
    int bAll,
    int bStrict);
TH8_API int Th8_AttrFlagsChange(
    Th8_Interp *interp,
    Th8_AfMap *pMap,
    const char *zChange,
    size_t nChange,
    th8_int64_t key);

TH8_API void Th8_SecureZero(Th8_Interp *interp, void *p, size_t n);

/*
 * Th8_SecureSetMasterKey --
 *	Set the master key for secure variable persistence.  The key
 *	must be exactly 32 bytes (AES-256).  TH8 copies it into the
 *	locked key page (slot 1).  Must be called before [secure save]
 *	or [secure load].
 */
TH8_API int Th8_SecureSetMasterKey(
    Th8_Interp *interp,
    const unsigned char *pKey,
    size_t nKey);

/*
 * Th8_SecureClearMasterKey --
 *	Securely zero and remove the master key from the locked page.
 *	Subsequent save/load operations will fail until a new key is set.
 */
TH8_API void Th8_SecureClearMasterKey(Th8_Interp *interp);

/*
 * Th8_EnableSecurePersist / Th8_IsSecurePersistEnabled --
 *	Security gate for secure variable persistence via xKeyValue.
 *	Uses the dual-field random-token pattern.  The embedder must
 *	explicitly enable persistence before scripts can save/load.
 */
TH8_API int Th8_EnableSecurePersist(Th8_Interp *interp, int bEnable);
TH8_API int Th8_IsSecurePersistEnabled(Th8_Interp *interp);

#if defined(TH8_ENABLE_CRYPTOGRAPHY)
/*
 *----------------------------------------------------------------------
 * Protected memory regions.
 *
 * Th8_ProtectedRegion wraps an mlock'd page flanked by guard pages.
 * Use for any sensitive data: AES key slots, RSA private keys,
 * session tokens, etc.  Independent of the variable system.
 *----------------------------------------------------------------------
 */

typedef struct Th8_ProtectedRegion {
    unsigned char *pPage; /* Data page (mlock'd). */
    size_t nPageSize; /* System page size. */
#  if defined(_WIN32) || defined(WIN32)
    unsigned char *pAlloc; /* Base of VirtualAlloc region. */
    size_t nAlloc; /* Total allocated (3 pages). */
#  else
    unsigned char *pRegion; /* Base of mmap region. */
    size_t nRegion;  /* Total mmap size. */
#  endif
} Th8_ProtectedRegion;

#endif /* TH8_ENABLE_CRYPTOGRAPHY */

/* ====================================================================
 * Section: Build Info
 * ==================================================================== */

/*
 * Th8_GetCompileOptions --
 *	Return a NULL-terminated array of strings listing the
 *	compile-time options active when TH8 was built.  Each
 *	string has the "TH8_" prefix removed (e.g., "ENABLE_REGEXP").
 */
TH8_API const char **Th8_GetCompileOptions(void);

/* ====================================================================
 * Section: Memory
 * ==================================================================== */

/*
 * Th8_Malloc --
 *	Allocate nByte bytes via the platform's xMalloc.  The returned
 *	memory is zero-filled.  Returns NULL on failure.
 */
TH8_API void *Th8_Malloc(Th8_Interp *interp, size_t nByte);

/*
 * Th8_Free --
 *	Free a block previously returned by Th8_Malloc or Th8_Realloc
 *	via the platform's xFree.  If p is NULL, does nothing.
 */
TH8_API void Th8_Free(Th8_Interp *interp, void *p);


/*
 * Th8_Realloc --
 *	Resize a block to nByte bytes via the platform's xRealloc.
 *	Returns the (possibly moved) pointer, or NULL on failure.
 */
TH8_API void *Th8_Realloc(Th8_Interp *interp, void *p, size_t nByte);

/*
 * Th8_AttemptMalloc --
 *	Like Th8_Malloc but returns NULL on failure without calling
 *	xPanic.  Use for script-reachable allocations with unbounded
 *	sizes.  Callers MUST check for NULL and return TH8_ERROR.
 */
TH8_API void *Th8_AttemptMalloc(Th8_Interp *interp, size_t nByte);

/*
 * Th8_AttemptRealloc --
 *	Like Th8_Realloc but returns NULL on failure without calling
 *	xPanic.  The original block is NOT freed on failure.
 *	Callers MUST check for NULL and return TH8_ERROR.
 */
TH8_API void *Th8_AttemptRealloc(Th8_Interp *interp, void *p, size_t nByte);

/*
 * Th8_SafeAlloc / Th8_SafeAllocStr / Th8_SafeAllocMul /
 * Th8_SafeAllocAdd / Th8_SafeAllocMulAdd --
 *
 *	Overflow-checked allocation functions.  Each variant checks
 *	its arithmetic (addition, multiplication, or both) for size_t
 *	overflow before calling Th8_AttemptMalloc.  Returns NULL on
 *	overflow or allocation failure.  The zFile/nLine parameters
 *	are for diagnostic tracing (typically supplied via TH8_ALLOC*
 *	macros from th8_int.h using __FILE__ and __LINE__).
 */
TH8_API void *
Th8_SafeAlloc(Th8_Interp *interp, size_t nByte, const char *zFile, int nLine);
TH8_API void *Th8_SafeAllocStr(
    Th8_Interp *interp,
    size_t nLen,
    const char *zFile,
    int nLine);
TH8_API void *Th8_SafeAllocMul(
    Th8_Interp *interp,
    size_t a,
    size_t b,
    const char *zFile,
    int nLine);
TH8_API void *Th8_SafeAllocAdd(
    Th8_Interp *interp,
    size_t a,
    size_t b,
    const char *zFile,
    int nLine);
TH8_API void *Th8_SafeAllocMulAdd(
    Th8_Interp *interp,
    size_t a,
    size_t b,
    size_t c,
    const char *zFile,
    int nLine);

/*
 * Th8_SafeRealloc / Th8_SafeAttemptRealloc --
 *	Reallocate with __FILE__/__LINE__ tracing for OOM diagnostics.
 *	Th8_SafeRealloc panics on OOM (matches Th8_Realloc); the
 *	"Attempt" variant returns NULL on OOM (matches
 *	Th8_AttemptRealloc).  Both reject pathologically large
 *	requests (> TH8_MX_ALLOC) before forwarding.  Use via the
 *	TH8_REALLOC / TH8_ATTEMPT_REALLOC macros so the call site
 *	captures __FILE__/__LINE__ automatically.
 */
TH8_API void *Th8_SafeRealloc(
    Th8_Interp *interp,
    void *p,
    size_t nByte,
    const char *zFile,
    int nLine);
TH8_API void *Th8_SafeAttemptRealloc(
    Th8_Interp *interp,
    void *p,
    size_t nByte,
    const char *zFile,
    int nLine);

/*
 * Th8_SafeAllocStrAdd --
 *	Allocate k + n + 1 bytes (overflow-safe at every step).
 *	Two internal safe-add operations: (k + n), then (+ 1).
 *	Returns NULL if either step overflows or allocation fails.
 *	Used for the "prefix + tail + NUL" string buffer pattern;
 *	replaces unsafe TH8_ALLOC_ADD(interp, k, n + 1).
 */
TH8_API void *Th8_SafeAllocStrAdd(
    Th8_Interp *interp,
    size_t k,
    size_t n,
    const char *zFile,
    int nLine);

/*
 * Th8_SafeAllocStrMul --
 *	Allocate (n + 1) * sz bytes (overflow-safe at every step).
 *	Two internal safe operations: (n * sz), then (+ sz).
 *	Returns NULL if either step overflows or allocation fails.
 *	Used for "wide-string buffer with NUL slot" pattern;
 *	replaces unsafe TH8_ALLOC_MUL(interp, n + 1, sz).
 */
TH8_API void *Th8_SafeAllocStrMul(
    Th8_Interp *interp,
    size_t n,
    size_t sz,
    const char *zFile,
    int nLine);

/*
 * Th8_SafeAllocMulAdd2 --
 *	Allocate (a*b) + (c*d) + e bytes with four sequential
 *	overflow checks: (a*b), (c*d), (sum of products), (+e).
 *	Returns NULL on any overflow or allocation failure.
 *	Used for packed (azElem, anElem, zStrings) record layouts;
 *	replaces unsafe pre-computed (nE*A + nE*B + nST) sizes.
 */
TH8_API void *Th8_SafeAllocMulAdd2(
    Th8_Interp *interp,
    size_t a,
    size_t b,
    size_t c,
    size_t d,
    size_t e,
    const char *zFile,
    int nLine);

/*
 * Th8_SafeMul --
 *	Compute a * b into *pOut with size_t overflow detection.
 *	Returns TH8_OK on success, TH8_ERROR if the multiplication
 *	would wrap.  On overflow *pOut is left unchanged.  No
 *	allocation is performed.  interp MAY be NULL.
 */
TH8_API int Th8_SafeMul(Th8_Interp *interp, size_t a, size_t b, size_t *pOut);

/*
 * Th8_SafeAdd --
 *	Compute a + b into *pOut with size_t overflow detection.
 *	Returns TH8_OK on success, TH8_ERROR if the addition would
 *	wrap.  On overflow *pOut is left unchanged.  interp MAY
 *	be NULL.
 */
TH8_API int Th8_SafeAdd(Th8_Interp *interp, size_t a, size_t b, size_t *pOut);

/*
 * Convenience macros for the overflow-checked allocators.
 * These capture __FILE__ and __LINE__ automatically.
 */

#define TH8_ALLOC(interp, nByte)                                             \
    Th8_SafeAlloc((interp), (nByte), __FILE__, __LINE__)

#define TH8_ALLOC_STR(interp, nLen)                                          \
    Th8_SafeAllocStr((interp), (nLen), __FILE__, __LINE__)

#define TH8_ALLOC_MUL(interp, a, b)                                          \
    Th8_SafeAllocMul((interp), (a), (b), __FILE__, __LINE__)

#define TH8_ALLOC_ADD(interp, a, b)                                          \
    Th8_SafeAllocAdd((interp), (a), (b), __FILE__, __LINE__)

#define TH8_ALLOC_MUL_ADD(interp, a, b, c)                                   \
    Th8_SafeAllocMulAdd((interp), (a), (b), (c), __FILE__, __LINE__)

#define TH8_ALLOC_MUL_ADD2(interp, a, b, c, d, e)                            \
    Th8_SafeAllocMulAdd2(                                                    \
        (interp), (a), (b), (c), (d), (e), __FILE__, __LINE__)

#define TH8_ALLOC_STR_ADD(interp, k, n)                                      \
    Th8_SafeAllocStrAdd((interp), (k), (n), __FILE__, __LINE__)

#define TH8_ALLOC_STR_MUL(interp, n, sz)                                     \
    Th8_SafeAllocStrMul((interp), (n), (sz), __FILE__, __LINE__)

/*
 * Reallocation macros.
 *
 * The TH8_REALLOC / TH8_ATTEMPT_REALLOC pair is the canonical
 * wrapper for in-place buffer growth.  They mirror the
 * TH8_ALLOC family by capturing __FILE__ and __LINE__ for OOM
 * tracing diagnostics, so the audit checker can distinguish a
 * deliberate, reviewed call from a bare Th8_(Attempt)?Realloc
 * call (forbidden in core code).
 *
 * Callers that need overflow-safe size computation should use
 * TH8_SAFE_MUL_SIZE / TH8_SAFE_ADD_SIZE to derive the new byte
 * count, then pass it to TH8_(ATTEMPT_)REALLOC.
 *
 * Failure semantics:
 *   TH8_REALLOC          -- panics via xPanic on OOM (matches
 *                           Th8_Realloc).
 *   TH8_ATTEMPT_REALLOC  -- returns NULL on OOM, leaving the
 *                           original block intact (matches
 *                           Th8_AttemptRealloc).
 */
#define TH8_REALLOC(interp, ptr, nByte)                                      \
    Th8_SafeRealloc((interp), (ptr), (nByte), __FILE__, __LINE__)

#define TH8_ATTEMPT_REALLOC(interp, ptr, nByte)                              \
    Th8_SafeAttemptRealloc((interp), (ptr), (nByte), __FILE__, __LINE__)

#if defined(TH8_ENABLE_LOAD)
/*
 * Th8_ListAppendLoaded --
 *	Append the names of all loaded libraries to the list.
 */
TH8_API int Th8_ListAppendLoaded(Th8_Interp *interp, char **pz, size_t *pn);
#endif


/*
 *----------------------------------------------------------------------
 *
 * Internal-Representation Cache
 *
 *	A per-interpreter hash table that caches the result of
 *	string-to-value conversions (integer, double, list, command
 *	resolution).  The cache is protected by a per-interpreter
 *	mutex and may be cleared at any time.  All data stored in
 *	the cache is owned by the cache; callers receive borrowed
 *	pointers.
 *
 *----------------------------------------------------------------------
 */

#if defined(TH8_BENCHMARKING)
/*
 * Th8_GetCacheStats --
 *
 *	Retrieve IR cache performance counters.  Any output pointer
 *	may be NULL if that counter is not needed.
 */
TH8_API void Th8_GetCacheStats(
    Th8_Interp *interp,
    th8_uint64_t *pnHit,
    th8_uint64_t *pnMiss,
    th8_uint64_t *pnEvict);

/*
 * Th8_ResetCacheStats --
 *
 *	Reset all IR cache performance counters to zero.
 */
TH8_API void Th8_ResetCacheStats(Th8_Interp *interp);
#endif /* TH8_BENCHMARKING */

/*
 * Th8_FindInCache --
 *
 *	Look up or create a cache entry for the string z (n bytes,
 *	or TH8_NOLEN) under the given cacheType (TH8_CACHE_*).
 *
 *	On a cache hit the existing Th8_Value is returned; its
 *	type-specific union fields (u.integer, u.real, u.list) will
 *	already be populated if a previous caller filled them in.
 *
 *	On a cache miss a new Th8_Value is created with eType =
 *	TH8_VALUE_STRING and zData pointing to a cache-owned copy
 *	of z.  The caller should then compute the desired conversion
 *	(integer, double, list, etc.) and write it into the returned
 *	Th8_Value's union fields so future lookups are instant.
 *
 *	Returns a cache-owned pointer, or NULL on OOM.  The pointer
 *	is valid until the next th8ClearCache or Th8_DeleteInterp.
 *	Thread-safe: acquires the per-interpreter cache mutex.
 */
TH8_API Th8_Value *
Th8_FindInCache(Th8_Interp *interp, int cacheType, const char *z, size_t n);


/*
 * Th8_GetLastError --
 *	Retrieve the most recent OS error code via the platform's
 *	xGetLastError callback.  Returns -1 if interp is NULL or
 *	the callback is not available.  The parameter is void* so
 *	this header can be parsed before Th8_Interp is defined.
 */
TH8_API int Th8_GetLastError(void *interp);

/*
 * Th8_EmitTrace --
 *	Emit a diagnostic (trace) message using platform-specific
 *	mechanisms for both string formatting and the diagnostic
 *	output.
 */
TH8_API void Th8_EmitTrace(Th8_Interp *interp, const char *zFmt, ...);

/*
 * Th8_DoesEnvExist --
 *	Return non-zero if the named environment variable exists.
 */
TH8_API int Th8_DoesEnvExist(Th8_Interp *interp, const char *zName);

#ifdef __cplusplus
}
#endif

#endif /* TH8_H */
