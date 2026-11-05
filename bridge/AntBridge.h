/*
 * AntBridge.h - plain C bridge between agave (C++/MSVC-compatible) and Ant (GNU C23)
 *
 * Rules for this header:
 *   - no <ant.h> / <internal.h> include, no ant_value_t, no GNU extensions
 *   - everything is opaque; agave's C++ translation units only see this file
 *   - strings cross the boundary as UTF-8 copies owned by the caller
 */

#ifndef ANT_BRIDGE_H
#define ANT_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Default is empty: the bridge is normally linked statically into the engine.
 * Define AGB_BUILD_DLL when building it as a separate DLL, AGB_USE_DLL when
 * consuming that DLL. */
#if defined(AGB_BUILD_DLL) && defined(_WIN32)
#  define AGB_API __declspec(dllexport)
#elif defined(AGB_USE_DLL) && defined(_WIN32)
#  define AGB_API __declspec(dllimport)
#else
#  define AGB_API
#endif

typedef struct AntbEngine AntbEngine;   /* one Ant isolate (= one JS global context) */
typedef struct AntbValue  AntbValue;    /* a handle to a JS value or object          */

/* Optional diagnostic sink. The bridge has no dependency on the embedder's
 * logger; the host installs one after loading the DLL. */
typedef void (*AntbLogFn)(const char* line, void* user);
AGB_API void antb_set_logger(AntbLogFn fn, void* user);

/* Native method callback.
 *   self  - the `this` value (usually the object the method was read from)
 *   args  - argument array (may be NULL when argc == 0)
 *   out   - storage the callback must fill with its return value
 * Returns 1 on success, 0 to make the call throw / return undefined. */
typedef int (*AntbNativeFn)(AntbEngine* engine, AntbValue* self,
                            AntbValue** args, int argc, AntbValue* out, void* user);

/* property hook used by antb_define_property */
typedef int (*AntbGetterFn)(AntbEngine* engine, AntbValue* self,
                            const char* name, AntbValue* out, void* user);

/* setter counterpart; return 1 when the value was accepted */
typedef int (*AntbSetterFn)(AntbEngine* engine, AntbValue* self,
                            const char* name, const AntbValue* value, void* user);

/* ---- lifecycle ---------------------------------------------------------- */

/* `stack_anchor` must be the address of a local variable in the calling thread
 * (Ant uses it to bound the JS stack). Returns NULL on failure. */
AGB_API AntbEngine* antb_engine_create(const void* stack_anchor);

AGB_API void antb_engine_destroy(AntbEngine* engine);

/* ---- evaluation --------------------------------------------------------- */

/* Evaluates `src` (UTF-8, `len` bytes, may contain NUL) in the global scope.
 * On a JS exception returns NULL and, when `err` is not NULL, stores a
 * malloc'd UTF-8 message there (release with antb_free). */
AGB_API AntbValue* antb_eval(AntbEngine* engine, const char* src, int len, char** err);

/* Evaluates `src` with classic <script> semantics (a program): top-level
 * var/function declarations become properties of the global object, which is
 * what page scripts do and what the property inline caches cover. antb_eval()
 * keeps direct-eval semantics (bindings live in an eval environment), and
 * reads of those bindings miss the caches in JITed code - measurably ~20x
 * slower on real page scripts. Use this for <script>/prelude/polyfills. */
AGB_API AntbValue* antb_eval_script(AntbEngine* engine, const char* src, int len, char** err);

/* ES module support: evaluates `src` as a module whose URL is `filename`
 * (relative imports resolve against it), and installs the http(s) fetcher used
 * for module bodies. The fetcher must return a malloc'd NUL-terminated buffer. */
typedef char* (*AntbModuleFetcher)(const char* url, unsigned long* out_size);
AGB_API AntbValue* antb_eval_module(AntbEngine* engine, const char* src, int len,
                                    const char* filename, char** err);
