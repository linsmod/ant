/*
 * AntBridge.c - the only translation unit that includes Ant's GNU C23 headers.
 * Everything above this file (agave's C++) sees AntBridge.h only.
 */

#include "AntBridge.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdatomic.h>

#include "ant.h"
#include "esm/loader.h"
#include "internal.h"
#include "realm.h"      /* ant_realm_create / enter / collect */
#include "errors.h"   /* js_print_stack_trace_vm */
#include "cfunc.h"     /* js_mkfun_dyn */
#include "descriptors.h" /* js_set_accessor_desc */
#include "reactor.h"   /* ant_uv_run, js_poll_events */
#include "runtime.h"
#include "debug.h"   /* sv_debug_enable: AGB_JIT_DEBUG */
#include "bootstrap.h"
#include "modules/timer.h" /* ant_realm_install_timers: browser realms need timers */
#include "modules/textcodec.h" /* td_set_provider: host-supplied legacy codecs */
#include "ptr.h"        /* js_set_native / js_get_native (static inline) */
#include "gc/roots.h"   /* gc_register_root: keep wrapper objects alive */
#include "gc.h"         /* gc_run: forced collection in tests */
#include "interrupt.h"  /* sv_jit_arm_interrupts: arming happens in antb_engine_create only */

/* Optional host-provided diagnostics. The bridge must not include the
 * embedder's logging header: antjsb is built inside the ant tree. */
static AntbLogFn g_logger = NULL;
static void*     g_loggerUser = NULL;

/* Most recently created engine: only used by the crash path, where the host
 * has no engine handle anymore (antb_print_last_js_stack). */
static AntbEngine* g_lastEngine = NULL;
static int g_call_debug = -1;

/* Every live engine. A stuck page script is aborted by the host from whatever
 * thread saw it hang (the UI thread, a test), and the flag lives on the isolate
 * the engine thread is currently running - so set it on all of them. Only the
 * engine whose code is inside a loop can observe it; the others are parked in
 * the reactor. The flag is cleared at the start of every top-level eval, so a
 * request that races with the end of the script cannot kill the next page.
 *
 * Threading: registration/unregistration happen on the engine thread, the
 * scans run on the watchdog/UI thread. Everything (the scan *and* the field
 * accesses it makes) is done under g_enginesLock, and antb_engine_destroy()
 * unlinks the engine under that lock before it tears anything down: a reader
 * can never hold a pointer to an engine that is already being freed. The
 * critical sections are a few pointer reads/one atomic store. */
static AntbEngine* g_engines[32];
static void antb_begin_top_level(ant_t* js);
static int g_engineCount = 0;
static atomic_flag g_enginesLock = ATOMIC_FLAG_INIT;

static void engines_lock(void)
{
    while (atomic_flag_test_and_set_explicit(&g_enginesLock, memory_order_acquire)) {
        /* spin: the holder is only scanning a handful of slots */
    }
}

static void engines_unlock(void)
{
    atomic_flag_clear_explicit(&g_enginesLock, memory_order_release);
}

static AntbValue* alloc_value(ant_t* js, ant_value_t v);

void antb_set_logger(AntbLogFn fn, void* user)
{
    g_logger = fn;
    g_loggerUser = user;
}

static void antbLog(const char* fmt, ...)
{
    char line[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    if (g_logger)
        g_logger(line, g_loggerUser);
    else
        fprintf(stderr, "%s\n", line);
}

struct AntbFilename {
    struct AntbFilename* next;
    char                 text[];
};

struct AntbEngine {
    ant_t* js;
    char   err[512];
    /* Owned copies of every filename ever set. The engine hands the current
     * pointer to compiled functions (fn->debug->filename), so an old copy must
     * stay alive for the whole engine lifetime - freeing the previous copy when
     * a new script starts (as an earlier revision did) left dangling names in
     * the stack frames of already-compiled scripts: error stacks then rendered
     * garbled file names. */
    struct AntbFilename* filenames;
    /* ant_runtime_init() only stores the argv pointer; it is read long after
     * the call (process.argv, and worker_build_argv() when a Worker starts,
     * which strdup()s argv[0]). A stack-local vector dangled: the first worker
     * still saw "agave" in the dead frame, the next one got NULL and the new
     * worker thread died in strlen() during bootstrap. */
    char* argv[2];
};

struct AntbValue {
    ant_t*      js;
    ant_value_t v;
};

/* ------------------------------------------------------------------------- */

static void copy_cstr(AntbEngine* e, const char* what, ant_value_t v)
{
    char nbuf[128];
    char mbuf[384];
    js_cstr_t name;
    js_cstr_t msg;
    ant_value_t nv = js_get(e->js, v, "name");
    ant_value_t mv = js_get(e->js, v, "message");

    if (vtype(nv) == kTypeString && vtype(mv) == kTypeString) {
        /* Include the first stack line when the engine provides one: the
         * message alone does not say where a page script broke. */
        ant_value_t sv = js_get(e->js, v, "stack");
        char sbuf[512];
        js_cstr_t stk;
        const char* stack = "";

        name = js_to_cstr(e->js, nv, nbuf, sizeof(nbuf));
        msg  = js_to_cstr(e->js, mv, mbuf, sizeof(mbuf));

        if (vtype(sv) == kTypeString) {
            stk = js_to_cstr(e->js, sv, sbuf, sizeof(sbuf));
            if (stk.ptr && stk.len)
                stack = stk.ptr;
        }

        if (stack[0])
            snprintf(e->err, sizeof(e->err), "%s: %.*s: %.*s | %s", what,
                     (int)name.len, name.ptr ? name.ptr : "",
                     (int)msg.len, msg.ptr ? msg.ptr : "", stack);
        else
            snprintf(e->err, sizeof(e->err), "%s: %.*s: %.*s", what,
                     (int)name.len, name.ptr ? name.ptr : "",
                     (int)msg.len, msg.ptr ? msg.ptr : "");
    } else if (vtype(nv) == kTypeString) {
        name = js_to_cstr(e->js, nv, nbuf, sizeof(nbuf));
        snprintf(e->err, sizeof(e->err), "%s: %.*s", what,
                 (int)name.len, name.ptr ? name.ptr : "");
    } else {
        char buf[512];
        js_cstr_t s = js_inspect_cstr(e->js, v, buf, sizeof(buf));
        snprintf(e->err, sizeof(e->err), "%s: %.*s", what,
                 s.len ? (int)s.len : 0, s.ptr ? s.ptr : "(unknown)");
    }
}

/* The wrapper, class and native-method tables are process wide; an engine that
 * goes away takes its entries with it (see antb_engine_destroy). */
static void agbReleaseEngineWrappers(AntbEngine* engine);
static void agbReleaseEngineClasses(AntbEngine* engine);
static void agbReleaseEngineMethods(AntbEngine* engine);

AntbEngine* antb_engine_create(const void* stack_anchor)
{
    AntbEngine* e = (AntbEngine*)calloc(1, sizeof(AntbEngine));
    if (!e)
        return NULL;

    e->js = ant_create(NULL);
    if (!e->js) {
        free(e);
        return NULL;
    }

    /* Ant needs the caller's stack bounds; without them the VM can overflow. */
    js_setstackbase(e->js, (void*)stack_anchor);
    js_setstacklimit(e->js, 32u * 1024u * 1024u);

    /* Arm interrupt polling from the start. The flag is consulted when JIT code
     * is emitted (emit_common.c), so arming it only when the host asks for an
     * abort is too late: functions compiled earlier have no poll sites and a
     * stuck loop keeps running until it returns on its own - the browser then
     * looks dead even though the request was accepted. Cost is one load and a
     * predictable branch per loop backedge / call. */
    sv_jit_arm_interrupts(e->js);

    /* Lifetime must match the engine: ant keeps this pointer. */
    e->argv[0] = (char*)"agave";
    e->argv[1] = NULL;
    ant_runtime_init(e->js, 1, e->argv, NULL);
    ant_bootstrap_modules(e->js);
    js_set_filename(e->js, "agave://page.js");

    /* AGB_JIT_DEBUG=1 turns on ant's JIT diagnostics (ineligible ops, OSR
     * compile results, bailout disables) for embedded engines: the CLI's
     * ANT_DEBUG parser is not reachable from the bridge. */
    {
        const char* jd = getenv("AGB_JIT_DEBUG");
        if (jd && jd[0] && strcmp(jd, "0") != 0) sv_debug_enable(SV_DEBUG_JIT_WARN);
    }

    g_lastEngine = e;

    engines_lock();
    if (g_engineCount < (int)(sizeof(g_engines) / sizeof(g_engines[0])))
        g_engines[g_engineCount++] = e;
    engines_unlock();

    return e;
}

void antb_engine_destroy(AntbEngine* engine)
{
    if (!engine)
        return;

    if (g_lastEngine == engine)
        g_lastEngine = NULL;

    /* Unlink before any teardown: a concurrent antb_interrupt_all_engines() /
     * antb_engines_js_depth() must not be able to reach an engine whose js is
     * about to be destroyed. */
    engines_lock();
    for (int i = 0; i < g_engineCount; ++i) {
        if (g_engines[i] != engine) continue;
        g_engines[i] = g_engines[--g_engineCount];
        break;
    }
    engines_unlock();

    /* The wrapper, class and native-method tables are process wide. An engine
     * that goes away must take its entries with it: a stale js pointer, a
     * wrapper that is handed out again, a table that runs out of slots (the
     * class table had 128 entries and one class per engine - one engine per
     * document; the method table is the one a reload cycle exhausts first). */
    agbReleaseEngineWrappers(engine);
    agbReleaseEngineClasses(engine);
    agbReleaseEngineMethods(engine);

    if (engine->js)
        js_destroy(engine->js);
    {
        struct AntbFilename* fn = engine->filenames;
        while (fn) {
            struct AntbFilename* next = fn->next;
            free(fn);
            fn = next;
        }
    }
    free(engine);
}

void antb_set_filename(AntbEngine* engine, const char* name)
{
    if (!engine)
        return;

    /* Take a copy: callers hand in transient strings (e.g. a script URL owned
     * by the HTML tokenizer), and compiled functions keep pointing at the copy
     * from their debug info, so every copy lives as long as the engine. */
    struct AntbFilename* node = NULL;
    if (name) {
        node = (struct AntbFilename*)malloc(sizeof(struct AntbFilename) + strlen(name) + 1);
        if (!node)
            return;                      /* keep the previous name on OOM */
        strcpy(node->text, name);
        node->next = engine->filenames;
        engine->filenames = node;
    }

    if (engine->js)
        js_set_filename(engine->js, node ? node->text : "agave://page.js");
}

static AntbValue* antb_eval_mode(AntbEngine* engine, const char* src, int len, char** err, int script)
{
    ant_value_t v;

    if (err)
        *err = NULL;
    if (!engine || !engine->js || !src || len < 0)
        return NULL;

    antb_begin_top_level(engine->js);


    v = script
        ? js_eval_bytecode(engine->js, src, (size_t)len)
        : js_eval_bytecode_eval(engine->js, src, (size_t)len);

    if (is_err(v)) {
        /* The value returned by eval is an error marker; the real thrown value
         * lives in the isolate until it is taken. */
        ant_value_t thrown = js_take_thrown(engine->js, v);
        copy_cstr(engine, "evaluation failed", is_err(thrown) ? v : thrown);
        if (err) {
            *err = (char*)malloc(strlen(engine->err) + 1);
            if (*err)
                strcpy(*err, engine->err);
        }
        return NULL;
    }

    {
        AntbValue* out = (AntbValue*)calloc(1, sizeof(AntbValue));
        if (!out)
            return NULL;
        out->js = engine->js;
        out->v  = v;
        return out;
    }
}

AntbValue* antb_eval(AntbEngine* engine, const char* src, int len, char** err)
{
    return antb_eval_mode(engine, src, len, err, 0);
}

AntbValue* antb_eval_script(AntbEngine* engine, const char* src, int len, char** err)
{
    return antb_eval_mode(engine, src, len, err, 1);
}
/* Evaluates `src` as an ES module whose URL is `filename`, so relative imports
 * resolve against it. Errors are reported the same way as antb_eval(). */
AntbValue* antb_eval_module(AntbEngine* engine, const char* src, int len,
                            const char* filename, char** err)
{
    ant_value_t v;

    if (err)
        *err = NULL;
    if (!engine || !engine->js || !src || len < 0)
        return NULL;

    antb_begin_top_level(engine->js);

    if (filename && filename[0]) {
        struct AntbFilename* node =
            (struct AntbFilename*)malloc(sizeof(struct AntbFilename) + strlen(filename) + 1);
        if (node) {
            strcpy(node->text, filename);
            node->next = engine->filenames;
            engine->filenames = node;
            js_set_filename(engine->js, node->text);
        }
    }

    /* Raw js_eval_bytecode_module() compiles in module mode but runs without a
     * module record: top-level `export` then dies with "export used outside
     * module", and import.meta/dynamic import have no owner. This is the entry
     * path ant's CLI uses: it creates the eval context, the module namespace and
     * import.meta, and resolves imports through the fetcher. */
    {
        ant_value_t ns = Ant_Object_CreateBare(engine->js);

        if (is_err(ns)) {
            copy_cstr(engine, "module evaluation failed", ns);
            if (err) {
                *err = (char*)malloc(strlen(engine->err) + 1);
                if (*err)
                    strcpy(*err, engine->err);
            }
            return NULL;
        }

        v = js_esm_eval_module_source(
            engine->js,
            (filename && filename[0]) ? filename : "agave://module",
            src, (size_t)len, ns);
    }

    if (is_err(v)) {
        ant_value_t thrown = js_take_thrown(engine->js, v);
        copy_cstr(engine, "module evaluation failed", is_err(thrown) ? v : thrown);
        if (err) {
            *err = (char*)malloc(strlen(engine->err) + 1);
            if (*err)
                strcpy(*err, engine->err);
        }
        return NULL;
    }

    {
        AntbValue* out = (AntbValue*)calloc(1, sizeof(AntbValue));

        if (!out)
            return NULL;
        out->js = engine->js;
        out->v  = v;
        return out;
    }
}

/* A browser must not die on an unhandled promise rejection: Ant's default is
 * Node's "unhandled rejection is fatal" (process_exit_with(EXIT_FAILURE) in
 * report_unhandled_rejection), which closes the whole browser when one page
 * promise goes unhandled. */
/* Dump the live JS stack of the engine thread. Used by the hang watchdog:
 * the caller suspends the engine thread first, so the VM state is frozen. */
void antb_print_js_stack(AntbEngine* engine)
{
    if (!engine || !engine->js)
        return;

    js_print_stack_trace_vm(engine->js, stderr);
    fflush(stderr);
}

/* Crash path: the host only has a faulting stack but no engine handle, so use
 * the most recently created engine. Best-effort - a fault inside the VM walk
 * just means the native stack already on file is all we get. */
void antb_print_last_js_stack(void)
{
    antb_print_js_stack(g_lastEngine);
}

/* Host-driven abort of a stuck page script: the browser's Reload/Stop while a
 * page loop never returns. The VM polls interrupt_pending in interpreter calls
 * and at JIT loop backedges, raises a termination error page code cannot catch,
 * and unwinds to the host eval. Callable from any thread (atomic store); engines
 * that are not executing anything simply carry the flag, and the next top-level
 * eval clears it.
 *
 * Deliberately does NOT (re-)arm the JIT here: arming happens once, on the
 * engine thread, in antb_engine_create(). sv_jit_arm_interrupts() is not a
 * bit-set - it calls jit_drop_all_code(), which frees the code pages the engine
 * thread may be executing right now. Calling it from the watchdog/UI thread was
 * a latent cross-thread use-after-free (harmless today only because the armed
 * flag made it return early).
 *
 * Worker isolates are not in g_engines[]: they have their own terminate(). */
void antb_interrupt_all_engines(void)
{
    engines_lock();

    for (int i = 0; i < g_engineCount; ++i) {
        AntbEngine* engine = g_engines[i];
        if (!engine || !engine->js) continue;
        atomic_store_explicit(&engine->js->interrupt_pending, 1, memory_order_relaxed);
    }

    engines_unlock();
}

/* How deep the VM's JS call stack is right now (0 = no JS executing). Ant uses
 * the very same field to tell whether it is at a job boundary
 * (src/modules/timer.c: `At_job_boundary = js->vm_exec_depth == 0`), so this is
 * literally "is a page script on the stack". The host polls it from another
 * thread; the read happens under the engine-table lock so a concurrent
 * antb_engine_destroy() cannot free the isolate under it (a torn value was
 * never a concern - the caller only compares against zero). */
unsigned int antb_engines_js_depth(void)
{
    unsigned int depth = 0;

    engines_lock();

    for (int i = 0; i < g_engineCount; ++i) {
        AntbEngine* engine = g_engines[i];
        if (!engine || !engine->js) continue;
        if (engine->js->vm_exec_depth > depth) depth = engine->js->vm_exec_depth;
    }

    engines_unlock();

    return depth;
}

/* A top-level eval starts non-terminating: a previous abort (or a request that
 * raced with the end of the last script) must not leak into this one. */
static void antb_begin_top_level(ant_t* js)
{
    if (!js) return;
    js->terminating = false;
    atomic_store_explicit(&js->interrupt_pending, 0, memory_order_relaxed);
}

/* Debugger path: write the JS stack to a file instead of stderr. A debugger
 * attached to a running browser cannot read the debuggee's stderr, and
 * .call'ing freopen/_dup2 inside it to redirect is a footgun - one unresolvable
 * cdb command aborts the command table before qd and the process is terminated
 * (docs/dev-debugging.md). Let the engine do the writing:
 *
 *   cdb -p <pid> -c "eza antjsb!antb_dump_path \"E:/tmp/js.txt\"; ^
 *                    .call antjsb!antb_dump_last_js_stack_to_file(antjsb!antb_dump_path); qd"
 *
 * cdb's MASM expression parser rejects string literals in .call, so the path is
 * deposited in antb_dump_path first (a plain writable buffer) and only its
 * address is passed. Unbuffered on purpose: if the VM walk itself faults, the
 * frames printed so far survive. Returns 1 on success. */
char antb_dump_path[512] = "agb_jsstack.txt";

int antb_dump_js_stack_to_file(AntbEngine* engine, const char* path)
{
    FILE* f;

    if (!path || !*path)
        path = antb_dump_path;

    f = fopen(path, "w");
    if (!f)
        return 0;

    setvbuf(f, NULL, _IONBF, 0);

    if (engine && engine->js) {
        js_print_stack_trace_vm(engine->js, f);
    } else {
        fprintf(f, "(no engine)\n");
        fclose(f);
        return 0;
    }

    fflush(f);
    fclose(f);
    return 1;
}

int antb_dump_last_js_stack_to_file(const char* path)
{
    return antb_dump_js_stack_to_file(g_lastEngine, path);
}

/* ---- legacy text codecs ------------------------------------------------- *
 * The host owns the tables; ant only routes unknown labels here. The provider
 * is translated from the public bridge struct into ant's own td_provider_t so
 * the engine side never sees Antb* types. */

static int bridge_textcodec_resolve(
    void* user, const char* label, size_t len, char* out, size_t cap
) {
    const AntbTextCodecProvider* host = (const AntbTextCodecProvider*)user;

    if (!host || !host->resolve) return 0;
    return host->resolve(host->user, label, len, out, cap);
}

static long bridge_textcodec_decode(
    void* user, const char* label,
    const uint8_t* in, size_t in_len,
    char* out, size_t out_cap, bool fatal
) {
    const AntbTextCodecProvider* host = (const AntbTextCodecProvider*)user;

    if (!host || !host->decode) return -1;
    return host->decode(host->user, label, in, in_len, out, out_cap, fatal ? 1 : 0);
}

void antb_set_text_codec_provider(const AntbTextCodecProvider* provider)
{
    /* The provider outlives every engine, so the translated struct is static;
     * the host's pointer is copied in by value to survive a host that passes a
     * stack temporary. */
    static AntbTextCodecProvider hostCopy;
    static td_provider_t antProvider;

    if (!provider || (!provider->resolve && !provider->decode)) {
        td_set_provider(NULL);
        return;
    }

    hostCopy = *provider;

    antProvider.resolve = bridge_textcodec_resolve;
    antProvider.decode  = bridge_textcodec_decode;
    antProvider.user    = &hostCopy;

    td_set_provider(&antProvider);
}

/* ---- realms ------------------------------------------------------------- */

AntbRealm* antb_realm_create(AntbEngine* engine)
{
    ant_realm_t* realm;

    if (!engine || !engine->js)
        return NULL;

    /* ant_realm_create() restores the previous realm before returning. Every
     * browser frame gets its own realm, and that realm's global needs its own
     * setTimeout/setInterval: init_timer_module() only installed them on the
     * main realm's global at engine startup. Enter the new realm first so the
     * native functions are interned with that realm as their owner (the VM
     * switches back to the owner realm when the timer callback fires). */
    realm = ant_realm_create(engine->js);
    if (!realm)
        return NULL;

    {
        ant_realm_t* prev = ant_realm_enter(engine->js, realm);

        ant_realm_install_timers(engine->js);
        ant_realm_enter(engine->js, prev);
    }

    return (AntbRealm*)realm;
}

AntbRealm* antb_realm_enter(AntbEngine* engine, AntbRealm* realm)
{
    if (!engine || !engine->js || !realm)
        return NULL;

    return (AntbRealm*)ant_realm_enter(engine->js, (ant_realm_t*)realm);
}

AntbRealm* antb_realm_current(AntbEngine* engine)
{
    if (!engine || !engine->js)
        return NULL;

    return (AntbRealm*)engine->js->realm;
}

AntbValue* antb_realm_global(AntbEngine* engine, AntbRealm* realm)
{
    ant_realm_t* r = (ant_realm_t*)realm;

    if (!engine || !engine->js || !r)
        return NULL;

    return alloc_value(engine->js, r->global);
}

void antb_realm_collect(AntbEngine* engine)
{
    if (engine && engine->js)
        ant_realm_collect(engine->js);
}

int antb_value_identical(const AntbValue* a, const AntbValue* b)
{
    if (!a || !b)
        return 0;

    return a->js == b->js && a->v == b->v;
}

void antb_set_uncaught_nonfatal(AntbEngine* engine, int on)
{
    if (engine && engine->js) {
        engine->js->uncaught_nonfatal = on ? true : false;
        antbLog("set js=%p on=%d -> %d", (void*)engine->js, on, (int)engine->js->uncaught_nonfatal);
    }
}

void antb_set_module_fetcher(AntbModuleFetcher fn)
{
    esm_set_http_fetch((esm_http_fetch_fn)fn);
}

/* Browser import maps: a host-provided resolver for bare module specifiers
 * (agave passes the current document's parsed <script type="importmap">). */
void antb_set_import_map_resolver(AntbImportMapResolver fn)
{
    esm_set_import_map_resolver((esm_import_map_resolve_fn)fn);
}

double antb_to_number(const AntbValue* value)
{
    if (!value)
        return 0.0;
    return js_getnum(value->v);
}

int antb_is_function(const AntbValue* value)
{
    return value && value->js && vtype(value->v) == kTypeFunction;
}

int antb_is_string(const AntbValue* value)
{
    return value && value->js && vtype(value->v) == kTypeString;
}

int antb_is_boolean(const AntbValue* value)
{
    return value && value->js && vtype(value->v) == kTypeBool;
}

/* Identity test for two wrappers: removeEventListener gets back the same
 * function object the page registered, and must not remove a different one. */
int antb_value_same(const AntbValue* a, const AntbValue* b)
{
    if (!a || !b || a->js != b->js)
        return 0;

    return strict_eq_values(a->js, a->v, b->v) ? 1 : 0;
}

int antb_is_number(const AntbValue* value)
{
    if (!value)
        return 0;
    return vtype(value->v) == kTypeNumber;
}

char* antb_to_utf8(const AntbValue* value)
{
    char buf[512];
    js_cstr_t s;
    char* out;

    if (!value)
        return NULL;

    s = js_to_cstr(value->js, value->v, buf, sizeof(buf));
    if (!s.ptr)
        return NULL;

    out = (char*)malloc(s.len + 1);
    if (!out)
        return NULL;
    memcpy(out, s.ptr, s.len);
    out[s.len] = '\0';
    return out;
}

void antb_value_release(AntbValue* value)
{
    free(value);
}

void antb_free(void* p)
{
    free(p);
}

const char* antb_last_error(const AntbEngine* engine)
{
    return engine ? engine->err : "";
}

/* ========================================================================= */
/* objects and native methods (task 2-1)                                     */
/* ========================================================================= */

#define AGB_TAG_METHOD 0x414D4554u   /* 'AMET' */
#define AGB_TAG_GETTER 0x41474554u   /* 'AGET' */

typedef struct AntbClass AntbClass;

typedef struct {
    AntbEngine*  engine;
    AntbGetterFn fn;      /* single-property getter (document.title) */
    AntbSetterFn setter;  /* optional setter */
    void*        user;
    AntbClass*   klass;   /* when set, dispatch by name through the class table */
} agb_getter_entry;

static AntbValue* alloc_value(ant_t* js, ant_value_t v)
{
    AntbValue* out = (AntbValue*)calloc(1, sizeof(AntbValue));
    if (!out)
        return NULL;
    out->js = js;
    out->v  = v;
    return out;
}

AntbValue* antb_make_object(AntbEngine* engine)
{
    if (!engine || !engine->js)
        return NULL;
    return alloc_value(engine->js, mkobj(engine->js, 0));
}

AntbValue* antb_make_string(AntbEngine* engine, const char* utf8)
{
    if (!engine || !engine->js || !utf8)
        return NULL;
    return alloc_value(engine->js, js_mkstr(engine->js, utf8, strlen(utf8)));
}

AntbValue* antb_make_null(AntbEngine* engine)
{
    AntbValue* out;

    if (!engine || !engine->js)
        return NULL;

    out = alloc_value(engine->js, js_mknull());
    return out;
}

AntbValue* antb_make_number(AntbEngine* engine, double v)
{
    if (!engine || !engine->js)
        return NULL;
    return alloc_value(engine->js, js_mknum(v));
}

AntbValue* antb_make_boolean(AntbEngine* engine, int v)
{
    if (!engine || !engine->js)
        return NULL;
    return alloc_value(engine->js, js_bool(v ? true : false));
}

AntbValue* antb_make_array(AntbEngine* engine)
{
    AntbValue* out;

    if (!engine || !engine->js)
        return NULL;

    out = alloc_value(engine->js, js_mkarr(engine->js));
    return out;
}

AntbValue* antb_array_get(AntbValue* array, int index)
{
    char key[16];

    if (!array || !array->js)
        return NULL;

    snprintf(key, sizeof(key), "%d", index);
    return alloc_value(array->js, js_get(array->js, array->v, key));
}

void antb_array_push(AntbEngine* engine, AntbValue* array, const AntbValue* value)
{
    if (!engine || !array || !value)
        return;
    js_arr_push(engine->js, array->v, value->v);
}

void antb_delete_prop(AntbEngine* engine, AntbValue* obj, const char* name)
{
    ant_value_t key;

    if (!engine || !obj || !name)
        return;

    key = js_mkstr(engine->js, name, strlen(name));
    js_setprop(engine->js, obj->v, key, js_mkundef());
}

void antb_set_prop(AntbEngine* engine, AntbValue* obj, const char* name, const AntbValue* value)
{
    if (!engine || !obj || !name || !value)
        return;
    js_setprop(engine->js, obj->v,
               js_mkstr(engine->js, name, strlen(name)), value->v);
}

AntbValue* antb_get_prop(AntbEngine* engine, AntbValue* obj, const char* name)
{
    ant_value_t v;

    if (!engine || !obj || !name)
        return NULL;

    v = js_getprop_fallback(engine->js, obj->v, name);
    if (vtype(v) == kTypeUndefined || is_err(v))
        return NULL;

    return alloc_value(engine->js, v);
}

void antb_set_native(AntbValue* obj, void* ptr, unsigned tag)
{
    if (obj)
        js_set_native(obj->v, ptr, (uint32_t)tag);
}

void* antb_get_native(const AntbValue* obj, unsigned tag)
{
    if (!obj)
        return NULL;
    return js_get_native(obj->v, (uint32_t)tag);
}

/* --- native methods ------------------------------------------------------ */

/* Ant's cfunc values carry no per-instance identity (js_set_native and property
 * lookup both fail on T_CFUNC), and its own modules give every method its own C
 * function. We do the same with a generated pool of trampolines: each slot
 * dispatches to the (object, callback) pair registered for it. */
#define AGB_TRAMPOLINE_COUNT 4096

typedef struct {
    AntbEngine*  engine;
    ant_t*       js;
    ant_value_t  obj;
    char         name[64];
    AntbNativeFn fn;
    void*        user;
} agb_method_entry;

static agb_method_entry g_methods[AGB_TRAMPOLINE_COUNT];

/* Diagnostics: name of the last bridged callback entered, so errors raised from
 * native code (which have no VM frame) can still be attributed. */
static const char* g_last_call = "";
static int g_trace_calls = -1;

/* Runs before every bridged native callback with the engine that owns the
 * method; lets the host switch to that engine's JS context (per-view contexts). */
static void (*g_context_activate)(AntbEngine* engine) = NULL;

/* Runs after that callback returns: the host restores whatever derived binding
 * state it switched in g_context_activate. ant restores its own realm after the
 * call, but the host cannot see that moment, so the pair has to bracket the
 * callback itself. */
static void (*g_context_leave)(AntbEngine* engine) = NULL;

void antb_set_context_activate(void (*fn)(AntbEngine* engine))
{
    g_context_activate = fn;
}

void antb_set_context_leave(void (*fn)(AntbEngine* engine))
{
    g_context_leave = fn;
}

/* Returns a malloc'd description of a pending exception and clears it, or NULL
 * when the engine is clean. Timer callbacks run outside evaluation, so their
 * exceptions would otherwise stay pending and poison later calls. */
char* antb_take_pending_error(AntbEngine* engine)
{
    ant_t* js;
    char* out;

    if (!engine || !(js = engine->js))
        return NULL;

    if (!Ant_Exception_Pending(js))
        return NULL;

    copy_cstr(engine, "uncaught", Ant_Exception_Current(js));
    Ant_Exception_Clear(js);

    out = (char*)malloc(strlen(engine->err) + 1);
    if (out)
        strcpy(out, engine->err);
    return out;
}

void antb_pump(AntbEngine* engine)
{
    ant_t* js;

    if (!engine || !(js = engine->js))
        return;

    if (js->loop)
        ant_uv_run(js->loop, UV_RUN_NOWAIT);
    js_poll_events(js);
}

AGB_API const char* antb_last_call(void)
{
    return g_last_call;
}
static int              g_method_count = 0;

static ant_value_t agb_dispatch(int slot, ant_t* js, ant_value_t* args, int nargs,
                                ant_value_t call_new_target);

#define AGB_TRAMP_DECL(n) \
    static ant_value_t agb_tramp_##n(ant_t* js, ant_value_t* args, int nargs, \
                                     ant_value_t call_new_target) \
    { return agb_dispatch(n, js, args, nargs, call_new_target); }

AGB_TRAMP_DECL(0)  AGB_TRAMP_DECL(1)  AGB_TRAMP_DECL(2)  AGB_TRAMP_DECL(3)  AGB_TRAMP_DECL(4)  AGB_TRAMP_DECL(5)  AGB_TRAMP_DECL(6)  AGB_TRAMP_DECL(7)
AGB_TRAMP_DECL(8)  AGB_TRAMP_DECL(9)  AGB_TRAMP_DECL(10)  AGB_TRAMP_DECL(11)  AGB_TRAMP_DECL(12)  AGB_TRAMP_DECL(13)  AGB_TRAMP_DECL(14)  AGB_TRAMP_DECL(15)
AGB_TRAMP_DECL(16)  AGB_TRAMP_DECL(17)  AGB_TRAMP_DECL(18)  AGB_TRAMP_DECL(19)  AGB_TRAMP_DECL(20)  AGB_TRAMP_DECL(21)  AGB_TRAMP_DECL(22)  AGB_TRAMP_DECL(23)
AGB_TRAMP_DECL(24)  AGB_TRAMP_DECL(25)  AGB_TRAMP_DECL(26)  AGB_TRAMP_DECL(27)  AGB_TRAMP_DECL(28)  AGB_TRAMP_DECL(29)  AGB_TRAMP_DECL(30)  AGB_TRAMP_DECL(31)
AGB_TRAMP_DECL(32)  AGB_TRAMP_DECL(33)  AGB_TRAMP_DECL(34)  AGB_TRAMP_DECL(35)  AGB_TRAMP_DECL(36)  AGB_TRAMP_DECL(37)  AGB_TRAMP_DECL(38)  AGB_TRAMP_DECL(39)
AGB_TRAMP_DECL(40)  AGB_TRAMP_DECL(41)  AGB_TRAMP_DECL(42)  AGB_TRAMP_DECL(43)  AGB_TRAMP_DECL(44)  AGB_TRAMP_DECL(45)  AGB_TRAMP_DECL(46)  AGB_TRAMP_DECL(47)
AGB_TRAMP_DECL(48)  AGB_TRAMP_DECL(49)  AGB_TRAMP_DECL(50)  AGB_TRAMP_DECL(51)  AGB_TRAMP_DECL(52)  AGB_TRAMP_DECL(53)  AGB_TRAMP_DECL(54)  AGB_TRAMP_DECL(55)
AGB_TRAMP_DECL(56)  AGB_TRAMP_DECL(57)  AGB_TRAMP_DECL(58)  AGB_TRAMP_DECL(59)  AGB_TRAMP_DECL(60)  AGB_TRAMP_DECL(61)  AGB_TRAMP_DECL(62)  AGB_TRAMP_DECL(63)
AGB_TRAMP_DECL(64)  AGB_TRAMP_DECL(65)  AGB_TRAMP_DECL(66)  AGB_TRAMP_DECL(67)  AGB_TRAMP_DECL(68)  AGB_TRAMP_DECL(69)  AGB_TRAMP_DECL(70)  AGB_TRAMP_DECL(71)
AGB_TRAMP_DECL(72)  AGB_TRAMP_DECL(73)  AGB_TRAMP_DECL(74)  AGB_TRAMP_DECL(75)  AGB_TRAMP_DECL(76)  AGB_TRAMP_DECL(77)  AGB_TRAMP_DECL(78)  AGB_TRAMP_DECL(79)
AGB_TRAMP_DECL(80)  AGB_TRAMP_DECL(81)  AGB_TRAMP_DECL(82)  AGB_TRAMP_DECL(83)  AGB_TRAMP_DECL(84)  AGB_TRAMP_DECL(85)  AGB_TRAMP_DECL(86)  AGB_TRAMP_DECL(87)
AGB_TRAMP_DECL(88)  AGB_TRAMP_DECL(89)  AGB_TRAMP_DECL(90)  AGB_TRAMP_DECL(91)  AGB_TRAMP_DECL(92)  AGB_TRAMP_DECL(93)  AGB_TRAMP_DECL(94)  AGB_TRAMP_DECL(95)
AGB_TRAMP_DECL(96)  AGB_TRAMP_DECL(97)  AGB_TRAMP_DECL(98)  AGB_TRAMP_DECL(99)  AGB_TRAMP_DECL(100)  AGB_TRAMP_DECL(101)  AGB_TRAMP_DECL(102)  AGB_TRAMP_DECL(103)
AGB_TRAMP_DECL(104)  AGB_TRAMP_DECL(105)  AGB_TRAMP_DECL(106)  AGB_TRAMP_DECL(107)  AGB_TRAMP_DECL(108)  AGB_TRAMP_DECL(109)  AGB_TRAMP_DECL(110)  AGB_TRAMP_DECL(111)
AGB_TRAMP_DECL(112)  AGB_TRAMP_DECL(113)  AGB_TRAMP_DECL(114)  AGB_TRAMP_DECL(115)  AGB_TRAMP_DECL(116)  AGB_TRAMP_DECL(117)  AGB_TRAMP_DECL(118)  AGB_TRAMP_DECL(119)
AGB_TRAMP_DECL(120)  AGB_TRAMP_DECL(121)  AGB_TRAMP_DECL(122)  AGB_TRAMP_DECL(123)  AGB_TRAMP_DECL(124)  AGB_TRAMP_DECL(125)  AGB_TRAMP_DECL(126)  AGB_TRAMP_DECL(127)
AGB_TRAMP_DECL(128)  AGB_TRAMP_DECL(129)  AGB_TRAMP_DECL(130)  AGB_TRAMP_DECL(131)  AGB_TRAMP_DECL(132)  AGB_TRAMP_DECL(133)  AGB_TRAMP_DECL(134)  AGB_TRAMP_DECL(135)
AGB_TRAMP_DECL(136)  AGB_TRAMP_DECL(137)  AGB_TRAMP_DECL(138)  AGB_TRAMP_DECL(139)  AGB_TRAMP_DECL(140)  AGB_TRAMP_DECL(141)  AGB_TRAMP_DECL(142)  AGB_TRAMP_DECL(143)
AGB_TRAMP_DECL(144)  AGB_TRAMP_DECL(145)  AGB_TRAMP_DECL(146)  AGB_TRAMP_DECL(147)  AGB_TRAMP_DECL(148)  AGB_TRAMP_DECL(149)  AGB_TRAMP_DECL(150)  AGB_TRAMP_DECL(151)
AGB_TRAMP_DECL(152)  AGB_TRAMP_DECL(153)  AGB_TRAMP_DECL(154)  AGB_TRAMP_DECL(155)  AGB_TRAMP_DECL(156)  AGB_TRAMP_DECL(157)  AGB_TRAMP_DECL(158)  AGB_TRAMP_DECL(159)
AGB_TRAMP_DECL(160)  AGB_TRAMP_DECL(161)  AGB_TRAMP_DECL(162)  AGB_TRAMP_DECL(163)  AGB_TRAMP_DECL(164)  AGB_TRAMP_DECL(165)  AGB_TRAMP_DECL(166)  AGB_TRAMP_DECL(167)
AGB_TRAMP_DECL(168)  AGB_TRAMP_DECL(169)  AGB_TRAMP_DECL(170)  AGB_TRAMP_DECL(171)  AGB_TRAMP_DECL(172)  AGB_TRAMP_DECL(173)  AGB_TRAMP_DECL(174)  AGB_TRAMP_DECL(175)
AGB_TRAMP_DECL(176)  AGB_TRAMP_DECL(177)  AGB_TRAMP_DECL(178)  AGB_TRAMP_DECL(179)  AGB_TRAMP_DECL(180)  AGB_TRAMP_DECL(181)  AGB_TRAMP_DECL(182)  AGB_TRAMP_DECL(183)
AGB_TRAMP_DECL(184)  AGB_TRAMP_DECL(185)  AGB_TRAMP_DECL(186)  AGB_TRAMP_DECL(187)  AGB_TRAMP_DECL(188)  AGB_TRAMP_DECL(189)  AGB_TRAMP_DECL(190)  AGB_TRAMP_DECL(191)
AGB_TRAMP_DECL(192)  AGB_TRAMP_DECL(193)  AGB_TRAMP_DECL(194)  AGB_TRAMP_DECL(195)  AGB_TRAMP_DECL(196)  AGB_TRAMP_DECL(197)  AGB_TRAMP_DECL(198)  AGB_TRAMP_DECL(199)
AGB_TRAMP_DECL(200)  AGB_TRAMP_DECL(201)  AGB_TRAMP_DECL(202)  AGB_TRAMP_DECL(203)  AGB_TRAMP_DECL(204)  AGB_TRAMP_DECL(205)  AGB_TRAMP_DECL(206)  AGB_TRAMP_DECL(207)
AGB_TRAMP_DECL(208)  AGB_TRAMP_DECL(209)  AGB_TRAMP_DECL(210)  AGB_TRAMP_DECL(211)  AGB_TRAMP_DECL(212)  AGB_TRAMP_DECL(213)  AGB_TRAMP_DECL(214)  AGB_TRAMP_DECL(215)
AGB_TRAMP_DECL(216)  AGB_TRAMP_DECL(217)  AGB_TRAMP_DECL(218)  AGB_TRAMP_DECL(219)  AGB_TRAMP_DECL(220)  AGB_TRAMP_DECL(221)  AGB_TRAMP_DECL(222)  AGB_TRAMP_DECL(223)
AGB_TRAMP_DECL(224)  AGB_TRAMP_DECL(225)  AGB_TRAMP_DECL(226)  AGB_TRAMP_DECL(227)  AGB_TRAMP_DECL(228)  AGB_TRAMP_DECL(229)  AGB_TRAMP_DECL(230)  AGB_TRAMP_DECL(231)
AGB_TRAMP_DECL(232)  AGB_TRAMP_DECL(233)  AGB_TRAMP_DECL(234)  AGB_TRAMP_DECL(235)  AGB_TRAMP_DECL(236)  AGB_TRAMP_DECL(237)  AGB_TRAMP_DECL(238)  AGB_TRAMP_DECL(239)
AGB_TRAMP_DECL(240)  AGB_TRAMP_DECL(241)  AGB_TRAMP_DECL(242)  AGB_TRAMP_DECL(243)  AGB_TRAMP_DECL(244)  AGB_TRAMP_DECL(245)  AGB_TRAMP_DECL(246)  AGB_TRAMP_DECL(247)
AGB_TRAMP_DECL(248)  AGB_TRAMP_DECL(249)  AGB_TRAMP_DECL(250)  AGB_TRAMP_DECL(251)  AGB_TRAMP_DECL(252)  AGB_TRAMP_DECL(253)  AGB_TRAMP_DECL(254)  AGB_TRAMP_DECL(255)
AGB_TRAMP_DECL(256)  AGB_TRAMP_DECL(257)  AGB_TRAMP_DECL(258)  AGB_TRAMP_DECL(259)  AGB_TRAMP_DECL(260)  AGB_TRAMP_DECL(261)  AGB_TRAMP_DECL(262)  AGB_TRAMP_DECL(263)
AGB_TRAMP_DECL(264)  AGB_TRAMP_DECL(265)  AGB_TRAMP_DECL(266)  AGB_TRAMP_DECL(267)  AGB_TRAMP_DECL(268)  AGB_TRAMP_DECL(269)  AGB_TRAMP_DECL(270)  AGB_TRAMP_DECL(271)
AGB_TRAMP_DECL(272)  AGB_TRAMP_DECL(273)  AGB_TRAMP_DECL(274)  AGB_TRAMP_DECL(275)  AGB_TRAMP_DECL(276)  AGB_TRAMP_DECL(277)  AGB_TRAMP_DECL(278)  AGB_TRAMP_DECL(279)
AGB_TRAMP_DECL(280)  AGB_TRAMP_DECL(281)  AGB_TRAMP_DECL(282)  AGB_TRAMP_DECL(283)  AGB_TRAMP_DECL(284)  AGB_TRAMP_DECL(285)  AGB_TRAMP_DECL(286)  AGB_TRAMP_DECL(287)
AGB_TRAMP_DECL(288)  AGB_TRAMP_DECL(289)  AGB_TRAMP_DECL(290)  AGB_TRAMP_DECL(291)  AGB_TRAMP_DECL(292)  AGB_TRAMP_DECL(293)  AGB_TRAMP_DECL(294)  AGB_TRAMP_DECL(295)
AGB_TRAMP_DECL(296)  AGB_TRAMP_DECL(297)  AGB_TRAMP_DECL(298)  AGB_TRAMP_DECL(299)  AGB_TRAMP_DECL(300)  AGB_TRAMP_DECL(301)  AGB_TRAMP_DECL(302)  AGB_TRAMP_DECL(303)
AGB_TRAMP_DECL(304)  AGB_TRAMP_DECL(305)  AGB_TRAMP_DECL(306)  AGB_TRAMP_DECL(307)  AGB_TRAMP_DECL(308)  AGB_TRAMP_DECL(309)  AGB_TRAMP_DECL(310)  AGB_TRAMP_DECL(311)
AGB_TRAMP_DECL(312)  AGB_TRAMP_DECL(313)  AGB_TRAMP_DECL(314)  AGB_TRAMP_DECL(315)  AGB_TRAMP_DECL(316)  AGB_TRAMP_DECL(317)  AGB_TRAMP_DECL(318)  AGB_TRAMP_DECL(319)
AGB_TRAMP_DECL(320)  AGB_TRAMP_DECL(321)  AGB_TRAMP_DECL(322)  AGB_TRAMP_DECL(323)  AGB_TRAMP_DECL(324)  AGB_TRAMP_DECL(325)  AGB_TRAMP_DECL(326)  AGB_TRAMP_DECL(327)
AGB_TRAMP_DECL(328)  AGB_TRAMP_DECL(329)  AGB_TRAMP_DECL(330)  AGB_TRAMP_DECL(331)  AGB_TRAMP_DECL(332)  AGB_TRAMP_DECL(333)  AGB_TRAMP_DECL(334)  AGB_TRAMP_DECL(335)
AGB_TRAMP_DECL(336)  AGB_TRAMP_DECL(337)  AGB_TRAMP_DECL(338)  AGB_TRAMP_DECL(339)  AGB_TRAMP_DECL(340)  AGB_TRAMP_DECL(341)  AGB_TRAMP_DECL(342)  AGB_TRAMP_DECL(343)
AGB_TRAMP_DECL(344)  AGB_TRAMP_DECL(345)  AGB_TRAMP_DECL(346)  AGB_TRAMP_DECL(347)  AGB_TRAMP_DECL(348)  AGB_TRAMP_DECL(349)  AGB_TRAMP_DECL(350)  AGB_TRAMP_DECL(351)
AGB_TRAMP_DECL(352)  AGB_TRAMP_DECL(353)  AGB_TRAMP_DECL(354)  AGB_TRAMP_DECL(355)  AGB_TRAMP_DECL(356)  AGB_TRAMP_DECL(357)  AGB_TRAMP_DECL(358)  AGB_TRAMP_DECL(359)
AGB_TRAMP_DECL(360)  AGB_TRAMP_DECL(361)  AGB_TRAMP_DECL(362)  AGB_TRAMP_DECL(363)  AGB_TRAMP_DECL(364)  AGB_TRAMP_DECL(365)  AGB_TRAMP_DECL(366)  AGB_TRAMP_DECL(367)
AGB_TRAMP_DECL(368)  AGB_TRAMP_DECL(369)  AGB_TRAMP_DECL(370)  AGB_TRAMP_DECL(371)  AGB_TRAMP_DECL(372)  AGB_TRAMP_DECL(373)  AGB_TRAMP_DECL(374)  AGB_TRAMP_DECL(375)
AGB_TRAMP_DECL(376)  AGB_TRAMP_DECL(377)  AGB_TRAMP_DECL(378)  AGB_TRAMP_DECL(379)  AGB_TRAMP_DECL(380)  AGB_TRAMP_DECL(381)  AGB_TRAMP_DECL(382)  AGB_TRAMP_DECL(383)
AGB_TRAMP_DECL(384)  AGB_TRAMP_DECL(385)  AGB_TRAMP_DECL(386)  AGB_TRAMP_DECL(387)  AGB_TRAMP_DECL(388)  AGB_TRAMP_DECL(389)  AGB_TRAMP_DECL(390)  AGB_TRAMP_DECL(391)
AGB_TRAMP_DECL(392)  AGB_TRAMP_DECL(393)  AGB_TRAMP_DECL(394)  AGB_TRAMP_DECL(395)  AGB_TRAMP_DECL(396)  AGB_TRAMP_DECL(397)  AGB_TRAMP_DECL(398)  AGB_TRAMP_DECL(399)
AGB_TRAMP_DECL(400)  AGB_TRAMP_DECL(401)  AGB_TRAMP_DECL(402)  AGB_TRAMP_DECL(403)  AGB_TRAMP_DECL(404)  AGB_TRAMP_DECL(405)  AGB_TRAMP_DECL(406)  AGB_TRAMP_DECL(407)
AGB_TRAMP_DECL(408)  AGB_TRAMP_DECL(409)  AGB_TRAMP_DECL(410)  AGB_TRAMP_DECL(411)  AGB_TRAMP_DECL(412)  AGB_TRAMP_DECL(413)  AGB_TRAMP_DECL(414)  AGB_TRAMP_DECL(415)
AGB_TRAMP_DECL(416)  AGB_TRAMP_DECL(417)  AGB_TRAMP_DECL(418)  AGB_TRAMP_DECL(419)  AGB_TRAMP_DECL(420)  AGB_TRAMP_DECL(421)  AGB_TRAMP_DECL(422)  AGB_TRAMP_DECL(423)
AGB_TRAMP_DECL(424)  AGB_TRAMP_DECL(425)  AGB_TRAMP_DECL(426)  AGB_TRAMP_DECL(427)  AGB_TRAMP_DECL(428)  AGB_TRAMP_DECL(429)  AGB_TRAMP_DECL(430)  AGB_TRAMP_DECL(431)
AGB_TRAMP_DECL(432)  AGB_TRAMP_DECL(433)  AGB_TRAMP_DECL(434)  AGB_TRAMP_DECL(435)  AGB_TRAMP_DECL(436)  AGB_TRAMP_DECL(437)  AGB_TRAMP_DECL(438)  AGB_TRAMP_DECL(439)
AGB_TRAMP_DECL(440)  AGB_TRAMP_DECL(441)  AGB_TRAMP_DECL(442)  AGB_TRAMP_DECL(443)  AGB_TRAMP_DECL(444)  AGB_TRAMP_DECL(445)  AGB_TRAMP_DECL(446)  AGB_TRAMP_DECL(447)
AGB_TRAMP_DECL(448)  AGB_TRAMP_DECL(449)  AGB_TRAMP_DECL(450)  AGB_TRAMP_DECL(451)  AGB_TRAMP_DECL(452)  AGB_TRAMP_DECL(453)  AGB_TRAMP_DECL(454)  AGB_TRAMP_DECL(455)
AGB_TRAMP_DECL(456)  AGB_TRAMP_DECL(457)  AGB_TRAMP_DECL(458)  AGB_TRAMP_DECL(459)  AGB_TRAMP_DECL(460)  AGB_TRAMP_DECL(461)  AGB_TRAMP_DECL(462)  AGB_TRAMP_DECL(463)
AGB_TRAMP_DECL(464)  AGB_TRAMP_DECL(465)  AGB_TRAMP_DECL(466)  AGB_TRAMP_DECL(467)  AGB_TRAMP_DECL(468)  AGB_TRAMP_DECL(469)  AGB_TRAMP_DECL(470)  AGB_TRAMP_DECL(471)
AGB_TRAMP_DECL(472)  AGB_TRAMP_DECL(473)  AGB_TRAMP_DECL(474)  AGB_TRAMP_DECL(475)  AGB_TRAMP_DECL(476)  AGB_TRAMP_DECL(477)  AGB_TRAMP_DECL(478)  AGB_TRAMP_DECL(479)
AGB_TRAMP_DECL(480)  AGB_TRAMP_DECL(481)  AGB_TRAMP_DECL(482)  AGB_TRAMP_DECL(483)  AGB_TRAMP_DECL(484)  AGB_TRAMP_DECL(485)  AGB_TRAMP_DECL(486)  AGB_TRAMP_DECL(487)
AGB_TRAMP_DECL(488)  AGB_TRAMP_DECL(489)  AGB_TRAMP_DECL(490)  AGB_TRAMP_DECL(491)  AGB_TRAMP_DECL(492)  AGB_TRAMP_DECL(493)  AGB_TRAMP_DECL(494)  AGB_TRAMP_DECL(495)
AGB_TRAMP_DECL(496)  AGB_TRAMP_DECL(497)  AGB_TRAMP_DECL(498)  AGB_TRAMP_DECL(499)  AGB_TRAMP_DECL(500)  AGB_TRAMP_DECL(501)  AGB_TRAMP_DECL(502)  AGB_TRAMP_DECL(503)
AGB_TRAMP_DECL(504)  AGB_TRAMP_DECL(505)  AGB_TRAMP_DECL(506)  AGB_TRAMP_DECL(507)  AGB_TRAMP_DECL(508)  AGB_TRAMP_DECL(509)  AGB_TRAMP_DECL(510)  AGB_TRAMP_DECL(511)
AGB_TRAMP_DECL(512)  AGB_TRAMP_DECL(513)  AGB_TRAMP_DECL(514)  AGB_TRAMP_DECL(515)  AGB_TRAMP_DECL(516)  AGB_TRAMP_DECL(517)  AGB_TRAMP_DECL(518)  AGB_TRAMP_DECL(519)
AGB_TRAMP_DECL(520)  AGB_TRAMP_DECL(521)  AGB_TRAMP_DECL(522)  AGB_TRAMP_DECL(523)  AGB_TRAMP_DECL(524)  AGB_TRAMP_DECL(525)  AGB_TRAMP_DECL(526)  AGB_TRAMP_DECL(527)
AGB_TRAMP_DECL(528)  AGB_TRAMP_DECL(529)  AGB_TRAMP_DECL(530)  AGB_TRAMP_DECL(531)  AGB_TRAMP_DECL(532)  AGB_TRAMP_DECL(533)  AGB_TRAMP_DECL(534)  AGB_TRAMP_DECL(535)
AGB_TRAMP_DECL(536)  AGB_TRAMP_DECL(537)  AGB_TRAMP_DECL(538)  AGB_TRAMP_DECL(539)  AGB_TRAMP_DECL(540)  AGB_TRAMP_DECL(541)  AGB_TRAMP_DECL(542)  AGB_TRAMP_DECL(543)
AGB_TRAMP_DECL(544)  AGB_TRAMP_DECL(545)  AGB_TRAMP_DECL(546)  AGB_TRAMP_DECL(547)  AGB_TRAMP_DECL(548)  AGB_TRAMP_DECL(549)  AGB_TRAMP_DECL(550)  AGB_TRAMP_DECL(551)
AGB_TRAMP_DECL(552)  AGB_TRAMP_DECL(553)  AGB_TRAMP_DECL(554)  AGB_TRAMP_DECL(555)  AGB_TRAMP_DECL(556)  AGB_TRAMP_DECL(557)  AGB_TRAMP_DECL(558)  AGB_TRAMP_DECL(559)
AGB_TRAMP_DECL(560)  AGB_TRAMP_DECL(561)  AGB_TRAMP_DECL(562)  AGB_TRAMP_DECL(563)  AGB_TRAMP_DECL(564)  AGB_TRAMP_DECL(565)  AGB_TRAMP_DECL(566)  AGB_TRAMP_DECL(567)
AGB_TRAMP_DECL(568)  AGB_TRAMP_DECL(569)  AGB_TRAMP_DECL(570)  AGB_TRAMP_DECL(571)  AGB_TRAMP_DECL(572)  AGB_TRAMP_DECL(573)  AGB_TRAMP_DECL(574)  AGB_TRAMP_DECL(575)
AGB_TRAMP_DECL(576)  AGB_TRAMP_DECL(577)  AGB_TRAMP_DECL(578)  AGB_TRAMP_DECL(579)  AGB_TRAMP_DECL(580)  AGB_TRAMP_DECL(581)  AGB_TRAMP_DECL(582)  AGB_TRAMP_DECL(583)
AGB_TRAMP_DECL(584)  AGB_TRAMP_DECL(585)  AGB_TRAMP_DECL(586)  AGB_TRAMP_DECL(587)  AGB_TRAMP_DECL(588)  AGB_TRAMP_DECL(589)  AGB_TRAMP_DECL(590)  AGB_TRAMP_DECL(591)
AGB_TRAMP_DECL(592)  AGB_TRAMP_DECL(593)  AGB_TRAMP_DECL(594)  AGB_TRAMP_DECL(595)  AGB_TRAMP_DECL(596)  AGB_TRAMP_DECL(597)  AGB_TRAMP_DECL(598)  AGB_TRAMP_DECL(599)
AGB_TRAMP_DECL(600)  AGB_TRAMP_DECL(601)  AGB_TRAMP_DECL(602)  AGB_TRAMP_DECL(603)  AGB_TRAMP_DECL(604)  AGB_TRAMP_DECL(605)  AGB_TRAMP_DECL(606)  AGB_TRAMP_DECL(607)
AGB_TRAMP_DECL(608)  AGB_TRAMP_DECL(609)  AGB_TRAMP_DECL(610)  AGB_TRAMP_DECL(611)  AGB_TRAMP_DECL(612)  AGB_TRAMP_DECL(613)  AGB_TRAMP_DECL(614)  AGB_TRAMP_DECL(615)
AGB_TRAMP_DECL(616)  AGB_TRAMP_DECL(617)  AGB_TRAMP_DECL(618)  AGB_TRAMP_DECL(619)  AGB_TRAMP_DECL(620)  AGB_TRAMP_DECL(621)  AGB_TRAMP_DECL(622)  AGB_TRAMP_DECL(623)
AGB_TRAMP_DECL(624)  AGB_TRAMP_DECL(625)  AGB_TRAMP_DECL(626)  AGB_TRAMP_DECL(627)  AGB_TRAMP_DECL(628)  AGB_TRAMP_DECL(629)  AGB_TRAMP_DECL(630)  AGB_TRAMP_DECL(631)
AGB_TRAMP_DECL(632)  AGB_TRAMP_DECL(633)  AGB_TRAMP_DECL(634)  AGB_TRAMP_DECL(635)  AGB_TRAMP_DECL(636)  AGB_TRAMP_DECL(637)  AGB_TRAMP_DECL(638)  AGB_TRAMP_DECL(639)
AGB_TRAMP_DECL(640)  AGB_TRAMP_DECL(641)  AGB_TRAMP_DECL(642)  AGB_TRAMP_DECL(643)  AGB_TRAMP_DECL(644)  AGB_TRAMP_DECL(645)  AGB_TRAMP_DECL(646)  AGB_TRAMP_DECL(647)
AGB_TRAMP_DECL(648)  AGB_TRAMP_DECL(649)  AGB_TRAMP_DECL(650)  AGB_TRAMP_DECL(651)  AGB_TRAMP_DECL(652)  AGB_TRAMP_DECL(653)  AGB_TRAMP_DECL(654)  AGB_TRAMP_DECL(655)
AGB_TRAMP_DECL(656)  AGB_TRAMP_DECL(657)  AGB_TRAMP_DECL(658)  AGB_TRAMP_DECL(659)  AGB_TRAMP_DECL(660)  AGB_TRAMP_DECL(661)  AGB_TRAMP_DECL(662)  AGB_TRAMP_DECL(663)
AGB_TRAMP_DECL(664)  AGB_TRAMP_DECL(665)  AGB_TRAMP_DECL(666)  AGB_TRAMP_DECL(667)  AGB_TRAMP_DECL(668)  AGB_TRAMP_DECL(669)  AGB_TRAMP_DECL(670)  AGB_TRAMP_DECL(671)
AGB_TRAMP_DECL(672)  AGB_TRAMP_DECL(673)  AGB_TRAMP_DECL(674)  AGB_TRAMP_DECL(675)  AGB_TRAMP_DECL(676)  AGB_TRAMP_DECL(677)  AGB_TRAMP_DECL(678)  AGB_TRAMP_DECL(679)
AGB_TRAMP_DECL(680)  AGB_TRAMP_DECL(681)  AGB_TRAMP_DECL(682)  AGB_TRAMP_DECL(683)  AGB_TRAMP_DECL(684)  AGB_TRAMP_DECL(685)  AGB_TRAMP_DECL(686)  AGB_TRAMP_DECL(687)
AGB_TRAMP_DECL(688)  AGB_TRAMP_DECL(689)  AGB_TRAMP_DECL(690)  AGB_TRAMP_DECL(691)  AGB_TRAMP_DECL(692)  AGB_TRAMP_DECL(693)  AGB_TRAMP_DECL(694)  AGB_TRAMP_DECL(695)
AGB_TRAMP_DECL(696)  AGB_TRAMP_DECL(697)  AGB_TRAMP_DECL(698)  AGB_TRAMP_DECL(699)  AGB_TRAMP_DECL(700)  AGB_TRAMP_DECL(701)  AGB_TRAMP_DECL(702)  AGB_TRAMP_DECL(703)
AGB_TRAMP_DECL(704)  AGB_TRAMP_DECL(705)  AGB_TRAMP_DECL(706)  AGB_TRAMP_DECL(707)  AGB_TRAMP_DECL(708)  AGB_TRAMP_DECL(709)  AGB_TRAMP_DECL(710)  AGB_TRAMP_DECL(711)
AGB_TRAMP_DECL(712)  AGB_TRAMP_DECL(713)  AGB_TRAMP_DECL(714)  AGB_TRAMP_DECL(715)  AGB_TRAMP_DECL(716)  AGB_TRAMP_DECL(717)  AGB_TRAMP_DECL(718)  AGB_TRAMP_DECL(719)
AGB_TRAMP_DECL(720)  AGB_TRAMP_DECL(721)  AGB_TRAMP_DECL(722)  AGB_TRAMP_DECL(723)  AGB_TRAMP_DECL(724)  AGB_TRAMP_DECL(725)  AGB_TRAMP_DECL(726)  AGB_TRAMP_DECL(727)
AGB_TRAMP_DECL(728)  AGB_TRAMP_DECL(729)  AGB_TRAMP_DECL(730)  AGB_TRAMP_DECL(731)  AGB_TRAMP_DECL(732)  AGB_TRAMP_DECL(733)  AGB_TRAMP_DECL(734)  AGB_TRAMP_DECL(735)
AGB_TRAMP_DECL(736)  AGB_TRAMP_DECL(737)  AGB_TRAMP_DECL(738)  AGB_TRAMP_DECL(739)  AGB_TRAMP_DECL(740)  AGB_TRAMP_DECL(741)  AGB_TRAMP_DECL(742)  AGB_TRAMP_DECL(743)
AGB_TRAMP_DECL(744)  AGB_TRAMP_DECL(745)  AGB_TRAMP_DECL(746)  AGB_TRAMP_DECL(747)  AGB_TRAMP_DECL(748)  AGB_TRAMP_DECL(749)  AGB_TRAMP_DECL(750)  AGB_TRAMP_DECL(751)
AGB_TRAMP_DECL(752)  AGB_TRAMP_DECL(753)  AGB_TRAMP_DECL(754)  AGB_TRAMP_DECL(755)  AGB_TRAMP_DECL(756)  AGB_TRAMP_DECL(757)  AGB_TRAMP_DECL(758)  AGB_TRAMP_DECL(759)
AGB_TRAMP_DECL(760)  AGB_TRAMP_DECL(761)  AGB_TRAMP_DECL(762)  AGB_TRAMP_DECL(763)  AGB_TRAMP_DECL(764)  AGB_TRAMP_DECL(765)  AGB_TRAMP_DECL(766)  AGB_TRAMP_DECL(767)
AGB_TRAMP_DECL(768)  AGB_TRAMP_DECL(769)  AGB_TRAMP_DECL(770)  AGB_TRAMP_DECL(771)  AGB_TRAMP_DECL(772)  AGB_TRAMP_DECL(773)  AGB_TRAMP_DECL(774)  AGB_TRAMP_DECL(775)
AGB_TRAMP_DECL(776)  AGB_TRAMP_DECL(777)  AGB_TRAMP_DECL(778)  AGB_TRAMP_DECL(779)  AGB_TRAMP_DECL(780)  AGB_TRAMP_DECL(781)  AGB_TRAMP_DECL(782)  AGB_TRAMP_DECL(783)
AGB_TRAMP_DECL(784)  AGB_TRAMP_DECL(785)  AGB_TRAMP_DECL(786)  AGB_TRAMP_DECL(787)  AGB_TRAMP_DECL(788)  AGB_TRAMP_DECL(789)  AGB_TRAMP_DECL(790)  AGB_TRAMP_DECL(791)
AGB_TRAMP_DECL(792)  AGB_TRAMP_DECL(793)  AGB_TRAMP_DECL(794)  AGB_TRAMP_DECL(795)  AGB_TRAMP_DECL(796)  AGB_TRAMP_DECL(797)  AGB_TRAMP_DECL(798)  AGB_TRAMP_DECL(799)
AGB_TRAMP_DECL(800)  AGB_TRAMP_DECL(801)  AGB_TRAMP_DECL(802)  AGB_TRAMP_DECL(803)  AGB_TRAMP_DECL(804)  AGB_TRAMP_DECL(805)  AGB_TRAMP_DECL(806)  AGB_TRAMP_DECL(807)
AGB_TRAMP_DECL(808)  AGB_TRAMP_DECL(809)  AGB_TRAMP_DECL(810)  AGB_TRAMP_DECL(811)  AGB_TRAMP_DECL(812)  AGB_TRAMP_DECL(813)  AGB_TRAMP_DECL(814)  AGB_TRAMP_DECL(815)
AGB_TRAMP_DECL(816)  AGB_TRAMP_DECL(817)  AGB_TRAMP_DECL(818)  AGB_TRAMP_DECL(819)  AGB_TRAMP_DECL(820)  AGB_TRAMP_DECL(821)  AGB_TRAMP_DECL(822)  AGB_TRAMP_DECL(823)
AGB_TRAMP_DECL(824)  AGB_TRAMP_DECL(825)  AGB_TRAMP_DECL(826)  AGB_TRAMP_DECL(827)  AGB_TRAMP_DECL(828)  AGB_TRAMP_DECL(829)  AGB_TRAMP_DECL(830)  AGB_TRAMP_DECL(831)
AGB_TRAMP_DECL(832)  AGB_TRAMP_DECL(833)  AGB_TRAMP_DECL(834)  AGB_TRAMP_DECL(835)  AGB_TRAMP_DECL(836)  AGB_TRAMP_DECL(837)  AGB_TRAMP_DECL(838)  AGB_TRAMP_DECL(839)
AGB_TRAMP_DECL(840)  AGB_TRAMP_DECL(841)  AGB_TRAMP_DECL(842)  AGB_TRAMP_DECL(843)  AGB_TRAMP_DECL(844)  AGB_TRAMP_DECL(845)  AGB_TRAMP_DECL(846)  AGB_TRAMP_DECL(847)
AGB_TRAMP_DECL(848)  AGB_TRAMP_DECL(849)  AGB_TRAMP_DECL(850)  AGB_TRAMP_DECL(851)  AGB_TRAMP_DECL(852)  AGB_TRAMP_DECL(853)  AGB_TRAMP_DECL(854)  AGB_TRAMP_DECL(855)
AGB_TRAMP_DECL(856)  AGB_TRAMP_DECL(857)  AGB_TRAMP_DECL(858)  AGB_TRAMP_DECL(859)  AGB_TRAMP_DECL(860)  AGB_TRAMP_DECL(861)  AGB_TRAMP_DECL(862)  AGB_TRAMP_DECL(863)
AGB_TRAMP_DECL(864)  AGB_TRAMP_DECL(865)  AGB_TRAMP_DECL(866)  AGB_TRAMP_DECL(867)  AGB_TRAMP_DECL(868)  AGB_TRAMP_DECL(869)  AGB_TRAMP_DECL(870)  AGB_TRAMP_DECL(871)
AGB_TRAMP_DECL(872)  AGB_TRAMP_DECL(873)  AGB_TRAMP_DECL(874)  AGB_TRAMP_DECL(875)  AGB_TRAMP_DECL(876)  AGB_TRAMP_DECL(877)  AGB_TRAMP_DECL(878)  AGB_TRAMP_DECL(879)
AGB_TRAMP_DECL(880)  AGB_TRAMP_DECL(881)  AGB_TRAMP_DECL(882)  AGB_TRAMP_DECL(883)  AGB_TRAMP_DECL(884)  AGB_TRAMP_DECL(885)  AGB_TRAMP_DECL(886)  AGB_TRAMP_DECL(887)
AGB_TRAMP_DECL(888)  AGB_TRAMP_DECL(889)  AGB_TRAMP_DECL(890)  AGB_TRAMP_DECL(891)  AGB_TRAMP_DECL(892)  AGB_TRAMP_DECL(893)  AGB_TRAMP_DECL(894)  AGB_TRAMP_DECL(895)
AGB_TRAMP_DECL(896)  AGB_TRAMP_DECL(897)  AGB_TRAMP_DECL(898)  AGB_TRAMP_DECL(899)  AGB_TRAMP_DECL(900)  AGB_TRAMP_DECL(901)  AGB_TRAMP_DECL(902)  AGB_TRAMP_DECL(903)
AGB_TRAMP_DECL(904)  AGB_TRAMP_DECL(905)  AGB_TRAMP_DECL(906)  AGB_TRAMP_DECL(907)  AGB_TRAMP_DECL(908)  AGB_TRAMP_DECL(909)  AGB_TRAMP_DECL(910)  AGB_TRAMP_DECL(911)
AGB_TRAMP_DECL(912)  AGB_TRAMP_DECL(913)  AGB_TRAMP_DECL(914)  AGB_TRAMP_DECL(915)  AGB_TRAMP_DECL(916)  AGB_TRAMP_DECL(917)  AGB_TRAMP_DECL(918)  AGB_TRAMP_DECL(919)
AGB_TRAMP_DECL(920)  AGB_TRAMP_DECL(921)  AGB_TRAMP_DECL(922)  AGB_TRAMP_DECL(923)  AGB_TRAMP_DECL(924)  AGB_TRAMP_DECL(925)  AGB_TRAMP_DECL(926)  AGB_TRAMP_DECL(927)
AGB_TRAMP_DECL(928)  AGB_TRAMP_DECL(929)  AGB_TRAMP_DECL(930)  AGB_TRAMP_DECL(931)  AGB_TRAMP_DECL(932)  AGB_TRAMP_DECL(933)  AGB_TRAMP_DECL(934)  AGB_TRAMP_DECL(935)
AGB_TRAMP_DECL(936)  AGB_TRAMP_DECL(937)  AGB_TRAMP_DECL(938)  AGB_TRAMP_DECL(939)  AGB_TRAMP_DECL(940)  AGB_TRAMP_DECL(941)  AGB_TRAMP_DECL(942)  AGB_TRAMP_DECL(943)
AGB_TRAMP_DECL(944)  AGB_TRAMP_DECL(945)  AGB_TRAMP_DECL(946)  AGB_TRAMP_DECL(947)  AGB_TRAMP_DECL(948)  AGB_TRAMP_DECL(949)  AGB_TRAMP_DECL(950)  AGB_TRAMP_DECL(951)
AGB_TRAMP_DECL(952)  AGB_TRAMP_DECL(953)  AGB_TRAMP_DECL(954)  AGB_TRAMP_DECL(955)  AGB_TRAMP_DECL(956)  AGB_TRAMP_DECL(957)  AGB_TRAMP_DECL(958)  AGB_TRAMP_DECL(959)
AGB_TRAMP_DECL(960)  AGB_TRAMP_DECL(961)  AGB_TRAMP_DECL(962)  AGB_TRAMP_DECL(963)  AGB_TRAMP_DECL(964)  AGB_TRAMP_DECL(965)  AGB_TRAMP_DECL(966)  AGB_TRAMP_DECL(967)
AGB_TRAMP_DECL(968)  AGB_TRAMP_DECL(969)  AGB_TRAMP_DECL(970)  AGB_TRAMP_DECL(971)  AGB_TRAMP_DECL(972)  AGB_TRAMP_DECL(973)  AGB_TRAMP_DECL(974)  AGB_TRAMP_DECL(975)
AGB_TRAMP_DECL(976)  AGB_TRAMP_DECL(977)  AGB_TRAMP_DECL(978)  AGB_TRAMP_DECL(979)  AGB_TRAMP_DECL(980)  AGB_TRAMP_DECL(981)  AGB_TRAMP_DECL(982)  AGB_TRAMP_DECL(983)
AGB_TRAMP_DECL(984)  AGB_TRAMP_DECL(985)  AGB_TRAMP_DECL(986)  AGB_TRAMP_DECL(987)  AGB_TRAMP_DECL(988)  AGB_TRAMP_DECL(989)  AGB_TRAMP_DECL(990)  AGB_TRAMP_DECL(991)
AGB_TRAMP_DECL(992)  AGB_TRAMP_DECL(993)  AGB_TRAMP_DECL(994)  AGB_TRAMP_DECL(995)  AGB_TRAMP_DECL(996)  AGB_TRAMP_DECL(997)  AGB_TRAMP_DECL(998)  AGB_TRAMP_DECL(999)
AGB_TRAMP_DECL(1000)  AGB_TRAMP_DECL(1001)  AGB_TRAMP_DECL(1002)  AGB_TRAMP_DECL(1003)  AGB_TRAMP_DECL(1004)  AGB_TRAMP_DECL(1005)  AGB_TRAMP_DECL(1006)  AGB_TRAMP_DECL(1007)
AGB_TRAMP_DECL(1008)  AGB_TRAMP_DECL(1009)  AGB_TRAMP_DECL(1010)  AGB_TRAMP_DECL(1011)  AGB_TRAMP_DECL(1012)  AGB_TRAMP_DECL(1013)  AGB_TRAMP_DECL(1014)  AGB_TRAMP_DECL(1015)
AGB_TRAMP_DECL(1016)  AGB_TRAMP_DECL(1017)  AGB_TRAMP_DECL(1018)  AGB_TRAMP_DECL(1019)  AGB_TRAMP_DECL(1020)  AGB_TRAMP_DECL(1021)  AGB_TRAMP_DECL(1022)  AGB_TRAMP_DECL(1023)
AGB_TRAMP_DECL(1024)  AGB_TRAMP_DECL(1025)  AGB_TRAMP_DECL(1026)  AGB_TRAMP_DECL(1027)  AGB_TRAMP_DECL(1028)  AGB_TRAMP_DECL(1029)  AGB_TRAMP_DECL(1030)  AGB_TRAMP_DECL(1031)
AGB_TRAMP_DECL(1032)  AGB_TRAMP_DECL(1033)  AGB_TRAMP_DECL(1034)  AGB_TRAMP_DECL(1035)  AGB_TRAMP_DECL(1036)  AGB_TRAMP_DECL(1037)  AGB_TRAMP_DECL(1038)  AGB_TRAMP_DECL(1039)
AGB_TRAMP_DECL(1040)  AGB_TRAMP_DECL(1041)  AGB_TRAMP_DECL(1042)  AGB_TRAMP_DECL(1043)  AGB_TRAMP_DECL(1044)  AGB_TRAMP_DECL(1045)  AGB_TRAMP_DECL(1046)  AGB_TRAMP_DECL(1047)
AGB_TRAMP_DECL(1048)  AGB_TRAMP_DECL(1049)  AGB_TRAMP_DECL(1050)  AGB_TRAMP_DECL(1051)  AGB_TRAMP_DECL(1052)  AGB_TRAMP_DECL(1053)  AGB_TRAMP_DECL(1054)  AGB_TRAMP_DECL(1055)
AGB_TRAMP_DECL(1056)  AGB_TRAMP_DECL(1057)  AGB_TRAMP_DECL(1058)  AGB_TRAMP_DECL(1059)  AGB_TRAMP_DECL(1060)  AGB_TRAMP_DECL(1061)  AGB_TRAMP_DECL(1062)  AGB_TRAMP_DECL(1063)
AGB_TRAMP_DECL(1064)  AGB_TRAMP_DECL(1065)  AGB_TRAMP_DECL(1066)  AGB_TRAMP_DECL(1067)  AGB_TRAMP_DECL(1068)  AGB_TRAMP_DECL(1069)  AGB_TRAMP_DECL(1070)  AGB_TRAMP_DECL(1071)
AGB_TRAMP_DECL(1072)  AGB_TRAMP_DECL(1073)  AGB_TRAMP_DECL(1074)  AGB_TRAMP_DECL(1075)  AGB_TRAMP_DECL(1076)  AGB_TRAMP_DECL(1077)  AGB_TRAMP_DECL(1078)  AGB_TRAMP_DECL(1079)
AGB_TRAMP_DECL(1080)  AGB_TRAMP_DECL(1081)  AGB_TRAMP_DECL(1082)  AGB_TRAMP_DECL(1083)  AGB_TRAMP_DECL(1084)  AGB_TRAMP_DECL(1085)  AGB_TRAMP_DECL(1086)  AGB_TRAMP_DECL(1087)
AGB_TRAMP_DECL(1088)  AGB_TRAMP_DECL(1089)  AGB_TRAMP_DECL(1090)  AGB_TRAMP_DECL(1091)  AGB_TRAMP_DECL(1092)  AGB_TRAMP_DECL(1093)  AGB_TRAMP_DECL(1094)  AGB_TRAMP_DECL(1095)
AGB_TRAMP_DECL(1096)  AGB_TRAMP_DECL(1097)  AGB_TRAMP_DECL(1098)  AGB_TRAMP_DECL(1099)  AGB_TRAMP_DECL(1100)  AGB_TRAMP_DECL(1101)  AGB_TRAMP_DECL(1102)  AGB_TRAMP_DECL(1103)
AGB_TRAMP_DECL(1104)  AGB_TRAMP_DECL(1105)  AGB_TRAMP_DECL(1106)  AGB_TRAMP_DECL(1107)  AGB_TRAMP_DECL(1108)  AGB_TRAMP_DECL(1109)  AGB_TRAMP_DECL(1110)  AGB_TRAMP_DECL(1111)
AGB_TRAMP_DECL(1112)  AGB_TRAMP_DECL(1113)  AGB_TRAMP_DECL(1114)  AGB_TRAMP_DECL(1115)  AGB_TRAMP_DECL(1116)  AGB_TRAMP_DECL(1117)  AGB_TRAMP_DECL(1118)  AGB_TRAMP_DECL(1119)
AGB_TRAMP_DECL(1120)  AGB_TRAMP_DECL(1121)  AGB_TRAMP_DECL(1122)  AGB_TRAMP_DECL(1123)  AGB_TRAMP_DECL(1124)  AGB_TRAMP_DECL(1125)  AGB_TRAMP_DECL(1126)  AGB_TRAMP_DECL(1127)
AGB_TRAMP_DECL(1128)  AGB_TRAMP_DECL(1129)  AGB_TRAMP_DECL(1130)  AGB_TRAMP_DECL(1131)  AGB_TRAMP_DECL(1132)  AGB_TRAMP_DECL(1133)  AGB_TRAMP_DECL(1134)  AGB_TRAMP_DECL(1135)
AGB_TRAMP_DECL(1136)  AGB_TRAMP_DECL(1137)  AGB_TRAMP_DECL(1138)  AGB_TRAMP_DECL(1139)  AGB_TRAMP_DECL(1140)  AGB_TRAMP_DECL(1141)  AGB_TRAMP_DECL(1142)  AGB_TRAMP_DECL(1143)
AGB_TRAMP_DECL(1144)  AGB_TRAMP_DECL(1145)  AGB_TRAMP_DECL(1146)  AGB_TRAMP_DECL(1147)  AGB_TRAMP_DECL(1148)  AGB_TRAMP_DECL(1149)  AGB_TRAMP_DECL(1150)  AGB_TRAMP_DECL(1151)
AGB_TRAMP_DECL(1152)  AGB_TRAMP_DECL(1153)  AGB_TRAMP_DECL(1154)  AGB_TRAMP_DECL(1155)  AGB_TRAMP_DECL(1156)  AGB_TRAMP_DECL(1157)  AGB_TRAMP_DECL(1158)  AGB_TRAMP_DECL(1159)
AGB_TRAMP_DECL(1160)  AGB_TRAMP_DECL(1161)  AGB_TRAMP_DECL(1162)  AGB_TRAMP_DECL(1163)  AGB_TRAMP_DECL(1164)  AGB_TRAMP_DECL(1165)  AGB_TRAMP_DECL(1166)  AGB_TRAMP_DECL(1167)
AGB_TRAMP_DECL(1168)  AGB_TRAMP_DECL(1169)  AGB_TRAMP_DECL(1170)  AGB_TRAMP_DECL(1171)  AGB_TRAMP_DECL(1172)  AGB_TRAMP_DECL(1173)  AGB_TRAMP_DECL(1174)  AGB_TRAMP_DECL(1175)
AGB_TRAMP_DECL(1176)  AGB_TRAMP_DECL(1177)  AGB_TRAMP_DECL(1178)  AGB_TRAMP_DECL(1179)  AGB_TRAMP_DECL(1180)  AGB_TRAMP_DECL(1181)  AGB_TRAMP_DECL(1182)  AGB_TRAMP_DECL(1183)
AGB_TRAMP_DECL(1184)  AGB_TRAMP_DECL(1185)  AGB_TRAMP_DECL(1186)  AGB_TRAMP_DECL(1187)  AGB_TRAMP_DECL(1188)  AGB_TRAMP_DECL(1189)  AGB_TRAMP_DECL(1190)  AGB_TRAMP_DECL(1191)
AGB_TRAMP_DECL(1192)  AGB_TRAMP_DECL(1193)  AGB_TRAMP_DECL(1194)  AGB_TRAMP_DECL(1195)  AGB_TRAMP_DECL(1196)  AGB_TRAMP_DECL(1197)  AGB_TRAMP_DECL(1198)  AGB_TRAMP_DECL(1199)
AGB_TRAMP_DECL(1200)  AGB_TRAMP_DECL(1201)  AGB_TRAMP_DECL(1202)  AGB_TRAMP_DECL(1203)  AGB_TRAMP_DECL(1204)  AGB_TRAMP_DECL(1205)  AGB_TRAMP_DECL(1206)  AGB_TRAMP_DECL(1207)
AGB_TRAMP_DECL(1208)  AGB_TRAMP_DECL(1209)  AGB_TRAMP_DECL(1210)  AGB_TRAMP_DECL(1211)  AGB_TRAMP_DECL(1212)  AGB_TRAMP_DECL(1213)  AGB_TRAMP_DECL(1214)  AGB_TRAMP_DECL(1215)
AGB_TRAMP_DECL(1216)  AGB_TRAMP_DECL(1217)  AGB_TRAMP_DECL(1218)  AGB_TRAMP_DECL(1219)  AGB_TRAMP_DECL(1220)  AGB_TRAMP_DECL(1221)  AGB_TRAMP_DECL(1222)  AGB_TRAMP_DECL(1223)
AGB_TRAMP_DECL(1224)  AGB_TRAMP_DECL(1225)  AGB_TRAMP_DECL(1226)  AGB_TRAMP_DECL(1227)  AGB_TRAMP_DECL(1228)  AGB_TRAMP_DECL(1229)  AGB_TRAMP_DECL(1230)  AGB_TRAMP_DECL(1231)
AGB_TRAMP_DECL(1232)  AGB_TRAMP_DECL(1233)  AGB_TRAMP_DECL(1234)  AGB_TRAMP_DECL(1235)  AGB_TRAMP_DECL(1236)  AGB_TRAMP_DECL(1237)  AGB_TRAMP_DECL(1238)  AGB_TRAMP_DECL(1239)
AGB_TRAMP_DECL(1240)  AGB_TRAMP_DECL(1241)  AGB_TRAMP_DECL(1242)  AGB_TRAMP_DECL(1243)  AGB_TRAMP_DECL(1244)  AGB_TRAMP_DECL(1245)  AGB_TRAMP_DECL(1246)  AGB_TRAMP_DECL(1247)
AGB_TRAMP_DECL(1248)  AGB_TRAMP_DECL(1249)  AGB_TRAMP_DECL(1250)  AGB_TRAMP_DECL(1251)  AGB_TRAMP_DECL(1252)  AGB_TRAMP_DECL(1253)  AGB_TRAMP_DECL(1254)  AGB_TRAMP_DECL(1255)
AGB_TRAMP_DECL(1256)  AGB_TRAMP_DECL(1257)  AGB_TRAMP_DECL(1258)  AGB_TRAMP_DECL(1259)  AGB_TRAMP_DECL(1260)  AGB_TRAMP_DECL(1261)  AGB_TRAMP_DECL(1262)  AGB_TRAMP_DECL(1263)
AGB_TRAMP_DECL(1264)  AGB_TRAMP_DECL(1265)  AGB_TRAMP_DECL(1266)  AGB_TRAMP_DECL(1267)  AGB_TRAMP_DECL(1268)  AGB_TRAMP_DECL(1269)  AGB_TRAMP_DECL(1270)  AGB_TRAMP_DECL(1271)
AGB_TRAMP_DECL(1272)  AGB_TRAMP_DECL(1273)  AGB_TRAMP_DECL(1274)  AGB_TRAMP_DECL(1275)  AGB_TRAMP_DECL(1276)  AGB_TRAMP_DECL(1277)  AGB_TRAMP_DECL(1278)  AGB_TRAMP_DECL(1279)
AGB_TRAMP_DECL(1280)  AGB_TRAMP_DECL(1281)  AGB_TRAMP_DECL(1282)  AGB_TRAMP_DECL(1283)  AGB_TRAMP_DECL(1284)  AGB_TRAMP_DECL(1285)  AGB_TRAMP_DECL(1286)  AGB_TRAMP_DECL(1287)
AGB_TRAMP_DECL(1288)  AGB_TRAMP_DECL(1289)  AGB_TRAMP_DECL(1290)  AGB_TRAMP_DECL(1291)  AGB_TRAMP_DECL(1292)  AGB_TRAMP_DECL(1293)  AGB_TRAMP_DECL(1294)  AGB_TRAMP_DECL(1295)
AGB_TRAMP_DECL(1296)  AGB_TRAMP_DECL(1297)  AGB_TRAMP_DECL(1298)  AGB_TRAMP_DECL(1299)  AGB_TRAMP_DECL(1300)  AGB_TRAMP_DECL(1301)  AGB_TRAMP_DECL(1302)  AGB_TRAMP_DECL(1303)
AGB_TRAMP_DECL(1304)  AGB_TRAMP_DECL(1305)  AGB_TRAMP_DECL(1306)  AGB_TRAMP_DECL(1307)  AGB_TRAMP_DECL(1308)  AGB_TRAMP_DECL(1309)  AGB_TRAMP_DECL(1310)  AGB_TRAMP_DECL(1311)
AGB_TRAMP_DECL(1312)  AGB_TRAMP_DECL(1313)  AGB_TRAMP_DECL(1314)  AGB_TRAMP_DECL(1315)  AGB_TRAMP_DECL(1316)  AGB_TRAMP_DECL(1317)  AGB_TRAMP_DECL(1318)  AGB_TRAMP_DECL(1319)
AGB_TRAMP_DECL(1320)  AGB_TRAMP_DECL(1321)  AGB_TRAMP_DECL(1322)  AGB_TRAMP_DECL(1323)  AGB_TRAMP_DECL(1324)  AGB_TRAMP_DECL(1325)  AGB_TRAMP_DECL(1326)  AGB_TRAMP_DECL(1327)
AGB_TRAMP_DECL(1328)  AGB_TRAMP_DECL(1329)  AGB_TRAMP_DECL(1330)  AGB_TRAMP_DECL(1331)  AGB_TRAMP_DECL(1332)  AGB_TRAMP_DECL(1333)  AGB_TRAMP_DECL(1334)  AGB_TRAMP_DECL(1335)
AGB_TRAMP_DECL(1336)  AGB_TRAMP_DECL(1337)  AGB_TRAMP_DECL(1338)  AGB_TRAMP_DECL(1339)  AGB_TRAMP_DECL(1340)  AGB_TRAMP_DECL(1341)  AGB_TRAMP_DECL(1342)  AGB_TRAMP_DECL(1343)
AGB_TRAMP_DECL(1344)  AGB_TRAMP_DECL(1345)  AGB_TRAMP_DECL(1346)  AGB_TRAMP_DECL(1347)  AGB_TRAMP_DECL(1348)  AGB_TRAMP_DECL(1349)  AGB_TRAMP_DECL(1350)  AGB_TRAMP_DECL(1351)
AGB_TRAMP_DECL(1352)  AGB_TRAMP_DECL(1353)  AGB_TRAMP_DECL(1354)  AGB_TRAMP_DECL(1355)  AGB_TRAMP_DECL(1356)  AGB_TRAMP_DECL(1357)  AGB_TRAMP_DECL(1358)  AGB_TRAMP_DECL(1359)
AGB_TRAMP_DECL(1360)  AGB_TRAMP_DECL(1361)  AGB_TRAMP_DECL(1362)  AGB_TRAMP_DECL(1363)  AGB_TRAMP_DECL(1364)  AGB_TRAMP_DECL(1365)  AGB_TRAMP_DECL(1366)  AGB_TRAMP_DECL(1367)
AGB_TRAMP_DECL(1368)  AGB_TRAMP_DECL(1369)  AGB_TRAMP_DECL(1370)  AGB_TRAMP_DECL(1371)  AGB_TRAMP_DECL(1372)  AGB_TRAMP_DECL(1373)  AGB_TRAMP_DECL(1374)  AGB_TRAMP_DECL(1375)
AGB_TRAMP_DECL(1376)  AGB_TRAMP_DECL(1377)  AGB_TRAMP_DECL(1378)  AGB_TRAMP_DECL(1379)  AGB_TRAMP_DECL(1380)  AGB_TRAMP_DECL(1381)  AGB_TRAMP_DECL(1382)  AGB_TRAMP_DECL(1383)
AGB_TRAMP_DECL(1384)  AGB_TRAMP_DECL(1385)  AGB_TRAMP_DECL(1386)  AGB_TRAMP_DECL(1387)  AGB_TRAMP_DECL(1388)  AGB_TRAMP_DECL(1389)  AGB_TRAMP_DECL(1390)  AGB_TRAMP_DECL(1391)
AGB_TRAMP_DECL(1392)  AGB_TRAMP_DECL(1393)  AGB_TRAMP_DECL(1394)  AGB_TRAMP_DECL(1395)  AGB_TRAMP_DECL(1396)  AGB_TRAMP_DECL(1397)  AGB_TRAMP_DECL(1398)  AGB_TRAMP_DECL(1399)
AGB_TRAMP_DECL(1400)  AGB_TRAMP_DECL(1401)  AGB_TRAMP_DECL(1402)  AGB_TRAMP_DECL(1403)  AGB_TRAMP_DECL(1404)  AGB_TRAMP_DECL(1405)  AGB_TRAMP_DECL(1406)  AGB_TRAMP_DECL(1407)
AGB_TRAMP_DECL(1408)  AGB_TRAMP_DECL(1409)  AGB_TRAMP_DECL(1410)  AGB_TRAMP_DECL(1411)  AGB_TRAMP_DECL(1412)  AGB_TRAMP_DECL(1413)  AGB_TRAMP_DECL(1414)  AGB_TRAMP_DECL(1415)
AGB_TRAMP_DECL(1416)  AGB_TRAMP_DECL(1417)  AGB_TRAMP_DECL(1418)  AGB_TRAMP_DECL(1419)  AGB_TRAMP_DECL(1420)  AGB_TRAMP_DECL(1421)  AGB_TRAMP_DECL(1422)  AGB_TRAMP_DECL(1423)
AGB_TRAMP_DECL(1424)  AGB_TRAMP_DECL(1425)  AGB_TRAMP_DECL(1426)  AGB_TRAMP_DECL(1427)  AGB_TRAMP_DECL(1428)  AGB_TRAMP_DECL(1429)  AGB_TRAMP_DECL(1430)  AGB_TRAMP_DECL(1431)
AGB_TRAMP_DECL(1432)  AGB_TRAMP_DECL(1433)  AGB_TRAMP_DECL(1434)  AGB_TRAMP_DECL(1435)  AGB_TRAMP_DECL(1436)  AGB_TRAMP_DECL(1437)  AGB_TRAMP_DECL(1438)  AGB_TRAMP_DECL(1439)
AGB_TRAMP_DECL(1440)  AGB_TRAMP_DECL(1441)  AGB_TRAMP_DECL(1442)  AGB_TRAMP_DECL(1443)  AGB_TRAMP_DECL(1444)  AGB_TRAMP_DECL(1445)  AGB_TRAMP_DECL(1446)  AGB_TRAMP_DECL(1447)
AGB_TRAMP_DECL(1448)  AGB_TRAMP_DECL(1449)  AGB_TRAMP_DECL(1450)  AGB_TRAMP_DECL(1451)  AGB_TRAMP_DECL(1452)  AGB_TRAMP_DECL(1453)  AGB_TRAMP_DECL(1454)  AGB_TRAMP_DECL(1455)
AGB_TRAMP_DECL(1456)  AGB_TRAMP_DECL(1457)  AGB_TRAMP_DECL(1458)  AGB_TRAMP_DECL(1459)  AGB_TRAMP_DECL(1460)  AGB_TRAMP_DECL(1461)  AGB_TRAMP_DECL(1462)  AGB_TRAMP_DECL(1463)
AGB_TRAMP_DECL(1464)  AGB_TRAMP_DECL(1465)  AGB_TRAMP_DECL(1466)  AGB_TRAMP_DECL(1467)  AGB_TRAMP_DECL(1468)  AGB_TRAMP_DECL(1469)  AGB_TRAMP_DECL(1470)  AGB_TRAMP_DECL(1471)
AGB_TRAMP_DECL(1472)  AGB_TRAMP_DECL(1473)  AGB_TRAMP_DECL(1474)  AGB_TRAMP_DECL(1475)  AGB_TRAMP_DECL(1476)  AGB_TRAMP_DECL(1477)  AGB_TRAMP_DECL(1478)  AGB_TRAMP_DECL(1479)
AGB_TRAMP_DECL(1480)  AGB_TRAMP_DECL(1481)  AGB_TRAMP_DECL(1482)  AGB_TRAMP_DECL(1483)  AGB_TRAMP_DECL(1484)  AGB_TRAMP_DECL(1485)  AGB_TRAMP_DECL(1486)  AGB_TRAMP_DECL(1487)
AGB_TRAMP_DECL(1488)  AGB_TRAMP_DECL(1489)  AGB_TRAMP_DECL(1490)  AGB_TRAMP_DECL(1491)  AGB_TRAMP_DECL(1492)  AGB_TRAMP_DECL(1493)  AGB_TRAMP_DECL(1494)  AGB_TRAMP_DECL(1495)
AGB_TRAMP_DECL(1496)  AGB_TRAMP_DECL(1497)  AGB_TRAMP_DECL(1498)  AGB_TRAMP_DECL(1499)  AGB_TRAMP_DECL(1500)  AGB_TRAMP_DECL(1501)  AGB_TRAMP_DECL(1502)  AGB_TRAMP_DECL(1503)
AGB_TRAMP_DECL(1504)  AGB_TRAMP_DECL(1505)  AGB_TRAMP_DECL(1506)  AGB_TRAMP_DECL(1507)  AGB_TRAMP_DECL(1508)  AGB_TRAMP_DECL(1509)  AGB_TRAMP_DECL(1510)  AGB_TRAMP_DECL(1511)
AGB_TRAMP_DECL(1512)  AGB_TRAMP_DECL(1513)  AGB_TRAMP_DECL(1514)  AGB_TRAMP_DECL(1515)  AGB_TRAMP_DECL(1516)  AGB_TRAMP_DECL(1517)  AGB_TRAMP_DECL(1518)  AGB_TRAMP_DECL(1519)
AGB_TRAMP_DECL(1520)  AGB_TRAMP_DECL(1521)  AGB_TRAMP_DECL(1522)  AGB_TRAMP_DECL(1523)  AGB_TRAMP_DECL(1524)  AGB_TRAMP_DECL(1525)  AGB_TRAMP_DECL(1526)  AGB_TRAMP_DECL(1527)
AGB_TRAMP_DECL(1528)  AGB_TRAMP_DECL(1529)  AGB_TRAMP_DECL(1530)  AGB_TRAMP_DECL(1531)  AGB_TRAMP_DECL(1532)  AGB_TRAMP_DECL(1533)  AGB_TRAMP_DECL(1534)  AGB_TRAMP_DECL(1535)
AGB_TRAMP_DECL(1536)  AGB_TRAMP_DECL(1537)  AGB_TRAMP_DECL(1538)  AGB_TRAMP_DECL(1539)  AGB_TRAMP_DECL(1540)  AGB_TRAMP_DECL(1541)  AGB_TRAMP_DECL(1542)  AGB_TRAMP_DECL(1543)
AGB_TRAMP_DECL(1544)  AGB_TRAMP_DECL(1545)  AGB_TRAMP_DECL(1546)  AGB_TRAMP_DECL(1547)  AGB_TRAMP_DECL(1548)  AGB_TRAMP_DECL(1549)  AGB_TRAMP_DECL(1550)  AGB_TRAMP_DECL(1551)
AGB_TRAMP_DECL(1552)  AGB_TRAMP_DECL(1553)  AGB_TRAMP_DECL(1554)  AGB_TRAMP_DECL(1555)  AGB_TRAMP_DECL(1556)  AGB_TRAMP_DECL(1557)  AGB_TRAMP_DECL(1558)  AGB_TRAMP_DECL(1559)
AGB_TRAMP_DECL(1560)  AGB_TRAMP_DECL(1561)  AGB_TRAMP_DECL(1562)  AGB_TRAMP_DECL(1563)  AGB_TRAMP_DECL(1564)  AGB_TRAMP_DECL(1565)  AGB_TRAMP_DECL(1566)  AGB_TRAMP_DECL(1567)
AGB_TRAMP_DECL(1568)  AGB_TRAMP_DECL(1569)  AGB_TRAMP_DECL(1570)  AGB_TRAMP_DECL(1571)  AGB_TRAMP_DECL(1572)  AGB_TRAMP_DECL(1573)  AGB_TRAMP_DECL(1574)  AGB_TRAMP_DECL(1575)
AGB_TRAMP_DECL(1576)  AGB_TRAMP_DECL(1577)  AGB_TRAMP_DECL(1578)  AGB_TRAMP_DECL(1579)  AGB_TRAMP_DECL(1580)  AGB_TRAMP_DECL(1581)  AGB_TRAMP_DECL(1582)  AGB_TRAMP_DECL(1583)
AGB_TRAMP_DECL(1584)  AGB_TRAMP_DECL(1585)  AGB_TRAMP_DECL(1586)  AGB_TRAMP_DECL(1587)  AGB_TRAMP_DECL(1588)  AGB_TRAMP_DECL(1589)  AGB_TRAMP_DECL(1590)  AGB_TRAMP_DECL(1591)
AGB_TRAMP_DECL(1592)  AGB_TRAMP_DECL(1593)  AGB_TRAMP_DECL(1594)  AGB_TRAMP_DECL(1595)  AGB_TRAMP_DECL(1596)  AGB_TRAMP_DECL(1597)  AGB_TRAMP_DECL(1598)  AGB_TRAMP_DECL(1599)
AGB_TRAMP_DECL(1600)  AGB_TRAMP_DECL(1601)  AGB_TRAMP_DECL(1602)  AGB_TRAMP_DECL(1603)  AGB_TRAMP_DECL(1604)  AGB_TRAMP_DECL(1605)  AGB_TRAMP_DECL(1606)  AGB_TRAMP_DECL(1607)
AGB_TRAMP_DECL(1608)  AGB_TRAMP_DECL(1609)  AGB_TRAMP_DECL(1610)  AGB_TRAMP_DECL(1611)  AGB_TRAMP_DECL(1612)  AGB_TRAMP_DECL(1613)  AGB_TRAMP_DECL(1614)  AGB_TRAMP_DECL(1615)
AGB_TRAMP_DECL(1616)  AGB_TRAMP_DECL(1617)  AGB_TRAMP_DECL(1618)  AGB_TRAMP_DECL(1619)  AGB_TRAMP_DECL(1620)  AGB_TRAMP_DECL(1621)  AGB_TRAMP_DECL(1622)  AGB_TRAMP_DECL(1623)
AGB_TRAMP_DECL(1624)  AGB_TRAMP_DECL(1625)  AGB_TRAMP_DECL(1626)  AGB_TRAMP_DECL(1627)  AGB_TRAMP_DECL(1628)  AGB_TRAMP_DECL(1629)  AGB_TRAMP_DECL(1630)  AGB_TRAMP_DECL(1631)
AGB_TRAMP_DECL(1632)  AGB_TRAMP_DECL(1633)  AGB_TRAMP_DECL(1634)  AGB_TRAMP_DECL(1635)  AGB_TRAMP_DECL(1636)  AGB_TRAMP_DECL(1637)  AGB_TRAMP_DECL(1638)  AGB_TRAMP_DECL(1639)
AGB_TRAMP_DECL(1640)  AGB_TRAMP_DECL(1641)  AGB_TRAMP_DECL(1642)  AGB_TRAMP_DECL(1643)  AGB_TRAMP_DECL(1644)  AGB_TRAMP_DECL(1645)  AGB_TRAMP_DECL(1646)  AGB_TRAMP_DECL(1647)
AGB_TRAMP_DECL(1648)  AGB_TRAMP_DECL(1649)  AGB_TRAMP_DECL(1650)  AGB_TRAMP_DECL(1651)  AGB_TRAMP_DECL(1652)  AGB_TRAMP_DECL(1653)  AGB_TRAMP_DECL(1654)  AGB_TRAMP_DECL(1655)
AGB_TRAMP_DECL(1656)  AGB_TRAMP_DECL(1657)  AGB_TRAMP_DECL(1658)  AGB_TRAMP_DECL(1659)  AGB_TRAMP_DECL(1660)  AGB_TRAMP_DECL(1661)  AGB_TRAMP_DECL(1662)  AGB_TRAMP_DECL(1663)
AGB_TRAMP_DECL(1664)  AGB_TRAMP_DECL(1665)  AGB_TRAMP_DECL(1666)  AGB_TRAMP_DECL(1667)  AGB_TRAMP_DECL(1668)  AGB_TRAMP_DECL(1669)  AGB_TRAMP_DECL(1670)  AGB_TRAMP_DECL(1671)
AGB_TRAMP_DECL(1672)  AGB_TRAMP_DECL(1673)  AGB_TRAMP_DECL(1674)  AGB_TRAMP_DECL(1675)  AGB_TRAMP_DECL(1676)  AGB_TRAMP_DECL(1677)  AGB_TRAMP_DECL(1678)  AGB_TRAMP_DECL(1679)
AGB_TRAMP_DECL(1680)  AGB_TRAMP_DECL(1681)  AGB_TRAMP_DECL(1682)  AGB_TRAMP_DECL(1683)  AGB_TRAMP_DECL(1684)  AGB_TRAMP_DECL(1685)  AGB_TRAMP_DECL(1686)  AGB_TRAMP_DECL(1687)
AGB_TRAMP_DECL(1688)  AGB_TRAMP_DECL(1689)  AGB_TRAMP_DECL(1690)  AGB_TRAMP_DECL(1691)  AGB_TRAMP_DECL(1692)  AGB_TRAMP_DECL(1693)  AGB_TRAMP_DECL(1694)  AGB_TRAMP_DECL(1695)
AGB_TRAMP_DECL(1696)  AGB_TRAMP_DECL(1697)  AGB_TRAMP_DECL(1698)  AGB_TRAMP_DECL(1699)  AGB_TRAMP_DECL(1700)  AGB_TRAMP_DECL(1701)  AGB_TRAMP_DECL(1702)  AGB_TRAMP_DECL(1703)
AGB_TRAMP_DECL(1704)  AGB_TRAMP_DECL(1705)  AGB_TRAMP_DECL(1706)  AGB_TRAMP_DECL(1707)  AGB_TRAMP_DECL(1708)  AGB_TRAMP_DECL(1709)  AGB_TRAMP_DECL(1710)  AGB_TRAMP_DECL(1711)
AGB_TRAMP_DECL(1712)  AGB_TRAMP_DECL(1713)  AGB_TRAMP_DECL(1714)  AGB_TRAMP_DECL(1715)  AGB_TRAMP_DECL(1716)  AGB_TRAMP_DECL(1717)  AGB_TRAMP_DECL(1718)  AGB_TRAMP_DECL(1719)
AGB_TRAMP_DECL(1720)  AGB_TRAMP_DECL(1721)  AGB_TRAMP_DECL(1722)  AGB_TRAMP_DECL(1723)  AGB_TRAMP_DECL(1724)  AGB_TRAMP_DECL(1725)  AGB_TRAMP_DECL(1726)  AGB_TRAMP_DECL(1727)
AGB_TRAMP_DECL(1728)  AGB_TRAMP_DECL(1729)  AGB_TRAMP_DECL(1730)  AGB_TRAMP_DECL(1731)  AGB_TRAMP_DECL(1732)  AGB_TRAMP_DECL(1733)  AGB_TRAMP_DECL(1734)  AGB_TRAMP_DECL(1735)
AGB_TRAMP_DECL(1736)  AGB_TRAMP_DECL(1737)  AGB_TRAMP_DECL(1738)  AGB_TRAMP_DECL(1739)  AGB_TRAMP_DECL(1740)  AGB_TRAMP_DECL(1741)  AGB_TRAMP_DECL(1742)  AGB_TRAMP_DECL(1743)
AGB_TRAMP_DECL(1744)  AGB_TRAMP_DECL(1745)  AGB_TRAMP_DECL(1746)  AGB_TRAMP_DECL(1747)  AGB_TRAMP_DECL(1748)  AGB_TRAMP_DECL(1749)  AGB_TRAMP_DECL(1750)  AGB_TRAMP_DECL(1751)
AGB_TRAMP_DECL(1752)  AGB_TRAMP_DECL(1753)  AGB_TRAMP_DECL(1754)  AGB_TRAMP_DECL(1755)  AGB_TRAMP_DECL(1756)  AGB_TRAMP_DECL(1757)  AGB_TRAMP_DECL(1758)  AGB_TRAMP_DECL(1759)
AGB_TRAMP_DECL(1760)  AGB_TRAMP_DECL(1761)  AGB_TRAMP_DECL(1762)  AGB_TRAMP_DECL(1763)  AGB_TRAMP_DECL(1764)  AGB_TRAMP_DECL(1765)  AGB_TRAMP_DECL(1766)  AGB_TRAMP_DECL(1767)
AGB_TRAMP_DECL(1768)  AGB_TRAMP_DECL(1769)  AGB_TRAMP_DECL(1770)  AGB_TRAMP_DECL(1771)  AGB_TRAMP_DECL(1772)  AGB_TRAMP_DECL(1773)  AGB_TRAMP_DECL(1774)  AGB_TRAMP_DECL(1775)
AGB_TRAMP_DECL(1776)  AGB_TRAMP_DECL(1777)  AGB_TRAMP_DECL(1778)  AGB_TRAMP_DECL(1779)  AGB_TRAMP_DECL(1780)  AGB_TRAMP_DECL(1781)  AGB_TRAMP_DECL(1782)  AGB_TRAMP_DECL(1783)
AGB_TRAMP_DECL(1784)  AGB_TRAMP_DECL(1785)  AGB_TRAMP_DECL(1786)  AGB_TRAMP_DECL(1787)  AGB_TRAMP_DECL(1788)  AGB_TRAMP_DECL(1789)  AGB_TRAMP_DECL(1790)  AGB_TRAMP_DECL(1791)
AGB_TRAMP_DECL(1792)  AGB_TRAMP_DECL(1793)  AGB_TRAMP_DECL(1794)  AGB_TRAMP_DECL(1795)  AGB_TRAMP_DECL(1796)  AGB_TRAMP_DECL(1797)  AGB_TRAMP_DECL(1798)  AGB_TRAMP_DECL(1799)
AGB_TRAMP_DECL(1800)  AGB_TRAMP_DECL(1801)  AGB_TRAMP_DECL(1802)  AGB_TRAMP_DECL(1803)  AGB_TRAMP_DECL(1804)  AGB_TRAMP_DECL(1805)  AGB_TRAMP_DECL(1806)  AGB_TRAMP_DECL(1807)
AGB_TRAMP_DECL(1808)  AGB_TRAMP_DECL(1809)  AGB_TRAMP_DECL(1810)  AGB_TRAMP_DECL(1811)  AGB_TRAMP_DECL(1812)  AGB_TRAMP_DECL(1813)  AGB_TRAMP_DECL(1814)  AGB_TRAMP_DECL(1815)
AGB_TRAMP_DECL(1816)  AGB_TRAMP_DECL(1817)  AGB_TRAMP_DECL(1818)  AGB_TRAMP_DECL(1819)  AGB_TRAMP_DECL(1820)  AGB_TRAMP_DECL(1821)  AGB_TRAMP_DECL(1822)  AGB_TRAMP_DECL(1823)
AGB_TRAMP_DECL(1824)  AGB_TRAMP_DECL(1825)  AGB_TRAMP_DECL(1826)  AGB_TRAMP_DECL(1827)  AGB_TRAMP_DECL(1828)  AGB_TRAMP_DECL(1829)  AGB_TRAMP_DECL(1830)  AGB_TRAMP_DECL(1831)
AGB_TRAMP_DECL(1832)  AGB_TRAMP_DECL(1833)  AGB_TRAMP_DECL(1834)  AGB_TRAMP_DECL(1835)  AGB_TRAMP_DECL(1836)  AGB_TRAMP_DECL(1837)  AGB_TRAMP_DECL(1838)  AGB_TRAMP_DECL(1839)
AGB_TRAMP_DECL(1840)  AGB_TRAMP_DECL(1841)  AGB_TRAMP_DECL(1842)  AGB_TRAMP_DECL(1843)  AGB_TRAMP_DECL(1844)  AGB_TRAMP_DECL(1845)  AGB_TRAMP_DECL(1846)  AGB_TRAMP_DECL(1847)
AGB_TRAMP_DECL(1848)  AGB_TRAMP_DECL(1849)  AGB_TRAMP_DECL(1850)  AGB_TRAMP_DECL(1851)  AGB_TRAMP_DECL(1852)  AGB_TRAMP_DECL(1853)  AGB_TRAMP_DECL(1854)  AGB_TRAMP_DECL(1855)
AGB_TRAMP_DECL(1856)  AGB_TRAMP_DECL(1857)  AGB_TRAMP_DECL(1858)  AGB_TRAMP_DECL(1859)  AGB_TRAMP_DECL(1860)  AGB_TRAMP_DECL(1861)  AGB_TRAMP_DECL(1862)  AGB_TRAMP_DECL(1863)
AGB_TRAMP_DECL(1864)  AGB_TRAMP_DECL(1865)  AGB_TRAMP_DECL(1866)  AGB_TRAMP_DECL(1867)  AGB_TRAMP_DECL(1868)  AGB_TRAMP_DECL(1869)  AGB_TRAMP_DECL(1870)  AGB_TRAMP_DECL(1871)
AGB_TRAMP_DECL(1872)  AGB_TRAMP_DECL(1873)  AGB_TRAMP_DECL(1874)  AGB_TRAMP_DECL(1875)  AGB_TRAMP_DECL(1876)  AGB_TRAMP_DECL(1877)  AGB_TRAMP_DECL(1878)  AGB_TRAMP_DECL(1879)
AGB_TRAMP_DECL(1880)  AGB_TRAMP_DECL(1881)  AGB_TRAMP_DECL(1882)  AGB_TRAMP_DECL(1883)  AGB_TRAMP_DECL(1884)  AGB_TRAMP_DECL(1885)  AGB_TRAMP_DECL(1886)  AGB_TRAMP_DECL(1887)
AGB_TRAMP_DECL(1888)  AGB_TRAMP_DECL(1889)  AGB_TRAMP_DECL(1890)  AGB_TRAMP_DECL(1891)  AGB_TRAMP_DECL(1892)  AGB_TRAMP_DECL(1893)  AGB_TRAMP_DECL(1894)  AGB_TRAMP_DECL(1895)
AGB_TRAMP_DECL(1896)  AGB_TRAMP_DECL(1897)  AGB_TRAMP_DECL(1898)  AGB_TRAMP_DECL(1899)  AGB_TRAMP_DECL(1900)  AGB_TRAMP_DECL(1901)  AGB_TRAMP_DECL(1902)  AGB_TRAMP_DECL(1903)
AGB_TRAMP_DECL(1904)  AGB_TRAMP_DECL(1905)  AGB_TRAMP_DECL(1906)  AGB_TRAMP_DECL(1907)  AGB_TRAMP_DECL(1908)  AGB_TRAMP_DECL(1909)  AGB_TRAMP_DECL(1910)  AGB_TRAMP_DECL(1911)
AGB_TRAMP_DECL(1912)  AGB_TRAMP_DECL(1913)  AGB_TRAMP_DECL(1914)  AGB_TRAMP_DECL(1915)  AGB_TRAMP_DECL(1916)  AGB_TRAMP_DECL(1917)  AGB_TRAMP_DECL(1918)  AGB_TRAMP_DECL(1919)
AGB_TRAMP_DECL(1920)  AGB_TRAMP_DECL(1921)  AGB_TRAMP_DECL(1922)  AGB_TRAMP_DECL(1923)  AGB_TRAMP_DECL(1924)  AGB_TRAMP_DECL(1925)  AGB_TRAMP_DECL(1926)  AGB_TRAMP_DECL(1927)
AGB_TRAMP_DECL(1928)  AGB_TRAMP_DECL(1929)  AGB_TRAMP_DECL(1930)  AGB_TRAMP_DECL(1931)  AGB_TRAMP_DECL(1932)  AGB_TRAMP_DECL(1933)  AGB_TRAMP_DECL(1934)  AGB_TRAMP_DECL(1935)
AGB_TRAMP_DECL(1936)  AGB_TRAMP_DECL(1937)  AGB_TRAMP_DECL(1938)  AGB_TRAMP_DECL(1939)  AGB_TRAMP_DECL(1940)  AGB_TRAMP_DECL(1941)  AGB_TRAMP_DECL(1942)  AGB_TRAMP_DECL(1943)
AGB_TRAMP_DECL(1944)  AGB_TRAMP_DECL(1945)  AGB_TRAMP_DECL(1946)  AGB_TRAMP_DECL(1947)  AGB_TRAMP_DECL(1948)  AGB_TRAMP_DECL(1949)  AGB_TRAMP_DECL(1950)  AGB_TRAMP_DECL(1951)
AGB_TRAMP_DECL(1952)  AGB_TRAMP_DECL(1953)  AGB_TRAMP_DECL(1954)  AGB_TRAMP_DECL(1955)  AGB_TRAMP_DECL(1956)  AGB_TRAMP_DECL(1957)  AGB_TRAMP_DECL(1958)  AGB_TRAMP_DECL(1959)
AGB_TRAMP_DECL(1960)  AGB_TRAMP_DECL(1961)  AGB_TRAMP_DECL(1962)  AGB_TRAMP_DECL(1963)  AGB_TRAMP_DECL(1964)  AGB_TRAMP_DECL(1965)  AGB_TRAMP_DECL(1966)  AGB_TRAMP_DECL(1967)
AGB_TRAMP_DECL(1968)  AGB_TRAMP_DECL(1969)  AGB_TRAMP_DECL(1970)  AGB_TRAMP_DECL(1971)  AGB_TRAMP_DECL(1972)  AGB_TRAMP_DECL(1973)  AGB_TRAMP_DECL(1974)  AGB_TRAMP_DECL(1975)
AGB_TRAMP_DECL(1976)  AGB_TRAMP_DECL(1977)  AGB_TRAMP_DECL(1978)  AGB_TRAMP_DECL(1979)  AGB_TRAMP_DECL(1980)  AGB_TRAMP_DECL(1981)  AGB_TRAMP_DECL(1982)  AGB_TRAMP_DECL(1983)
AGB_TRAMP_DECL(1984)  AGB_TRAMP_DECL(1985)  AGB_TRAMP_DECL(1986)  AGB_TRAMP_DECL(1987)  AGB_TRAMP_DECL(1988)  AGB_TRAMP_DECL(1989)  AGB_TRAMP_DECL(1990)  AGB_TRAMP_DECL(1991)
AGB_TRAMP_DECL(1992)  AGB_TRAMP_DECL(1993)  AGB_TRAMP_DECL(1994)  AGB_TRAMP_DECL(1995)  AGB_TRAMP_DECL(1996)  AGB_TRAMP_DECL(1997)  AGB_TRAMP_DECL(1998)  AGB_TRAMP_DECL(1999)
AGB_TRAMP_DECL(2000)  AGB_TRAMP_DECL(2001)  AGB_TRAMP_DECL(2002)  AGB_TRAMP_DECL(2003)  AGB_TRAMP_DECL(2004)  AGB_TRAMP_DECL(2005)  AGB_TRAMP_DECL(2006)  AGB_TRAMP_DECL(2007)
AGB_TRAMP_DECL(2008)  AGB_TRAMP_DECL(2009)  AGB_TRAMP_DECL(2010)  AGB_TRAMP_DECL(2011)  AGB_TRAMP_DECL(2012)  AGB_TRAMP_DECL(2013)  AGB_TRAMP_DECL(2014)  AGB_TRAMP_DECL(2015)
AGB_TRAMP_DECL(2016)  AGB_TRAMP_DECL(2017)  AGB_TRAMP_DECL(2018)  AGB_TRAMP_DECL(2019)  AGB_TRAMP_DECL(2020)  AGB_TRAMP_DECL(2021)  AGB_TRAMP_DECL(2022)  AGB_TRAMP_DECL(2023)
AGB_TRAMP_DECL(2024)  AGB_TRAMP_DECL(2025)  AGB_TRAMP_DECL(2026)  AGB_TRAMP_DECL(2027)  AGB_TRAMP_DECL(2028)  AGB_TRAMP_DECL(2029)  AGB_TRAMP_DECL(2030)  AGB_TRAMP_DECL(2031)
AGB_TRAMP_DECL(2032)  AGB_TRAMP_DECL(2033)  AGB_TRAMP_DECL(2034)  AGB_TRAMP_DECL(2035)  AGB_TRAMP_DECL(2036)  AGB_TRAMP_DECL(2037)  AGB_TRAMP_DECL(2038)  AGB_TRAMP_DECL(2039)
AGB_TRAMP_DECL(2040)  AGB_TRAMP_DECL(2041)  AGB_TRAMP_DECL(2042)  AGB_TRAMP_DECL(2043)  AGB_TRAMP_DECL(2044)  AGB_TRAMP_DECL(2045)  AGB_TRAMP_DECL(2046)  AGB_TRAMP_DECL(2047)
AGB_TRAMP_DECL(2048)  AGB_TRAMP_DECL(2049)  AGB_TRAMP_DECL(2050)  AGB_TRAMP_DECL(2051)  AGB_TRAMP_DECL(2052)  AGB_TRAMP_DECL(2053)  AGB_TRAMP_DECL(2054)  AGB_TRAMP_DECL(2055)
AGB_TRAMP_DECL(2056)  AGB_TRAMP_DECL(2057)  AGB_TRAMP_DECL(2058)  AGB_TRAMP_DECL(2059)  AGB_TRAMP_DECL(2060)  AGB_TRAMP_DECL(2061)  AGB_TRAMP_DECL(2062)  AGB_TRAMP_DECL(2063)
AGB_TRAMP_DECL(2064)  AGB_TRAMP_DECL(2065)  AGB_TRAMP_DECL(2066)  AGB_TRAMP_DECL(2067)  AGB_TRAMP_DECL(2068)  AGB_TRAMP_DECL(2069)  AGB_TRAMP_DECL(2070)  AGB_TRAMP_DECL(2071)
AGB_TRAMP_DECL(2072)  AGB_TRAMP_DECL(2073)  AGB_TRAMP_DECL(2074)  AGB_TRAMP_DECL(2075)  AGB_TRAMP_DECL(2076)  AGB_TRAMP_DECL(2077)  AGB_TRAMP_DECL(2078)  AGB_TRAMP_DECL(2079)
AGB_TRAMP_DECL(2080)  AGB_TRAMP_DECL(2081)  AGB_TRAMP_DECL(2082)  AGB_TRAMP_DECL(2083)  AGB_TRAMP_DECL(2084)  AGB_TRAMP_DECL(2085)  AGB_TRAMP_DECL(2086)  AGB_TRAMP_DECL(2087)
AGB_TRAMP_DECL(2088)  AGB_TRAMP_DECL(2089)  AGB_TRAMP_DECL(2090)  AGB_TRAMP_DECL(2091)  AGB_TRAMP_DECL(2092)  AGB_TRAMP_DECL(2093)  AGB_TRAMP_DECL(2094)  AGB_TRAMP_DECL(2095)
AGB_TRAMP_DECL(2096)  AGB_TRAMP_DECL(2097)  AGB_TRAMP_DECL(2098)  AGB_TRAMP_DECL(2099)  AGB_TRAMP_DECL(2100)  AGB_TRAMP_DECL(2101)  AGB_TRAMP_DECL(2102)  AGB_TRAMP_DECL(2103)
AGB_TRAMP_DECL(2104)  AGB_TRAMP_DECL(2105)  AGB_TRAMP_DECL(2106)  AGB_TRAMP_DECL(2107)  AGB_TRAMP_DECL(2108)  AGB_TRAMP_DECL(2109)  AGB_TRAMP_DECL(2110)  AGB_TRAMP_DECL(2111)
AGB_TRAMP_DECL(2112)  AGB_TRAMP_DECL(2113)  AGB_TRAMP_DECL(2114)  AGB_TRAMP_DECL(2115)  AGB_TRAMP_DECL(2116)  AGB_TRAMP_DECL(2117)  AGB_TRAMP_DECL(2118)  AGB_TRAMP_DECL(2119)
AGB_TRAMP_DECL(2120)  AGB_TRAMP_DECL(2121)  AGB_TRAMP_DECL(2122)  AGB_TRAMP_DECL(2123)  AGB_TRAMP_DECL(2124)  AGB_TRAMP_DECL(2125)  AGB_TRAMP_DECL(2126)  AGB_TRAMP_DECL(2127)
AGB_TRAMP_DECL(2128)  AGB_TRAMP_DECL(2129)  AGB_TRAMP_DECL(2130)  AGB_TRAMP_DECL(2131)  AGB_TRAMP_DECL(2132)  AGB_TRAMP_DECL(2133)  AGB_TRAMP_DECL(2134)  AGB_TRAMP_DECL(2135)
AGB_TRAMP_DECL(2136)  AGB_TRAMP_DECL(2137)  AGB_TRAMP_DECL(2138)  AGB_TRAMP_DECL(2139)  AGB_TRAMP_DECL(2140)  AGB_TRAMP_DECL(2141)  AGB_TRAMP_DECL(2142)  AGB_TRAMP_DECL(2143)
AGB_TRAMP_DECL(2144)  AGB_TRAMP_DECL(2145)  AGB_TRAMP_DECL(2146)  AGB_TRAMP_DECL(2147)  AGB_TRAMP_DECL(2148)  AGB_TRAMP_DECL(2149)  AGB_TRAMP_DECL(2150)  AGB_TRAMP_DECL(2151)
AGB_TRAMP_DECL(2152)  AGB_TRAMP_DECL(2153)  AGB_TRAMP_DECL(2154)  AGB_TRAMP_DECL(2155)  AGB_TRAMP_DECL(2156)  AGB_TRAMP_DECL(2157)  AGB_TRAMP_DECL(2158)  AGB_TRAMP_DECL(2159)
AGB_TRAMP_DECL(2160)  AGB_TRAMP_DECL(2161)  AGB_TRAMP_DECL(2162)  AGB_TRAMP_DECL(2163)  AGB_TRAMP_DECL(2164)  AGB_TRAMP_DECL(2165)  AGB_TRAMP_DECL(2166)  AGB_TRAMP_DECL(2167)
AGB_TRAMP_DECL(2168)  AGB_TRAMP_DECL(2169)  AGB_TRAMP_DECL(2170)  AGB_TRAMP_DECL(2171)  AGB_TRAMP_DECL(2172)  AGB_TRAMP_DECL(2173)  AGB_TRAMP_DECL(2174)  AGB_TRAMP_DECL(2175)
AGB_TRAMP_DECL(2176)  AGB_TRAMP_DECL(2177)  AGB_TRAMP_DECL(2178)  AGB_TRAMP_DECL(2179)  AGB_TRAMP_DECL(2180)  AGB_TRAMP_DECL(2181)  AGB_TRAMP_DECL(2182)  AGB_TRAMP_DECL(2183)
AGB_TRAMP_DECL(2184)  AGB_TRAMP_DECL(2185)  AGB_TRAMP_DECL(2186)  AGB_TRAMP_DECL(2187)  AGB_TRAMP_DECL(2188)  AGB_TRAMP_DECL(2189)  AGB_TRAMP_DECL(2190)  AGB_TRAMP_DECL(2191)
AGB_TRAMP_DECL(2192)  AGB_TRAMP_DECL(2193)  AGB_TRAMP_DECL(2194)  AGB_TRAMP_DECL(2195)  AGB_TRAMP_DECL(2196)  AGB_TRAMP_DECL(2197)  AGB_TRAMP_DECL(2198)  AGB_TRAMP_DECL(2199)
AGB_TRAMP_DECL(2200)  AGB_TRAMP_DECL(2201)  AGB_TRAMP_DECL(2202)  AGB_TRAMP_DECL(2203)  AGB_TRAMP_DECL(2204)  AGB_TRAMP_DECL(2205)  AGB_TRAMP_DECL(2206)  AGB_TRAMP_DECL(2207)
AGB_TRAMP_DECL(2208)  AGB_TRAMP_DECL(2209)  AGB_TRAMP_DECL(2210)  AGB_TRAMP_DECL(2211)  AGB_TRAMP_DECL(2212)  AGB_TRAMP_DECL(2213)  AGB_TRAMP_DECL(2214)  AGB_TRAMP_DECL(2215)
AGB_TRAMP_DECL(2216)  AGB_TRAMP_DECL(2217)  AGB_TRAMP_DECL(2218)  AGB_TRAMP_DECL(2219)  AGB_TRAMP_DECL(2220)  AGB_TRAMP_DECL(2221)  AGB_TRAMP_DECL(2222)  AGB_TRAMP_DECL(2223)
AGB_TRAMP_DECL(2224)  AGB_TRAMP_DECL(2225)  AGB_TRAMP_DECL(2226)  AGB_TRAMP_DECL(2227)  AGB_TRAMP_DECL(2228)  AGB_TRAMP_DECL(2229)  AGB_TRAMP_DECL(2230)  AGB_TRAMP_DECL(2231)
AGB_TRAMP_DECL(2232)  AGB_TRAMP_DECL(2233)  AGB_TRAMP_DECL(2234)  AGB_TRAMP_DECL(2235)  AGB_TRAMP_DECL(2236)  AGB_TRAMP_DECL(2237)  AGB_TRAMP_DECL(2238)  AGB_TRAMP_DECL(2239)
AGB_TRAMP_DECL(2240)  AGB_TRAMP_DECL(2241)  AGB_TRAMP_DECL(2242)  AGB_TRAMP_DECL(2243)  AGB_TRAMP_DECL(2244)  AGB_TRAMP_DECL(2245)  AGB_TRAMP_DECL(2246)  AGB_TRAMP_DECL(2247)
AGB_TRAMP_DECL(2248)  AGB_TRAMP_DECL(2249)  AGB_TRAMP_DECL(2250)  AGB_TRAMP_DECL(2251)  AGB_TRAMP_DECL(2252)  AGB_TRAMP_DECL(2253)  AGB_TRAMP_DECL(2254)  AGB_TRAMP_DECL(2255)
AGB_TRAMP_DECL(2256)  AGB_TRAMP_DECL(2257)  AGB_TRAMP_DECL(2258)  AGB_TRAMP_DECL(2259)  AGB_TRAMP_DECL(2260)  AGB_TRAMP_DECL(2261)  AGB_TRAMP_DECL(2262)  AGB_TRAMP_DECL(2263)
AGB_TRAMP_DECL(2264)  AGB_TRAMP_DECL(2265)  AGB_TRAMP_DECL(2266)  AGB_TRAMP_DECL(2267)  AGB_TRAMP_DECL(2268)  AGB_TRAMP_DECL(2269)  AGB_TRAMP_DECL(2270)  AGB_TRAMP_DECL(2271)
AGB_TRAMP_DECL(2272)  AGB_TRAMP_DECL(2273)  AGB_TRAMP_DECL(2274)  AGB_TRAMP_DECL(2275)  AGB_TRAMP_DECL(2276)  AGB_TRAMP_DECL(2277)  AGB_TRAMP_DECL(2278)  AGB_TRAMP_DECL(2279)
AGB_TRAMP_DECL(2280)  AGB_TRAMP_DECL(2281)  AGB_TRAMP_DECL(2282)  AGB_TRAMP_DECL(2283)  AGB_TRAMP_DECL(2284)  AGB_TRAMP_DECL(2285)  AGB_TRAMP_DECL(2286)  AGB_TRAMP_DECL(2287)
AGB_TRAMP_DECL(2288)  AGB_TRAMP_DECL(2289)  AGB_TRAMP_DECL(2290)  AGB_TRAMP_DECL(2291)  AGB_TRAMP_DECL(2292)  AGB_TRAMP_DECL(2293)  AGB_TRAMP_DECL(2294)  AGB_TRAMP_DECL(2295)
AGB_TRAMP_DECL(2296)  AGB_TRAMP_DECL(2297)  AGB_TRAMP_DECL(2298)  AGB_TRAMP_DECL(2299)  AGB_TRAMP_DECL(2300)  AGB_TRAMP_DECL(2301)  AGB_TRAMP_DECL(2302)  AGB_TRAMP_DECL(2303)
AGB_TRAMP_DECL(2304)  AGB_TRAMP_DECL(2305)  AGB_TRAMP_DECL(2306)  AGB_TRAMP_DECL(2307)  AGB_TRAMP_DECL(2308)  AGB_TRAMP_DECL(2309)  AGB_TRAMP_DECL(2310)  AGB_TRAMP_DECL(2311)
AGB_TRAMP_DECL(2312)  AGB_TRAMP_DECL(2313)  AGB_TRAMP_DECL(2314)  AGB_TRAMP_DECL(2315)  AGB_TRAMP_DECL(2316)  AGB_TRAMP_DECL(2317)  AGB_TRAMP_DECL(2318)  AGB_TRAMP_DECL(2319)
AGB_TRAMP_DECL(2320)  AGB_TRAMP_DECL(2321)  AGB_TRAMP_DECL(2322)  AGB_TRAMP_DECL(2323)  AGB_TRAMP_DECL(2324)  AGB_TRAMP_DECL(2325)  AGB_TRAMP_DECL(2326)  AGB_TRAMP_DECL(2327)
AGB_TRAMP_DECL(2328)  AGB_TRAMP_DECL(2329)  AGB_TRAMP_DECL(2330)  AGB_TRAMP_DECL(2331)  AGB_TRAMP_DECL(2332)  AGB_TRAMP_DECL(2333)  AGB_TRAMP_DECL(2334)  AGB_TRAMP_DECL(2335)
AGB_TRAMP_DECL(2336)  AGB_TRAMP_DECL(2337)  AGB_TRAMP_DECL(2338)  AGB_TRAMP_DECL(2339)  AGB_TRAMP_DECL(2340)  AGB_TRAMP_DECL(2341)  AGB_TRAMP_DECL(2342)  AGB_TRAMP_DECL(2343)
AGB_TRAMP_DECL(2344)  AGB_TRAMP_DECL(2345)  AGB_TRAMP_DECL(2346)  AGB_TRAMP_DECL(2347)  AGB_TRAMP_DECL(2348)  AGB_TRAMP_DECL(2349)  AGB_TRAMP_DECL(2350)  AGB_TRAMP_DECL(2351)
AGB_TRAMP_DECL(2352)  AGB_TRAMP_DECL(2353)  AGB_TRAMP_DECL(2354)  AGB_TRAMP_DECL(2355)  AGB_TRAMP_DECL(2356)  AGB_TRAMP_DECL(2357)  AGB_TRAMP_DECL(2358)  AGB_TRAMP_DECL(2359)
AGB_TRAMP_DECL(2360)  AGB_TRAMP_DECL(2361)  AGB_TRAMP_DECL(2362)  AGB_TRAMP_DECL(2363)  AGB_TRAMP_DECL(2364)  AGB_TRAMP_DECL(2365)  AGB_TRAMP_DECL(2366)  AGB_TRAMP_DECL(2367)
AGB_TRAMP_DECL(2368)  AGB_TRAMP_DECL(2369)  AGB_TRAMP_DECL(2370)  AGB_TRAMP_DECL(2371)  AGB_TRAMP_DECL(2372)  AGB_TRAMP_DECL(2373)  AGB_TRAMP_DECL(2374)  AGB_TRAMP_DECL(2375)
AGB_TRAMP_DECL(2376)  AGB_TRAMP_DECL(2377)  AGB_TRAMP_DECL(2378)  AGB_TRAMP_DECL(2379)  AGB_TRAMP_DECL(2380)  AGB_TRAMP_DECL(2381)  AGB_TRAMP_DECL(2382)  AGB_TRAMP_DECL(2383)
AGB_TRAMP_DECL(2384)  AGB_TRAMP_DECL(2385)  AGB_TRAMP_DECL(2386)  AGB_TRAMP_DECL(2387)  AGB_TRAMP_DECL(2388)  AGB_TRAMP_DECL(2389)  AGB_TRAMP_DECL(2390)  AGB_TRAMP_DECL(2391)
AGB_TRAMP_DECL(2392)  AGB_TRAMP_DECL(2393)  AGB_TRAMP_DECL(2394)  AGB_TRAMP_DECL(2395)  AGB_TRAMP_DECL(2396)  AGB_TRAMP_DECL(2397)  AGB_TRAMP_DECL(2398)  AGB_TRAMP_DECL(2399)
AGB_TRAMP_DECL(2400)  AGB_TRAMP_DECL(2401)  AGB_TRAMP_DECL(2402)  AGB_TRAMP_DECL(2403)  AGB_TRAMP_DECL(2404)  AGB_TRAMP_DECL(2405)  AGB_TRAMP_DECL(2406)  AGB_TRAMP_DECL(2407)
AGB_TRAMP_DECL(2408)  AGB_TRAMP_DECL(2409)  AGB_TRAMP_DECL(2410)  AGB_TRAMP_DECL(2411)  AGB_TRAMP_DECL(2412)  AGB_TRAMP_DECL(2413)  AGB_TRAMP_DECL(2414)  AGB_TRAMP_DECL(2415)
AGB_TRAMP_DECL(2416)  AGB_TRAMP_DECL(2417)  AGB_TRAMP_DECL(2418)  AGB_TRAMP_DECL(2419)  AGB_TRAMP_DECL(2420)  AGB_TRAMP_DECL(2421)  AGB_TRAMP_DECL(2422)  AGB_TRAMP_DECL(2423)
AGB_TRAMP_DECL(2424)  AGB_TRAMP_DECL(2425)  AGB_TRAMP_DECL(2426)  AGB_TRAMP_DECL(2427)  AGB_TRAMP_DECL(2428)  AGB_TRAMP_DECL(2429)  AGB_TRAMP_DECL(2430)  AGB_TRAMP_DECL(2431)
AGB_TRAMP_DECL(2432)  AGB_TRAMP_DECL(2433)  AGB_TRAMP_DECL(2434)  AGB_TRAMP_DECL(2435)  AGB_TRAMP_DECL(2436)  AGB_TRAMP_DECL(2437)  AGB_TRAMP_DECL(2438)  AGB_TRAMP_DECL(2439)
AGB_TRAMP_DECL(2440)  AGB_TRAMP_DECL(2441)  AGB_TRAMP_DECL(2442)  AGB_TRAMP_DECL(2443)  AGB_TRAMP_DECL(2444)  AGB_TRAMP_DECL(2445)  AGB_TRAMP_DECL(2446)  AGB_TRAMP_DECL(2447)
AGB_TRAMP_DECL(2448)  AGB_TRAMP_DECL(2449)  AGB_TRAMP_DECL(2450)  AGB_TRAMP_DECL(2451)  AGB_TRAMP_DECL(2452)  AGB_TRAMP_DECL(2453)  AGB_TRAMP_DECL(2454)  AGB_TRAMP_DECL(2455)
AGB_TRAMP_DECL(2456)  AGB_TRAMP_DECL(2457)  AGB_TRAMP_DECL(2458)  AGB_TRAMP_DECL(2459)  AGB_TRAMP_DECL(2460)  AGB_TRAMP_DECL(2461)  AGB_TRAMP_DECL(2462)  AGB_TRAMP_DECL(2463)
AGB_TRAMP_DECL(2464)  AGB_TRAMP_DECL(2465)  AGB_TRAMP_DECL(2466)  AGB_TRAMP_DECL(2467)  AGB_TRAMP_DECL(2468)  AGB_TRAMP_DECL(2469)  AGB_TRAMP_DECL(2470)  AGB_TRAMP_DECL(2471)
AGB_TRAMP_DECL(2472)  AGB_TRAMP_DECL(2473)  AGB_TRAMP_DECL(2474)  AGB_TRAMP_DECL(2475)  AGB_TRAMP_DECL(2476)  AGB_TRAMP_DECL(2477)  AGB_TRAMP_DECL(2478)  AGB_TRAMP_DECL(2479)
AGB_TRAMP_DECL(2480)  AGB_TRAMP_DECL(2481)  AGB_TRAMP_DECL(2482)  AGB_TRAMP_DECL(2483)  AGB_TRAMP_DECL(2484)  AGB_TRAMP_DECL(2485)  AGB_TRAMP_DECL(2486)  AGB_TRAMP_DECL(2487)
AGB_TRAMP_DECL(2488)  AGB_TRAMP_DECL(2489)  AGB_TRAMP_DECL(2490)  AGB_TRAMP_DECL(2491)  AGB_TRAMP_DECL(2492)  AGB_TRAMP_DECL(2493)  AGB_TRAMP_DECL(2494)  AGB_TRAMP_DECL(2495)
AGB_TRAMP_DECL(2496)  AGB_TRAMP_DECL(2497)  AGB_TRAMP_DECL(2498)  AGB_TRAMP_DECL(2499)  AGB_TRAMP_DECL(2500)  AGB_TRAMP_DECL(2501)  AGB_TRAMP_DECL(2502)  AGB_TRAMP_DECL(2503)
AGB_TRAMP_DECL(2504)  AGB_TRAMP_DECL(2505)  AGB_TRAMP_DECL(2506)  AGB_TRAMP_DECL(2507)  AGB_TRAMP_DECL(2508)  AGB_TRAMP_DECL(2509)  AGB_TRAMP_DECL(2510)  AGB_TRAMP_DECL(2511)
AGB_TRAMP_DECL(2512)  AGB_TRAMP_DECL(2513)  AGB_TRAMP_DECL(2514)  AGB_TRAMP_DECL(2515)  AGB_TRAMP_DECL(2516)  AGB_TRAMP_DECL(2517)  AGB_TRAMP_DECL(2518)  AGB_TRAMP_DECL(2519)
AGB_TRAMP_DECL(2520)  AGB_TRAMP_DECL(2521)  AGB_TRAMP_DECL(2522)  AGB_TRAMP_DECL(2523)  AGB_TRAMP_DECL(2524)  AGB_TRAMP_DECL(2525)  AGB_TRAMP_DECL(2526)  AGB_TRAMP_DECL(2527)
AGB_TRAMP_DECL(2528)  AGB_TRAMP_DECL(2529)  AGB_TRAMP_DECL(2530)  AGB_TRAMP_DECL(2531)  AGB_TRAMP_DECL(2532)  AGB_TRAMP_DECL(2533)  AGB_TRAMP_DECL(2534)  AGB_TRAMP_DECL(2535)
AGB_TRAMP_DECL(2536)  AGB_TRAMP_DECL(2537)  AGB_TRAMP_DECL(2538)  AGB_TRAMP_DECL(2539)  AGB_TRAMP_DECL(2540)  AGB_TRAMP_DECL(2541)  AGB_TRAMP_DECL(2542)  AGB_TRAMP_DECL(2543)
AGB_TRAMP_DECL(2544)  AGB_TRAMP_DECL(2545)  AGB_TRAMP_DECL(2546)  AGB_TRAMP_DECL(2547)  AGB_TRAMP_DECL(2548)  AGB_TRAMP_DECL(2549)  AGB_TRAMP_DECL(2550)  AGB_TRAMP_DECL(2551)
AGB_TRAMP_DECL(2552)  AGB_TRAMP_DECL(2553)  AGB_TRAMP_DECL(2554)  AGB_TRAMP_DECL(2555)  AGB_TRAMP_DECL(2556)  AGB_TRAMP_DECL(2557)  AGB_TRAMP_DECL(2558)  AGB_TRAMP_DECL(2559)
AGB_TRAMP_DECL(2560)  AGB_TRAMP_DECL(2561)  AGB_TRAMP_DECL(2562)  AGB_TRAMP_DECL(2563)  AGB_TRAMP_DECL(2564)  AGB_TRAMP_DECL(2565)  AGB_TRAMP_DECL(2566)  AGB_TRAMP_DECL(2567)
AGB_TRAMP_DECL(2568)  AGB_TRAMP_DECL(2569)  AGB_TRAMP_DECL(2570)  AGB_TRAMP_DECL(2571)  AGB_TRAMP_DECL(2572)  AGB_TRAMP_DECL(2573)  AGB_TRAMP_DECL(2574)  AGB_TRAMP_DECL(2575)
AGB_TRAMP_DECL(2576)  AGB_TRAMP_DECL(2577)  AGB_TRAMP_DECL(2578)  AGB_TRAMP_DECL(2579)  AGB_TRAMP_DECL(2580)  AGB_TRAMP_DECL(2581)  AGB_TRAMP_DECL(2582)  AGB_TRAMP_DECL(2583)
AGB_TRAMP_DECL(2584)  AGB_TRAMP_DECL(2585)  AGB_TRAMP_DECL(2586)  AGB_TRAMP_DECL(2587)  AGB_TRAMP_DECL(2588)  AGB_TRAMP_DECL(2589)  AGB_TRAMP_DECL(2590)  AGB_TRAMP_DECL(2591)
AGB_TRAMP_DECL(2592)  AGB_TRAMP_DECL(2593)  AGB_TRAMP_DECL(2594)  AGB_TRAMP_DECL(2595)  AGB_TRAMP_DECL(2596)  AGB_TRAMP_DECL(2597)  AGB_TRAMP_DECL(2598)  AGB_TRAMP_DECL(2599)
AGB_TRAMP_DECL(2600)  AGB_TRAMP_DECL(2601)  AGB_TRAMP_DECL(2602)  AGB_TRAMP_DECL(2603)  AGB_TRAMP_DECL(2604)  AGB_TRAMP_DECL(2605)  AGB_TRAMP_DECL(2606)  AGB_TRAMP_DECL(2607)
AGB_TRAMP_DECL(2608)  AGB_TRAMP_DECL(2609)  AGB_TRAMP_DECL(2610)  AGB_TRAMP_DECL(2611)  AGB_TRAMP_DECL(2612)  AGB_TRAMP_DECL(2613)  AGB_TRAMP_DECL(2614)  AGB_TRAMP_DECL(2615)
AGB_TRAMP_DECL(2616)  AGB_TRAMP_DECL(2617)  AGB_TRAMP_DECL(2618)  AGB_TRAMP_DECL(2619)  AGB_TRAMP_DECL(2620)  AGB_TRAMP_DECL(2621)  AGB_TRAMP_DECL(2622)  AGB_TRAMP_DECL(2623)
AGB_TRAMP_DECL(2624)  AGB_TRAMP_DECL(2625)  AGB_TRAMP_DECL(2626)  AGB_TRAMP_DECL(2627)  AGB_TRAMP_DECL(2628)  AGB_TRAMP_DECL(2629)  AGB_TRAMP_DECL(2630)  AGB_TRAMP_DECL(2631)
AGB_TRAMP_DECL(2632)  AGB_TRAMP_DECL(2633)  AGB_TRAMP_DECL(2634)  AGB_TRAMP_DECL(2635)  AGB_TRAMP_DECL(2636)  AGB_TRAMP_DECL(2637)  AGB_TRAMP_DECL(2638)  AGB_TRAMP_DECL(2639)
AGB_TRAMP_DECL(2640)  AGB_TRAMP_DECL(2641)  AGB_TRAMP_DECL(2642)  AGB_TRAMP_DECL(2643)  AGB_TRAMP_DECL(2644)  AGB_TRAMP_DECL(2645)  AGB_TRAMP_DECL(2646)  AGB_TRAMP_DECL(2647)
AGB_TRAMP_DECL(2648)  AGB_TRAMP_DECL(2649)  AGB_TRAMP_DECL(2650)  AGB_TRAMP_DECL(2651)  AGB_TRAMP_DECL(2652)  AGB_TRAMP_DECL(2653)  AGB_TRAMP_DECL(2654)  AGB_TRAMP_DECL(2655)
AGB_TRAMP_DECL(2656)  AGB_TRAMP_DECL(2657)  AGB_TRAMP_DECL(2658)  AGB_TRAMP_DECL(2659)  AGB_TRAMP_DECL(2660)  AGB_TRAMP_DECL(2661)  AGB_TRAMP_DECL(2662)  AGB_TRAMP_DECL(2663)
AGB_TRAMP_DECL(2664)  AGB_TRAMP_DECL(2665)  AGB_TRAMP_DECL(2666)  AGB_TRAMP_DECL(2667)  AGB_TRAMP_DECL(2668)  AGB_TRAMP_DECL(2669)  AGB_TRAMP_DECL(2670)  AGB_TRAMP_DECL(2671)
AGB_TRAMP_DECL(2672)  AGB_TRAMP_DECL(2673)  AGB_TRAMP_DECL(2674)  AGB_TRAMP_DECL(2675)  AGB_TRAMP_DECL(2676)  AGB_TRAMP_DECL(2677)  AGB_TRAMP_DECL(2678)  AGB_TRAMP_DECL(2679)
AGB_TRAMP_DECL(2680)  AGB_TRAMP_DECL(2681)  AGB_TRAMP_DECL(2682)  AGB_TRAMP_DECL(2683)  AGB_TRAMP_DECL(2684)  AGB_TRAMP_DECL(2685)  AGB_TRAMP_DECL(2686)  AGB_TRAMP_DECL(2687)
AGB_TRAMP_DECL(2688)  AGB_TRAMP_DECL(2689)  AGB_TRAMP_DECL(2690)  AGB_TRAMP_DECL(2691)  AGB_TRAMP_DECL(2692)  AGB_TRAMP_DECL(2693)  AGB_TRAMP_DECL(2694)  AGB_TRAMP_DECL(2695)
AGB_TRAMP_DECL(2696)  AGB_TRAMP_DECL(2697)  AGB_TRAMP_DECL(2698)  AGB_TRAMP_DECL(2699)  AGB_TRAMP_DECL(2700)  AGB_TRAMP_DECL(2701)  AGB_TRAMP_DECL(2702)  AGB_TRAMP_DECL(2703)
AGB_TRAMP_DECL(2704)  AGB_TRAMP_DECL(2705)  AGB_TRAMP_DECL(2706)  AGB_TRAMP_DECL(2707)  AGB_TRAMP_DECL(2708)  AGB_TRAMP_DECL(2709)  AGB_TRAMP_DECL(2710)  AGB_TRAMP_DECL(2711)
AGB_TRAMP_DECL(2712)  AGB_TRAMP_DECL(2713)  AGB_TRAMP_DECL(2714)  AGB_TRAMP_DECL(2715)  AGB_TRAMP_DECL(2716)  AGB_TRAMP_DECL(2717)  AGB_TRAMP_DECL(2718)  AGB_TRAMP_DECL(2719)
AGB_TRAMP_DECL(2720)  AGB_TRAMP_DECL(2721)  AGB_TRAMP_DECL(2722)  AGB_TRAMP_DECL(2723)  AGB_TRAMP_DECL(2724)  AGB_TRAMP_DECL(2725)  AGB_TRAMP_DECL(2726)  AGB_TRAMP_DECL(2727)
AGB_TRAMP_DECL(2728)  AGB_TRAMP_DECL(2729)  AGB_TRAMP_DECL(2730)  AGB_TRAMP_DECL(2731)  AGB_TRAMP_DECL(2732)  AGB_TRAMP_DECL(2733)  AGB_TRAMP_DECL(2734)  AGB_TRAMP_DECL(2735)
AGB_TRAMP_DECL(2736)  AGB_TRAMP_DECL(2737)  AGB_TRAMP_DECL(2738)  AGB_TRAMP_DECL(2739)  AGB_TRAMP_DECL(2740)  AGB_TRAMP_DECL(2741)  AGB_TRAMP_DECL(2742)  AGB_TRAMP_DECL(2743)
AGB_TRAMP_DECL(2744)  AGB_TRAMP_DECL(2745)  AGB_TRAMP_DECL(2746)  AGB_TRAMP_DECL(2747)  AGB_TRAMP_DECL(2748)  AGB_TRAMP_DECL(2749)  AGB_TRAMP_DECL(2750)  AGB_TRAMP_DECL(2751)
AGB_TRAMP_DECL(2752)  AGB_TRAMP_DECL(2753)  AGB_TRAMP_DECL(2754)  AGB_TRAMP_DECL(2755)  AGB_TRAMP_DECL(2756)  AGB_TRAMP_DECL(2757)  AGB_TRAMP_DECL(2758)  AGB_TRAMP_DECL(2759)
AGB_TRAMP_DECL(2760)  AGB_TRAMP_DECL(2761)  AGB_TRAMP_DECL(2762)  AGB_TRAMP_DECL(2763)  AGB_TRAMP_DECL(2764)  AGB_TRAMP_DECL(2765)  AGB_TRAMP_DECL(2766)  AGB_TRAMP_DECL(2767)
AGB_TRAMP_DECL(2768)  AGB_TRAMP_DECL(2769)  AGB_TRAMP_DECL(2770)  AGB_TRAMP_DECL(2771)  AGB_TRAMP_DECL(2772)  AGB_TRAMP_DECL(2773)  AGB_TRAMP_DECL(2774)  AGB_TRAMP_DECL(2775)
AGB_TRAMP_DECL(2776)  AGB_TRAMP_DECL(2777)  AGB_TRAMP_DECL(2778)  AGB_TRAMP_DECL(2779)  AGB_TRAMP_DECL(2780)  AGB_TRAMP_DECL(2781)  AGB_TRAMP_DECL(2782)  AGB_TRAMP_DECL(2783)
AGB_TRAMP_DECL(2784)  AGB_TRAMP_DECL(2785)  AGB_TRAMP_DECL(2786)  AGB_TRAMP_DECL(2787)  AGB_TRAMP_DECL(2788)  AGB_TRAMP_DECL(2789)  AGB_TRAMP_DECL(2790)  AGB_TRAMP_DECL(2791)
AGB_TRAMP_DECL(2792)  AGB_TRAMP_DECL(2793)  AGB_TRAMP_DECL(2794)  AGB_TRAMP_DECL(2795)  AGB_TRAMP_DECL(2796)  AGB_TRAMP_DECL(2797)  AGB_TRAMP_DECL(2798)  AGB_TRAMP_DECL(2799)
AGB_TRAMP_DECL(2800)  AGB_TRAMP_DECL(2801)  AGB_TRAMP_DECL(2802)  AGB_TRAMP_DECL(2803)  AGB_TRAMP_DECL(2804)  AGB_TRAMP_DECL(2805)  AGB_TRAMP_DECL(2806)  AGB_TRAMP_DECL(2807)
AGB_TRAMP_DECL(2808)  AGB_TRAMP_DECL(2809)  AGB_TRAMP_DECL(2810)  AGB_TRAMP_DECL(2811)  AGB_TRAMP_DECL(2812)  AGB_TRAMP_DECL(2813)  AGB_TRAMP_DECL(2814)  AGB_TRAMP_DECL(2815)
AGB_TRAMP_DECL(2816)  AGB_TRAMP_DECL(2817)  AGB_TRAMP_DECL(2818)  AGB_TRAMP_DECL(2819)  AGB_TRAMP_DECL(2820)  AGB_TRAMP_DECL(2821)  AGB_TRAMP_DECL(2822)  AGB_TRAMP_DECL(2823)
AGB_TRAMP_DECL(2824)  AGB_TRAMP_DECL(2825)  AGB_TRAMP_DECL(2826)  AGB_TRAMP_DECL(2827)  AGB_TRAMP_DECL(2828)  AGB_TRAMP_DECL(2829)  AGB_TRAMP_DECL(2830)  AGB_TRAMP_DECL(2831)
AGB_TRAMP_DECL(2832)  AGB_TRAMP_DECL(2833)  AGB_TRAMP_DECL(2834)  AGB_TRAMP_DECL(2835)  AGB_TRAMP_DECL(2836)  AGB_TRAMP_DECL(2837)  AGB_TRAMP_DECL(2838)  AGB_TRAMP_DECL(2839)
AGB_TRAMP_DECL(2840)  AGB_TRAMP_DECL(2841)  AGB_TRAMP_DECL(2842)  AGB_TRAMP_DECL(2843)  AGB_TRAMP_DECL(2844)  AGB_TRAMP_DECL(2845)  AGB_TRAMP_DECL(2846)  AGB_TRAMP_DECL(2847)
AGB_TRAMP_DECL(2848)  AGB_TRAMP_DECL(2849)  AGB_TRAMP_DECL(2850)  AGB_TRAMP_DECL(2851)  AGB_TRAMP_DECL(2852)  AGB_TRAMP_DECL(2853)  AGB_TRAMP_DECL(2854)  AGB_TRAMP_DECL(2855)
AGB_TRAMP_DECL(2856)  AGB_TRAMP_DECL(2857)  AGB_TRAMP_DECL(2858)  AGB_TRAMP_DECL(2859)  AGB_TRAMP_DECL(2860)  AGB_TRAMP_DECL(2861)  AGB_TRAMP_DECL(2862)  AGB_TRAMP_DECL(2863)
AGB_TRAMP_DECL(2864)  AGB_TRAMP_DECL(2865)  AGB_TRAMP_DECL(2866)  AGB_TRAMP_DECL(2867)  AGB_TRAMP_DECL(2868)  AGB_TRAMP_DECL(2869)  AGB_TRAMP_DECL(2870)  AGB_TRAMP_DECL(2871)
AGB_TRAMP_DECL(2872)  AGB_TRAMP_DECL(2873)  AGB_TRAMP_DECL(2874)  AGB_TRAMP_DECL(2875)  AGB_TRAMP_DECL(2876)  AGB_TRAMP_DECL(2877)  AGB_TRAMP_DECL(2878)  AGB_TRAMP_DECL(2879)
AGB_TRAMP_DECL(2880)  AGB_TRAMP_DECL(2881)  AGB_TRAMP_DECL(2882)  AGB_TRAMP_DECL(2883)  AGB_TRAMP_DECL(2884)  AGB_TRAMP_DECL(2885)  AGB_TRAMP_DECL(2886)  AGB_TRAMP_DECL(2887)
AGB_TRAMP_DECL(2888)  AGB_TRAMP_DECL(2889)  AGB_TRAMP_DECL(2890)  AGB_TRAMP_DECL(2891)  AGB_TRAMP_DECL(2892)  AGB_TRAMP_DECL(2893)  AGB_TRAMP_DECL(2894)  AGB_TRAMP_DECL(2895)
AGB_TRAMP_DECL(2896)  AGB_TRAMP_DECL(2897)  AGB_TRAMP_DECL(2898)  AGB_TRAMP_DECL(2899)  AGB_TRAMP_DECL(2900)  AGB_TRAMP_DECL(2901)  AGB_TRAMP_DECL(2902)  AGB_TRAMP_DECL(2903)
AGB_TRAMP_DECL(2904)  AGB_TRAMP_DECL(2905)  AGB_TRAMP_DECL(2906)  AGB_TRAMP_DECL(2907)  AGB_TRAMP_DECL(2908)  AGB_TRAMP_DECL(2909)  AGB_TRAMP_DECL(2910)  AGB_TRAMP_DECL(2911)
AGB_TRAMP_DECL(2912)  AGB_TRAMP_DECL(2913)  AGB_TRAMP_DECL(2914)  AGB_TRAMP_DECL(2915)  AGB_TRAMP_DECL(2916)  AGB_TRAMP_DECL(2917)  AGB_TRAMP_DECL(2918)  AGB_TRAMP_DECL(2919)
AGB_TRAMP_DECL(2920)  AGB_TRAMP_DECL(2921)  AGB_TRAMP_DECL(2922)  AGB_TRAMP_DECL(2923)  AGB_TRAMP_DECL(2924)  AGB_TRAMP_DECL(2925)  AGB_TRAMP_DECL(2926)  AGB_TRAMP_DECL(2927)
AGB_TRAMP_DECL(2928)  AGB_TRAMP_DECL(2929)  AGB_TRAMP_DECL(2930)  AGB_TRAMP_DECL(2931)  AGB_TRAMP_DECL(2932)  AGB_TRAMP_DECL(2933)  AGB_TRAMP_DECL(2934)  AGB_TRAMP_DECL(2935)
AGB_TRAMP_DECL(2936)  AGB_TRAMP_DECL(2937)  AGB_TRAMP_DECL(2938)  AGB_TRAMP_DECL(2939)  AGB_TRAMP_DECL(2940)  AGB_TRAMP_DECL(2941)  AGB_TRAMP_DECL(2942)  AGB_TRAMP_DECL(2943)
AGB_TRAMP_DECL(2944)  AGB_TRAMP_DECL(2945)  AGB_TRAMP_DECL(2946)  AGB_TRAMP_DECL(2947)  AGB_TRAMP_DECL(2948)  AGB_TRAMP_DECL(2949)  AGB_TRAMP_DECL(2950)  AGB_TRAMP_DECL(2951)
AGB_TRAMP_DECL(2952)  AGB_TRAMP_DECL(2953)  AGB_TRAMP_DECL(2954)  AGB_TRAMP_DECL(2955)  AGB_TRAMP_DECL(2956)  AGB_TRAMP_DECL(2957)  AGB_TRAMP_DECL(2958)  AGB_TRAMP_DECL(2959)
AGB_TRAMP_DECL(2960)  AGB_TRAMP_DECL(2961)  AGB_TRAMP_DECL(2962)  AGB_TRAMP_DECL(2963)  AGB_TRAMP_DECL(2964)  AGB_TRAMP_DECL(2965)  AGB_TRAMP_DECL(2966)  AGB_TRAMP_DECL(2967)
AGB_TRAMP_DECL(2968)  AGB_TRAMP_DECL(2969)  AGB_TRAMP_DECL(2970)  AGB_TRAMP_DECL(2971)  AGB_TRAMP_DECL(2972)  AGB_TRAMP_DECL(2973)  AGB_TRAMP_DECL(2974)  AGB_TRAMP_DECL(2975)
AGB_TRAMP_DECL(2976)  AGB_TRAMP_DECL(2977)  AGB_TRAMP_DECL(2978)  AGB_TRAMP_DECL(2979)  AGB_TRAMP_DECL(2980)  AGB_TRAMP_DECL(2981)  AGB_TRAMP_DECL(2982)  AGB_TRAMP_DECL(2983)
AGB_TRAMP_DECL(2984)  AGB_TRAMP_DECL(2985)  AGB_TRAMP_DECL(2986)  AGB_TRAMP_DECL(2987)  AGB_TRAMP_DECL(2988)  AGB_TRAMP_DECL(2989)  AGB_TRAMP_DECL(2990)  AGB_TRAMP_DECL(2991)
AGB_TRAMP_DECL(2992)  AGB_TRAMP_DECL(2993)  AGB_TRAMP_DECL(2994)  AGB_TRAMP_DECL(2995)  AGB_TRAMP_DECL(2996)  AGB_TRAMP_DECL(2997)  AGB_TRAMP_DECL(2998)  AGB_TRAMP_DECL(2999)
AGB_TRAMP_DECL(3000)  AGB_TRAMP_DECL(3001)  AGB_TRAMP_DECL(3002)  AGB_TRAMP_DECL(3003)  AGB_TRAMP_DECL(3004)  AGB_TRAMP_DECL(3005)  AGB_TRAMP_DECL(3006)  AGB_TRAMP_DECL(3007)
AGB_TRAMP_DECL(3008)  AGB_TRAMP_DECL(3009)  AGB_TRAMP_DECL(3010)  AGB_TRAMP_DECL(3011)  AGB_TRAMP_DECL(3012)  AGB_TRAMP_DECL(3013)  AGB_TRAMP_DECL(3014)  AGB_TRAMP_DECL(3015)
AGB_TRAMP_DECL(3016)  AGB_TRAMP_DECL(3017)  AGB_TRAMP_DECL(3018)  AGB_TRAMP_DECL(3019)  AGB_TRAMP_DECL(3020)  AGB_TRAMP_DECL(3021)  AGB_TRAMP_DECL(3022)  AGB_TRAMP_DECL(3023)
AGB_TRAMP_DECL(3024)  AGB_TRAMP_DECL(3025)  AGB_TRAMP_DECL(3026)  AGB_TRAMP_DECL(3027)  AGB_TRAMP_DECL(3028)  AGB_TRAMP_DECL(3029)  AGB_TRAMP_DECL(3030)  AGB_TRAMP_DECL(3031)
AGB_TRAMP_DECL(3032)  AGB_TRAMP_DECL(3033)  AGB_TRAMP_DECL(3034)  AGB_TRAMP_DECL(3035)  AGB_TRAMP_DECL(3036)  AGB_TRAMP_DECL(3037)  AGB_TRAMP_DECL(3038)  AGB_TRAMP_DECL(3039)
AGB_TRAMP_DECL(3040)  AGB_TRAMP_DECL(3041)  AGB_TRAMP_DECL(3042)  AGB_TRAMP_DECL(3043)  AGB_TRAMP_DECL(3044)  AGB_TRAMP_DECL(3045)  AGB_TRAMP_DECL(3046)  AGB_TRAMP_DECL(3047)
AGB_TRAMP_DECL(3048)  AGB_TRAMP_DECL(3049)  AGB_TRAMP_DECL(3050)  AGB_TRAMP_DECL(3051)  AGB_TRAMP_DECL(3052)  AGB_TRAMP_DECL(3053)  AGB_TRAMP_DECL(3054)  AGB_TRAMP_DECL(3055)
AGB_TRAMP_DECL(3056)  AGB_TRAMP_DECL(3057)  AGB_TRAMP_DECL(3058)  AGB_TRAMP_DECL(3059)  AGB_TRAMP_DECL(3060)  AGB_TRAMP_DECL(3061)  AGB_TRAMP_DECL(3062)  AGB_TRAMP_DECL(3063)
AGB_TRAMP_DECL(3064)  AGB_TRAMP_DECL(3065)  AGB_TRAMP_DECL(3066)  AGB_TRAMP_DECL(3067)  AGB_TRAMP_DECL(3068)  AGB_TRAMP_DECL(3069)  AGB_TRAMP_DECL(3070)  AGB_TRAMP_DECL(3071)
AGB_TRAMP_DECL(3072)  AGB_TRAMP_DECL(3073)  AGB_TRAMP_DECL(3074)  AGB_TRAMP_DECL(3075)  AGB_TRAMP_DECL(3076)  AGB_TRAMP_DECL(3077)  AGB_TRAMP_DECL(3078)  AGB_TRAMP_DECL(3079)
AGB_TRAMP_DECL(3080)  AGB_TRAMP_DECL(3081)  AGB_TRAMP_DECL(3082)  AGB_TRAMP_DECL(3083)  AGB_TRAMP_DECL(3084)  AGB_TRAMP_DECL(3085)  AGB_TRAMP_DECL(3086)  AGB_TRAMP_DECL(3087)
AGB_TRAMP_DECL(3088)  AGB_TRAMP_DECL(3089)  AGB_TRAMP_DECL(3090)  AGB_TRAMP_DECL(3091)  AGB_TRAMP_DECL(3092)  AGB_TRAMP_DECL(3093)  AGB_TRAMP_DECL(3094)  AGB_TRAMP_DECL(3095)
AGB_TRAMP_DECL(3096)  AGB_TRAMP_DECL(3097)  AGB_TRAMP_DECL(3098)  AGB_TRAMP_DECL(3099)  AGB_TRAMP_DECL(3100)  AGB_TRAMP_DECL(3101)  AGB_TRAMP_DECL(3102)  AGB_TRAMP_DECL(3103)
AGB_TRAMP_DECL(3104)  AGB_TRAMP_DECL(3105)  AGB_TRAMP_DECL(3106)  AGB_TRAMP_DECL(3107)  AGB_TRAMP_DECL(3108)  AGB_TRAMP_DECL(3109)  AGB_TRAMP_DECL(3110)  AGB_TRAMP_DECL(3111)
AGB_TRAMP_DECL(3112)  AGB_TRAMP_DECL(3113)  AGB_TRAMP_DECL(3114)  AGB_TRAMP_DECL(3115)  AGB_TRAMP_DECL(3116)  AGB_TRAMP_DECL(3117)  AGB_TRAMP_DECL(3118)  AGB_TRAMP_DECL(3119)
AGB_TRAMP_DECL(3120)  AGB_TRAMP_DECL(3121)  AGB_TRAMP_DECL(3122)  AGB_TRAMP_DECL(3123)  AGB_TRAMP_DECL(3124)  AGB_TRAMP_DECL(3125)  AGB_TRAMP_DECL(3126)  AGB_TRAMP_DECL(3127)
AGB_TRAMP_DECL(3128)  AGB_TRAMP_DECL(3129)  AGB_TRAMP_DECL(3130)  AGB_TRAMP_DECL(3131)  AGB_TRAMP_DECL(3132)  AGB_TRAMP_DECL(3133)  AGB_TRAMP_DECL(3134)  AGB_TRAMP_DECL(3135)
AGB_TRAMP_DECL(3136)  AGB_TRAMP_DECL(3137)  AGB_TRAMP_DECL(3138)  AGB_TRAMP_DECL(3139)  AGB_TRAMP_DECL(3140)  AGB_TRAMP_DECL(3141)  AGB_TRAMP_DECL(3142)  AGB_TRAMP_DECL(3143)
AGB_TRAMP_DECL(3144)  AGB_TRAMP_DECL(3145)  AGB_TRAMP_DECL(3146)  AGB_TRAMP_DECL(3147)  AGB_TRAMP_DECL(3148)  AGB_TRAMP_DECL(3149)  AGB_TRAMP_DECL(3150)  AGB_TRAMP_DECL(3151)
AGB_TRAMP_DECL(3152)  AGB_TRAMP_DECL(3153)  AGB_TRAMP_DECL(3154)  AGB_TRAMP_DECL(3155)  AGB_TRAMP_DECL(3156)  AGB_TRAMP_DECL(3157)  AGB_TRAMP_DECL(3158)  AGB_TRAMP_DECL(3159)
AGB_TRAMP_DECL(3160)  AGB_TRAMP_DECL(3161)  AGB_TRAMP_DECL(3162)  AGB_TRAMP_DECL(3163)  AGB_TRAMP_DECL(3164)  AGB_TRAMP_DECL(3165)  AGB_TRAMP_DECL(3166)  AGB_TRAMP_DECL(3167)
AGB_TRAMP_DECL(3168)  AGB_TRAMP_DECL(3169)  AGB_TRAMP_DECL(3170)  AGB_TRAMP_DECL(3171)  AGB_TRAMP_DECL(3172)  AGB_TRAMP_DECL(3173)  AGB_TRAMP_DECL(3174)  AGB_TRAMP_DECL(3175)
AGB_TRAMP_DECL(3176)  AGB_TRAMP_DECL(3177)  AGB_TRAMP_DECL(3178)  AGB_TRAMP_DECL(3179)  AGB_TRAMP_DECL(3180)  AGB_TRAMP_DECL(3181)  AGB_TRAMP_DECL(3182)  AGB_TRAMP_DECL(3183)
AGB_TRAMP_DECL(3184)  AGB_TRAMP_DECL(3185)  AGB_TRAMP_DECL(3186)  AGB_TRAMP_DECL(3187)  AGB_TRAMP_DECL(3188)  AGB_TRAMP_DECL(3189)  AGB_TRAMP_DECL(3190)  AGB_TRAMP_DECL(3191)
AGB_TRAMP_DECL(3192)  AGB_TRAMP_DECL(3193)  AGB_TRAMP_DECL(3194)  AGB_TRAMP_DECL(3195)  AGB_TRAMP_DECL(3196)  AGB_TRAMP_DECL(3197)  AGB_TRAMP_DECL(3198)  AGB_TRAMP_DECL(3199)
AGB_TRAMP_DECL(3200)  AGB_TRAMP_DECL(3201)  AGB_TRAMP_DECL(3202)  AGB_TRAMP_DECL(3203)  AGB_TRAMP_DECL(3204)  AGB_TRAMP_DECL(3205)  AGB_TRAMP_DECL(3206)  AGB_TRAMP_DECL(3207)
AGB_TRAMP_DECL(3208)  AGB_TRAMP_DECL(3209)  AGB_TRAMP_DECL(3210)  AGB_TRAMP_DECL(3211)  AGB_TRAMP_DECL(3212)  AGB_TRAMP_DECL(3213)  AGB_TRAMP_DECL(3214)  AGB_TRAMP_DECL(3215)
AGB_TRAMP_DECL(3216)  AGB_TRAMP_DECL(3217)  AGB_TRAMP_DECL(3218)  AGB_TRAMP_DECL(3219)  AGB_TRAMP_DECL(3220)  AGB_TRAMP_DECL(3221)  AGB_TRAMP_DECL(3222)  AGB_TRAMP_DECL(3223)
AGB_TRAMP_DECL(3224)  AGB_TRAMP_DECL(3225)  AGB_TRAMP_DECL(3226)  AGB_TRAMP_DECL(3227)  AGB_TRAMP_DECL(3228)  AGB_TRAMP_DECL(3229)  AGB_TRAMP_DECL(3230)  AGB_TRAMP_DECL(3231)
AGB_TRAMP_DECL(3232)  AGB_TRAMP_DECL(3233)  AGB_TRAMP_DECL(3234)  AGB_TRAMP_DECL(3235)  AGB_TRAMP_DECL(3236)  AGB_TRAMP_DECL(3237)  AGB_TRAMP_DECL(3238)  AGB_TRAMP_DECL(3239)
AGB_TRAMP_DECL(3240)  AGB_TRAMP_DECL(3241)  AGB_TRAMP_DECL(3242)  AGB_TRAMP_DECL(3243)  AGB_TRAMP_DECL(3244)  AGB_TRAMP_DECL(3245)  AGB_TRAMP_DECL(3246)  AGB_TRAMP_DECL(3247)
AGB_TRAMP_DECL(3248)  AGB_TRAMP_DECL(3249)  AGB_TRAMP_DECL(3250)  AGB_TRAMP_DECL(3251)  AGB_TRAMP_DECL(3252)  AGB_TRAMP_DECL(3253)  AGB_TRAMP_DECL(3254)  AGB_TRAMP_DECL(3255)
AGB_TRAMP_DECL(3256)  AGB_TRAMP_DECL(3257)  AGB_TRAMP_DECL(3258)  AGB_TRAMP_DECL(3259)  AGB_TRAMP_DECL(3260)  AGB_TRAMP_DECL(3261)  AGB_TRAMP_DECL(3262)  AGB_TRAMP_DECL(3263)
AGB_TRAMP_DECL(3264)  AGB_TRAMP_DECL(3265)  AGB_TRAMP_DECL(3266)  AGB_TRAMP_DECL(3267)  AGB_TRAMP_DECL(3268)  AGB_TRAMP_DECL(3269)  AGB_TRAMP_DECL(3270)  AGB_TRAMP_DECL(3271)
AGB_TRAMP_DECL(3272)  AGB_TRAMP_DECL(3273)  AGB_TRAMP_DECL(3274)  AGB_TRAMP_DECL(3275)  AGB_TRAMP_DECL(3276)  AGB_TRAMP_DECL(3277)  AGB_TRAMP_DECL(3278)  AGB_TRAMP_DECL(3279)
AGB_TRAMP_DECL(3280)  AGB_TRAMP_DECL(3281)  AGB_TRAMP_DECL(3282)  AGB_TRAMP_DECL(3283)  AGB_TRAMP_DECL(3284)  AGB_TRAMP_DECL(3285)  AGB_TRAMP_DECL(3286)  AGB_TRAMP_DECL(3287)
AGB_TRAMP_DECL(3288)  AGB_TRAMP_DECL(3289)  AGB_TRAMP_DECL(3290)  AGB_TRAMP_DECL(3291)  AGB_TRAMP_DECL(3292)  AGB_TRAMP_DECL(3293)  AGB_TRAMP_DECL(3294)  AGB_TRAMP_DECL(3295)
AGB_TRAMP_DECL(3296)  AGB_TRAMP_DECL(3297)  AGB_TRAMP_DECL(3298)  AGB_TRAMP_DECL(3299)  AGB_TRAMP_DECL(3300)  AGB_TRAMP_DECL(3301)  AGB_TRAMP_DECL(3302)  AGB_TRAMP_DECL(3303)
AGB_TRAMP_DECL(3304)  AGB_TRAMP_DECL(3305)  AGB_TRAMP_DECL(3306)  AGB_TRAMP_DECL(3307)  AGB_TRAMP_DECL(3308)  AGB_TRAMP_DECL(3309)  AGB_TRAMP_DECL(3310)  AGB_TRAMP_DECL(3311)
AGB_TRAMP_DECL(3312)  AGB_TRAMP_DECL(3313)  AGB_TRAMP_DECL(3314)  AGB_TRAMP_DECL(3315)  AGB_TRAMP_DECL(3316)  AGB_TRAMP_DECL(3317)  AGB_TRAMP_DECL(3318)  AGB_TRAMP_DECL(3319)
AGB_TRAMP_DECL(3320)  AGB_TRAMP_DECL(3321)  AGB_TRAMP_DECL(3322)  AGB_TRAMP_DECL(3323)  AGB_TRAMP_DECL(3324)  AGB_TRAMP_DECL(3325)  AGB_TRAMP_DECL(3326)  AGB_TRAMP_DECL(3327)
AGB_TRAMP_DECL(3328)  AGB_TRAMP_DECL(3329)  AGB_TRAMP_DECL(3330)  AGB_TRAMP_DECL(3331)  AGB_TRAMP_DECL(3332)  AGB_TRAMP_DECL(3333)  AGB_TRAMP_DECL(3334)  AGB_TRAMP_DECL(3335)
AGB_TRAMP_DECL(3336)  AGB_TRAMP_DECL(3337)  AGB_TRAMP_DECL(3338)  AGB_TRAMP_DECL(3339)  AGB_TRAMP_DECL(3340)  AGB_TRAMP_DECL(3341)  AGB_TRAMP_DECL(3342)  AGB_TRAMP_DECL(3343)
AGB_TRAMP_DECL(3344)  AGB_TRAMP_DECL(3345)  AGB_TRAMP_DECL(3346)  AGB_TRAMP_DECL(3347)  AGB_TRAMP_DECL(3348)  AGB_TRAMP_DECL(3349)  AGB_TRAMP_DECL(3350)  AGB_TRAMP_DECL(3351)
AGB_TRAMP_DECL(3352)  AGB_TRAMP_DECL(3353)  AGB_TRAMP_DECL(3354)  AGB_TRAMP_DECL(3355)  AGB_TRAMP_DECL(3356)  AGB_TRAMP_DECL(3357)  AGB_TRAMP_DECL(3358)  AGB_TRAMP_DECL(3359)
AGB_TRAMP_DECL(3360)  AGB_TRAMP_DECL(3361)  AGB_TRAMP_DECL(3362)  AGB_TRAMP_DECL(3363)  AGB_TRAMP_DECL(3364)  AGB_TRAMP_DECL(3365)  AGB_TRAMP_DECL(3366)  AGB_TRAMP_DECL(3367)
AGB_TRAMP_DECL(3368)  AGB_TRAMP_DECL(3369)  AGB_TRAMP_DECL(3370)  AGB_TRAMP_DECL(3371)  AGB_TRAMP_DECL(3372)  AGB_TRAMP_DECL(3373)  AGB_TRAMP_DECL(3374)  AGB_TRAMP_DECL(3375)
AGB_TRAMP_DECL(3376)  AGB_TRAMP_DECL(3377)  AGB_TRAMP_DECL(3378)  AGB_TRAMP_DECL(3379)  AGB_TRAMP_DECL(3380)  AGB_TRAMP_DECL(3381)  AGB_TRAMP_DECL(3382)  AGB_TRAMP_DECL(3383)
AGB_TRAMP_DECL(3384)  AGB_TRAMP_DECL(3385)  AGB_TRAMP_DECL(3386)  AGB_TRAMP_DECL(3387)  AGB_TRAMP_DECL(3388)  AGB_TRAMP_DECL(3389)  AGB_TRAMP_DECL(3390)  AGB_TRAMP_DECL(3391)
AGB_TRAMP_DECL(3392)  AGB_TRAMP_DECL(3393)  AGB_TRAMP_DECL(3394)  AGB_TRAMP_DECL(3395)  AGB_TRAMP_DECL(3396)  AGB_TRAMP_DECL(3397)  AGB_TRAMP_DECL(3398)  AGB_TRAMP_DECL(3399)
AGB_TRAMP_DECL(3400)  AGB_TRAMP_DECL(3401)  AGB_TRAMP_DECL(3402)  AGB_TRAMP_DECL(3403)  AGB_TRAMP_DECL(3404)  AGB_TRAMP_DECL(3405)  AGB_TRAMP_DECL(3406)  AGB_TRAMP_DECL(3407)
AGB_TRAMP_DECL(3408)  AGB_TRAMP_DECL(3409)  AGB_TRAMP_DECL(3410)  AGB_TRAMP_DECL(3411)  AGB_TRAMP_DECL(3412)  AGB_TRAMP_DECL(3413)  AGB_TRAMP_DECL(3414)  AGB_TRAMP_DECL(3415)
AGB_TRAMP_DECL(3416)  AGB_TRAMP_DECL(3417)  AGB_TRAMP_DECL(3418)  AGB_TRAMP_DECL(3419)  AGB_TRAMP_DECL(3420)  AGB_TRAMP_DECL(3421)  AGB_TRAMP_DECL(3422)  AGB_TRAMP_DECL(3423)
AGB_TRAMP_DECL(3424)  AGB_TRAMP_DECL(3425)  AGB_TRAMP_DECL(3426)  AGB_TRAMP_DECL(3427)  AGB_TRAMP_DECL(3428)  AGB_TRAMP_DECL(3429)  AGB_TRAMP_DECL(3430)  AGB_TRAMP_DECL(3431)
AGB_TRAMP_DECL(3432)  AGB_TRAMP_DECL(3433)  AGB_TRAMP_DECL(3434)  AGB_TRAMP_DECL(3435)  AGB_TRAMP_DECL(3436)  AGB_TRAMP_DECL(3437)  AGB_TRAMP_DECL(3438)  AGB_TRAMP_DECL(3439)
AGB_TRAMP_DECL(3440)  AGB_TRAMP_DECL(3441)  AGB_TRAMP_DECL(3442)  AGB_TRAMP_DECL(3443)  AGB_TRAMP_DECL(3444)  AGB_TRAMP_DECL(3445)  AGB_TRAMP_DECL(3446)  AGB_TRAMP_DECL(3447)
AGB_TRAMP_DECL(3448)  AGB_TRAMP_DECL(3449)  AGB_TRAMP_DECL(3450)  AGB_TRAMP_DECL(3451)  AGB_TRAMP_DECL(3452)  AGB_TRAMP_DECL(3453)  AGB_TRAMP_DECL(3454)  AGB_TRAMP_DECL(3455)
AGB_TRAMP_DECL(3456)  AGB_TRAMP_DECL(3457)  AGB_TRAMP_DECL(3458)  AGB_TRAMP_DECL(3459)  AGB_TRAMP_DECL(3460)  AGB_TRAMP_DECL(3461)  AGB_TRAMP_DECL(3462)  AGB_TRAMP_DECL(3463)
AGB_TRAMP_DECL(3464)  AGB_TRAMP_DECL(3465)  AGB_TRAMP_DECL(3466)  AGB_TRAMP_DECL(3467)  AGB_TRAMP_DECL(3468)  AGB_TRAMP_DECL(3469)  AGB_TRAMP_DECL(3470)  AGB_TRAMP_DECL(3471)
AGB_TRAMP_DECL(3472)  AGB_TRAMP_DECL(3473)  AGB_TRAMP_DECL(3474)  AGB_TRAMP_DECL(3475)  AGB_TRAMP_DECL(3476)  AGB_TRAMP_DECL(3477)  AGB_TRAMP_DECL(3478)  AGB_TRAMP_DECL(3479)
AGB_TRAMP_DECL(3480)  AGB_TRAMP_DECL(3481)  AGB_TRAMP_DECL(3482)  AGB_TRAMP_DECL(3483)  AGB_TRAMP_DECL(3484)  AGB_TRAMP_DECL(3485)  AGB_TRAMP_DECL(3486)  AGB_TRAMP_DECL(3487)
AGB_TRAMP_DECL(3488)  AGB_TRAMP_DECL(3489)  AGB_TRAMP_DECL(3490)  AGB_TRAMP_DECL(3491)  AGB_TRAMP_DECL(3492)  AGB_TRAMP_DECL(3493)  AGB_TRAMP_DECL(3494)  AGB_TRAMP_DECL(3495)
AGB_TRAMP_DECL(3496)  AGB_TRAMP_DECL(3497)  AGB_TRAMP_DECL(3498)  AGB_TRAMP_DECL(3499)  AGB_TRAMP_DECL(3500)  AGB_TRAMP_DECL(3501)  AGB_TRAMP_DECL(3502)  AGB_TRAMP_DECL(3503)
AGB_TRAMP_DECL(3504)  AGB_TRAMP_DECL(3505)  AGB_TRAMP_DECL(3506)  AGB_TRAMP_DECL(3507)  AGB_TRAMP_DECL(3508)  AGB_TRAMP_DECL(3509)  AGB_TRAMP_DECL(3510)  AGB_TRAMP_DECL(3511)
AGB_TRAMP_DECL(3512)  AGB_TRAMP_DECL(3513)  AGB_TRAMP_DECL(3514)  AGB_TRAMP_DECL(3515)  AGB_TRAMP_DECL(3516)  AGB_TRAMP_DECL(3517)  AGB_TRAMP_DECL(3518)  AGB_TRAMP_DECL(3519)
AGB_TRAMP_DECL(3520)  AGB_TRAMP_DECL(3521)  AGB_TRAMP_DECL(3522)  AGB_TRAMP_DECL(3523)  AGB_TRAMP_DECL(3524)  AGB_TRAMP_DECL(3525)  AGB_TRAMP_DECL(3526)  AGB_TRAMP_DECL(3527)
AGB_TRAMP_DECL(3528)  AGB_TRAMP_DECL(3529)  AGB_TRAMP_DECL(3530)  AGB_TRAMP_DECL(3531)  AGB_TRAMP_DECL(3532)  AGB_TRAMP_DECL(3533)  AGB_TRAMP_DECL(3534)  AGB_TRAMP_DECL(3535)
AGB_TRAMP_DECL(3536)  AGB_TRAMP_DECL(3537)  AGB_TRAMP_DECL(3538)  AGB_TRAMP_DECL(3539)  AGB_TRAMP_DECL(3540)  AGB_TRAMP_DECL(3541)  AGB_TRAMP_DECL(3542)  AGB_TRAMP_DECL(3543)
AGB_TRAMP_DECL(3544)  AGB_TRAMP_DECL(3545)  AGB_TRAMP_DECL(3546)  AGB_TRAMP_DECL(3547)  AGB_TRAMP_DECL(3548)  AGB_TRAMP_DECL(3549)  AGB_TRAMP_DECL(3550)  AGB_TRAMP_DECL(3551)
AGB_TRAMP_DECL(3552)  AGB_TRAMP_DECL(3553)  AGB_TRAMP_DECL(3554)  AGB_TRAMP_DECL(3555)  AGB_TRAMP_DECL(3556)  AGB_TRAMP_DECL(3557)  AGB_TRAMP_DECL(3558)  AGB_TRAMP_DECL(3559)
AGB_TRAMP_DECL(3560)  AGB_TRAMP_DECL(3561)  AGB_TRAMP_DECL(3562)  AGB_TRAMP_DECL(3563)  AGB_TRAMP_DECL(3564)  AGB_TRAMP_DECL(3565)  AGB_TRAMP_DECL(3566)  AGB_TRAMP_DECL(3567)
AGB_TRAMP_DECL(3568)  AGB_TRAMP_DECL(3569)  AGB_TRAMP_DECL(3570)  AGB_TRAMP_DECL(3571)  AGB_TRAMP_DECL(3572)  AGB_TRAMP_DECL(3573)  AGB_TRAMP_DECL(3574)  AGB_TRAMP_DECL(3575)
AGB_TRAMP_DECL(3576)  AGB_TRAMP_DECL(3577)  AGB_TRAMP_DECL(3578)  AGB_TRAMP_DECL(3579)  AGB_TRAMP_DECL(3580)  AGB_TRAMP_DECL(3581)  AGB_TRAMP_DECL(3582)  AGB_TRAMP_DECL(3583)
AGB_TRAMP_DECL(3584)  AGB_TRAMP_DECL(3585)  AGB_TRAMP_DECL(3586)  AGB_TRAMP_DECL(3587)  AGB_TRAMP_DECL(3588)  AGB_TRAMP_DECL(3589)  AGB_TRAMP_DECL(3590)  AGB_TRAMP_DECL(3591)
AGB_TRAMP_DECL(3592)  AGB_TRAMP_DECL(3593)  AGB_TRAMP_DECL(3594)  AGB_TRAMP_DECL(3595)  AGB_TRAMP_DECL(3596)  AGB_TRAMP_DECL(3597)  AGB_TRAMP_DECL(3598)  AGB_TRAMP_DECL(3599)
AGB_TRAMP_DECL(3600)  AGB_TRAMP_DECL(3601)  AGB_TRAMP_DECL(3602)  AGB_TRAMP_DECL(3603)  AGB_TRAMP_DECL(3604)  AGB_TRAMP_DECL(3605)  AGB_TRAMP_DECL(3606)  AGB_TRAMP_DECL(3607)
AGB_TRAMP_DECL(3608)  AGB_TRAMP_DECL(3609)  AGB_TRAMP_DECL(3610)  AGB_TRAMP_DECL(3611)  AGB_TRAMP_DECL(3612)  AGB_TRAMP_DECL(3613)  AGB_TRAMP_DECL(3614)  AGB_TRAMP_DECL(3615)
AGB_TRAMP_DECL(3616)  AGB_TRAMP_DECL(3617)  AGB_TRAMP_DECL(3618)  AGB_TRAMP_DECL(3619)  AGB_TRAMP_DECL(3620)  AGB_TRAMP_DECL(3621)  AGB_TRAMP_DECL(3622)  AGB_TRAMP_DECL(3623)
AGB_TRAMP_DECL(3624)  AGB_TRAMP_DECL(3625)  AGB_TRAMP_DECL(3626)  AGB_TRAMP_DECL(3627)  AGB_TRAMP_DECL(3628)  AGB_TRAMP_DECL(3629)  AGB_TRAMP_DECL(3630)  AGB_TRAMP_DECL(3631)
AGB_TRAMP_DECL(3632)  AGB_TRAMP_DECL(3633)  AGB_TRAMP_DECL(3634)  AGB_TRAMP_DECL(3635)  AGB_TRAMP_DECL(3636)  AGB_TRAMP_DECL(3637)  AGB_TRAMP_DECL(3638)  AGB_TRAMP_DECL(3639)
AGB_TRAMP_DECL(3640)  AGB_TRAMP_DECL(3641)  AGB_TRAMP_DECL(3642)  AGB_TRAMP_DECL(3643)  AGB_TRAMP_DECL(3644)  AGB_TRAMP_DECL(3645)  AGB_TRAMP_DECL(3646)  AGB_TRAMP_DECL(3647)
AGB_TRAMP_DECL(3648)  AGB_TRAMP_DECL(3649)  AGB_TRAMP_DECL(3650)  AGB_TRAMP_DECL(3651)  AGB_TRAMP_DECL(3652)  AGB_TRAMP_DECL(3653)  AGB_TRAMP_DECL(3654)  AGB_TRAMP_DECL(3655)
AGB_TRAMP_DECL(3656)  AGB_TRAMP_DECL(3657)  AGB_TRAMP_DECL(3658)  AGB_TRAMP_DECL(3659)  AGB_TRAMP_DECL(3660)  AGB_TRAMP_DECL(3661)  AGB_TRAMP_DECL(3662)  AGB_TRAMP_DECL(3663)
AGB_TRAMP_DECL(3664)  AGB_TRAMP_DECL(3665)  AGB_TRAMP_DECL(3666)  AGB_TRAMP_DECL(3667)  AGB_TRAMP_DECL(3668)  AGB_TRAMP_DECL(3669)  AGB_TRAMP_DECL(3670)  AGB_TRAMP_DECL(3671)
AGB_TRAMP_DECL(3672)  AGB_TRAMP_DECL(3673)  AGB_TRAMP_DECL(3674)  AGB_TRAMP_DECL(3675)  AGB_TRAMP_DECL(3676)  AGB_TRAMP_DECL(3677)  AGB_TRAMP_DECL(3678)  AGB_TRAMP_DECL(3679)
AGB_TRAMP_DECL(3680)  AGB_TRAMP_DECL(3681)  AGB_TRAMP_DECL(3682)  AGB_TRAMP_DECL(3683)  AGB_TRAMP_DECL(3684)  AGB_TRAMP_DECL(3685)  AGB_TRAMP_DECL(3686)  AGB_TRAMP_DECL(3687)
AGB_TRAMP_DECL(3688)  AGB_TRAMP_DECL(3689)  AGB_TRAMP_DECL(3690)  AGB_TRAMP_DECL(3691)  AGB_TRAMP_DECL(3692)  AGB_TRAMP_DECL(3693)  AGB_TRAMP_DECL(3694)  AGB_TRAMP_DECL(3695)
AGB_TRAMP_DECL(3696)  AGB_TRAMP_DECL(3697)  AGB_TRAMP_DECL(3698)  AGB_TRAMP_DECL(3699)  AGB_TRAMP_DECL(3700)  AGB_TRAMP_DECL(3701)  AGB_TRAMP_DECL(3702)  AGB_TRAMP_DECL(3703)
AGB_TRAMP_DECL(3704)  AGB_TRAMP_DECL(3705)  AGB_TRAMP_DECL(3706)  AGB_TRAMP_DECL(3707)  AGB_TRAMP_DECL(3708)  AGB_TRAMP_DECL(3709)  AGB_TRAMP_DECL(3710)  AGB_TRAMP_DECL(3711)
AGB_TRAMP_DECL(3712)  AGB_TRAMP_DECL(3713)  AGB_TRAMP_DECL(3714)  AGB_TRAMP_DECL(3715)  AGB_TRAMP_DECL(3716)  AGB_TRAMP_DECL(3717)  AGB_TRAMP_DECL(3718)  AGB_TRAMP_DECL(3719)
AGB_TRAMP_DECL(3720)  AGB_TRAMP_DECL(3721)  AGB_TRAMP_DECL(3722)  AGB_TRAMP_DECL(3723)  AGB_TRAMP_DECL(3724)  AGB_TRAMP_DECL(3725)  AGB_TRAMP_DECL(3726)  AGB_TRAMP_DECL(3727)
AGB_TRAMP_DECL(3728)  AGB_TRAMP_DECL(3729)  AGB_TRAMP_DECL(3730)  AGB_TRAMP_DECL(3731)  AGB_TRAMP_DECL(3732)  AGB_TRAMP_DECL(3733)  AGB_TRAMP_DECL(3734)  AGB_TRAMP_DECL(3735)
AGB_TRAMP_DECL(3736)  AGB_TRAMP_DECL(3737)  AGB_TRAMP_DECL(3738)  AGB_TRAMP_DECL(3739)  AGB_TRAMP_DECL(3740)  AGB_TRAMP_DECL(3741)  AGB_TRAMP_DECL(3742)  AGB_TRAMP_DECL(3743)
AGB_TRAMP_DECL(3744)  AGB_TRAMP_DECL(3745)  AGB_TRAMP_DECL(3746)  AGB_TRAMP_DECL(3747)  AGB_TRAMP_DECL(3748)  AGB_TRAMP_DECL(3749)  AGB_TRAMP_DECL(3750)  AGB_TRAMP_DECL(3751)
AGB_TRAMP_DECL(3752)  AGB_TRAMP_DECL(3753)  AGB_TRAMP_DECL(3754)  AGB_TRAMP_DECL(3755)  AGB_TRAMP_DECL(3756)  AGB_TRAMP_DECL(3757)  AGB_TRAMP_DECL(3758)  AGB_TRAMP_DECL(3759)
AGB_TRAMP_DECL(3760)  AGB_TRAMP_DECL(3761)  AGB_TRAMP_DECL(3762)  AGB_TRAMP_DECL(3763)  AGB_TRAMP_DECL(3764)  AGB_TRAMP_DECL(3765)  AGB_TRAMP_DECL(3766)  AGB_TRAMP_DECL(3767)
AGB_TRAMP_DECL(3768)  AGB_TRAMP_DECL(3769)  AGB_TRAMP_DECL(3770)  AGB_TRAMP_DECL(3771)  AGB_TRAMP_DECL(3772)  AGB_TRAMP_DECL(3773)  AGB_TRAMP_DECL(3774)  AGB_TRAMP_DECL(3775)
AGB_TRAMP_DECL(3776)  AGB_TRAMP_DECL(3777)  AGB_TRAMP_DECL(3778)  AGB_TRAMP_DECL(3779)  AGB_TRAMP_DECL(3780)  AGB_TRAMP_DECL(3781)  AGB_TRAMP_DECL(3782)  AGB_TRAMP_DECL(3783)
AGB_TRAMP_DECL(3784)  AGB_TRAMP_DECL(3785)  AGB_TRAMP_DECL(3786)  AGB_TRAMP_DECL(3787)  AGB_TRAMP_DECL(3788)  AGB_TRAMP_DECL(3789)  AGB_TRAMP_DECL(3790)  AGB_TRAMP_DECL(3791)
AGB_TRAMP_DECL(3792)  AGB_TRAMP_DECL(3793)  AGB_TRAMP_DECL(3794)  AGB_TRAMP_DECL(3795)  AGB_TRAMP_DECL(3796)  AGB_TRAMP_DECL(3797)  AGB_TRAMP_DECL(3798)  AGB_TRAMP_DECL(3799)
AGB_TRAMP_DECL(3800)  AGB_TRAMP_DECL(3801)  AGB_TRAMP_DECL(3802)  AGB_TRAMP_DECL(3803)  AGB_TRAMP_DECL(3804)  AGB_TRAMP_DECL(3805)  AGB_TRAMP_DECL(3806)  AGB_TRAMP_DECL(3807)
AGB_TRAMP_DECL(3808)  AGB_TRAMP_DECL(3809)  AGB_TRAMP_DECL(3810)  AGB_TRAMP_DECL(3811)  AGB_TRAMP_DECL(3812)  AGB_TRAMP_DECL(3813)  AGB_TRAMP_DECL(3814)  AGB_TRAMP_DECL(3815)
AGB_TRAMP_DECL(3816)  AGB_TRAMP_DECL(3817)  AGB_TRAMP_DECL(3818)  AGB_TRAMP_DECL(3819)  AGB_TRAMP_DECL(3820)  AGB_TRAMP_DECL(3821)  AGB_TRAMP_DECL(3822)  AGB_TRAMP_DECL(3823)
AGB_TRAMP_DECL(3824)  AGB_TRAMP_DECL(3825)  AGB_TRAMP_DECL(3826)  AGB_TRAMP_DECL(3827)  AGB_TRAMP_DECL(3828)  AGB_TRAMP_DECL(3829)  AGB_TRAMP_DECL(3830)  AGB_TRAMP_DECL(3831)
AGB_TRAMP_DECL(3832)  AGB_TRAMP_DECL(3833)  AGB_TRAMP_DECL(3834)  AGB_TRAMP_DECL(3835)  AGB_TRAMP_DECL(3836)  AGB_TRAMP_DECL(3837)  AGB_TRAMP_DECL(3838)  AGB_TRAMP_DECL(3839)
AGB_TRAMP_DECL(3840)  AGB_TRAMP_DECL(3841)  AGB_TRAMP_DECL(3842)  AGB_TRAMP_DECL(3843)  AGB_TRAMP_DECL(3844)  AGB_TRAMP_DECL(3845)  AGB_TRAMP_DECL(3846)  AGB_TRAMP_DECL(3847)
AGB_TRAMP_DECL(3848)  AGB_TRAMP_DECL(3849)  AGB_TRAMP_DECL(3850)  AGB_TRAMP_DECL(3851)  AGB_TRAMP_DECL(3852)  AGB_TRAMP_DECL(3853)  AGB_TRAMP_DECL(3854)  AGB_TRAMP_DECL(3855)
AGB_TRAMP_DECL(3856)  AGB_TRAMP_DECL(3857)  AGB_TRAMP_DECL(3858)  AGB_TRAMP_DECL(3859)  AGB_TRAMP_DECL(3860)  AGB_TRAMP_DECL(3861)  AGB_TRAMP_DECL(3862)  AGB_TRAMP_DECL(3863)
AGB_TRAMP_DECL(3864)  AGB_TRAMP_DECL(3865)  AGB_TRAMP_DECL(3866)  AGB_TRAMP_DECL(3867)  AGB_TRAMP_DECL(3868)  AGB_TRAMP_DECL(3869)  AGB_TRAMP_DECL(3870)  AGB_TRAMP_DECL(3871)
AGB_TRAMP_DECL(3872)  AGB_TRAMP_DECL(3873)  AGB_TRAMP_DECL(3874)  AGB_TRAMP_DECL(3875)  AGB_TRAMP_DECL(3876)  AGB_TRAMP_DECL(3877)  AGB_TRAMP_DECL(3878)  AGB_TRAMP_DECL(3879)
AGB_TRAMP_DECL(3880)  AGB_TRAMP_DECL(3881)  AGB_TRAMP_DECL(3882)  AGB_TRAMP_DECL(3883)  AGB_TRAMP_DECL(3884)  AGB_TRAMP_DECL(3885)  AGB_TRAMP_DECL(3886)  AGB_TRAMP_DECL(3887)
AGB_TRAMP_DECL(3888)  AGB_TRAMP_DECL(3889)  AGB_TRAMP_DECL(3890)  AGB_TRAMP_DECL(3891)  AGB_TRAMP_DECL(3892)  AGB_TRAMP_DECL(3893)  AGB_TRAMP_DECL(3894)  AGB_TRAMP_DECL(3895)
AGB_TRAMP_DECL(3896)  AGB_TRAMP_DECL(3897)  AGB_TRAMP_DECL(3898)  AGB_TRAMP_DECL(3899)  AGB_TRAMP_DECL(3900)  AGB_TRAMP_DECL(3901)  AGB_TRAMP_DECL(3902)  AGB_TRAMP_DECL(3903)
AGB_TRAMP_DECL(3904)  AGB_TRAMP_DECL(3905)  AGB_TRAMP_DECL(3906)  AGB_TRAMP_DECL(3907)  AGB_TRAMP_DECL(3908)  AGB_TRAMP_DECL(3909)  AGB_TRAMP_DECL(3910)  AGB_TRAMP_DECL(3911)
AGB_TRAMP_DECL(3912)  AGB_TRAMP_DECL(3913)  AGB_TRAMP_DECL(3914)  AGB_TRAMP_DECL(3915)  AGB_TRAMP_DECL(3916)  AGB_TRAMP_DECL(3917)  AGB_TRAMP_DECL(3918)  AGB_TRAMP_DECL(3919)
AGB_TRAMP_DECL(3920)  AGB_TRAMP_DECL(3921)  AGB_TRAMP_DECL(3922)  AGB_TRAMP_DECL(3923)  AGB_TRAMP_DECL(3924)  AGB_TRAMP_DECL(3925)  AGB_TRAMP_DECL(3926)  AGB_TRAMP_DECL(3927)
AGB_TRAMP_DECL(3928)  AGB_TRAMP_DECL(3929)  AGB_TRAMP_DECL(3930)  AGB_TRAMP_DECL(3931)  AGB_TRAMP_DECL(3932)  AGB_TRAMP_DECL(3933)  AGB_TRAMP_DECL(3934)  AGB_TRAMP_DECL(3935)
AGB_TRAMP_DECL(3936)  AGB_TRAMP_DECL(3937)  AGB_TRAMP_DECL(3938)  AGB_TRAMP_DECL(3939)  AGB_TRAMP_DECL(3940)  AGB_TRAMP_DECL(3941)  AGB_TRAMP_DECL(3942)  AGB_TRAMP_DECL(3943)
AGB_TRAMP_DECL(3944)  AGB_TRAMP_DECL(3945)  AGB_TRAMP_DECL(3946)  AGB_TRAMP_DECL(3947)  AGB_TRAMP_DECL(3948)  AGB_TRAMP_DECL(3949)  AGB_TRAMP_DECL(3950)  AGB_TRAMP_DECL(3951)
AGB_TRAMP_DECL(3952)  AGB_TRAMP_DECL(3953)  AGB_TRAMP_DECL(3954)  AGB_TRAMP_DECL(3955)  AGB_TRAMP_DECL(3956)  AGB_TRAMP_DECL(3957)  AGB_TRAMP_DECL(3958)  AGB_TRAMP_DECL(3959)
AGB_TRAMP_DECL(3960)  AGB_TRAMP_DECL(3961)  AGB_TRAMP_DECL(3962)  AGB_TRAMP_DECL(3963)  AGB_TRAMP_DECL(3964)  AGB_TRAMP_DECL(3965)  AGB_TRAMP_DECL(3966)  AGB_TRAMP_DECL(3967)
AGB_TRAMP_DECL(3968)  AGB_TRAMP_DECL(3969)  AGB_TRAMP_DECL(3970)  AGB_TRAMP_DECL(3971)  AGB_TRAMP_DECL(3972)  AGB_TRAMP_DECL(3973)  AGB_TRAMP_DECL(3974)  AGB_TRAMP_DECL(3975)
AGB_TRAMP_DECL(3976)  AGB_TRAMP_DECL(3977)  AGB_TRAMP_DECL(3978)  AGB_TRAMP_DECL(3979)  AGB_TRAMP_DECL(3980)  AGB_TRAMP_DECL(3981)  AGB_TRAMP_DECL(3982)  AGB_TRAMP_DECL(3983)
AGB_TRAMP_DECL(3984)  AGB_TRAMP_DECL(3985)  AGB_TRAMP_DECL(3986)  AGB_TRAMP_DECL(3987)  AGB_TRAMP_DECL(3988)  AGB_TRAMP_DECL(3989)  AGB_TRAMP_DECL(3990)  AGB_TRAMP_DECL(3991)
AGB_TRAMP_DECL(3992)  AGB_TRAMP_DECL(3993)  AGB_TRAMP_DECL(3994)  AGB_TRAMP_DECL(3995)  AGB_TRAMP_DECL(3996)  AGB_TRAMP_DECL(3997)  AGB_TRAMP_DECL(3998)  AGB_TRAMP_DECL(3999)
AGB_TRAMP_DECL(4000)  AGB_TRAMP_DECL(4001)  AGB_TRAMP_DECL(4002)  AGB_TRAMP_DECL(4003)  AGB_TRAMP_DECL(4004)  AGB_TRAMP_DECL(4005)  AGB_TRAMP_DECL(4006)  AGB_TRAMP_DECL(4007)
AGB_TRAMP_DECL(4008)  AGB_TRAMP_DECL(4009)  AGB_TRAMP_DECL(4010)  AGB_TRAMP_DECL(4011)  AGB_TRAMP_DECL(4012)  AGB_TRAMP_DECL(4013)  AGB_TRAMP_DECL(4014)  AGB_TRAMP_DECL(4015)
AGB_TRAMP_DECL(4016)  AGB_TRAMP_DECL(4017)  AGB_TRAMP_DECL(4018)  AGB_TRAMP_DECL(4019)  AGB_TRAMP_DECL(4020)  AGB_TRAMP_DECL(4021)  AGB_TRAMP_DECL(4022)  AGB_TRAMP_DECL(4023)
AGB_TRAMP_DECL(4024)  AGB_TRAMP_DECL(4025)  AGB_TRAMP_DECL(4026)  AGB_TRAMP_DECL(4027)  AGB_TRAMP_DECL(4028)  AGB_TRAMP_DECL(4029)  AGB_TRAMP_DECL(4030)  AGB_TRAMP_DECL(4031)
AGB_TRAMP_DECL(4032)  AGB_TRAMP_DECL(4033)  AGB_TRAMP_DECL(4034)  AGB_TRAMP_DECL(4035)  AGB_TRAMP_DECL(4036)  AGB_TRAMP_DECL(4037)  AGB_TRAMP_DECL(4038)  AGB_TRAMP_DECL(4039)
AGB_TRAMP_DECL(4040)  AGB_TRAMP_DECL(4041)  AGB_TRAMP_DECL(4042)  AGB_TRAMP_DECL(4043)  AGB_TRAMP_DECL(4044)  AGB_TRAMP_DECL(4045)  AGB_TRAMP_DECL(4046)  AGB_TRAMP_DECL(4047)
AGB_TRAMP_DECL(4048)  AGB_TRAMP_DECL(4049)  AGB_TRAMP_DECL(4050)  AGB_TRAMP_DECL(4051)  AGB_TRAMP_DECL(4052)  AGB_TRAMP_DECL(4053)  AGB_TRAMP_DECL(4054)  AGB_TRAMP_DECL(4055)
AGB_TRAMP_DECL(4056)  AGB_TRAMP_DECL(4057)  AGB_TRAMP_DECL(4058)  AGB_TRAMP_DECL(4059)  AGB_TRAMP_DECL(4060)  AGB_TRAMP_DECL(4061)  AGB_TRAMP_DECL(4062)  AGB_TRAMP_DECL(4063)
AGB_TRAMP_DECL(4064)  AGB_TRAMP_DECL(4065)  AGB_TRAMP_DECL(4066)  AGB_TRAMP_DECL(4067)  AGB_TRAMP_DECL(4068)  AGB_TRAMP_DECL(4069)  AGB_TRAMP_DECL(4070)  AGB_TRAMP_DECL(4071)
AGB_TRAMP_DECL(4072)  AGB_TRAMP_DECL(4073)  AGB_TRAMP_DECL(4074)  AGB_TRAMP_DECL(4075)  AGB_TRAMP_DECL(4076)  AGB_TRAMP_DECL(4077)  AGB_TRAMP_DECL(4078)  AGB_TRAMP_DECL(4079)
AGB_TRAMP_DECL(4080)  AGB_TRAMP_DECL(4081)  AGB_TRAMP_DECL(4082)  AGB_TRAMP_DECL(4083)  AGB_TRAMP_DECL(4084)  AGB_TRAMP_DECL(4085)  AGB_TRAMP_DECL(4086)  AGB_TRAMP_DECL(4087)
AGB_TRAMP_DECL(4088)  AGB_TRAMP_DECL(4089)  AGB_TRAMP_DECL(4090)  AGB_TRAMP_DECL(4091)  AGB_TRAMP_DECL(4092)  AGB_TRAMP_DECL(4093)  AGB_TRAMP_DECL(4094)  AGB_TRAMP_DECL(4095)

typedef ant_value_t (*agb_cfunc)(ant_t*, ant_value_t*, int, ant_value_t);

static agb_cfunc const g_tramps[AGB_TRAMPOLINE_COUNT] = {
    agb_tramp_0,  agb_tramp_1,  agb_tramp_2,  agb_tramp_3,
    agb_tramp_4,  agb_tramp_5,  agb_tramp_6,  agb_tramp_7,
    agb_tramp_8,  agb_tramp_9,  agb_tramp_10,  agb_tramp_11,
    agb_tramp_12,  agb_tramp_13,  agb_tramp_14,  agb_tramp_15,
    agb_tramp_16,  agb_tramp_17,  agb_tramp_18,  agb_tramp_19,
    agb_tramp_20,  agb_tramp_21,  agb_tramp_22,  agb_tramp_23,
    agb_tramp_24,  agb_tramp_25,  agb_tramp_26,  agb_tramp_27,
    agb_tramp_28,  agb_tramp_29,  agb_tramp_30,  agb_tramp_31,
    agb_tramp_32,  agb_tramp_33,  agb_tramp_34,  agb_tramp_35,
    agb_tramp_36,  agb_tramp_37,  agb_tramp_38,  agb_tramp_39,
    agb_tramp_40,  agb_tramp_41,  agb_tramp_42,  agb_tramp_43,
    agb_tramp_44,  agb_tramp_45,  agb_tramp_46,  agb_tramp_47,
    agb_tramp_48,  agb_tramp_49,  agb_tramp_50,  agb_tramp_51,
    agb_tramp_52,  agb_tramp_53,  agb_tramp_54,  agb_tramp_55,
    agb_tramp_56,  agb_tramp_57,  agb_tramp_58,  agb_tramp_59,
    agb_tramp_60,  agb_tramp_61,  agb_tramp_62,  agb_tramp_63,
    agb_tramp_64,  agb_tramp_65,  agb_tramp_66,  agb_tramp_67,
    agb_tramp_68,  agb_tramp_69,  agb_tramp_70,  agb_tramp_71,
    agb_tramp_72,  agb_tramp_73,  agb_tramp_74,  agb_tramp_75,
    agb_tramp_76,  agb_tramp_77,  agb_tramp_78,  agb_tramp_79,
    agb_tramp_80,  agb_tramp_81,  agb_tramp_82,  agb_tramp_83,
    agb_tramp_84,  agb_tramp_85,  agb_tramp_86,  agb_tramp_87,
    agb_tramp_88,  agb_tramp_89,  agb_tramp_90,  agb_tramp_91,
    agb_tramp_92,  agb_tramp_93,  agb_tramp_94,  agb_tramp_95,
    agb_tramp_96,  agb_tramp_97,  agb_tramp_98,  agb_tramp_99,
    agb_tramp_100,  agb_tramp_101,  agb_tramp_102,  agb_tramp_103,
    agb_tramp_104,  agb_tramp_105,  agb_tramp_106,  agb_tramp_107,
    agb_tramp_108,  agb_tramp_109,  agb_tramp_110,  agb_tramp_111,
    agb_tramp_112,  agb_tramp_113,  agb_tramp_114,  agb_tramp_115,
    agb_tramp_116,  agb_tramp_117,  agb_tramp_118,  agb_tramp_119,
    agb_tramp_120,  agb_tramp_121,  agb_tramp_122,  agb_tramp_123,
    agb_tramp_124,  agb_tramp_125,  agb_tramp_126,  agb_tramp_127,
    agb_tramp_128,  agb_tramp_129,  agb_tramp_130,  agb_tramp_131,
    agb_tramp_132,  agb_tramp_133,  agb_tramp_134,  agb_tramp_135,
    agb_tramp_136,  agb_tramp_137,  agb_tramp_138,  agb_tramp_139,
    agb_tramp_140,  agb_tramp_141,  agb_tramp_142,  agb_tramp_143,
    agb_tramp_144,  agb_tramp_145,  agb_tramp_146,  agb_tramp_147,
    agb_tramp_148,  agb_tramp_149,  agb_tramp_150,  agb_tramp_151,
    agb_tramp_152,  agb_tramp_153,  agb_tramp_154,  agb_tramp_155,
    agb_tramp_156,  agb_tramp_157,  agb_tramp_158,  agb_tramp_159,
    agb_tramp_160,  agb_tramp_161,  agb_tramp_162,  agb_tramp_163,
    agb_tramp_164,  agb_tramp_165,  agb_tramp_166,  agb_tramp_167,
    agb_tramp_168,  agb_tramp_169,  agb_tramp_170,  agb_tramp_171,
    agb_tramp_172,  agb_tramp_173,  agb_tramp_174,  agb_tramp_175,
    agb_tramp_176,  agb_tramp_177,  agb_tramp_178,  agb_tramp_179,
    agb_tramp_180,  agb_tramp_181,  agb_tramp_182,  agb_tramp_183,
    agb_tramp_184,  agb_tramp_185,  agb_tramp_186,  agb_tramp_187,
    agb_tramp_188,  agb_tramp_189,  agb_tramp_190,  agb_tramp_191,
    agb_tramp_192,  agb_tramp_193,  agb_tramp_194,  agb_tramp_195,
    agb_tramp_196,  agb_tramp_197,  agb_tramp_198,  agb_tramp_199,
    agb_tramp_200,  agb_tramp_201,  agb_tramp_202,  agb_tramp_203,
    agb_tramp_204,  agb_tramp_205,  agb_tramp_206,  agb_tramp_207,
    agb_tramp_208,  agb_tramp_209,  agb_tramp_210,  agb_tramp_211,
    agb_tramp_212,  agb_tramp_213,  agb_tramp_214,  agb_tramp_215,
    agb_tramp_216,  agb_tramp_217,  agb_tramp_218,  agb_tramp_219,
    agb_tramp_220,  agb_tramp_221,  agb_tramp_222,  agb_tramp_223,
    agb_tramp_224,  agb_tramp_225,  agb_tramp_226,  agb_tramp_227,
    agb_tramp_228,  agb_tramp_229,  agb_tramp_230,  agb_tramp_231,
    agb_tramp_232,  agb_tramp_233,  agb_tramp_234,  agb_tramp_235,
    agb_tramp_236,  agb_tramp_237,  agb_tramp_238,  agb_tramp_239,
    agb_tramp_240,  agb_tramp_241,  agb_tramp_242,  agb_tramp_243,
    agb_tramp_244,  agb_tramp_245,  agb_tramp_246,  agb_tramp_247,
    agb_tramp_248,  agb_tramp_249,  agb_tramp_250,  agb_tramp_251,
    agb_tramp_252,  agb_tramp_253,  agb_tramp_254,  agb_tramp_255,
    agb_tramp_256,  agb_tramp_257,  agb_tramp_258,  agb_tramp_259,
    agb_tramp_260,  agb_tramp_261,  agb_tramp_262,  agb_tramp_263,
    agb_tramp_264,  agb_tramp_265,  agb_tramp_266,  agb_tramp_267,
    agb_tramp_268,  agb_tramp_269,  agb_tramp_270,  agb_tramp_271,
    agb_tramp_272,  agb_tramp_273,  agb_tramp_274,  agb_tramp_275,
    agb_tramp_276,  agb_tramp_277,  agb_tramp_278,  agb_tramp_279,
    agb_tramp_280,  agb_tramp_281,  agb_tramp_282,  agb_tramp_283,
    agb_tramp_284,  agb_tramp_285,  agb_tramp_286,  agb_tramp_287,
    agb_tramp_288,  agb_tramp_289,  agb_tramp_290,  agb_tramp_291,
    agb_tramp_292,  agb_tramp_293,  agb_tramp_294,  agb_tramp_295,
    agb_tramp_296,  agb_tramp_297,  agb_tramp_298,  agb_tramp_299,
    agb_tramp_300,  agb_tramp_301,  agb_tramp_302,  agb_tramp_303,
    agb_tramp_304,  agb_tramp_305,  agb_tramp_306,  agb_tramp_307,
    agb_tramp_308,  agb_tramp_309,  agb_tramp_310,  agb_tramp_311,
    agb_tramp_312,  agb_tramp_313,  agb_tramp_314,  agb_tramp_315,
    agb_tramp_316,  agb_tramp_317,  agb_tramp_318,  agb_tramp_319,
    agb_tramp_320,  agb_tramp_321,  agb_tramp_322,  agb_tramp_323,
    agb_tramp_324,  agb_tramp_325,  agb_tramp_326,  agb_tramp_327,
    agb_tramp_328,  agb_tramp_329,  agb_tramp_330,  agb_tramp_331,
    agb_tramp_332,  agb_tramp_333,  agb_tramp_334,  agb_tramp_335,
    agb_tramp_336,  agb_tramp_337,  agb_tramp_338,  agb_tramp_339,
    agb_tramp_340,  agb_tramp_341,  agb_tramp_342,  agb_tramp_343,
    agb_tramp_344,  agb_tramp_345,  agb_tramp_346,  agb_tramp_347,
    agb_tramp_348,  agb_tramp_349,  agb_tramp_350,  agb_tramp_351,
    agb_tramp_352,  agb_tramp_353,  agb_tramp_354,  agb_tramp_355,
    agb_tramp_356,  agb_tramp_357,  agb_tramp_358,  agb_tramp_359,
    agb_tramp_360,  agb_tramp_361,  agb_tramp_362,  agb_tramp_363,
    agb_tramp_364,  agb_tramp_365,  agb_tramp_366,  agb_tramp_367,
    agb_tramp_368,  agb_tramp_369,  agb_tramp_370,  agb_tramp_371,
    agb_tramp_372,  agb_tramp_373,  agb_tramp_374,  agb_tramp_375,
    agb_tramp_376,  agb_tramp_377,  agb_tramp_378,  agb_tramp_379,
    agb_tramp_380,  agb_tramp_381,  agb_tramp_382,  agb_tramp_383,
    agb_tramp_384,  agb_tramp_385,  agb_tramp_386,  agb_tramp_387,
    agb_tramp_388,  agb_tramp_389,  agb_tramp_390,  agb_tramp_391,
    agb_tramp_392,  agb_tramp_393,  agb_tramp_394,  agb_tramp_395,
    agb_tramp_396,  agb_tramp_397,  agb_tramp_398,  agb_tramp_399,
    agb_tramp_400,  agb_tramp_401,  agb_tramp_402,  agb_tramp_403,
    agb_tramp_404,  agb_tramp_405,  agb_tramp_406,  agb_tramp_407,
    agb_tramp_408,  agb_tramp_409,  agb_tramp_410,  agb_tramp_411,
    agb_tramp_412,  agb_tramp_413,  agb_tramp_414,  agb_tramp_415,
    agb_tramp_416,  agb_tramp_417,  agb_tramp_418,  agb_tramp_419,
    agb_tramp_420,  agb_tramp_421,  agb_tramp_422,  agb_tramp_423,
    agb_tramp_424,  agb_tramp_425,  agb_tramp_426,  agb_tramp_427,
    agb_tramp_428,  agb_tramp_429,  agb_tramp_430,  agb_tramp_431,
    agb_tramp_432,  agb_tramp_433,  agb_tramp_434,  agb_tramp_435,
    agb_tramp_436,  agb_tramp_437,  agb_tramp_438,  agb_tramp_439,
    agb_tramp_440,  agb_tramp_441,  agb_tramp_442,  agb_tramp_443,
    agb_tramp_444,  agb_tramp_445,  agb_tramp_446,  agb_tramp_447,
    agb_tramp_448,  agb_tramp_449,  agb_tramp_450,  agb_tramp_451,
    agb_tramp_452,  agb_tramp_453,  agb_tramp_454,  agb_tramp_455,
    agb_tramp_456,  agb_tramp_457,  agb_tramp_458,  agb_tramp_459,
    agb_tramp_460,  agb_tramp_461,  agb_tramp_462,  agb_tramp_463,
    agb_tramp_464,  agb_tramp_465,  agb_tramp_466,  agb_tramp_467,
    agb_tramp_468,  agb_tramp_469,  agb_tramp_470,  agb_tramp_471,
    agb_tramp_472,  agb_tramp_473,  agb_tramp_474,  agb_tramp_475,
    agb_tramp_476,  agb_tramp_477,  agb_tramp_478,  agb_tramp_479,
    agb_tramp_480,  agb_tramp_481,  agb_tramp_482,  agb_tramp_483,
    agb_tramp_484,  agb_tramp_485,  agb_tramp_486,  agb_tramp_487,
    agb_tramp_488,  agb_tramp_489,  agb_tramp_490,  agb_tramp_491,
    agb_tramp_492,  agb_tramp_493,  agb_tramp_494,  agb_tramp_495,
    agb_tramp_496,  agb_tramp_497,  agb_tramp_498,  agb_tramp_499,
    agb_tramp_500,  agb_tramp_501,  agb_tramp_502,  agb_tramp_503,
    agb_tramp_504,  agb_tramp_505,  agb_tramp_506,  agb_tramp_507,
    agb_tramp_508,  agb_tramp_509,  agb_tramp_510,  agb_tramp_511,
    agb_tramp_512,  agb_tramp_513,  agb_tramp_514,  agb_tramp_515,
    agb_tramp_516,  agb_tramp_517,  agb_tramp_518,  agb_tramp_519,
    agb_tramp_520,  agb_tramp_521,  agb_tramp_522,  agb_tramp_523,
    agb_tramp_524,  agb_tramp_525,  agb_tramp_526,  agb_tramp_527,
    agb_tramp_528,  agb_tramp_529,  agb_tramp_530,  agb_tramp_531,
    agb_tramp_532,  agb_tramp_533,  agb_tramp_534,  agb_tramp_535,
    agb_tramp_536,  agb_tramp_537,  agb_tramp_538,  agb_tramp_539,
    agb_tramp_540,  agb_tramp_541,  agb_tramp_542,  agb_tramp_543,
    agb_tramp_544,  agb_tramp_545,  agb_tramp_546,  agb_tramp_547,
    agb_tramp_548,  agb_tramp_549,  agb_tramp_550,  agb_tramp_551,
    agb_tramp_552,  agb_tramp_553,  agb_tramp_554,  agb_tramp_555,
    agb_tramp_556,  agb_tramp_557,  agb_tramp_558,  agb_tramp_559,
    agb_tramp_560,  agb_tramp_561,  agb_tramp_562,  agb_tramp_563,
    agb_tramp_564,  agb_tramp_565,  agb_tramp_566,  agb_tramp_567,
    agb_tramp_568,  agb_tramp_569,  agb_tramp_570,  agb_tramp_571,
    agb_tramp_572,  agb_tramp_573,  agb_tramp_574,  agb_tramp_575,
    agb_tramp_576,  agb_tramp_577,  agb_tramp_578,  agb_tramp_579,
    agb_tramp_580,  agb_tramp_581,  agb_tramp_582,  agb_tramp_583,
    agb_tramp_584,  agb_tramp_585,  agb_tramp_586,  agb_tramp_587,
    agb_tramp_588,  agb_tramp_589,  agb_tramp_590,  agb_tramp_591,
    agb_tramp_592,  agb_tramp_593,  agb_tramp_594,  agb_tramp_595,
    agb_tramp_596,  agb_tramp_597,  agb_tramp_598,  agb_tramp_599,
    agb_tramp_600,  agb_tramp_601,  agb_tramp_602,  agb_tramp_603,
    agb_tramp_604,  agb_tramp_605,  agb_tramp_606,  agb_tramp_607,
    agb_tramp_608,  agb_tramp_609,  agb_tramp_610,  agb_tramp_611,
    agb_tramp_612,  agb_tramp_613,  agb_tramp_614,  agb_tramp_615,
    agb_tramp_616,  agb_tramp_617,  agb_tramp_618,  agb_tramp_619,
    agb_tramp_620,  agb_tramp_621,  agb_tramp_622,  agb_tramp_623,
    agb_tramp_624,  agb_tramp_625,  agb_tramp_626,  agb_tramp_627,
    agb_tramp_628,  agb_tramp_629,  agb_tramp_630,  agb_tramp_631,
    agb_tramp_632,  agb_tramp_633,  agb_tramp_634,  agb_tramp_635,
    agb_tramp_636,  agb_tramp_637,  agb_tramp_638,  agb_tramp_639,
    agb_tramp_640,  agb_tramp_641,  agb_tramp_642,  agb_tramp_643,
    agb_tramp_644,  agb_tramp_645,  agb_tramp_646,  agb_tramp_647,
    agb_tramp_648,  agb_tramp_649,  agb_tramp_650,  agb_tramp_651,
    agb_tramp_652,  agb_tramp_653,  agb_tramp_654,  agb_tramp_655,
    agb_tramp_656,  agb_tramp_657,  agb_tramp_658,  agb_tramp_659,
    agb_tramp_660,  agb_tramp_661,  agb_tramp_662,  agb_tramp_663,
    agb_tramp_664,  agb_tramp_665,  agb_tramp_666,  agb_tramp_667,
    agb_tramp_668,  agb_tramp_669,  agb_tramp_670,  agb_tramp_671,
    agb_tramp_672,  agb_tramp_673,  agb_tramp_674,  agb_tramp_675,
    agb_tramp_676,  agb_tramp_677,  agb_tramp_678,  agb_tramp_679,
    agb_tramp_680,  agb_tramp_681,  agb_tramp_682,  agb_tramp_683,
    agb_tramp_684,  agb_tramp_685,  agb_tramp_686,  agb_tramp_687,
    agb_tramp_688,  agb_tramp_689,  agb_tramp_690,  agb_tramp_691,
    agb_tramp_692,  agb_tramp_693,  agb_tramp_694,  agb_tramp_695,
    agb_tramp_696,  agb_tramp_697,  agb_tramp_698,  agb_tramp_699,
    agb_tramp_700,  agb_tramp_701,  agb_tramp_702,  agb_tramp_703,
    agb_tramp_704,  agb_tramp_705,  agb_tramp_706,  agb_tramp_707,
    agb_tramp_708,  agb_tramp_709,  agb_tramp_710,  agb_tramp_711,
    agb_tramp_712,  agb_tramp_713,  agb_tramp_714,  agb_tramp_715,
    agb_tramp_716,  agb_tramp_717,  agb_tramp_718,  agb_tramp_719,
    agb_tramp_720,  agb_tramp_721,  agb_tramp_722,  agb_tramp_723,
    agb_tramp_724,  agb_tramp_725,  agb_tramp_726,  agb_tramp_727,
    agb_tramp_728,  agb_tramp_729,  agb_tramp_730,  agb_tramp_731,
    agb_tramp_732,  agb_tramp_733,  agb_tramp_734,  agb_tramp_735,
    agb_tramp_736,  agb_tramp_737,  agb_tramp_738,  agb_tramp_739,
    agb_tramp_740,  agb_tramp_741,  agb_tramp_742,  agb_tramp_743,
    agb_tramp_744,  agb_tramp_745,  agb_tramp_746,  agb_tramp_747,
    agb_tramp_748,  agb_tramp_749,  agb_tramp_750,  agb_tramp_751,
    agb_tramp_752,  agb_tramp_753,  agb_tramp_754,  agb_tramp_755,
    agb_tramp_756,  agb_tramp_757,  agb_tramp_758,  agb_tramp_759,
    agb_tramp_760,  agb_tramp_761,  agb_tramp_762,  agb_tramp_763,
    agb_tramp_764,  agb_tramp_765,  agb_tramp_766,  agb_tramp_767,
    agb_tramp_768,  agb_tramp_769,  agb_tramp_770,  agb_tramp_771,
    agb_tramp_772,  agb_tramp_773,  agb_tramp_774,  agb_tramp_775,
    agb_tramp_776,  agb_tramp_777,  agb_tramp_778,  agb_tramp_779,
    agb_tramp_780,  agb_tramp_781,  agb_tramp_782,  agb_tramp_783,
    agb_tramp_784,  agb_tramp_785,  agb_tramp_786,  agb_tramp_787,
    agb_tramp_788,  agb_tramp_789,  agb_tramp_790,  agb_tramp_791,
    agb_tramp_792,  agb_tramp_793,  agb_tramp_794,  agb_tramp_795,
    agb_tramp_796,  agb_tramp_797,  agb_tramp_798,  agb_tramp_799,
    agb_tramp_800,  agb_tramp_801,  agb_tramp_802,  agb_tramp_803,
    agb_tramp_804,  agb_tramp_805,  agb_tramp_806,  agb_tramp_807,
    agb_tramp_808,  agb_tramp_809,  agb_tramp_810,  agb_tramp_811,
    agb_tramp_812,  agb_tramp_813,  agb_tramp_814,  agb_tramp_815,
    agb_tramp_816,  agb_tramp_817,  agb_tramp_818,  agb_tramp_819,
    agb_tramp_820,  agb_tramp_821,  agb_tramp_822,  agb_tramp_823,
    agb_tramp_824,  agb_tramp_825,  agb_tramp_826,  agb_tramp_827,
    agb_tramp_828,  agb_tramp_829,  agb_tramp_830,  agb_tramp_831,
    agb_tramp_832,  agb_tramp_833,  agb_tramp_834,  agb_tramp_835,
    agb_tramp_836,  agb_tramp_837,  agb_tramp_838,  agb_tramp_839,
    agb_tramp_840,  agb_tramp_841,  agb_tramp_842,  agb_tramp_843,
    agb_tramp_844,  agb_tramp_845,  agb_tramp_846,  agb_tramp_847,
    agb_tramp_848,  agb_tramp_849,  agb_tramp_850,  agb_tramp_851,
    agb_tramp_852,  agb_tramp_853,  agb_tramp_854,  agb_tramp_855,
    agb_tramp_856,  agb_tramp_857,  agb_tramp_858,  agb_tramp_859,
    agb_tramp_860,  agb_tramp_861,  agb_tramp_862,  agb_tramp_863,
    agb_tramp_864,  agb_tramp_865,  agb_tramp_866,  agb_tramp_867,
    agb_tramp_868,  agb_tramp_869,  agb_tramp_870,  agb_tramp_871,
    agb_tramp_872,  agb_tramp_873,  agb_tramp_874,  agb_tramp_875,
    agb_tramp_876,  agb_tramp_877,  agb_tramp_878,  agb_tramp_879,
    agb_tramp_880,  agb_tramp_881,  agb_tramp_882,  agb_tramp_883,
    agb_tramp_884,  agb_tramp_885,  agb_tramp_886,  agb_tramp_887,
    agb_tramp_888,  agb_tramp_889,  agb_tramp_890,  agb_tramp_891,
    agb_tramp_892,  agb_tramp_893,  agb_tramp_894,  agb_tramp_895,
    agb_tramp_896,  agb_tramp_897,  agb_tramp_898,  agb_tramp_899,
    agb_tramp_900,  agb_tramp_901,  agb_tramp_902,  agb_tramp_903,
    agb_tramp_904,  agb_tramp_905,  agb_tramp_906,  agb_tramp_907,
    agb_tramp_908,  agb_tramp_909,  agb_tramp_910,  agb_tramp_911,
    agb_tramp_912,  agb_tramp_913,  agb_tramp_914,  agb_tramp_915,
    agb_tramp_916,  agb_tramp_917,  agb_tramp_918,  agb_tramp_919,
    agb_tramp_920,  agb_tramp_921,  agb_tramp_922,  agb_tramp_923,
    agb_tramp_924,  agb_tramp_925,  agb_tramp_926,  agb_tramp_927,
    agb_tramp_928,  agb_tramp_929,  agb_tramp_930,  agb_tramp_931,
    agb_tramp_932,  agb_tramp_933,  agb_tramp_934,  agb_tramp_935,
    agb_tramp_936,  agb_tramp_937,  agb_tramp_938,  agb_tramp_939,
    agb_tramp_940,  agb_tramp_941,  agb_tramp_942,  agb_tramp_943,
    agb_tramp_944,  agb_tramp_945,  agb_tramp_946,  agb_tramp_947,
    agb_tramp_948,  agb_tramp_949,  agb_tramp_950,  agb_tramp_951,
    agb_tramp_952,  agb_tramp_953,  agb_tramp_954,  agb_tramp_955,
    agb_tramp_956,  agb_tramp_957,  agb_tramp_958,  agb_tramp_959,
    agb_tramp_960,  agb_tramp_961,  agb_tramp_962,  agb_tramp_963,
    agb_tramp_964,  agb_tramp_965,  agb_tramp_966,  agb_tramp_967,
    agb_tramp_968,  agb_tramp_969,  agb_tramp_970,  agb_tramp_971,
    agb_tramp_972,  agb_tramp_973,  agb_tramp_974,  agb_tramp_975,
    agb_tramp_976,  agb_tramp_977,  agb_tramp_978,  agb_tramp_979,
    agb_tramp_980,  agb_tramp_981,  agb_tramp_982,  agb_tramp_983,
    agb_tramp_984,  agb_tramp_985,  agb_tramp_986,  agb_tramp_987,
    agb_tramp_988,  agb_tramp_989,  agb_tramp_990,  agb_tramp_991,
    agb_tramp_992,  agb_tramp_993,  agb_tramp_994,  agb_tramp_995,
    agb_tramp_996,  agb_tramp_997,  agb_tramp_998,  agb_tramp_999,
    agb_tramp_1000,  agb_tramp_1001,  agb_tramp_1002,  agb_tramp_1003,
    agb_tramp_1004,  agb_tramp_1005,  agb_tramp_1006,  agb_tramp_1007,
    agb_tramp_1008,  agb_tramp_1009,  agb_tramp_1010,  agb_tramp_1011,
    agb_tramp_1012,  agb_tramp_1013,  agb_tramp_1014,  agb_tramp_1015,
    agb_tramp_1016,  agb_tramp_1017,  agb_tramp_1018,  agb_tramp_1019,
    agb_tramp_1020,  agb_tramp_1021,  agb_tramp_1022,  agb_tramp_1023,
    agb_tramp_1024,  agb_tramp_1025,  agb_tramp_1026,  agb_tramp_1027,
    agb_tramp_1028,  agb_tramp_1029,  agb_tramp_1030,  agb_tramp_1031,
    agb_tramp_1032,  agb_tramp_1033,  agb_tramp_1034,  agb_tramp_1035,
    agb_tramp_1036,  agb_tramp_1037,  agb_tramp_1038,  agb_tramp_1039,
    agb_tramp_1040,  agb_tramp_1041,  agb_tramp_1042,  agb_tramp_1043,
    agb_tramp_1044,  agb_tramp_1045,  agb_tramp_1046,  agb_tramp_1047,
    agb_tramp_1048,  agb_tramp_1049,  agb_tramp_1050,  agb_tramp_1051,
    agb_tramp_1052,  agb_tramp_1053,  agb_tramp_1054,  agb_tramp_1055,
    agb_tramp_1056,  agb_tramp_1057,  agb_tramp_1058,  agb_tramp_1059,
    agb_tramp_1060,  agb_tramp_1061,  agb_tramp_1062,  agb_tramp_1063,
    agb_tramp_1064,  agb_tramp_1065,  agb_tramp_1066,  agb_tramp_1067,
    agb_tramp_1068,  agb_tramp_1069,  agb_tramp_1070,  agb_tramp_1071,
    agb_tramp_1072,  agb_tramp_1073,  agb_tramp_1074,  agb_tramp_1075,
    agb_tramp_1076,  agb_tramp_1077,  agb_tramp_1078,  agb_tramp_1079,
    agb_tramp_1080,  agb_tramp_1081,  agb_tramp_1082,  agb_tramp_1083,
    agb_tramp_1084,  agb_tramp_1085,  agb_tramp_1086,  agb_tramp_1087,
    agb_tramp_1088,  agb_tramp_1089,  agb_tramp_1090,  agb_tramp_1091,
    agb_tramp_1092,  agb_tramp_1093,  agb_tramp_1094,  agb_tramp_1095,
    agb_tramp_1096,  agb_tramp_1097,  agb_tramp_1098,  agb_tramp_1099,
    agb_tramp_1100,  agb_tramp_1101,  agb_tramp_1102,  agb_tramp_1103,
    agb_tramp_1104,  agb_tramp_1105,  agb_tramp_1106,  agb_tramp_1107,
    agb_tramp_1108,  agb_tramp_1109,  agb_tramp_1110,  agb_tramp_1111,
    agb_tramp_1112,  agb_tramp_1113,  agb_tramp_1114,  agb_tramp_1115,
    agb_tramp_1116,  agb_tramp_1117,  agb_tramp_1118,  agb_tramp_1119,
    agb_tramp_1120,  agb_tramp_1121,  agb_tramp_1122,  agb_tramp_1123,
    agb_tramp_1124,  agb_tramp_1125,  agb_tramp_1126,  agb_tramp_1127,
    agb_tramp_1128,  agb_tramp_1129,  agb_tramp_1130,  agb_tramp_1131,
    agb_tramp_1132,  agb_tramp_1133,  agb_tramp_1134,  agb_tramp_1135,
    agb_tramp_1136,  agb_tramp_1137,  agb_tramp_1138,  agb_tramp_1139,
    agb_tramp_1140,  agb_tramp_1141,  agb_tramp_1142,  agb_tramp_1143,
    agb_tramp_1144,  agb_tramp_1145,  agb_tramp_1146,  agb_tramp_1147,
    agb_tramp_1148,  agb_tramp_1149,  agb_tramp_1150,  agb_tramp_1151,
    agb_tramp_1152,  agb_tramp_1153,  agb_tramp_1154,  agb_tramp_1155,
    agb_tramp_1156,  agb_tramp_1157,  agb_tramp_1158,  agb_tramp_1159,
    agb_tramp_1160,  agb_tramp_1161,  agb_tramp_1162,  agb_tramp_1163,
    agb_tramp_1164,  agb_tramp_1165,  agb_tramp_1166,  agb_tramp_1167,
    agb_tramp_1168,  agb_tramp_1169,  agb_tramp_1170,  agb_tramp_1171,
    agb_tramp_1172,  agb_tramp_1173,  agb_tramp_1174,  agb_tramp_1175,
    agb_tramp_1176,  agb_tramp_1177,  agb_tramp_1178,  agb_tramp_1179,
    agb_tramp_1180,  agb_tramp_1181,  agb_tramp_1182,  agb_tramp_1183,
    agb_tramp_1184,  agb_tramp_1185,  agb_tramp_1186,  agb_tramp_1187,
    agb_tramp_1188,  agb_tramp_1189,  agb_tramp_1190,  agb_tramp_1191,
    agb_tramp_1192,  agb_tramp_1193,  agb_tramp_1194,  agb_tramp_1195,
    agb_tramp_1196,  agb_tramp_1197,  agb_tramp_1198,  agb_tramp_1199,
    agb_tramp_1200,  agb_tramp_1201,  agb_tramp_1202,  agb_tramp_1203,
    agb_tramp_1204,  agb_tramp_1205,  agb_tramp_1206,  agb_tramp_1207,
    agb_tramp_1208,  agb_tramp_1209,  agb_tramp_1210,  agb_tramp_1211,
    agb_tramp_1212,  agb_tramp_1213,  agb_tramp_1214,  agb_tramp_1215,
    agb_tramp_1216,  agb_tramp_1217,  agb_tramp_1218,  agb_tramp_1219,
    agb_tramp_1220,  agb_tramp_1221,  agb_tramp_1222,  agb_tramp_1223,
    agb_tramp_1224,  agb_tramp_1225,  agb_tramp_1226,  agb_tramp_1227,
    agb_tramp_1228,  agb_tramp_1229,  agb_tramp_1230,  agb_tramp_1231,
    agb_tramp_1232,  agb_tramp_1233,  agb_tramp_1234,  agb_tramp_1235,
    agb_tramp_1236,  agb_tramp_1237,  agb_tramp_1238,  agb_tramp_1239,
    agb_tramp_1240,  agb_tramp_1241,  agb_tramp_1242,  agb_tramp_1243,
    agb_tramp_1244,  agb_tramp_1245,  agb_tramp_1246,  agb_tramp_1247,
    agb_tramp_1248,  agb_tramp_1249,  agb_tramp_1250,  agb_tramp_1251,
    agb_tramp_1252,  agb_tramp_1253,  agb_tramp_1254,  agb_tramp_1255,
    agb_tramp_1256,  agb_tramp_1257,  agb_tramp_1258,  agb_tramp_1259,
    agb_tramp_1260,  agb_tramp_1261,  agb_tramp_1262,  agb_tramp_1263,
    agb_tramp_1264,  agb_tramp_1265,  agb_tramp_1266,  agb_tramp_1267,
    agb_tramp_1268,  agb_tramp_1269,  agb_tramp_1270,  agb_tramp_1271,
    agb_tramp_1272,  agb_tramp_1273,  agb_tramp_1274,  agb_tramp_1275,
    agb_tramp_1276,  agb_tramp_1277,  agb_tramp_1278,  agb_tramp_1279,
    agb_tramp_1280,  agb_tramp_1281,  agb_tramp_1282,  agb_tramp_1283,
    agb_tramp_1284,  agb_tramp_1285,  agb_tramp_1286,  agb_tramp_1287,
    agb_tramp_1288,  agb_tramp_1289,  agb_tramp_1290,  agb_tramp_1291,
    agb_tramp_1292,  agb_tramp_1293,  agb_tramp_1294,  agb_tramp_1295,
    agb_tramp_1296,  agb_tramp_1297,  agb_tramp_1298,  agb_tramp_1299,
    agb_tramp_1300,  agb_tramp_1301,  agb_tramp_1302,  agb_tramp_1303,
    agb_tramp_1304,  agb_tramp_1305,  agb_tramp_1306,  agb_tramp_1307,
    agb_tramp_1308,  agb_tramp_1309,  agb_tramp_1310,  agb_tramp_1311,
    agb_tramp_1312,  agb_tramp_1313,  agb_tramp_1314,  agb_tramp_1315,
    agb_tramp_1316,  agb_tramp_1317,  agb_tramp_1318,  agb_tramp_1319,
    agb_tramp_1320,  agb_tramp_1321,  agb_tramp_1322,  agb_tramp_1323,
    agb_tramp_1324,  agb_tramp_1325,  agb_tramp_1326,  agb_tramp_1327,
    agb_tramp_1328,  agb_tramp_1329,  agb_tramp_1330,  agb_tramp_1331,
    agb_tramp_1332,  agb_tramp_1333,  agb_tramp_1334,  agb_tramp_1335,
    agb_tramp_1336,  agb_tramp_1337,  agb_tramp_1338,  agb_tramp_1339,
    agb_tramp_1340,  agb_tramp_1341,  agb_tramp_1342,  agb_tramp_1343,
    agb_tramp_1344,  agb_tramp_1345,  agb_tramp_1346,  agb_tramp_1347,
    agb_tramp_1348,  agb_tramp_1349,  agb_tramp_1350,  agb_tramp_1351,
    agb_tramp_1352,  agb_tramp_1353,  agb_tramp_1354,  agb_tramp_1355,
    agb_tramp_1356,  agb_tramp_1357,  agb_tramp_1358,  agb_tramp_1359,
    agb_tramp_1360,  agb_tramp_1361,  agb_tramp_1362,  agb_tramp_1363,
    agb_tramp_1364,  agb_tramp_1365,  agb_tramp_1366,  agb_tramp_1367,
    agb_tramp_1368,  agb_tramp_1369,  agb_tramp_1370,  agb_tramp_1371,
    agb_tramp_1372,  agb_tramp_1373,  agb_tramp_1374,  agb_tramp_1375,
    agb_tramp_1376,  agb_tramp_1377,  agb_tramp_1378,  agb_tramp_1379,
    agb_tramp_1380,  agb_tramp_1381,  agb_tramp_1382,  agb_tramp_1383,
    agb_tramp_1384,  agb_tramp_1385,  agb_tramp_1386,  agb_tramp_1387,
    agb_tramp_1388,  agb_tramp_1389,  agb_tramp_1390,  agb_tramp_1391,
    agb_tramp_1392,  agb_tramp_1393,  agb_tramp_1394,  agb_tramp_1395,
    agb_tramp_1396,  agb_tramp_1397,  agb_tramp_1398,  agb_tramp_1399,
    agb_tramp_1400,  agb_tramp_1401,  agb_tramp_1402,  agb_tramp_1403,
    agb_tramp_1404,  agb_tramp_1405,  agb_tramp_1406,  agb_tramp_1407,
    agb_tramp_1408,  agb_tramp_1409,  agb_tramp_1410,  agb_tramp_1411,
    agb_tramp_1412,  agb_tramp_1413,  agb_tramp_1414,  agb_tramp_1415,
    agb_tramp_1416,  agb_tramp_1417,  agb_tramp_1418,  agb_tramp_1419,
    agb_tramp_1420,  agb_tramp_1421,  agb_tramp_1422,  agb_tramp_1423,
    agb_tramp_1424,  agb_tramp_1425,  agb_tramp_1426,  agb_tramp_1427,
    agb_tramp_1428,  agb_tramp_1429,  agb_tramp_1430,  agb_tramp_1431,
    agb_tramp_1432,  agb_tramp_1433,  agb_tramp_1434,  agb_tramp_1435,
    agb_tramp_1436,  agb_tramp_1437,  agb_tramp_1438,  agb_tramp_1439,
    agb_tramp_1440,  agb_tramp_1441,  agb_tramp_1442,  agb_tramp_1443,
    agb_tramp_1444,  agb_tramp_1445,  agb_tramp_1446,  agb_tramp_1447,
    agb_tramp_1448,  agb_tramp_1449,  agb_tramp_1450,  agb_tramp_1451,
    agb_tramp_1452,  agb_tramp_1453,  agb_tramp_1454,  agb_tramp_1455,
    agb_tramp_1456,  agb_tramp_1457,  agb_tramp_1458,  agb_tramp_1459,
    agb_tramp_1460,  agb_tramp_1461,  agb_tramp_1462,  agb_tramp_1463,
    agb_tramp_1464,  agb_tramp_1465,  agb_tramp_1466,  agb_tramp_1467,
    agb_tramp_1468,  agb_tramp_1469,  agb_tramp_1470,  agb_tramp_1471,
    agb_tramp_1472,  agb_tramp_1473,  agb_tramp_1474,  agb_tramp_1475,
    agb_tramp_1476,  agb_tramp_1477,  agb_tramp_1478,  agb_tramp_1479,
    agb_tramp_1480,  agb_tramp_1481,  agb_tramp_1482,  agb_tramp_1483,
    agb_tramp_1484,  agb_tramp_1485,  agb_tramp_1486,  agb_tramp_1487,
    agb_tramp_1488,  agb_tramp_1489,  agb_tramp_1490,  agb_tramp_1491,
    agb_tramp_1492,  agb_tramp_1493,  agb_tramp_1494,  agb_tramp_1495,
    agb_tramp_1496,  agb_tramp_1497,  agb_tramp_1498,  agb_tramp_1499,
    agb_tramp_1500,  agb_tramp_1501,  agb_tramp_1502,  agb_tramp_1503,
    agb_tramp_1504,  agb_tramp_1505,  agb_tramp_1506,  agb_tramp_1507,
    agb_tramp_1508,  agb_tramp_1509,  agb_tramp_1510,  agb_tramp_1511,
    agb_tramp_1512,  agb_tramp_1513,  agb_tramp_1514,  agb_tramp_1515,
    agb_tramp_1516,  agb_tramp_1517,  agb_tramp_1518,  agb_tramp_1519,
    agb_tramp_1520,  agb_tramp_1521,  agb_tramp_1522,  agb_tramp_1523,
    agb_tramp_1524,  agb_tramp_1525,  agb_tramp_1526,  agb_tramp_1527,
    agb_tramp_1528,  agb_tramp_1529,  agb_tramp_1530,  agb_tramp_1531,
    agb_tramp_1532,  agb_tramp_1533,  agb_tramp_1534,  agb_tramp_1535,
    agb_tramp_1536,  agb_tramp_1537,  agb_tramp_1538,  agb_tramp_1539,
    agb_tramp_1540,  agb_tramp_1541,  agb_tramp_1542,  agb_tramp_1543,
    agb_tramp_1544,  agb_tramp_1545,  agb_tramp_1546,  agb_tramp_1547,
    agb_tramp_1548,  agb_tramp_1549,  agb_tramp_1550,  agb_tramp_1551,
    agb_tramp_1552,  agb_tramp_1553,  agb_tramp_1554,  agb_tramp_1555,
    agb_tramp_1556,  agb_tramp_1557,  agb_tramp_1558,  agb_tramp_1559,
    agb_tramp_1560,  agb_tramp_1561,  agb_tramp_1562,  agb_tramp_1563,
    agb_tramp_1564,  agb_tramp_1565,  agb_tramp_1566,  agb_tramp_1567,
    agb_tramp_1568,  agb_tramp_1569,  agb_tramp_1570,  agb_tramp_1571,
    agb_tramp_1572,  agb_tramp_1573,  agb_tramp_1574,  agb_tramp_1575,
    agb_tramp_1576,  agb_tramp_1577,  agb_tramp_1578,  agb_tramp_1579,
    agb_tramp_1580,  agb_tramp_1581,  agb_tramp_1582,  agb_tramp_1583,
    agb_tramp_1584,  agb_tramp_1585,  agb_tramp_1586,  agb_tramp_1587,
    agb_tramp_1588,  agb_tramp_1589,  agb_tramp_1590,  agb_tramp_1591,
    agb_tramp_1592,  agb_tramp_1593,  agb_tramp_1594,  agb_tramp_1595,
    agb_tramp_1596,  agb_tramp_1597,  agb_tramp_1598,  agb_tramp_1599,
    agb_tramp_1600,  agb_tramp_1601,  agb_tramp_1602,  agb_tramp_1603,
    agb_tramp_1604,  agb_tramp_1605,  agb_tramp_1606,  agb_tramp_1607,
    agb_tramp_1608,  agb_tramp_1609,  agb_tramp_1610,  agb_tramp_1611,
    agb_tramp_1612,  agb_tramp_1613,  agb_tramp_1614,  agb_tramp_1615,
    agb_tramp_1616,  agb_tramp_1617,  agb_tramp_1618,  agb_tramp_1619,
    agb_tramp_1620,  agb_tramp_1621,  agb_tramp_1622,  agb_tramp_1623,
    agb_tramp_1624,  agb_tramp_1625,  agb_tramp_1626,  agb_tramp_1627,
    agb_tramp_1628,  agb_tramp_1629,  agb_tramp_1630,  agb_tramp_1631,
    agb_tramp_1632,  agb_tramp_1633,  agb_tramp_1634,  agb_tramp_1635,
    agb_tramp_1636,  agb_tramp_1637,  agb_tramp_1638,  agb_tramp_1639,
    agb_tramp_1640,  agb_tramp_1641,  agb_tramp_1642,  agb_tramp_1643,
    agb_tramp_1644,  agb_tramp_1645,  agb_tramp_1646,  agb_tramp_1647,
    agb_tramp_1648,  agb_tramp_1649,  agb_tramp_1650,  agb_tramp_1651,
    agb_tramp_1652,  agb_tramp_1653,  agb_tramp_1654,  agb_tramp_1655,
    agb_tramp_1656,  agb_tramp_1657,  agb_tramp_1658,  agb_tramp_1659,
    agb_tramp_1660,  agb_tramp_1661,  agb_tramp_1662,  agb_tramp_1663,
    agb_tramp_1664,  agb_tramp_1665,  agb_tramp_1666,  agb_tramp_1667,
    agb_tramp_1668,  agb_tramp_1669,  agb_tramp_1670,  agb_tramp_1671,
    agb_tramp_1672,  agb_tramp_1673,  agb_tramp_1674,  agb_tramp_1675,
    agb_tramp_1676,  agb_tramp_1677,  agb_tramp_1678,  agb_tramp_1679,
    agb_tramp_1680,  agb_tramp_1681,  agb_tramp_1682,  agb_tramp_1683,
    agb_tramp_1684,  agb_tramp_1685,  agb_tramp_1686,  agb_tramp_1687,
    agb_tramp_1688,  agb_tramp_1689,  agb_tramp_1690,  agb_tramp_1691,
    agb_tramp_1692,  agb_tramp_1693,  agb_tramp_1694,  agb_tramp_1695,
    agb_tramp_1696,  agb_tramp_1697,  agb_tramp_1698,  agb_tramp_1699,
    agb_tramp_1700,  agb_tramp_1701,  agb_tramp_1702,  agb_tramp_1703,
    agb_tramp_1704,  agb_tramp_1705,  agb_tramp_1706,  agb_tramp_1707,
    agb_tramp_1708,  agb_tramp_1709,  agb_tramp_1710,  agb_tramp_1711,
    agb_tramp_1712,  agb_tramp_1713,  agb_tramp_1714,  agb_tramp_1715,
    agb_tramp_1716,  agb_tramp_1717,  agb_tramp_1718,  agb_tramp_1719,
    agb_tramp_1720,  agb_tramp_1721,  agb_tramp_1722,  agb_tramp_1723,
    agb_tramp_1724,  agb_tramp_1725,  agb_tramp_1726,  agb_tramp_1727,
    agb_tramp_1728,  agb_tramp_1729,  agb_tramp_1730,  agb_tramp_1731,
    agb_tramp_1732,  agb_tramp_1733,  agb_tramp_1734,  agb_tramp_1735,
    agb_tramp_1736,  agb_tramp_1737,  agb_tramp_1738,  agb_tramp_1739,
    agb_tramp_1740,  agb_tramp_1741,  agb_tramp_1742,  agb_tramp_1743,
    agb_tramp_1744,  agb_tramp_1745,  agb_tramp_1746,  agb_tramp_1747,
    agb_tramp_1748,  agb_tramp_1749,  agb_tramp_1750,  agb_tramp_1751,
    agb_tramp_1752,  agb_tramp_1753,  agb_tramp_1754,  agb_tramp_1755,
    agb_tramp_1756,  agb_tramp_1757,  agb_tramp_1758,  agb_tramp_1759,
    agb_tramp_1760,  agb_tramp_1761,  agb_tramp_1762,  agb_tramp_1763,
    agb_tramp_1764,  agb_tramp_1765,  agb_tramp_1766,  agb_tramp_1767,
    agb_tramp_1768,  agb_tramp_1769,  agb_tramp_1770,  agb_tramp_1771,
    agb_tramp_1772,  agb_tramp_1773,  agb_tramp_1774,  agb_tramp_1775,
    agb_tramp_1776,  agb_tramp_1777,  agb_tramp_1778,  agb_tramp_1779,
    agb_tramp_1780,  agb_tramp_1781,  agb_tramp_1782,  agb_tramp_1783,
    agb_tramp_1784,  agb_tramp_1785,  agb_tramp_1786,  agb_tramp_1787,
    agb_tramp_1788,  agb_tramp_1789,  agb_tramp_1790,  agb_tramp_1791,
    agb_tramp_1792,  agb_tramp_1793,  agb_tramp_1794,  agb_tramp_1795,
    agb_tramp_1796,  agb_tramp_1797,  agb_tramp_1798,  agb_tramp_1799,
    agb_tramp_1800,  agb_tramp_1801,  agb_tramp_1802,  agb_tramp_1803,
    agb_tramp_1804,  agb_tramp_1805,  agb_tramp_1806,  agb_tramp_1807,
    agb_tramp_1808,  agb_tramp_1809,  agb_tramp_1810,  agb_tramp_1811,
    agb_tramp_1812,  agb_tramp_1813,  agb_tramp_1814,  agb_tramp_1815,
    agb_tramp_1816,  agb_tramp_1817,  agb_tramp_1818,  agb_tramp_1819,
    agb_tramp_1820,  agb_tramp_1821,  agb_tramp_1822,  agb_tramp_1823,
    agb_tramp_1824,  agb_tramp_1825,  agb_tramp_1826,  agb_tramp_1827,
    agb_tramp_1828,  agb_tramp_1829,  agb_tramp_1830,  agb_tramp_1831,
    agb_tramp_1832,  agb_tramp_1833,  agb_tramp_1834,  agb_tramp_1835,
    agb_tramp_1836,  agb_tramp_1837,  agb_tramp_1838,  agb_tramp_1839,
    agb_tramp_1840,  agb_tramp_1841,  agb_tramp_1842,  agb_tramp_1843,
    agb_tramp_1844,  agb_tramp_1845,  agb_tramp_1846,  agb_tramp_1847,
    agb_tramp_1848,  agb_tramp_1849,  agb_tramp_1850,  agb_tramp_1851,
    agb_tramp_1852,  agb_tramp_1853,  agb_tramp_1854,  agb_tramp_1855,
    agb_tramp_1856,  agb_tramp_1857,  agb_tramp_1858,  agb_tramp_1859,
    agb_tramp_1860,  agb_tramp_1861,  agb_tramp_1862,  agb_tramp_1863,
    agb_tramp_1864,  agb_tramp_1865,  agb_tramp_1866,  agb_tramp_1867,
    agb_tramp_1868,  agb_tramp_1869,  agb_tramp_1870,  agb_tramp_1871,
    agb_tramp_1872,  agb_tramp_1873,  agb_tramp_1874,  agb_tramp_1875,
    agb_tramp_1876,  agb_tramp_1877,  agb_tramp_1878,  agb_tramp_1879,
    agb_tramp_1880,  agb_tramp_1881,  agb_tramp_1882,  agb_tramp_1883,
    agb_tramp_1884,  agb_tramp_1885,  agb_tramp_1886,  agb_tramp_1887,
    agb_tramp_1888,  agb_tramp_1889,  agb_tramp_1890,  agb_tramp_1891,
    agb_tramp_1892,  agb_tramp_1893,  agb_tramp_1894,  agb_tramp_1895,
    agb_tramp_1896,  agb_tramp_1897,  agb_tramp_1898,  agb_tramp_1899,
    agb_tramp_1900,  agb_tramp_1901,  agb_tramp_1902,  agb_tramp_1903,
    agb_tramp_1904,  agb_tramp_1905,  agb_tramp_1906,  agb_tramp_1907,
    agb_tramp_1908,  agb_tramp_1909,  agb_tramp_1910,  agb_tramp_1911,
    agb_tramp_1912,  agb_tramp_1913,  agb_tramp_1914,  agb_tramp_1915,
    agb_tramp_1916,  agb_tramp_1917,  agb_tramp_1918,  agb_tramp_1919,
    agb_tramp_1920,  agb_tramp_1921,  agb_tramp_1922,  agb_tramp_1923,
    agb_tramp_1924,  agb_tramp_1925,  agb_tramp_1926,  agb_tramp_1927,
    agb_tramp_1928,  agb_tramp_1929,  agb_tramp_1930,  agb_tramp_1931,
    agb_tramp_1932,  agb_tramp_1933,  agb_tramp_1934,  agb_tramp_1935,
    agb_tramp_1936,  agb_tramp_1937,  agb_tramp_1938,  agb_tramp_1939,
    agb_tramp_1940,  agb_tramp_1941,  agb_tramp_1942,  agb_tramp_1943,
    agb_tramp_1944,  agb_tramp_1945,  agb_tramp_1946,  agb_tramp_1947,
    agb_tramp_1948,  agb_tramp_1949,  agb_tramp_1950,  agb_tramp_1951,
    agb_tramp_1952,  agb_tramp_1953,  agb_tramp_1954,  agb_tramp_1955,
    agb_tramp_1956,  agb_tramp_1957,  agb_tramp_1958,  agb_tramp_1959,
    agb_tramp_1960,  agb_tramp_1961,  agb_tramp_1962,  agb_tramp_1963,
    agb_tramp_1964,  agb_tramp_1965,  agb_tramp_1966,  agb_tramp_1967,
    agb_tramp_1968,  agb_tramp_1969,  agb_tramp_1970,  agb_tramp_1971,
    agb_tramp_1972,  agb_tramp_1973,  agb_tramp_1974,  agb_tramp_1975,
    agb_tramp_1976,  agb_tramp_1977,  agb_tramp_1978,  agb_tramp_1979,
    agb_tramp_1980,  agb_tramp_1981,  agb_tramp_1982,  agb_tramp_1983,
    agb_tramp_1984,  agb_tramp_1985,  agb_tramp_1986,  agb_tramp_1987,
    agb_tramp_1988,  agb_tramp_1989,  agb_tramp_1990,  agb_tramp_1991,
    agb_tramp_1992,  agb_tramp_1993,  agb_tramp_1994,  agb_tramp_1995,
    agb_tramp_1996,  agb_tramp_1997,  agb_tramp_1998,  agb_tramp_1999,
    agb_tramp_2000,  agb_tramp_2001,  agb_tramp_2002,  agb_tramp_2003,
    agb_tramp_2004,  agb_tramp_2005,  agb_tramp_2006,  agb_tramp_2007,
    agb_tramp_2008,  agb_tramp_2009,  agb_tramp_2010,  agb_tramp_2011,
    agb_tramp_2012,  agb_tramp_2013,  agb_tramp_2014,  agb_tramp_2015,
    agb_tramp_2016,  agb_tramp_2017,  agb_tramp_2018,  agb_tramp_2019,
    agb_tramp_2020,  agb_tramp_2021,  agb_tramp_2022,  agb_tramp_2023,
    agb_tramp_2024,  agb_tramp_2025,  agb_tramp_2026,  agb_tramp_2027,
    agb_tramp_2028,  agb_tramp_2029,  agb_tramp_2030,  agb_tramp_2031,
    agb_tramp_2032,  agb_tramp_2033,  agb_tramp_2034,  agb_tramp_2035,
    agb_tramp_2036,  agb_tramp_2037,  agb_tramp_2038,  agb_tramp_2039,
    agb_tramp_2040,  agb_tramp_2041,  agb_tramp_2042,  agb_tramp_2043,
    agb_tramp_2044,  agb_tramp_2045,  agb_tramp_2046,  agb_tramp_2047,
    agb_tramp_2048,  agb_tramp_2049,  agb_tramp_2050,  agb_tramp_2051,
    agb_tramp_2052,  agb_tramp_2053,  agb_tramp_2054,  agb_tramp_2055,
    agb_tramp_2056,  agb_tramp_2057,  agb_tramp_2058,  agb_tramp_2059,
    agb_tramp_2060,  agb_tramp_2061,  agb_tramp_2062,  agb_tramp_2063,
    agb_tramp_2064,  agb_tramp_2065,  agb_tramp_2066,  agb_tramp_2067,
    agb_tramp_2068,  agb_tramp_2069,  agb_tramp_2070,  agb_tramp_2071,
    agb_tramp_2072,  agb_tramp_2073,  agb_tramp_2074,  agb_tramp_2075,
    agb_tramp_2076,  agb_tramp_2077,  agb_tramp_2078,  agb_tramp_2079,
    agb_tramp_2080,  agb_tramp_2081,  agb_tramp_2082,  agb_tramp_2083,
    agb_tramp_2084,  agb_tramp_2085,  agb_tramp_2086,  agb_tramp_2087,
    agb_tramp_2088,  agb_tramp_2089,  agb_tramp_2090,  agb_tramp_2091,
    agb_tramp_2092,  agb_tramp_2093,  agb_tramp_2094,  agb_tramp_2095,
    agb_tramp_2096,  agb_tramp_2097,  agb_tramp_2098,  agb_tramp_2099,
    agb_tramp_2100,  agb_tramp_2101,  agb_tramp_2102,  agb_tramp_2103,
    agb_tramp_2104,  agb_tramp_2105,  agb_tramp_2106,  agb_tramp_2107,
    agb_tramp_2108,  agb_tramp_2109,  agb_tramp_2110,  agb_tramp_2111,
    agb_tramp_2112,  agb_tramp_2113,  agb_tramp_2114,  agb_tramp_2115,
    agb_tramp_2116,  agb_tramp_2117,  agb_tramp_2118,  agb_tramp_2119,
    agb_tramp_2120,  agb_tramp_2121,  agb_tramp_2122,  agb_tramp_2123,
    agb_tramp_2124,  agb_tramp_2125,  agb_tramp_2126,  agb_tramp_2127,
    agb_tramp_2128,  agb_tramp_2129,  agb_tramp_2130,  agb_tramp_2131,
    agb_tramp_2132,  agb_tramp_2133,  agb_tramp_2134,  agb_tramp_2135,
    agb_tramp_2136,  agb_tramp_2137,  agb_tramp_2138,  agb_tramp_2139,
    agb_tramp_2140,  agb_tramp_2141,  agb_tramp_2142,  agb_tramp_2143,
    agb_tramp_2144,  agb_tramp_2145,  agb_tramp_2146,  agb_tramp_2147,
    agb_tramp_2148,  agb_tramp_2149,  agb_tramp_2150,  agb_tramp_2151,
    agb_tramp_2152,  agb_tramp_2153,  agb_tramp_2154,  agb_tramp_2155,
    agb_tramp_2156,  agb_tramp_2157,  agb_tramp_2158,  agb_tramp_2159,
    agb_tramp_2160,  agb_tramp_2161,  agb_tramp_2162,  agb_tramp_2163,
    agb_tramp_2164,  agb_tramp_2165,  agb_tramp_2166,  agb_tramp_2167,
    agb_tramp_2168,  agb_tramp_2169,  agb_tramp_2170,  agb_tramp_2171,
    agb_tramp_2172,  agb_tramp_2173,  agb_tramp_2174,  agb_tramp_2175,
    agb_tramp_2176,  agb_tramp_2177,  agb_tramp_2178,  agb_tramp_2179,
    agb_tramp_2180,  agb_tramp_2181,  agb_tramp_2182,  agb_tramp_2183,
    agb_tramp_2184,  agb_tramp_2185,  agb_tramp_2186,  agb_tramp_2187,
    agb_tramp_2188,  agb_tramp_2189,  agb_tramp_2190,  agb_tramp_2191,
    agb_tramp_2192,  agb_tramp_2193,  agb_tramp_2194,  agb_tramp_2195,
    agb_tramp_2196,  agb_tramp_2197,  agb_tramp_2198,  agb_tramp_2199,
    agb_tramp_2200,  agb_tramp_2201,  agb_tramp_2202,  agb_tramp_2203,
    agb_tramp_2204,  agb_tramp_2205,  agb_tramp_2206,  agb_tramp_2207,
    agb_tramp_2208,  agb_tramp_2209,  agb_tramp_2210,  agb_tramp_2211,
    agb_tramp_2212,  agb_tramp_2213,  agb_tramp_2214,  agb_tramp_2215,
    agb_tramp_2216,  agb_tramp_2217,  agb_tramp_2218,  agb_tramp_2219,
    agb_tramp_2220,  agb_tramp_2221,  agb_tramp_2222,  agb_tramp_2223,
    agb_tramp_2224,  agb_tramp_2225,  agb_tramp_2226,  agb_tramp_2227,
    agb_tramp_2228,  agb_tramp_2229,  agb_tramp_2230,  agb_tramp_2231,
    agb_tramp_2232,  agb_tramp_2233,  agb_tramp_2234,  agb_tramp_2235,
    agb_tramp_2236,  agb_tramp_2237,  agb_tramp_2238,  agb_tramp_2239,
    agb_tramp_2240,  agb_tramp_2241,  agb_tramp_2242,  agb_tramp_2243,
    agb_tramp_2244,  agb_tramp_2245,  agb_tramp_2246,  agb_tramp_2247,
    agb_tramp_2248,  agb_tramp_2249,  agb_tramp_2250,  agb_tramp_2251,
    agb_tramp_2252,  agb_tramp_2253,  agb_tramp_2254,  agb_tramp_2255,
    agb_tramp_2256,  agb_tramp_2257,  agb_tramp_2258,  agb_tramp_2259,
    agb_tramp_2260,  agb_tramp_2261,  agb_tramp_2262,  agb_tramp_2263,
    agb_tramp_2264,  agb_tramp_2265,  agb_tramp_2266,  agb_tramp_2267,
    agb_tramp_2268,  agb_tramp_2269,  agb_tramp_2270,  agb_tramp_2271,
    agb_tramp_2272,  agb_tramp_2273,  agb_tramp_2274,  agb_tramp_2275,
    agb_tramp_2276,  agb_tramp_2277,  agb_tramp_2278,  agb_tramp_2279,
    agb_tramp_2280,  agb_tramp_2281,  agb_tramp_2282,  agb_tramp_2283,
    agb_tramp_2284,  agb_tramp_2285,  agb_tramp_2286,  agb_tramp_2287,
    agb_tramp_2288,  agb_tramp_2289,  agb_tramp_2290,  agb_tramp_2291,
    agb_tramp_2292,  agb_tramp_2293,  agb_tramp_2294,  agb_tramp_2295,
    agb_tramp_2296,  agb_tramp_2297,  agb_tramp_2298,  agb_tramp_2299,
    agb_tramp_2300,  agb_tramp_2301,  agb_tramp_2302,  agb_tramp_2303,
    agb_tramp_2304,  agb_tramp_2305,  agb_tramp_2306,  agb_tramp_2307,
    agb_tramp_2308,  agb_tramp_2309,  agb_tramp_2310,  agb_tramp_2311,
    agb_tramp_2312,  agb_tramp_2313,  agb_tramp_2314,  agb_tramp_2315,
    agb_tramp_2316,  agb_tramp_2317,  agb_tramp_2318,  agb_tramp_2319,
    agb_tramp_2320,  agb_tramp_2321,  agb_tramp_2322,  agb_tramp_2323,
    agb_tramp_2324,  agb_tramp_2325,  agb_tramp_2326,  agb_tramp_2327,
    agb_tramp_2328,  agb_tramp_2329,  agb_tramp_2330,  agb_tramp_2331,
    agb_tramp_2332,  agb_tramp_2333,  agb_tramp_2334,  agb_tramp_2335,
    agb_tramp_2336,  agb_tramp_2337,  agb_tramp_2338,  agb_tramp_2339,
    agb_tramp_2340,  agb_tramp_2341,  agb_tramp_2342,  agb_tramp_2343,
    agb_tramp_2344,  agb_tramp_2345,  agb_tramp_2346,  agb_tramp_2347,
    agb_tramp_2348,  agb_tramp_2349,  agb_tramp_2350,  agb_tramp_2351,
    agb_tramp_2352,  agb_tramp_2353,  agb_tramp_2354,  agb_tramp_2355,
    agb_tramp_2356,  agb_tramp_2357,  agb_tramp_2358,  agb_tramp_2359,
    agb_tramp_2360,  agb_tramp_2361,  agb_tramp_2362,  agb_tramp_2363,
    agb_tramp_2364,  agb_tramp_2365,  agb_tramp_2366,  agb_tramp_2367,
    agb_tramp_2368,  agb_tramp_2369,  agb_tramp_2370,  agb_tramp_2371,
    agb_tramp_2372,  agb_tramp_2373,  agb_tramp_2374,  agb_tramp_2375,
    agb_tramp_2376,  agb_tramp_2377,  agb_tramp_2378,  agb_tramp_2379,
    agb_tramp_2380,  agb_tramp_2381,  agb_tramp_2382,  agb_tramp_2383,
    agb_tramp_2384,  agb_tramp_2385,  agb_tramp_2386,  agb_tramp_2387,
    agb_tramp_2388,  agb_tramp_2389,  agb_tramp_2390,  agb_tramp_2391,
    agb_tramp_2392,  agb_tramp_2393,  agb_tramp_2394,  agb_tramp_2395,
    agb_tramp_2396,  agb_tramp_2397,  agb_tramp_2398,  agb_tramp_2399,
    agb_tramp_2400,  agb_tramp_2401,  agb_tramp_2402,  agb_tramp_2403,
    agb_tramp_2404,  agb_tramp_2405,  agb_tramp_2406,  agb_tramp_2407,
    agb_tramp_2408,  agb_tramp_2409,  agb_tramp_2410,  agb_tramp_2411,
    agb_tramp_2412,  agb_tramp_2413,  agb_tramp_2414,  agb_tramp_2415,
    agb_tramp_2416,  agb_tramp_2417,  agb_tramp_2418,  agb_tramp_2419,
    agb_tramp_2420,  agb_tramp_2421,  agb_tramp_2422,  agb_tramp_2423,
    agb_tramp_2424,  agb_tramp_2425,  agb_tramp_2426,  agb_tramp_2427,
    agb_tramp_2428,  agb_tramp_2429,  agb_tramp_2430,  agb_tramp_2431,
    agb_tramp_2432,  agb_tramp_2433,  agb_tramp_2434,  agb_tramp_2435,
    agb_tramp_2436,  agb_tramp_2437,  agb_tramp_2438,  agb_tramp_2439,
    agb_tramp_2440,  agb_tramp_2441,  agb_tramp_2442,  agb_tramp_2443,
    agb_tramp_2444,  agb_tramp_2445,  agb_tramp_2446,  agb_tramp_2447,
    agb_tramp_2448,  agb_tramp_2449,  agb_tramp_2450,  agb_tramp_2451,
    agb_tramp_2452,  agb_tramp_2453,  agb_tramp_2454,  agb_tramp_2455,
    agb_tramp_2456,  agb_tramp_2457,  agb_tramp_2458,  agb_tramp_2459,
    agb_tramp_2460,  agb_tramp_2461,  agb_tramp_2462,  agb_tramp_2463,
    agb_tramp_2464,  agb_tramp_2465,  agb_tramp_2466,  agb_tramp_2467,
    agb_tramp_2468,  agb_tramp_2469,  agb_tramp_2470,  agb_tramp_2471,
    agb_tramp_2472,  agb_tramp_2473,  agb_tramp_2474,  agb_tramp_2475,
    agb_tramp_2476,  agb_tramp_2477,  agb_tramp_2478,  agb_tramp_2479,
    agb_tramp_2480,  agb_tramp_2481,  agb_tramp_2482,  agb_tramp_2483,
    agb_tramp_2484,  agb_tramp_2485,  agb_tramp_2486,  agb_tramp_2487,
    agb_tramp_2488,  agb_tramp_2489,  agb_tramp_2490,  agb_tramp_2491,
    agb_tramp_2492,  agb_tramp_2493,  agb_tramp_2494,  agb_tramp_2495,
    agb_tramp_2496,  agb_tramp_2497,  agb_tramp_2498,  agb_tramp_2499,
    agb_tramp_2500,  agb_tramp_2501,  agb_tramp_2502,  agb_tramp_2503,
    agb_tramp_2504,  agb_tramp_2505,  agb_tramp_2506,  agb_tramp_2507,
    agb_tramp_2508,  agb_tramp_2509,  agb_tramp_2510,  agb_tramp_2511,
    agb_tramp_2512,  agb_tramp_2513,  agb_tramp_2514,  agb_tramp_2515,
    agb_tramp_2516,  agb_tramp_2517,  agb_tramp_2518,  agb_tramp_2519,
    agb_tramp_2520,  agb_tramp_2521,  agb_tramp_2522,  agb_tramp_2523,
    agb_tramp_2524,  agb_tramp_2525,  agb_tramp_2526,  agb_tramp_2527,
    agb_tramp_2528,  agb_tramp_2529,  agb_tramp_2530,  agb_tramp_2531,
    agb_tramp_2532,  agb_tramp_2533,  agb_tramp_2534,  agb_tramp_2535,
    agb_tramp_2536,  agb_tramp_2537,  agb_tramp_2538,  agb_tramp_2539,
    agb_tramp_2540,  agb_tramp_2541,  agb_tramp_2542,  agb_tramp_2543,
    agb_tramp_2544,  agb_tramp_2545,  agb_tramp_2546,  agb_tramp_2547,
    agb_tramp_2548,  agb_tramp_2549,  agb_tramp_2550,  agb_tramp_2551,
    agb_tramp_2552,  agb_tramp_2553,  agb_tramp_2554,  agb_tramp_2555,
    agb_tramp_2556,  agb_tramp_2557,  agb_tramp_2558,  agb_tramp_2559,
    agb_tramp_2560,  agb_tramp_2561,  agb_tramp_2562,  agb_tramp_2563,
    agb_tramp_2564,  agb_tramp_2565,  agb_tramp_2566,  agb_tramp_2567,
    agb_tramp_2568,  agb_tramp_2569,  agb_tramp_2570,  agb_tramp_2571,
    agb_tramp_2572,  agb_tramp_2573,  agb_tramp_2574,  agb_tramp_2575,
    agb_tramp_2576,  agb_tramp_2577,  agb_tramp_2578,  agb_tramp_2579,
    agb_tramp_2580,  agb_tramp_2581,  agb_tramp_2582,  agb_tramp_2583,
    agb_tramp_2584,  agb_tramp_2585,  agb_tramp_2586,  agb_tramp_2587,
    agb_tramp_2588,  agb_tramp_2589,  agb_tramp_2590,  agb_tramp_2591,
    agb_tramp_2592,  agb_tramp_2593,  agb_tramp_2594,  agb_tramp_2595,
    agb_tramp_2596,  agb_tramp_2597,  agb_tramp_2598,  agb_tramp_2599,
    agb_tramp_2600,  agb_tramp_2601,  agb_tramp_2602,  agb_tramp_2603,
    agb_tramp_2604,  agb_tramp_2605,  agb_tramp_2606,  agb_tramp_2607,
    agb_tramp_2608,  agb_tramp_2609,  agb_tramp_2610,  agb_tramp_2611,
    agb_tramp_2612,  agb_tramp_2613,  agb_tramp_2614,  agb_tramp_2615,
    agb_tramp_2616,  agb_tramp_2617,  agb_tramp_2618,  agb_tramp_2619,
    agb_tramp_2620,  agb_tramp_2621,  agb_tramp_2622,  agb_tramp_2623,
    agb_tramp_2624,  agb_tramp_2625,  agb_tramp_2626,  agb_tramp_2627,
    agb_tramp_2628,  agb_tramp_2629,  agb_tramp_2630,  agb_tramp_2631,
    agb_tramp_2632,  agb_tramp_2633,  agb_tramp_2634,  agb_tramp_2635,
    agb_tramp_2636,  agb_tramp_2637,  agb_tramp_2638,  agb_tramp_2639,
    agb_tramp_2640,  agb_tramp_2641,  agb_tramp_2642,  agb_tramp_2643,
    agb_tramp_2644,  agb_tramp_2645,  agb_tramp_2646,  agb_tramp_2647,
    agb_tramp_2648,  agb_tramp_2649,  agb_tramp_2650,  agb_tramp_2651,
    agb_tramp_2652,  agb_tramp_2653,  agb_tramp_2654,  agb_tramp_2655,
    agb_tramp_2656,  agb_tramp_2657,  agb_tramp_2658,  agb_tramp_2659,
    agb_tramp_2660,  agb_tramp_2661,  agb_tramp_2662,  agb_tramp_2663,
    agb_tramp_2664,  agb_tramp_2665,  agb_tramp_2666,  agb_tramp_2667,
    agb_tramp_2668,  agb_tramp_2669,  agb_tramp_2670,  agb_tramp_2671,
    agb_tramp_2672,  agb_tramp_2673,  agb_tramp_2674,  agb_tramp_2675,
    agb_tramp_2676,  agb_tramp_2677,  agb_tramp_2678,  agb_tramp_2679,
    agb_tramp_2680,  agb_tramp_2681,  agb_tramp_2682,  agb_tramp_2683,
    agb_tramp_2684,  agb_tramp_2685,  agb_tramp_2686,  agb_tramp_2687,
    agb_tramp_2688,  agb_tramp_2689,  agb_tramp_2690,  agb_tramp_2691,
    agb_tramp_2692,  agb_tramp_2693,  agb_tramp_2694,  agb_tramp_2695,
    agb_tramp_2696,  agb_tramp_2697,  agb_tramp_2698,  agb_tramp_2699,
    agb_tramp_2700,  agb_tramp_2701,  agb_tramp_2702,  agb_tramp_2703,
    agb_tramp_2704,  agb_tramp_2705,  agb_tramp_2706,  agb_tramp_2707,
    agb_tramp_2708,  agb_tramp_2709,  agb_tramp_2710,  agb_tramp_2711,
    agb_tramp_2712,  agb_tramp_2713,  agb_tramp_2714,  agb_tramp_2715,
    agb_tramp_2716,  agb_tramp_2717,  agb_tramp_2718,  agb_tramp_2719,
    agb_tramp_2720,  agb_tramp_2721,  agb_tramp_2722,  agb_tramp_2723,
    agb_tramp_2724,  agb_tramp_2725,  agb_tramp_2726,  agb_tramp_2727,
    agb_tramp_2728,  agb_tramp_2729,  agb_tramp_2730,  agb_tramp_2731,
    agb_tramp_2732,  agb_tramp_2733,  agb_tramp_2734,  agb_tramp_2735,
    agb_tramp_2736,  agb_tramp_2737,  agb_tramp_2738,  agb_tramp_2739,
    agb_tramp_2740,  agb_tramp_2741,  agb_tramp_2742,  agb_tramp_2743,
    agb_tramp_2744,  agb_tramp_2745,  agb_tramp_2746,  agb_tramp_2747,
    agb_tramp_2748,  agb_tramp_2749,  agb_tramp_2750,  agb_tramp_2751,
    agb_tramp_2752,  agb_tramp_2753,  agb_tramp_2754,  agb_tramp_2755,
    agb_tramp_2756,  agb_tramp_2757,  agb_tramp_2758,  agb_tramp_2759,
    agb_tramp_2760,  agb_tramp_2761,  agb_tramp_2762,  agb_tramp_2763,
    agb_tramp_2764,  agb_tramp_2765,  agb_tramp_2766,  agb_tramp_2767,
    agb_tramp_2768,  agb_tramp_2769,  agb_tramp_2770,  agb_tramp_2771,
    agb_tramp_2772,  agb_tramp_2773,  agb_tramp_2774,  agb_tramp_2775,
    agb_tramp_2776,  agb_tramp_2777,  agb_tramp_2778,  agb_tramp_2779,
    agb_tramp_2780,  agb_tramp_2781,  agb_tramp_2782,  agb_tramp_2783,
    agb_tramp_2784,  agb_tramp_2785,  agb_tramp_2786,  agb_tramp_2787,
    agb_tramp_2788,  agb_tramp_2789,  agb_tramp_2790,  agb_tramp_2791,
    agb_tramp_2792,  agb_tramp_2793,  agb_tramp_2794,  agb_tramp_2795,
    agb_tramp_2796,  agb_tramp_2797,  agb_tramp_2798,  agb_tramp_2799,
    agb_tramp_2800,  agb_tramp_2801,  agb_tramp_2802,  agb_tramp_2803,
    agb_tramp_2804,  agb_tramp_2805,  agb_tramp_2806,  agb_tramp_2807,
    agb_tramp_2808,  agb_tramp_2809,  agb_tramp_2810,  agb_tramp_2811,
    agb_tramp_2812,  agb_tramp_2813,  agb_tramp_2814,  agb_tramp_2815,
    agb_tramp_2816,  agb_tramp_2817,  agb_tramp_2818,  agb_tramp_2819,
    agb_tramp_2820,  agb_tramp_2821,  agb_tramp_2822,  agb_tramp_2823,
    agb_tramp_2824,  agb_tramp_2825,  agb_tramp_2826,  agb_tramp_2827,
    agb_tramp_2828,  agb_tramp_2829,  agb_tramp_2830,  agb_tramp_2831,
    agb_tramp_2832,  agb_tramp_2833,  agb_tramp_2834,  agb_tramp_2835,
    agb_tramp_2836,  agb_tramp_2837,  agb_tramp_2838,  agb_tramp_2839,
    agb_tramp_2840,  agb_tramp_2841,  agb_tramp_2842,  agb_tramp_2843,
    agb_tramp_2844,  agb_tramp_2845,  agb_tramp_2846,  agb_tramp_2847,
    agb_tramp_2848,  agb_tramp_2849,  agb_tramp_2850,  agb_tramp_2851,
    agb_tramp_2852,  agb_tramp_2853,  agb_tramp_2854,  agb_tramp_2855,
    agb_tramp_2856,  agb_tramp_2857,  agb_tramp_2858,  agb_tramp_2859,
    agb_tramp_2860,  agb_tramp_2861,  agb_tramp_2862,  agb_tramp_2863,
    agb_tramp_2864,  agb_tramp_2865,  agb_tramp_2866,  agb_tramp_2867,
    agb_tramp_2868,  agb_tramp_2869,  agb_tramp_2870,  agb_tramp_2871,
    agb_tramp_2872,  agb_tramp_2873,  agb_tramp_2874,  agb_tramp_2875,
    agb_tramp_2876,  agb_tramp_2877,  agb_tramp_2878,  agb_tramp_2879,
    agb_tramp_2880,  agb_tramp_2881,  agb_tramp_2882,  agb_tramp_2883,
    agb_tramp_2884,  agb_tramp_2885,  agb_tramp_2886,  agb_tramp_2887,
    agb_tramp_2888,  agb_tramp_2889,  agb_tramp_2890,  agb_tramp_2891,
    agb_tramp_2892,  agb_tramp_2893,  agb_tramp_2894,  agb_tramp_2895,
    agb_tramp_2896,  agb_tramp_2897,  agb_tramp_2898,  agb_tramp_2899,
    agb_tramp_2900,  agb_tramp_2901,  agb_tramp_2902,  agb_tramp_2903,
    agb_tramp_2904,  agb_tramp_2905,  agb_tramp_2906,  agb_tramp_2907,
    agb_tramp_2908,  agb_tramp_2909,  agb_tramp_2910,  agb_tramp_2911,
    agb_tramp_2912,  agb_tramp_2913,  agb_tramp_2914,  agb_tramp_2915,
    agb_tramp_2916,  agb_tramp_2917,  agb_tramp_2918,  agb_tramp_2919,
    agb_tramp_2920,  agb_tramp_2921,  agb_tramp_2922,  agb_tramp_2923,
    agb_tramp_2924,  agb_tramp_2925,  agb_tramp_2926,  agb_tramp_2927,
    agb_tramp_2928,  agb_tramp_2929,  agb_tramp_2930,  agb_tramp_2931,
    agb_tramp_2932,  agb_tramp_2933,  agb_tramp_2934,  agb_tramp_2935,
    agb_tramp_2936,  agb_tramp_2937,  agb_tramp_2938,  agb_tramp_2939,
    agb_tramp_2940,  agb_tramp_2941,  agb_tramp_2942,  agb_tramp_2943,
    agb_tramp_2944,  agb_tramp_2945,  agb_tramp_2946,  agb_tramp_2947,
    agb_tramp_2948,  agb_tramp_2949,  agb_tramp_2950,  agb_tramp_2951,
    agb_tramp_2952,  agb_tramp_2953,  agb_tramp_2954,  agb_tramp_2955,
    agb_tramp_2956,  agb_tramp_2957,  agb_tramp_2958,  agb_tramp_2959,
    agb_tramp_2960,  agb_tramp_2961,  agb_tramp_2962,  agb_tramp_2963,
    agb_tramp_2964,  agb_tramp_2965,  agb_tramp_2966,  agb_tramp_2967,
    agb_tramp_2968,  agb_tramp_2969,  agb_tramp_2970,  agb_tramp_2971,
    agb_tramp_2972,  agb_tramp_2973,  agb_tramp_2974,  agb_tramp_2975,
    agb_tramp_2976,  agb_tramp_2977,  agb_tramp_2978,  agb_tramp_2979,
    agb_tramp_2980,  agb_tramp_2981,  agb_tramp_2982,  agb_tramp_2983,
    agb_tramp_2984,  agb_tramp_2985,  agb_tramp_2986,  agb_tramp_2987,
    agb_tramp_2988,  agb_tramp_2989,  agb_tramp_2990,  agb_tramp_2991,
    agb_tramp_2992,  agb_tramp_2993,  agb_tramp_2994,  agb_tramp_2995,
    agb_tramp_2996,  agb_tramp_2997,  agb_tramp_2998,  agb_tramp_2999,
    agb_tramp_3000,  agb_tramp_3001,  agb_tramp_3002,  agb_tramp_3003,
    agb_tramp_3004,  agb_tramp_3005,  agb_tramp_3006,  agb_tramp_3007,
    agb_tramp_3008,  agb_tramp_3009,  agb_tramp_3010,  agb_tramp_3011,
    agb_tramp_3012,  agb_tramp_3013,  agb_tramp_3014,  agb_tramp_3015,
    agb_tramp_3016,  agb_tramp_3017,  agb_tramp_3018,  agb_tramp_3019,
    agb_tramp_3020,  agb_tramp_3021,  agb_tramp_3022,  agb_tramp_3023,
    agb_tramp_3024,  agb_tramp_3025,  agb_tramp_3026,  agb_tramp_3027,
    agb_tramp_3028,  agb_tramp_3029,  agb_tramp_3030,  agb_tramp_3031,
    agb_tramp_3032,  agb_tramp_3033,  agb_tramp_3034,  agb_tramp_3035,
    agb_tramp_3036,  agb_tramp_3037,  agb_tramp_3038,  agb_tramp_3039,
    agb_tramp_3040,  agb_tramp_3041,  agb_tramp_3042,  agb_tramp_3043,
    agb_tramp_3044,  agb_tramp_3045,  agb_tramp_3046,  agb_tramp_3047,
    agb_tramp_3048,  agb_tramp_3049,  agb_tramp_3050,  agb_tramp_3051,
    agb_tramp_3052,  agb_tramp_3053,  agb_tramp_3054,  agb_tramp_3055,
    agb_tramp_3056,  agb_tramp_3057,  agb_tramp_3058,  agb_tramp_3059,
    agb_tramp_3060,  agb_tramp_3061,  agb_tramp_3062,  agb_tramp_3063,
    agb_tramp_3064,  agb_tramp_3065,  agb_tramp_3066,  agb_tramp_3067,
    agb_tramp_3068,  agb_tramp_3069,  agb_tramp_3070,  agb_tramp_3071,
    agb_tramp_3072,  agb_tramp_3073,  agb_tramp_3074,  agb_tramp_3075,
    agb_tramp_3076,  agb_tramp_3077,  agb_tramp_3078,  agb_tramp_3079,
    agb_tramp_3080,  agb_tramp_3081,  agb_tramp_3082,  agb_tramp_3083,
    agb_tramp_3084,  agb_tramp_3085,  agb_tramp_3086,  agb_tramp_3087,
    agb_tramp_3088,  agb_tramp_3089,  agb_tramp_3090,  agb_tramp_3091,
    agb_tramp_3092,  agb_tramp_3093,  agb_tramp_3094,  agb_tramp_3095,
    agb_tramp_3096,  agb_tramp_3097,  agb_tramp_3098,  agb_tramp_3099,
    agb_tramp_3100,  agb_tramp_3101,  agb_tramp_3102,  agb_tramp_3103,
    agb_tramp_3104,  agb_tramp_3105,  agb_tramp_3106,  agb_tramp_3107,
    agb_tramp_3108,  agb_tramp_3109,  agb_tramp_3110,  agb_tramp_3111,
    agb_tramp_3112,  agb_tramp_3113,  agb_tramp_3114,  agb_tramp_3115,
    agb_tramp_3116,  agb_tramp_3117,  agb_tramp_3118,  agb_tramp_3119,
    agb_tramp_3120,  agb_tramp_3121,  agb_tramp_3122,  agb_tramp_3123,
    agb_tramp_3124,  agb_tramp_3125,  agb_tramp_3126,  agb_tramp_3127,
    agb_tramp_3128,  agb_tramp_3129,  agb_tramp_3130,  agb_tramp_3131,
    agb_tramp_3132,  agb_tramp_3133,  agb_tramp_3134,  agb_tramp_3135,
    agb_tramp_3136,  agb_tramp_3137,  agb_tramp_3138,  agb_tramp_3139,
    agb_tramp_3140,  agb_tramp_3141,  agb_tramp_3142,  agb_tramp_3143,
    agb_tramp_3144,  agb_tramp_3145,  agb_tramp_3146,  agb_tramp_3147,
    agb_tramp_3148,  agb_tramp_3149,  agb_tramp_3150,  agb_tramp_3151,
    agb_tramp_3152,  agb_tramp_3153,  agb_tramp_3154,  agb_tramp_3155,
    agb_tramp_3156,  agb_tramp_3157,  agb_tramp_3158,  agb_tramp_3159,
    agb_tramp_3160,  agb_tramp_3161,  agb_tramp_3162,  agb_tramp_3163,
    agb_tramp_3164,  agb_tramp_3165,  agb_tramp_3166,  agb_tramp_3167,
    agb_tramp_3168,  agb_tramp_3169,  agb_tramp_3170,  agb_tramp_3171,
    agb_tramp_3172,  agb_tramp_3173,  agb_tramp_3174,  agb_tramp_3175,
    agb_tramp_3176,  agb_tramp_3177,  agb_tramp_3178,  agb_tramp_3179,
    agb_tramp_3180,  agb_tramp_3181,  agb_tramp_3182,  agb_tramp_3183,
    agb_tramp_3184,  agb_tramp_3185,  agb_tramp_3186,  agb_tramp_3187,
    agb_tramp_3188,  agb_tramp_3189,  agb_tramp_3190,  agb_tramp_3191,
    agb_tramp_3192,  agb_tramp_3193,  agb_tramp_3194,  agb_tramp_3195,
    agb_tramp_3196,  agb_tramp_3197,  agb_tramp_3198,  agb_tramp_3199,
    agb_tramp_3200,  agb_tramp_3201,  agb_tramp_3202,  agb_tramp_3203,
    agb_tramp_3204,  agb_tramp_3205,  agb_tramp_3206,  agb_tramp_3207,
    agb_tramp_3208,  agb_tramp_3209,  agb_tramp_3210,  agb_tramp_3211,
    agb_tramp_3212,  agb_tramp_3213,  agb_tramp_3214,  agb_tramp_3215,
    agb_tramp_3216,  agb_tramp_3217,  agb_tramp_3218,  agb_tramp_3219,
    agb_tramp_3220,  agb_tramp_3221,  agb_tramp_3222,  agb_tramp_3223,
    agb_tramp_3224,  agb_tramp_3225,  agb_tramp_3226,  agb_tramp_3227,
    agb_tramp_3228,  agb_tramp_3229,  agb_tramp_3230,  agb_tramp_3231,
    agb_tramp_3232,  agb_tramp_3233,  agb_tramp_3234,  agb_tramp_3235,
    agb_tramp_3236,  agb_tramp_3237,  agb_tramp_3238,  agb_tramp_3239,
    agb_tramp_3240,  agb_tramp_3241,  agb_tramp_3242,  agb_tramp_3243,
    agb_tramp_3244,  agb_tramp_3245,  agb_tramp_3246,  agb_tramp_3247,
    agb_tramp_3248,  agb_tramp_3249,  agb_tramp_3250,  agb_tramp_3251,
    agb_tramp_3252,  agb_tramp_3253,  agb_tramp_3254,  agb_tramp_3255,
    agb_tramp_3256,  agb_tramp_3257,  agb_tramp_3258,  agb_tramp_3259,
    agb_tramp_3260,  agb_tramp_3261,  agb_tramp_3262,  agb_tramp_3263,
    agb_tramp_3264,  agb_tramp_3265,  agb_tramp_3266,  agb_tramp_3267,
    agb_tramp_3268,  agb_tramp_3269,  agb_tramp_3270,  agb_tramp_3271,
    agb_tramp_3272,  agb_tramp_3273,  agb_tramp_3274,  agb_tramp_3275,
    agb_tramp_3276,  agb_tramp_3277,  agb_tramp_3278,  agb_tramp_3279,
    agb_tramp_3280,  agb_tramp_3281,  agb_tramp_3282,  agb_tramp_3283,
    agb_tramp_3284,  agb_tramp_3285,  agb_tramp_3286,  agb_tramp_3287,
    agb_tramp_3288,  agb_tramp_3289,  agb_tramp_3290,  agb_tramp_3291,
    agb_tramp_3292,  agb_tramp_3293,  agb_tramp_3294,  agb_tramp_3295,
    agb_tramp_3296,  agb_tramp_3297,  agb_tramp_3298,  agb_tramp_3299,
    agb_tramp_3300,  agb_tramp_3301,  agb_tramp_3302,  agb_tramp_3303,
    agb_tramp_3304,  agb_tramp_3305,  agb_tramp_3306,  agb_tramp_3307,
    agb_tramp_3308,  agb_tramp_3309,  agb_tramp_3310,  agb_tramp_3311,
    agb_tramp_3312,  agb_tramp_3313,  agb_tramp_3314,  agb_tramp_3315,
    agb_tramp_3316,  agb_tramp_3317,  agb_tramp_3318,  agb_tramp_3319,
    agb_tramp_3320,  agb_tramp_3321,  agb_tramp_3322,  agb_tramp_3323,
    agb_tramp_3324,  agb_tramp_3325,  agb_tramp_3326,  agb_tramp_3327,
    agb_tramp_3328,  agb_tramp_3329,  agb_tramp_3330,  agb_tramp_3331,
    agb_tramp_3332,  agb_tramp_3333,  agb_tramp_3334,  agb_tramp_3335,
    agb_tramp_3336,  agb_tramp_3337,  agb_tramp_3338,  agb_tramp_3339,
    agb_tramp_3340,  agb_tramp_3341,  agb_tramp_3342,  agb_tramp_3343,
    agb_tramp_3344,  agb_tramp_3345,  agb_tramp_3346,  agb_tramp_3347,
    agb_tramp_3348,  agb_tramp_3349,  agb_tramp_3350,  agb_tramp_3351,
    agb_tramp_3352,  agb_tramp_3353,  agb_tramp_3354,  agb_tramp_3355,
    agb_tramp_3356,  agb_tramp_3357,  agb_tramp_3358,  agb_tramp_3359,
    agb_tramp_3360,  agb_tramp_3361,  agb_tramp_3362,  agb_tramp_3363,
    agb_tramp_3364,  agb_tramp_3365,  agb_tramp_3366,  agb_tramp_3367,
    agb_tramp_3368,  agb_tramp_3369,  agb_tramp_3370,  agb_tramp_3371,
    agb_tramp_3372,  agb_tramp_3373,  agb_tramp_3374,  agb_tramp_3375,
    agb_tramp_3376,  agb_tramp_3377,  agb_tramp_3378,  agb_tramp_3379,
    agb_tramp_3380,  agb_tramp_3381,  agb_tramp_3382,  agb_tramp_3383,
    agb_tramp_3384,  agb_tramp_3385,  agb_tramp_3386,  agb_tramp_3387,
    agb_tramp_3388,  agb_tramp_3389,  agb_tramp_3390,  agb_tramp_3391,
    agb_tramp_3392,  agb_tramp_3393,  agb_tramp_3394,  agb_tramp_3395,
    agb_tramp_3396,  agb_tramp_3397,  agb_tramp_3398,  agb_tramp_3399,
    agb_tramp_3400,  agb_tramp_3401,  agb_tramp_3402,  agb_tramp_3403,
    agb_tramp_3404,  agb_tramp_3405,  agb_tramp_3406,  agb_tramp_3407,
    agb_tramp_3408,  agb_tramp_3409,  agb_tramp_3410,  agb_tramp_3411,
    agb_tramp_3412,  agb_tramp_3413,  agb_tramp_3414,  agb_tramp_3415,
    agb_tramp_3416,  agb_tramp_3417,  agb_tramp_3418,  agb_tramp_3419,
    agb_tramp_3420,  agb_tramp_3421,  agb_tramp_3422,  agb_tramp_3423,
    agb_tramp_3424,  agb_tramp_3425,  agb_tramp_3426,  agb_tramp_3427,
    agb_tramp_3428,  agb_tramp_3429,  agb_tramp_3430,  agb_tramp_3431,
    agb_tramp_3432,  agb_tramp_3433,  agb_tramp_3434,  agb_tramp_3435,
    agb_tramp_3436,  agb_tramp_3437,  agb_tramp_3438,  agb_tramp_3439,
    agb_tramp_3440,  agb_tramp_3441,  agb_tramp_3442,  agb_tramp_3443,
    agb_tramp_3444,  agb_tramp_3445,  agb_tramp_3446,  agb_tramp_3447,
    agb_tramp_3448,  agb_tramp_3449,  agb_tramp_3450,  agb_tramp_3451,
    agb_tramp_3452,  agb_tramp_3453,  agb_tramp_3454,  agb_tramp_3455,
    agb_tramp_3456,  agb_tramp_3457,  agb_tramp_3458,  agb_tramp_3459,
    agb_tramp_3460,  agb_tramp_3461,  agb_tramp_3462,  agb_tramp_3463,
    agb_tramp_3464,  agb_tramp_3465,  agb_tramp_3466,  agb_tramp_3467,
    agb_tramp_3468,  agb_tramp_3469,  agb_tramp_3470,  agb_tramp_3471,
    agb_tramp_3472,  agb_tramp_3473,  agb_tramp_3474,  agb_tramp_3475,
    agb_tramp_3476,  agb_tramp_3477,  agb_tramp_3478,  agb_tramp_3479,
    agb_tramp_3480,  agb_tramp_3481,  agb_tramp_3482,  agb_tramp_3483,
    agb_tramp_3484,  agb_tramp_3485,  agb_tramp_3486,  agb_tramp_3487,
    agb_tramp_3488,  agb_tramp_3489,  agb_tramp_3490,  agb_tramp_3491,
    agb_tramp_3492,  agb_tramp_3493,  agb_tramp_3494,  agb_tramp_3495,
    agb_tramp_3496,  agb_tramp_3497,  agb_tramp_3498,  agb_tramp_3499,
    agb_tramp_3500,  agb_tramp_3501,  agb_tramp_3502,  agb_tramp_3503,
    agb_tramp_3504,  agb_tramp_3505,  agb_tramp_3506,  agb_tramp_3507,
    agb_tramp_3508,  agb_tramp_3509,  agb_tramp_3510,  agb_tramp_3511,
    agb_tramp_3512,  agb_tramp_3513,  agb_tramp_3514,  agb_tramp_3515,
    agb_tramp_3516,  agb_tramp_3517,  agb_tramp_3518,  agb_tramp_3519,
    agb_tramp_3520,  agb_tramp_3521,  agb_tramp_3522,  agb_tramp_3523,
    agb_tramp_3524,  agb_tramp_3525,  agb_tramp_3526,  agb_tramp_3527,
    agb_tramp_3528,  agb_tramp_3529,  agb_tramp_3530,  agb_tramp_3531,
    agb_tramp_3532,  agb_tramp_3533,  agb_tramp_3534,  agb_tramp_3535,
    agb_tramp_3536,  agb_tramp_3537,  agb_tramp_3538,  agb_tramp_3539,
    agb_tramp_3540,  agb_tramp_3541,  agb_tramp_3542,  agb_tramp_3543,
    agb_tramp_3544,  agb_tramp_3545,  agb_tramp_3546,  agb_tramp_3547,
    agb_tramp_3548,  agb_tramp_3549,  agb_tramp_3550,  agb_tramp_3551,
    agb_tramp_3552,  agb_tramp_3553,  agb_tramp_3554,  agb_tramp_3555,
    agb_tramp_3556,  agb_tramp_3557,  agb_tramp_3558,  agb_tramp_3559,
    agb_tramp_3560,  agb_tramp_3561,  agb_tramp_3562,  agb_tramp_3563,
    agb_tramp_3564,  agb_tramp_3565,  agb_tramp_3566,  agb_tramp_3567,
    agb_tramp_3568,  agb_tramp_3569,  agb_tramp_3570,  agb_tramp_3571,
    agb_tramp_3572,  agb_tramp_3573,  agb_tramp_3574,  agb_tramp_3575,
    agb_tramp_3576,  agb_tramp_3577,  agb_tramp_3578,  agb_tramp_3579,
    agb_tramp_3580,  agb_tramp_3581,  agb_tramp_3582,  agb_tramp_3583,
    agb_tramp_3584,  agb_tramp_3585,  agb_tramp_3586,  agb_tramp_3587,
    agb_tramp_3588,  agb_tramp_3589,  agb_tramp_3590,  agb_tramp_3591,
    agb_tramp_3592,  agb_tramp_3593,  agb_tramp_3594,  agb_tramp_3595,
    agb_tramp_3596,  agb_tramp_3597,  agb_tramp_3598,  agb_tramp_3599,
    agb_tramp_3600,  agb_tramp_3601,  agb_tramp_3602,  agb_tramp_3603,
    agb_tramp_3604,  agb_tramp_3605,  agb_tramp_3606,  agb_tramp_3607,
    agb_tramp_3608,  agb_tramp_3609,  agb_tramp_3610,  agb_tramp_3611,
    agb_tramp_3612,  agb_tramp_3613,  agb_tramp_3614,  agb_tramp_3615,
    agb_tramp_3616,  agb_tramp_3617,  agb_tramp_3618,  agb_tramp_3619,
    agb_tramp_3620,  agb_tramp_3621,  agb_tramp_3622,  agb_tramp_3623,
    agb_tramp_3624,  agb_tramp_3625,  agb_tramp_3626,  agb_tramp_3627,
    agb_tramp_3628,  agb_tramp_3629,  agb_tramp_3630,  agb_tramp_3631,
    agb_tramp_3632,  agb_tramp_3633,  agb_tramp_3634,  agb_tramp_3635,
    agb_tramp_3636,  agb_tramp_3637,  agb_tramp_3638,  agb_tramp_3639,
    agb_tramp_3640,  agb_tramp_3641,  agb_tramp_3642,  agb_tramp_3643,
    agb_tramp_3644,  agb_tramp_3645,  agb_tramp_3646,  agb_tramp_3647,
    agb_tramp_3648,  agb_tramp_3649,  agb_tramp_3650,  agb_tramp_3651,
    agb_tramp_3652,  agb_tramp_3653,  agb_tramp_3654,  agb_tramp_3655,
    agb_tramp_3656,  agb_tramp_3657,  agb_tramp_3658,  agb_tramp_3659,
    agb_tramp_3660,  agb_tramp_3661,  agb_tramp_3662,  agb_tramp_3663,
    agb_tramp_3664,  agb_tramp_3665,  agb_tramp_3666,  agb_tramp_3667,
    agb_tramp_3668,  agb_tramp_3669,  agb_tramp_3670,  agb_tramp_3671,
    agb_tramp_3672,  agb_tramp_3673,  agb_tramp_3674,  agb_tramp_3675,
    agb_tramp_3676,  agb_tramp_3677,  agb_tramp_3678,  agb_tramp_3679,
    agb_tramp_3680,  agb_tramp_3681,  agb_tramp_3682,  agb_tramp_3683,
    agb_tramp_3684,  agb_tramp_3685,  agb_tramp_3686,  agb_tramp_3687,
    agb_tramp_3688,  agb_tramp_3689,  agb_tramp_3690,  agb_tramp_3691,
    agb_tramp_3692,  agb_tramp_3693,  agb_tramp_3694,  agb_tramp_3695,
    agb_tramp_3696,  agb_tramp_3697,  agb_tramp_3698,  agb_tramp_3699,
    agb_tramp_3700,  agb_tramp_3701,  agb_tramp_3702,  agb_tramp_3703,
    agb_tramp_3704,  agb_tramp_3705,  agb_tramp_3706,  agb_tramp_3707,
    agb_tramp_3708,  agb_tramp_3709,  agb_tramp_3710,  agb_tramp_3711,
    agb_tramp_3712,  agb_tramp_3713,  agb_tramp_3714,  agb_tramp_3715,
    agb_tramp_3716,  agb_tramp_3717,  agb_tramp_3718,  agb_tramp_3719,
    agb_tramp_3720,  agb_tramp_3721,  agb_tramp_3722,  agb_tramp_3723,
    agb_tramp_3724,  agb_tramp_3725,  agb_tramp_3726,  agb_tramp_3727,
    agb_tramp_3728,  agb_tramp_3729,  agb_tramp_3730,  agb_tramp_3731,
    agb_tramp_3732,  agb_tramp_3733,  agb_tramp_3734,  agb_tramp_3735,
    agb_tramp_3736,  agb_tramp_3737,  agb_tramp_3738,  agb_tramp_3739,
    agb_tramp_3740,  agb_tramp_3741,  agb_tramp_3742,  agb_tramp_3743,
    agb_tramp_3744,  agb_tramp_3745,  agb_tramp_3746,  agb_tramp_3747,
    agb_tramp_3748,  agb_tramp_3749,  agb_tramp_3750,  agb_tramp_3751,
    agb_tramp_3752,  agb_tramp_3753,  agb_tramp_3754,  agb_tramp_3755,
    agb_tramp_3756,  agb_tramp_3757,  agb_tramp_3758,  agb_tramp_3759,
    agb_tramp_3760,  agb_tramp_3761,  agb_tramp_3762,  agb_tramp_3763,
    agb_tramp_3764,  agb_tramp_3765,  agb_tramp_3766,  agb_tramp_3767,
    agb_tramp_3768,  agb_tramp_3769,  agb_tramp_3770,  agb_tramp_3771,
    agb_tramp_3772,  agb_tramp_3773,  agb_tramp_3774,  agb_tramp_3775,
    agb_tramp_3776,  agb_tramp_3777,  agb_tramp_3778,  agb_tramp_3779,
    agb_tramp_3780,  agb_tramp_3781,  agb_tramp_3782,  agb_tramp_3783,
    agb_tramp_3784,  agb_tramp_3785,  agb_tramp_3786,  agb_tramp_3787,
    agb_tramp_3788,  agb_tramp_3789,  agb_tramp_3790,  agb_tramp_3791,
    agb_tramp_3792,  agb_tramp_3793,  agb_tramp_3794,  agb_tramp_3795,
    agb_tramp_3796,  agb_tramp_3797,  agb_tramp_3798,  agb_tramp_3799,
    agb_tramp_3800,  agb_tramp_3801,  agb_tramp_3802,  agb_tramp_3803,
    agb_tramp_3804,  agb_tramp_3805,  agb_tramp_3806,  agb_tramp_3807,
    agb_tramp_3808,  agb_tramp_3809,  agb_tramp_3810,  agb_tramp_3811,
    agb_tramp_3812,  agb_tramp_3813,  agb_tramp_3814,  agb_tramp_3815,
    agb_tramp_3816,  agb_tramp_3817,  agb_tramp_3818,  agb_tramp_3819,
    agb_tramp_3820,  agb_tramp_3821,  agb_tramp_3822,  agb_tramp_3823,
    agb_tramp_3824,  agb_tramp_3825,  agb_tramp_3826,  agb_tramp_3827,
    agb_tramp_3828,  agb_tramp_3829,  agb_tramp_3830,  agb_tramp_3831,
    agb_tramp_3832,  agb_tramp_3833,  agb_tramp_3834,  agb_tramp_3835,
    agb_tramp_3836,  agb_tramp_3837,  agb_tramp_3838,  agb_tramp_3839,
    agb_tramp_3840,  agb_tramp_3841,  agb_tramp_3842,  agb_tramp_3843,
    agb_tramp_3844,  agb_tramp_3845,  agb_tramp_3846,  agb_tramp_3847,
    agb_tramp_3848,  agb_tramp_3849,  agb_tramp_3850,  agb_tramp_3851,
    agb_tramp_3852,  agb_tramp_3853,  agb_tramp_3854,  agb_tramp_3855,
    agb_tramp_3856,  agb_tramp_3857,  agb_tramp_3858,  agb_tramp_3859,
    agb_tramp_3860,  agb_tramp_3861,  agb_tramp_3862,  agb_tramp_3863,
    agb_tramp_3864,  agb_tramp_3865,  agb_tramp_3866,  agb_tramp_3867,
    agb_tramp_3868,  agb_tramp_3869,  agb_tramp_3870,  agb_tramp_3871,
    agb_tramp_3872,  agb_tramp_3873,  agb_tramp_3874,  agb_tramp_3875,
    agb_tramp_3876,  agb_tramp_3877,  agb_tramp_3878,  agb_tramp_3879,
    agb_tramp_3880,  agb_tramp_3881,  agb_tramp_3882,  agb_tramp_3883,
    agb_tramp_3884,  agb_tramp_3885,  agb_tramp_3886,  agb_tramp_3887,
    agb_tramp_3888,  agb_tramp_3889,  agb_tramp_3890,  agb_tramp_3891,
    agb_tramp_3892,  agb_tramp_3893,  agb_tramp_3894,  agb_tramp_3895,
    agb_tramp_3896,  agb_tramp_3897,  agb_tramp_3898,  agb_tramp_3899,
    agb_tramp_3900,  agb_tramp_3901,  agb_tramp_3902,  agb_tramp_3903,
    agb_tramp_3904,  agb_tramp_3905,  agb_tramp_3906,  agb_tramp_3907,
    agb_tramp_3908,  agb_tramp_3909,  agb_tramp_3910,  agb_tramp_3911,
    agb_tramp_3912,  agb_tramp_3913,  agb_tramp_3914,  agb_tramp_3915,
    agb_tramp_3916,  agb_tramp_3917,  agb_tramp_3918,  agb_tramp_3919,
    agb_tramp_3920,  agb_tramp_3921,  agb_tramp_3922,  agb_tramp_3923,
    agb_tramp_3924,  agb_tramp_3925,  agb_tramp_3926,  agb_tramp_3927,
    agb_tramp_3928,  agb_tramp_3929,  agb_tramp_3930,  agb_tramp_3931,
    agb_tramp_3932,  agb_tramp_3933,  agb_tramp_3934,  agb_tramp_3935,
    agb_tramp_3936,  agb_tramp_3937,  agb_tramp_3938,  agb_tramp_3939,
    agb_tramp_3940,  agb_tramp_3941,  agb_tramp_3942,  agb_tramp_3943,
    agb_tramp_3944,  agb_tramp_3945,  agb_tramp_3946,  agb_tramp_3947,
    agb_tramp_3948,  agb_tramp_3949,  agb_tramp_3950,  agb_tramp_3951,
    agb_tramp_3952,  agb_tramp_3953,  agb_tramp_3954,  agb_tramp_3955,
    agb_tramp_3956,  agb_tramp_3957,  agb_tramp_3958,  agb_tramp_3959,
    agb_tramp_3960,  agb_tramp_3961,  agb_tramp_3962,  agb_tramp_3963,
    agb_tramp_3964,  agb_tramp_3965,  agb_tramp_3966,  agb_tramp_3967,
    agb_tramp_3968,  agb_tramp_3969,  agb_tramp_3970,  agb_tramp_3971,
    agb_tramp_3972,  agb_tramp_3973,  agb_tramp_3974,  agb_tramp_3975,
    agb_tramp_3976,  agb_tramp_3977,  agb_tramp_3978,  agb_tramp_3979,
    agb_tramp_3980,  agb_tramp_3981,  agb_tramp_3982,  agb_tramp_3983,
    agb_tramp_3984,  agb_tramp_3985,  agb_tramp_3986,  agb_tramp_3987,
    agb_tramp_3988,  agb_tramp_3989,  agb_tramp_3990,  agb_tramp_3991,
    agb_tramp_3992,  agb_tramp_3993,  agb_tramp_3994,  agb_tramp_3995,
    agb_tramp_3996,  agb_tramp_3997,  agb_tramp_3998,  agb_tramp_3999,
    agb_tramp_4000,  agb_tramp_4001,  agb_tramp_4002,  agb_tramp_4003,
    agb_tramp_4004,  agb_tramp_4005,  agb_tramp_4006,  agb_tramp_4007,
    agb_tramp_4008,  agb_tramp_4009,  agb_tramp_4010,  agb_tramp_4011,
    agb_tramp_4012,  agb_tramp_4013,  agb_tramp_4014,  agb_tramp_4015,
    agb_tramp_4016,  agb_tramp_4017,  agb_tramp_4018,  agb_tramp_4019,
    agb_tramp_4020,  agb_tramp_4021,  agb_tramp_4022,  agb_tramp_4023,
    agb_tramp_4024,  agb_tramp_4025,  agb_tramp_4026,  agb_tramp_4027,
    agb_tramp_4028,  agb_tramp_4029,  agb_tramp_4030,  agb_tramp_4031,
    agb_tramp_4032,  agb_tramp_4033,  agb_tramp_4034,  agb_tramp_4035,
    agb_tramp_4036,  agb_tramp_4037,  agb_tramp_4038,  agb_tramp_4039,
    agb_tramp_4040,  agb_tramp_4041,  agb_tramp_4042,  agb_tramp_4043,
    agb_tramp_4044,  agb_tramp_4045,  agb_tramp_4046,  agb_tramp_4047,
    agb_tramp_4048,  agb_tramp_4049,  agb_tramp_4050,  agb_tramp_4051,
    agb_tramp_4052,  agb_tramp_4053,  agb_tramp_4054,  agb_tramp_4055,
    agb_tramp_4056,  agb_tramp_4057,  agb_tramp_4058,  agb_tramp_4059,
    agb_tramp_4060,  agb_tramp_4061,  agb_tramp_4062,  agb_tramp_4063,
    agb_tramp_4064,  agb_tramp_4065,  agb_tramp_4066,  agb_tramp_4067,
    agb_tramp_4068,  agb_tramp_4069,  agb_tramp_4070,  agb_tramp_4071,
    agb_tramp_4072,  agb_tramp_4073,  agb_tramp_4074,  agb_tramp_4075,
    agb_tramp_4076,  agb_tramp_4077,  agb_tramp_4078,  agb_tramp_4079,
    agb_tramp_4080,  agb_tramp_4081,  agb_tramp_4082,  agb_tramp_4083,
    agb_tramp_4084,  agb_tramp_4085,  agb_tramp_4086,  agb_tramp_4087,
    agb_tramp_4088,  agb_tramp_4089,  agb_tramp_4090,  agb_tramp_4091,
    agb_tramp_4092,  agb_tramp_4093,  agb_tramp_4094,  agb_tramp_4095,
};
static ant_value_t agb_dispatch(int slot, ant_t* js, ant_value_t* args, int nargs,
                                ant_value_t call_new_target)
{
    agb_method_entry* e;
    AntbValue  self_v;
    AntbValue  arg_buf[16];
    AntbValue* arg_ptrs[16];
    AntbValue  out_v;
    ant_value_t thiz;
    AntbValue*  self_p = NULL;
    int i, n;

    (void)call_new_target;

    if (slot < 0 || slot >= g_method_count)
        return js_mkundef();
    e = &g_methods[slot];
    if (g_trace_calls < 0) g_trace_calls = getenv("AGB_JS_TRACE") ? 1 : 0;
    g_last_call = e->name;
    if (g_trace_calls) fprintf(stderr, "[call] %s nargs=%d\n", e->name, nargs);
    if (e->js != js) {
        return js_mkundef();
    }

    /* Single funnel for every JS -> C call: let the host switch to the runtime
     * state that owns this engine (per-view JS contexts). */
    if (g_context_activate && e->engine)
        g_context_activate(e->engine);

    thiz = js_getthis(js);
    if (vtype(thiz) != kTypeUndefined && vtype(thiz) != kTypeNull) {
        self_v.js = js;
        self_v.v  = thiz;
        self_p    = &self_v;
    }

    n = nargs < 16 ? nargs : 16;
    for (i = 0; i < n; i++) {
        arg_buf[i].js = js;
        arg_buf[i].v  = args[i];
        arg_ptrs[i]   = &arg_buf[i];
    }

    out_v.js = js;
    out_v.v  = js_mkundef();

    {
        int handled = e->fn(e->engine, self_p, arg_ptrs, n, &out_v, e->user);

        /* Paired with g_context_activate above, on both paths: the host has to
         * put its document/realm binding back before the JS that called this
         * method continues. */
        if (g_context_leave)
            g_context_leave(e->engine);

        if (!handled)
            return js_mkundef();
    }

    return out_v.v;
}

/* Frees the native-method slots of an engine that is going away.
 *
 * The table holds one entry per bridged method, and each entry owns one
 * trampoline (g_tramps[slot]). A slot may be reused as soon as its engine is
 * destroyed: the JS function values that point at that trampoline live in the
 * destroyed engine's heap, so no live caller can reach it any more.
 *
 * Leaving the slots allocated ran the table out after ~6 document reloads
 * (each realm installs ~170 methods), after which antb_define_method silently
 * did nothing: the next document got a window without its host API and a
 * `document` object with no methods, and the page's load listeners never
 * registered - the "reload until the page goes blank" report. */
static void agbReleaseEngineMethods(AntbEngine* engine)
{
    int i;

    if (!engine)
        return;

    for (i = 0; i < g_method_count; ++i) {
        agb_method_entry* e = &g_methods[i];

        if (e->fn && e->engine == engine) {
            e->engine = NULL;
            e->js     = NULL;
            e->obj    = 0;
            e->fn     = NULL;
            e->user   = NULL;
            e->name[0] = '\0';
        }
    }

    /* Give the tail back: the fresh-engine case starts from an empty table. */
    while (g_method_count > 0 && !g_methods[g_method_count - 1].fn)
        --g_method_count;
}

void antb_define_method(AntbEngine* engine, AntbValue* obj, const char* name,
                        AntbNativeFn fn, void* user)
{
    agb_method_entry* e;
    ant_value_t f;
    int slot = -1;
    int i;

    if (!engine || !obj || !name || !fn)
        return;

    /* Reuse slots freed by destroyed engines before growing the table. */
    for (i = 0; i < g_method_count; ++i) {
        if (!g_methods[i].fn) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        if (g_method_count >= AGB_TRAMPOLINE_COUNT)
            return;
        slot = g_method_count++;
    }

    e = &g_methods[slot];
    e->engine = engine;
    e->js   = engine->js;
    e->obj  = obj->v;
    e->fn   = fn;
    e->user = user;
    strncpy(e->name, name, sizeof(e->name) - 1);
    e->name[sizeof(e->name) - 1] = '\0';

    f = js_mkfun_dyn(g_tramps[slot]);
    /* js_set_function_name needs a real object; T_CFUNC rejects it */
    js_setprop(engine->js, obj->v, js_mkstr(engine->js, name, strlen(name)), f);
}

/* --- accessor properties ------------------------------------------------- */

/* defined further down (class property tables) */
static int agb_class_lookup_prop(AntbClass* k, const char* name,
                                 AntbGetterFn* fn, AntbSetterFn* setter,
                                 void** user);

static ant_value_t agb_getter_trampoline(ant_t* js, ant_value_t obj,
                                         const char* key, size_t klen)
{
    agb_getter_entry* entry = (agb_getter_entry*)js_get_native(obj, AGB_TAG_GETTER);
    AntbValue  self_v;
    AntbValue  out_v;
    AntbGetterFn fn = NULL;
    AntbSetterFn setter = NULL;
    void*      user = NULL;
    char name[128];

    if (!entry)
        return js_mkundef();

    if (klen >= sizeof(name))
        klen = sizeof(name) - 1;
    memcpy(name, key, klen);
    name[klen] = '\0';

    if (entry->klass) {
        if (!agb_class_lookup_prop(entry->klass, name, &fn, &setter, &user))
            return js_mkundef();
        /* A NULL getter is a "declined" name: the class has a prototype
         * accessor for it (antb_class_define_attribute), so answer undefined
         * and let ant continue into the prototype. */
        if (!fn)
            return js_mkundef();
    } else {
        fn   = entry->fn;
        user = entry->user;
        if (!fn)
            return js_mkundef();
    }

    self_v.js = js;
    self_v.v  = obj;
    out_v.js  = js;
    out_v.v   = js_mkundef();

    if (!fn(entry->engine, &self_v, name, &out_v, user))
        return js_mkundef();

    return out_v.v;
}

static bool agb_setter_trampoline(ant_t* js, ant_value_t obj,
                                  const char* key, size_t klen, ant_value_t value)
{
    agb_getter_entry* entry = (agb_getter_entry*)js_get_native(obj, AGB_TAG_GETTER);
    AntbValue self_v, val_v;
    AntbGetterFn getter = NULL;
    AntbSetterFn setter = NULL;
    void* user = NULL;
    char name[128];

    if (!entry)
        return false;

    if (klen >= sizeof(name))
        klen = sizeof(name) - 1;
    memcpy(name, key, klen);
    name[klen] = '\0';

    if (entry->klass) {
        if (!agb_class_lookup_prop(entry->klass, name, &getter, &setter, &user))
            return false;
    } else {
        setter = entry->setter;
        user   = entry->user;
    }

    if (!setter)
        return false;

    self_v.js = js;
    self_v.v  = obj;
    val_v.js  = js;
    val_v.v   = value;

    return setter(entry->engine, &self_v, name, &val_v, user) != 0;
}

void antb_define_property_rw(AntbEngine* engine, AntbValue* obj, const char* name,
                             AntbGetterFn getter, AntbSetterFn setter, void* user)
{
    agb_getter_entry* entry;

    if (!engine || !obj || !name)
        return;

    entry = (agb_getter_entry*)js_get_native(obj->v, AGB_TAG_GETTER);
    if (!entry) {
        entry = (agb_getter_entry*)calloc(1, sizeof(*entry));
        if (!entry)
            return;
        js_set_native(obj->v, entry, AGB_TAG_GETTER);
        js_set_getter(obj->v, agb_getter_trampoline);
        js_set_setter(obj->v, agb_setter_trampoline);
    }
    entry->engine = engine;
    entry->fn     = getter;
    entry->setter = setter;
    entry->user   = user;
    entry->klass  = NULL;   /* take over from the class level dispatcher */

    /* the object may already carry the class-installed hooks; the setter one
     * has to be (re)installed here or assignments would fall through */
    js_set_getter(obj->v, agb_getter_trampoline);
    js_set_setter(obj->v, agb_setter_trampoline);
}

void antb_define_property(AntbEngine* engine, AntbValue* obj, const char* name,
                          AntbGetterFn fn, void* user)
{
    agb_getter_entry* entry;

    if (!engine || !obj || !name || !fn)
        return;

    entry = (agb_getter_entry*)js_get_native(obj->v, AGB_TAG_GETTER);
    if (!entry) {
        entry = (agb_getter_entry*)calloc(1, sizeof(*entry));
        if (!entry)
            return;
        js_set_native(obj->v, entry, AGB_TAG_GETTER);
        js_set_getter(obj->v, agb_getter_trampoline);
    }
    entry->engine = engine;
    entry->fn   = fn;
    entry->user = user;
}

AntbValue* antb_global(AntbEngine* engine)
{
    if (!engine || !engine->js)
        return NULL;
    return alloc_value(engine->js, js_glob(engine->js));
}

AntbValue* antb_call(AntbEngine* engine, const AntbValue* fn, const AntbValue* self,
                     AntbValue** args, int argc, char** err)
{
    ant_value_t argv[16];
    ant_value_t this_v;
    ant_value_t r;
    int i;

    if (err)
        *err = NULL;
    if (!engine || !engine->js || !fn)
        return NULL;

    if (argc > 16)
        argc = 16;
    for (i = 0; i < argc; i++)
        argv[i] = args[i] ? args[i]->v : js_mkundef();

    this_v = (self && self->js) ? self->v : js_glob(engine->js);

    /* Cached: this runs on every antb_call and getenv() is a linear scan of the
     * process environment - the trace switches are read once per process. */
    if (g_call_debug < 0) g_call_debug = getenv("AGB_CALL_DEBUG") ? 1 : 0;
    if (g_call_debug)
        fprintf(stderr, "[call] enter fn=%p argc=%d\n", (void*)fn->v, argc);

    r = js_call_value(engine->js, fn->v, this_v, argv, argc);

    if (g_call_debug)
        fprintf(stderr, "[call] leave fn=%p\n", (void*)fn->v);
    if (is_err(r)) {
        copy_cstr(engine, "call failed", js_take_thrown(engine->js, r));
        if (err) {
            *err = (char*)malloc(strlen(engine->err) + 1);
            if (*err)
                strcpy(*err, engine->err);
        }
        return NULL;
    }
    return alloc_value(engine->js, r);
}

void antb_value_assign(AntbValue* out, const AntbValue* value)
{
    if (out && value) {
        out->js = value->js;
        out->v  = value->v;
    }
}

/* --- wrapper cache ------------------------------------------------------- */

#define AGB_MAX_WRAPPERS 65536

typedef struct {
    ant_t*      js;
    void*       realm;      /* the realm the wrapper object lives in */
    void*       ptr;
    unsigned    tag;
    void*       owner;      /* the document that owns the native object */
    ant_value_t value;   /* GC root: registered with gc_register_root() */
    int         used;
    int         next;    /* hash chain index+1 */
    int         freeNext;/* free list index+1 */
} agb_wrapper_entry;

static agb_wrapper_entry g_wrappers[AGB_MAX_WRAPPERS];
#define AGB_WRAPPER_HASH_SIZE 65521   /* prime */

static int                g_wrapperHash[AGB_WRAPPER_HASH_SIZE];   /* index+1 */
static int                g_wrapper_count = 0;   /* entries in use */
static int                g_wrapper_free  = 0;    /* free list head (index+1) */

static unsigned agbWrapperBucket(ant_t* js, void* realm, void* ptr, unsigned tag)
{
    unsigned long long h = ((unsigned long long)(size_t)ptr >> 4) ^ tag;

    h ^= (unsigned long long)(size_t)js >> 4;
    h ^= (unsigned long long)(size_t)realm >> 4;
    h *= 1099511628211ULL;
    return (unsigned)(h % AGB_WRAPPER_HASH_SIZE);
}

AntbValue* antb_wrap_native_owner(AntbEngine* engine, void* ptr, unsigned tag, void* owner)
{
    int idx;
    void* realm;

    if (!engine || !engine->js || !ptr)
        return NULL;

    /* One wrapper object PER REALM: the same DOM node seen from two frames is
     * two objects with two prototypes, as the spec requires (`parent.document`
     * accessed from an iframe is not the iframe's own wrapper). */
    realm = (void*)engine->js->realm;

    /* O(1) lookup: the old linear scan over every wrapper turned node-heavy
     * pages into O(n^2) (163.com spent minutes here). */
    {
        unsigned bucket = agbWrapperBucket(engine->js, realm, ptr, tag);
        int i = g_wrapperHash[bucket];

        while (i) {
            agb_wrapper_entry* e = &g_wrappers[i - 1];

            if (e->used && e->js == engine->js && e->realm == realm &&
                e->ptr == ptr && e->tag == tag)
                return alloc_value(engine->js, e->value);
            i = e->next;
        }
    }

    /* A free slot first: the table is process wide and an engine that died
     * gave its slots back. */
    if (g_wrapper_free) {
        idx = g_wrapper_free - 1;
        g_wrapper_free = g_wrappers[idx].freeNext;
    } else {
        if (g_wrapper_count >= AGB_MAX_WRAPPERS)
            return NULL;

        idx = g_wrapper_count++;
    }

    {
        agb_wrapper_entry* e = &g_wrappers[idx];
        ant_value_t obj = mkobj(engine->js, 0);

        js_set_native(obj, ptr, (uint32_t)tag);

        e->js    = engine->js;
        e->realm = realm;
        e->ptr   = ptr;
        e->tag   = tag;
        e->owner = owner;
        e->value = obj;
        e->used  = 1;
        e->freeNext = 0;

        /* keep the wrapper alive for the lifetime of the engine */
        gc_pin_permanent(e->js, e->value);

        {
            unsigned bucket = agbWrapperBucket(e->js, e->realm, e->ptr, e->tag);

            e->next = g_wrapperHash[bucket];
            g_wrapperHash[bucket] = idx + 1;
        }

        return alloc_value(engine->js, obj);
    }
}

AntbValue* antb_wrap_native(AntbEngine* engine, void* ptr, unsigned tag)
{
    return antb_wrap_native_owner(engine, ptr, tag, NULL);
}

AntbValue* antb_retain(const AntbValue* value)
{
    AntbValue* out;

    if (!value || !value->js)
        return NULL;

    out = alloc_value(value->js, value->v);
    if (!out)
        return NULL;

    gc_pin_permanent(value->js, out->v);
    return out;
}

int antb_to_boolean(const AntbValue* value)
{
    if (!value)
        return 0;
    return js_truthy(value->js, value->v) ? 1 : 0;
}

void antb_gc(AntbEngine* engine)
{
    if (engine && engine->js)
        gc_run(engine->js);
}
/* --- classes / prototypes ------------------------------------------------ *
 * A class owns a prototype object whose methods/properties are shared by all
 * wrappers of that class.
 *
 * The class registry is PER ENGINE and dynamic. It used to be a process-wide
 * array of 128 slots with a 32-entry property table per class; the DOM
 * interface layer needs ~120 classes per engine (every HTML element interface
 * is its own class), so the static table filled up after a single engine and
 * the second tab silently lost its DOM classes. The property table is dynamic
 * for the same reason: an interface can declare more than 32 attributes. */

struct AntbClassProp {
    char         name[64];
    AntbGetterFn getter;
    AntbSetterFn setter;
    void*        user;
};

struct AntbClass {
    AntbEngine*       engine;
    ant_t*            js;
    ant_value_t       proto;      /* GC root */
    struct AntbClass* next;       /* per-engine list */
    struct AntbClass* parent;     /* interface parent, NULL for a plain class */
    ant_value_t       ctor;       /* interface object; undefined for a plain class */
    struct AntbClassProp* props;  /* growable, owned */
    int               propCount;
    int               propCap;
    char              name[64];
};

static struct AntbClass* g_classes = NULL;

AntbClass* antb_class_create(AntbEngine* engine, const char* name)
{
    struct AntbClass* k;

    if (!engine || !engine->js || !name)
        return NULL;

    k = (struct AntbClass*)calloc(1, sizeof(struct AntbClass));
    if (!k)
        return NULL;

    k->engine = engine;
    k->js     = engine->js;
    k->proto  = mkobj(engine->js, 0);
    strncpy(k->name, name, sizeof(k->name) - 1);

    /* the prototype must outlive every instance */
    gc_pin_permanent(engine->js, k->proto);

    k->next = g_classes;
    g_classes = k;
    return k;
}

/* Releases the classes of an engine: the registry is process wide and an
 * engine that goes away must not leave its classes behind. */
static void agbReleaseEngineClasses(AntbEngine* engine)
{
    struct AntbClass** link = &g_classes;

    if (!engine || !engine->js)
        return;

    while (*link) {
        struct AntbClass* k = *link;

        if (k->js == engine->js) {
            *link = k->next;
            free(k->props);
            free(k);
            continue;
        }

        link = &k->next;
    }
}

/* --- interfaces (DOM bindings) ------------------------------------------- */

/* Every DOM interface constructor is non-constructible: WebIDL says
 * `new HTMLElement()` throws a TypeError, only the host can create one. */
static ant_value_t agb_iface_ctor(ant_params_t)
{
    (void)args;
    (void)nargs;
    (void)call_new_target;
    return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal constructor");
}

/* The interface constructor's cfunc metadata is keyed BY ADDRESS by ant's
 * process-global cfunc registry (ant_cfunc_meta_intern), and registry entries
 * are never removed. A per-class meta therefore breaks as soon as a class is
 * freed and a later engine's calloc() reuses the address: the registry then
 * hands back the old entry, whose name pointer belonged to the dead engine, and
 * creating the next constructor crashed in strlen on a navigation.
 *
 * One meta per interface NAME, allocated once and never freed, is stable for
 * the process; the pool is bounded by the number of interface names ever used
 * (a few hundred at most). */
typedef struct AgbIfaceMeta {
    ant_cfunc_meta_t    meta;
    char                name[64];
    struct AgbIfaceMeta* next;
} AgbIfaceMeta;

static AgbIfaceMeta* g_ifaceMetas = NULL;

static const ant_cfunc_meta_t* agb_iface_meta(const char* name)
{
    AgbIfaceMeta* e;

    for (e = g_ifaceMetas; e; e = e->next) {
        if (strcmp(e->name, name) == 0)
            return &e->meta;
    }

    e = (AgbIfaceMeta*)calloc(1, sizeof(*e));
    if (!e)
        return NULL;

    strncpy(e->name, name, sizeof(e->name) - 1);
    e->meta.fn     = agb_iface_ctor;
    e->meta.name   = e->name;
    e->meta.length = 0;
    e->meta.flags  = 0;
    e->next        = g_ifaceMetas;
    g_ifaceMetas   = e;
    return &e->meta;
}

static void agb_iface_make_ctor(struct AntbClass* k)
{
    ant_t* js = k->js;
    const ant_cfunc_meta_t* meta;
    ant_value_t cfunc;
    ant_value_t ctor;

    if (vtype(k->ctor) == kTypeFunction)
        return;

    meta = agb_iface_meta(k->name);
    if (!meta)
        return;

    cfunc = js_mkfun_meta(meta);
    if (is_err(cfunc))
        return;

    ctor = js_make_ctor_cfunc(js, cfunc, k->proto, 0);
    if (is_err(ctor))
        return;

    /* Interface objects inherit from their parent interface object, so
     * HTMLDivElement.ELEMENT_NODE resolves through HTMLElement -> ... -> Node. */
    js_set_proto_wb(js, ctor,
                    (k->parent && vtype(k->parent->ctor) == kTypeFunction)
                        ? k->parent->ctor : js->sym.function_proto);
    k->ctor = ctor;
}

AntbClass* antb_class_create_iface(AntbEngine* engine, const char* name,
                                   AntbClass* parent)
{
    struct AntbClass* k = antb_class_create(engine, name);

    if (!k)
        return NULL;

    k->parent = parent;
    js_set_proto_wb(k->js, k->proto,
                    parent ? parent->proto : k->js->sym.object_proto);
    mkprop(k->js, k->proto, k->js->sym.toStringTag_sym,
           js_mkstr(k->js, k->name, strlen(k->name)),
           ANT_PROP_ATTR_CONFIGURABLE);
    agb_iface_make_ctor(k);
    return k;
}

void antb_class_set_parent_value(AntbEngine* engine, AntbClass* klass,
                                 AntbValue* parent_proto, AntbValue* parent_ctor)
{
    if (!engine || !klass || !parent_proto)
        return;

    js_set_proto_wb(klass->js, klass->proto, parent_proto->v);

    if (parent_ctor && vtype(parent_ctor->v) == kTypeFunction) {
        if (vtype(klass->ctor) != kTypeFunction)
            agb_iface_make_ctor(klass);
        if (vtype(klass->ctor) == kTypeFunction)
            js_set_proto_wb(klass->js, klass->ctor, parent_ctor->v);
    }
}

void antb_class_add_constant(AntbEngine* engine, AntbClass* klass,
                             const char* name, double value)
{
    if (!engine || !klass || !name)
        return;

    if (vtype(klass->ctor) != kTypeFunction)
        agb_iface_make_ctor(klass);
    if (vtype(klass->ctor) != kTypeFunction)
        return;

    defhidden(klass->js, klass->ctor, name, strlen(name), js_mknum(value));
}

void antb_class_expose(AntbEngine* engine, AntbClass* klass)
{
    ant_t* js;
    ant_value_t global;
    ant_value_t existing;

    if (!engine || !klass || !klass->js)
        return;

    js = klass->js;
    if (vtype(klass->ctor) != kTypeFunction)
        return;

    global = js_glob(js);
    existing = js_get(js, global, klass->name);
    if (vtype(existing) != kTypeUndefined)
        return;

    js_set_global_builtin(js, klass->name, klass->ctor);
}

void antb_expose_classes(AntbEngine* engine)
{
    struct AntbClass* k;

    if (!engine || !engine->js)
        return;

    for (k = g_classes; k; k = k->next) {
        if (k->js == engine->js)
            antb_class_expose(engine, k);
    }
}

void antb_class_add_method(AntbEngine* engine, AntbClass* klass,
                           const char* name, AntbNativeFn fn, void* user)
{
    AntbValue proto;

    if (!engine || !klass || !name || !fn)
        return;

    proto.js = klass->js;
    proto.v  = klass->proto;
    antb_define_method(engine, &proto, name, fn, user);
}

void antb_class_add_property(AntbEngine* engine, AntbClass* klass,
                             const char* name, AntbGetterFn fn, void* user)
{
    /* stored in the class table; installed per instance by antb_wrap_native_as */
    antb_class_set_property(engine, klass, name, fn, user);
}

/* Takes an entry out of its hash chain and puts it on the free list. The GC
 * root of the entry stays: ant's GC has no unpin, so the root goes away with
 * the engine that owns it. */
static void agbWrapperRelease(int idx)
{
    agb_wrapper_entry* e = &g_wrappers[idx];
    unsigned bucket;
    int* link;

    if (!e->used)
        return;

    bucket = agbWrapperBucket(e->js, e->realm, e->ptr, e->tag);
    link = &g_wrapperHash[bucket];

    while (*link) {
        int cur = *link - 1;

        if (cur == idx) {
            *link = g_wrappers[cur].next;
            break;
        }

        link = &g_wrappers[cur].next;
    }

    /* Drop the permanent pin: a pinned wrapper object would keep its realm
     * marked (and therefore alive) for the lifetime of the engine, which
     * defeats per-frame realm collection. JS references keep it alive on
     * their own. */
    gc_unpin_permanent(e->js, e->value);

    e->used = 0;
    e->next = 0;
    e->freeNext = g_wrapper_free;
    g_wrapper_free = idx + 1;

    if (g_wrapper_count > 0)
        g_wrapper_count--;
}

/* Releases the wrappers of one owner (the document that owned the native
 * objects). Used when a document goes away: the cache must not hand out its
 * wrappers to the next document. */
void antb_release_owner(AntbEngine* engine, void* owner)
{
    int i;

    if (!engine || !engine->js || !owner)
        return;

    for (i = 0; i < AGB_MAX_WRAPPERS; ++i) {
        agb_wrapper_entry* e = &g_wrappers[i];

        if (e->used && e->js == engine->js && e->owner == owner)
            agbWrapperRelease(i);
    }
}

/* Releases the wrappers that belong to one realm: a frame's JS world is going
 * away and its wrapper objects (and their permanent pins) must not outlive it. */
void antb_release_realm(AntbEngine* engine, AntbRealm* realm)
{
    int i;

    if (!engine || !engine->js || !realm)
        return;

    for (i = 0; i < AGB_MAX_WRAPPERS; ++i) {
        agb_wrapper_entry* e = &g_wrappers[i];

        if (e->used && e->js == engine->js && e->realm == (void*)realm)
            agbWrapperRelease(i);
    }
}

/* Releases every wrapper of an engine: the tables are process wide, and an
 * engine that goes away must not leave entries behind (a stale js pointer, a
 * wrapper handed out again, a table that runs out of slots). */
static void agbReleaseEngineWrappers(AntbEngine* engine)
{
    int i;

    if (!engine || !engine->js)
        return;

    for (i = 0; i < AGB_MAX_WRAPPERS; ++i) {
        agb_wrapper_entry* e = &g_wrappers[i];

        if (e->used && e->js == engine->js)
            agbWrapperRelease(i);
    }
}

AntbValue* antb_wrap_native_as_owner(AntbEngine* engine, AntbClass* klass,
                                    void* ptr, unsigned tag, void* owner)
{
    AntbValue* v;

    if (!engine || !engine->js || !klass || !ptr)
        return NULL;

    v = antb_wrap_native_owner(engine, ptr, tag, owner);
    if (!v)
        return NULL;

    js_set_proto(engine->js, v->v, klass->proto);

    /* one dispatching getter per instance for the class' properties; only on
     * first wrap (the cache may hand back an already prepared wrapper) */
    if (!js_get_native(v->v, AGB_TAG_GETTER))
        antb_class_install_properties(engine, klass, v);

    return v;
}

AntbValue* antb_wrap_native_as(AntbEngine* engine, AntbClass* klass,
                               void* ptr, unsigned tag)
{
    return antb_wrap_native_as_owner(engine, klass, ptr, tag, NULL);
}
/* --- class property tables ----------------------------------------------- *
 * ant does not run an object's exotic getter when the property is found on a
 * prototype, so class level properties are installed as one dispatching
 * getter per instance instead. The table is per class and grows on demand. */

static int agb_class_lookup_prop(AntbClass* k, const char* name,
                                 AntbGetterFn* fn, AntbSetterFn* setter,
                                 void** user)
{
    struct AntbClass* c;
    int i;

    if (!k)
        return 0;

    /* Explicit names first, through the whole interface chain: a child's
     * catch-all must not shadow an attribute its parent declared. */
    for (c = k; c; c = c->parent) {
        for (i = 0; i < c->propCount; i++) {
            if (strcmp(c->props[i].name, name) == 0) {
                *fn     = c->props[i].getter;
                *setter = c->props[i].setter;
                *user   = c->props[i].user;
                return 1;
            }
        }
    }

    /* Catch-all entry (registered under the empty name) handles every name
     * the chain does not list explicitly; the getter itself dispatches. */
    for (c = k; c; c = c->parent) {
        for (i = 0; i < c->propCount; i++) {
            if (c->props[i].name[0] == '\0') {
                *fn     = c->props[i].getter;
                *setter = c->props[i].setter;
                *user   = c->props[i].user;
                return 1;
            }
        }
    }
    return 0;
}

int antb_class_set_property_rw(AntbEngine* engine, AntbClass* klass,
                               const char* name, AntbGetterFn getter,
                               AntbSetterFn setter, void* user)
{
    int n;

    (void)engine;

    if (!klass || !name || !getter)
        return 0;

    if (klass->propCount == klass->propCap) {
        int cap = klass->propCap ? klass->propCap * 2 : 8;
        struct AntbClassProp* grown = (struct AntbClassProp*)realloc(
            klass->props, (size_t)cap * sizeof(struct AntbClassProp));

        if (!grown)
            return 0;
        klass->props   = grown;
        klass->propCap = cap;
    }

    n = klass->propCount;
    memset(&klass->props[n], 0, sizeof(klass->props[n]));
    strncpy(klass->props[n].name, name, sizeof(klass->props[n].name) - 1);
    klass->props[n].getter = getter;
    klass->props[n].setter = setter;
    klass->props[n].user   = user;
    klass->propCount = n + 1;
    return 1;
}

int antb_class_set_property(AntbEngine* engine, AntbClass* klass,
                            const char* name, AntbGetterFn fn, void* user)
{
    return antb_class_set_property_rw(engine, klass, name, fn, NULL, user);
}

/* Appends a class property entry; `getter` may be NULL, which marks the name
 * as "declined": the catch-all must not answer it (the prototype accessor
 * does). Used by antb_class_define_attribute(). */
static int agb_class_add_prop(AntbClass* klass, const char* name,
                              AntbGetterFn getter, AntbSetterFn setter, void* user)
{
    int n;

    if (!klass || !name || !name[0])
        return 0;

    if (klass->propCount == klass->propCap) {
        int cap = klass->propCap ? klass->propCap * 2 : 8;
        struct AntbClassProp* grown = (struct AntbClassProp*)realloc(
            klass->props, (size_t)cap * sizeof(struct AntbClassProp));

        if (!grown)
            return 0;
        klass->props   = grown;
        klass->propCap = cap;
    }

    n = klass->propCount;
    memset(&klass->props[n], 0, sizeof(klass->props[n]));
    strncpy(klass->props[n].name, name, sizeof(klass->props[n].name) - 1);
    klass->props[n].getter = getter;
    klass->props[n].setter = setter;
    klass->props[n].user   = user;
    klass->propCount = n + 1;
    return 1;
}

/* --- interface attributes (WebIDL accessors) ----------------------------- */

#define AGB_TAG_ATTR 0x41545452u  /* 'ATTR' */

typedef struct {
    AntbEngine*  engine;
    AntbGetterFn getter;
    AntbSetterFn setter;
    void*        user;
    char         name[96];
} agb_attr_entry;

static agb_attr_entry* agb_attr_entry_of(ant_t* js)
{
    if (!js || !js->current_func)
        return NULL;
    return (agb_attr_entry*)js_get_native(js->current_func, AGB_TAG_ATTR);
}

static ant_value_t agb_attr_get_cfunc(ant_params_t)
{
    agb_attr_entry* e = agb_attr_entry_of(js);
    AntbValue self_v;
    AntbValue out_v;

    if (!e || !e->getter)
        return js_mkundef();

    self_v.js = js;
    self_v.v  = js->this_val;
    out_v.js  = js;
    out_v.v   = js_mkundef();

    if (!e->getter(e->engine, &self_v, e->name, &out_v, e->user))
        return js_mkundef();

    return out_v.v;
}

static ant_value_t agb_attr_set_cfunc(ant_params_t)
{
    agb_attr_entry* e = agb_attr_entry_of(js);
    AntbValue self_v;
    AntbValue val_v;

    if (!e || !e->setter || nargs < 1)
        return js_mkundef();

    self_v.js = js;
    self_v.v  = js->this_val;
    val_v.js  = js;
    val_v.v   = args[0];

    e->setter(e->engine, &self_v, e->name, &val_v, e->user);
    return js_mkundef();
}

int antb_class_define_attribute(AntbEngine* engine, AntbClass* klass, const char* name,
                                AntbGetterFn getter, AntbSetterFn setter, void* user)
{
    agb_attr_entry* e;
    ant_value_t getfn;
    ant_value_t setfn;

    if (!engine || !engine->js || !klass || !klass->proto || !name || !name[0] || !getter)
        return 0;

    /* Register the decline first: the class dispatch answers undefined for the
     * name, and ant continues the ordinary lookup into the prototype. */
    if (!agb_class_add_prop(klass, name, NULL, NULL, NULL))
        return 0;

    e = (agb_attr_entry*)calloc(1, sizeof(*e));
    if (!e)
        return 0;

    e->engine = engine;
    e->getter = getter;
    e->setter = setter;
    e->user   = user;
    strncpy(e->name, name, sizeof(e->name) - 1);

    getfn = js_heavy_mkfun_native(engine->js, agb_attr_get_cfunc, e, AGB_TAG_ATTR);
    setfn = setter ? js_heavy_mkfun_native(engine->js, agb_attr_set_cfunc, e, AGB_TAG_ATTR)
                   : js_mkundef();

    js_set_accessor_desc(engine->js, klass->proto, name, strlen(name), getfn, setfn,
                         JS_DESC_E | JS_DESC_C);
    return 1;
}

void antb_class_install_properties(AntbEngine* engine, AntbClass* klass, AntbValue* instance)
{
    agb_getter_entry* entry;

    if (!engine || !klass || !instance)
        return;

    entry = (agb_getter_entry*)calloc(1, sizeof(*entry));
    if (!entry)
        return;

    entry->engine = engine;
    entry->klass  = klass;
    entry->fn     = NULL;

    js_set_native(instance->v, entry, AGB_TAG_GETTER);
    js_set_getter(instance->v, agb_getter_trampoline);

    /* Writes must reach the class table too: a wrapper whose only write path
     * is the property store would turn `el.id = x` into an expando. The
     * trampoline answers false for names the class does not handle, and the
     * store then runs normally. */
    js_set_setter(instance->v, agb_setter_trampoline);
}