AGB_API void antb_set_uncaught_nonfatal(AntbEngine* engine, int on);
AGB_API void antb_print_js_stack(AntbEngine* engine);
/* Crash path: dump the most recently created engine's JS stack. */
AGB_API void antb_print_last_js_stack(void);
AGB_API void antb_set_module_fetcher(AntbModuleFetcher fn);

/* Name used in stack traces for the next evaluations (usually the script URL). */
AGB_API void antb_set_filename(AntbEngine* engine, const char* name);

/* Runs one non-blocking pass of the engine event loop: timers, immediates and
 * microtasks. Call once per frame. */
AGB_API void antb_pump(AntbEngine* engine);

/* Pending-exception report from timer/immediate callbacks (NULL when clean). */
AGB_API char* antb_take_pending_error(AntbEngine* engine);

/* Name of the most recent bridged callback entered (diagnostics only). */
AGB_API const char* antb_last_call(void);

/* Installs a hook that runs before every bridged native callback, receiving the
 * engine that owns the method (used to switch per-view JS contexts). NULL
 * removes it. */
AGB_API void antb_set_context_activate(void (*fn)(AntbEngine* engine));

/* ---- value inspection --------------------------------------------------- */

AGB_API double antb_to_number(const AntbValue* value);

/* malloc'd UTF-8 representation (release with antb_free); never NULL. */
AGB_API char* antb_to_utf8(const AntbValue* value);

AGB_API int antb_is_number(const AntbValue* value);
AGB_API int antb_is_string(const AntbValue* value);
AGB_API int antb_is_function(const AntbValue* value);
/* 1 when both wrappers refer to the same JS value (used by removeEventListener). */
AGB_API int antb_value_same(const AntbValue* a, const AntbValue* b);

AGB_API void antb_value_release(AntbValue* value);

/* ---- objects (task 2-1) ------------------------------------------------- */

/* Creates a plain JS object. */
AGB_API AntbValue* antb_make_object(AntbEngine* engine);

/* Creates a JS string from UTF-8. */
AGB_API AntbValue* antb_make_string(AntbEngine* engine, const char* utf8);

/* Creates a JS number. */
AGB_API AntbValue* antb_make_number(AntbEngine* engine, double v);

/* Creates the JS null value (history.state is null, not undefined). */
AGB_API AntbValue* antb_make_null(AntbEngine* engine);

/* Creates an empty array; antb_array_push appends to it. */
AGB_API AntbValue* antb_make_array(AntbEngine* engine);
AGB_API void antb_array_push(AntbEngine* engine, AntbValue* array, const AntbValue* value);

/* Element access for arrays created by antb_make_array. */
AGB_API AntbValue* antb_array_get(AntbValue* array, int index);

/* Deletes `name` from obj (used by removeAttribute style APIs). */
AGB_API void antb_delete_prop(AntbEngine* engine, AntbValue* obj, const char* name);

/* obj.name = value */
AGB_API void antb_set_prop(AntbEngine* engine, AntbValue* obj, const char* name, const AntbValue* value);

/* obj.name, or NULL when the property does not exist / is undefined. */
AGB_API AntbValue* antb_get_prop(AntbEngine* engine, AntbValue* obj, const char* name);

/* Attaches an opaque C pointer to a JS object (the equivalent of
 * JSObjectSetPrivate). Retrieve it from a native callback with
 * antb_get_native(self, tag). */
AGB_API void  antb_set_native(AntbValue* obj, void* ptr, unsigned tag);
AGB_API void* antb_get_native(const AntbValue* obj, unsigned tag);

/* obj.name = function(...) { ... }. `user` is handed back to the callback. */
AGB_API void antb_define_method(AntbEngine* engine, AntbValue* obj, const char* name,
                                AntbNativeFn fn, void* user);

/* obj.name becomes an accessor whose getter is `fn`. */
AGB_API void antb_define_property(AntbEngine* engine, AntbValue* obj, const char* name,
                                  AntbGetterFn fn, void* user);

/* Same, with a setter: `obj.name = v` calls `setter`. */
AGB_API void antb_define_property_rw(AntbEngine* engine, AntbValue* obj, const char* name,
                                     AntbGetterFn getter, AntbSetterFn setter, void* user);

/* The global object of the engine. */
AGB_API AntbValue* antb_global(AntbEngine* engine);

/* Calls a JS function value with `this` = `self`. */
AGB_API AntbValue* antb_call(AntbEngine* engine, const AntbValue* fn, const AntbValue* self,
                             AntbValue** args, int argc, char** err);

/* Sets out->v from an existing handle (used by callbacks to return a value). */
AGB_API void antb_value_assign(AntbValue* out, const AntbValue* value);

/* ---- wrapper cache (task 2-2) ------------------------------------------- *
 * DOM requires that a C object maps to exactly one JS wrapper, so that
 * `el === document.getElementById('x')` holds and event listeners keep working.
 * antb_wrap_native() returns the existing wrapper for (ptr, tag) or creates one,
 * registers it as a GC root, and attaches `ptr` via antb_set_native(). */
AGB_API AntbValue* antb_wrap_native(AntbEngine* engine, void* ptr, unsigned tag);

/* Same, but the wrapper is owned by `owner` (the document that owns the
 * native object). When the owner goes away, antb_release_owner() drops the
 * wrappers, so the next document never gets a wrapper of the old one. */
AGB_API AntbValue* antb_wrap_native_owner(AntbEngine* engine, void* ptr, unsigned tag, void* owner);

/* Releases the wrappers that belong to `owner`. The GC roots of those
 * wrappers stay until the engine is destroyed (ant's GC cannot unpin), but
 * the cache stops handing them out. */
AGB_API void antb_release_owner(AntbEngine* engine, void* owner);

/* Makes a copy of `value` that survives garbage collection (the handle leaks on
 * purpose; use it for stored callbacks such as DOM event listeners). */
AGB_API AntbValue* antb_retain(const AntbValue* value);

/* JS truthiness of a value (used to assert `a === b` results). */
AGB_API int antb_to_boolean(const AntbValue* value);

/* Forces a full garbage collection (used by tests to prove wrappers stay alive). */
AGB_API void antb_gc(AntbEngine* engine);

/* ---- classes / prototypes (task 2-2) ------------------------------------ *
 * DOM methods must live on a shared prototype, not on every instance, or the
 * method-slot pool is exhausted immediately. A class owns a prototype object
 * whose methods/properties are shared by all wrappers of that class. */
typedef struct AntbClass AntbClass;

AGB_API AntbClass* antb_class_create(AntbEngine* engine, const char* name);

AGB_API void antb_class_add_method(AntbEngine* engine, AntbClass* klass,
                                   const char* name, AntbNativeFn fn, void* user);

AGB_API void antb_class_add_property(AntbEngine* engine, AntbClass* klass,
                                     const char* name, AntbGetterFn fn, void* user);

/* Wraps a C object as an instance of `klass`, caching per (ptr, tag). */
AGB_API AntbValue* antb_wrap_native_as(AntbEngine* engine, AntbClass* klass,
                                       void* ptr, unsigned tag);

/* Same, but the wrapper is owned by `owner` (the document that owns the
 * native object). */
AGB_API AntbValue* antb_wrap_native_as_owner(AntbEngine* engine, AntbClass* klass,
                                             void* ptr, unsigned tag, void* owner);

/* Internal helpers (still exported so the C++ side can use them):
 * antb_class_set_property registers a getter in the class table,
 * antb_class_install_properties attaches the dispatching getter to one instance. */
AGB_API int  antb_class_set_property(AntbEngine* engine, AntbClass* klass,
                                     const char* name, AntbGetterFn fn, void* user);
AGB_API void antb_class_install_properties(AntbEngine* engine, AntbClass* klass,
                                           AntbValue* instance);

/* ---- misc --------------------------------------------------------------- */

AGB_API void antb_free(void* p);

/* Engine-wide last error (UTF-8, owned by the engine; do not free). */
AGB_API const char* antb_last_error(const AntbEngine* engine);

#ifdef __cplusplus
}
#endif

#endif /* ANT_BRIDGE_H */
