/*
 * AntBridge.c - the only translation unit that includes Ant's GNU C23 headers.
 * Everything above this file (agave's C++) sees AntBridge.h only.
 */

#include "AntBridge.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "ant.h"
#include "esm/loader.h"
#include "internal.h"
#include "cfunc.h"     /* js_mkfun_dyn */
#include "reactor.h"   /* ant_uv_run, js_poll_events */
#include "runtime.h"
#include "bootstrap.h"
#include "ptr.h"        /* js_set_native / js_get_native (static inline) */
#include "gc/roots.h"   /* gc_register_root: keep wrapper objects alive */
#include "gc.h"         /* gc_run: forced collection in tests */

struct AntbEngine {
    ant_t* js;
    char   err[512];
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

    {
        char* argv[2];
        argv[0] = (char*)"agave";
        argv[1] = NULL;
        ant_runtime_init(e->js, 1, argv, NULL);
    }
    ant_bootstrap_modules(e->js);
    js_set_filename(e->js, "agave://page.js");

    return e;
}

void antb_engine_destroy(AntbEngine* engine)
{
    if (!engine)
        return;
    if (engine->js)
        js_destroy(engine->js);
    free(engine);
}

void antb_set_filename(AntbEngine* engine, const char* name)
{
    if (engine && engine->js && name)
        js_set_filename(engine->js, name);
}

AntbValue* antb_eval(AntbEngine* engine, const char* src, int len, char** err)
{
    ant_value_t v;

    if (err)
        *err = NULL;
    if (!engine || !engine->js || !src || len < 0)
        return NULL;

    v = js_eval_bytecode_eval(engine->js, src, (size_t)len);

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

    if (filename && filename[0]) {
        js_setup_import_meta(engine->js, filename);
        js_set_filename(engine->js, filename);
    }

    v = js_eval_bytecode_module(engine->js, src, (size_t)len);

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

void antb_set_module_fetcher(AntbModuleFetcher fn)
{
    esm_set_http_fetch((esm_http_fetch_fn)fn);
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

AntbValue* antb_make_number(AntbEngine* engine, double v)
{
    if (!engine || !engine->js)
        return NULL;
    return alloc_value(engine->js, js_mknum(v));
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
#define AGB_TRAMPOLINE_COUNT 1024

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

void antb_set_context_activate(void (*fn)(AntbEngine* engine))
{
    g_context_activate = fn;
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

AGB_TRAMP_DECL(0)  AGB_TRAMP_DECL(1)  AGB_TRAMP_DECL(2)  AGB_TRAMP_DECL(3)
AGB_TRAMP_DECL(4)  AGB_TRAMP_DECL(5)  AGB_TRAMP_DECL(6)  AGB_TRAMP_DECL(7)
AGB_TRAMP_DECL(8)  AGB_TRAMP_DECL(9)  AGB_TRAMP_DECL(10)  AGB_TRAMP_DECL(11)
AGB_TRAMP_DECL(12)  AGB_TRAMP_DECL(13)  AGB_TRAMP_DECL(14)  AGB_TRAMP_DECL(15)
AGB_TRAMP_DECL(16)  AGB_TRAMP_DECL(17)  AGB_TRAMP_DECL(18)  AGB_TRAMP_DECL(19)
AGB_TRAMP_DECL(20)  AGB_TRAMP_DECL(21)  AGB_TRAMP_DECL(22)  AGB_TRAMP_DECL(23)
AGB_TRAMP_DECL(24)  AGB_TRAMP_DECL(25)  AGB_TRAMP_DECL(26)  AGB_TRAMP_DECL(27)
AGB_TRAMP_DECL(28)  AGB_TRAMP_DECL(29)  AGB_TRAMP_DECL(30)  AGB_TRAMP_DECL(31)
AGB_TRAMP_DECL(32)  AGB_TRAMP_DECL(33)  AGB_TRAMP_DECL(34)  AGB_TRAMP_DECL(35)
AGB_TRAMP_DECL(36)  AGB_TRAMP_DECL(37)  AGB_TRAMP_DECL(38)  AGB_TRAMP_DECL(39)
AGB_TRAMP_DECL(40)  AGB_TRAMP_DECL(41)  AGB_TRAMP_DECL(42)  AGB_TRAMP_DECL(43)
AGB_TRAMP_DECL(44)  AGB_TRAMP_DECL(45)  AGB_TRAMP_DECL(46)  AGB_TRAMP_DECL(47)
AGB_TRAMP_DECL(48)  AGB_TRAMP_DECL(49)  AGB_TRAMP_DECL(50)  AGB_TRAMP_DECL(51)
AGB_TRAMP_DECL(52)  AGB_TRAMP_DECL(53)  AGB_TRAMP_DECL(54)  AGB_TRAMP_DECL(55)
AGB_TRAMP_DECL(56)  AGB_TRAMP_DECL(57)  AGB_TRAMP_DECL(58)  AGB_TRAMP_DECL(59)
AGB_TRAMP_DECL(60)  AGB_TRAMP_DECL(61)  AGB_TRAMP_DECL(62)  AGB_TRAMP_DECL(63)
AGB_TRAMP_DECL(64)  AGB_TRAMP_DECL(65)  AGB_TRAMP_DECL(66)  AGB_TRAMP_DECL(67)
AGB_TRAMP_DECL(68)  AGB_TRAMP_DECL(69)  AGB_TRAMP_DECL(70)  AGB_TRAMP_DECL(71)
AGB_TRAMP_DECL(72)  AGB_TRAMP_DECL(73)  AGB_TRAMP_DECL(74)  AGB_TRAMP_DECL(75)
AGB_TRAMP_DECL(76)  AGB_TRAMP_DECL(77)  AGB_TRAMP_DECL(78)  AGB_TRAMP_DECL(79)
AGB_TRAMP_DECL(80)  AGB_TRAMP_DECL(81)  AGB_TRAMP_DECL(82)  AGB_TRAMP_DECL(83)
AGB_TRAMP_DECL(84)  AGB_TRAMP_DECL(85)  AGB_TRAMP_DECL(86)  AGB_TRAMP_DECL(87)
AGB_TRAMP_DECL(88)  AGB_TRAMP_DECL(89)  AGB_TRAMP_DECL(90)  AGB_TRAMP_DECL(91)
AGB_TRAMP_DECL(92)  AGB_TRAMP_DECL(93)  AGB_TRAMP_DECL(94)  AGB_TRAMP_DECL(95)
AGB_TRAMP_DECL(96)  AGB_TRAMP_DECL(97)  AGB_TRAMP_DECL(98)  AGB_TRAMP_DECL(99)
AGB_TRAMP_DECL(100)  AGB_TRAMP_DECL(101)  AGB_TRAMP_DECL(102)  AGB_TRAMP_DECL(103)
AGB_TRAMP_DECL(104)  AGB_TRAMP_DECL(105)  AGB_TRAMP_DECL(106)  AGB_TRAMP_DECL(107)
AGB_TRAMP_DECL(108)  AGB_TRAMP_DECL(109)  AGB_TRAMP_DECL(110)  AGB_TRAMP_DECL(111)
AGB_TRAMP_DECL(112)  AGB_TRAMP_DECL(113)  AGB_TRAMP_DECL(114)  AGB_TRAMP_DECL(115)
AGB_TRAMP_DECL(116)  AGB_TRAMP_DECL(117)  AGB_TRAMP_DECL(118)  AGB_TRAMP_DECL(119)
AGB_TRAMP_DECL(120)  AGB_TRAMP_DECL(121)  AGB_TRAMP_DECL(122)  AGB_TRAMP_DECL(123)
AGB_TRAMP_DECL(124)  AGB_TRAMP_DECL(125)  AGB_TRAMP_DECL(126)  AGB_TRAMP_DECL(127)
AGB_TRAMP_DECL(128)  AGB_TRAMP_DECL(129)  AGB_TRAMP_DECL(130)  AGB_TRAMP_DECL(131)
AGB_TRAMP_DECL(132)  AGB_TRAMP_DECL(133)  AGB_TRAMP_DECL(134)  AGB_TRAMP_DECL(135)
AGB_TRAMP_DECL(136)  AGB_TRAMP_DECL(137)  AGB_TRAMP_DECL(138)  AGB_TRAMP_DECL(139)
AGB_TRAMP_DECL(140)  AGB_TRAMP_DECL(141)  AGB_TRAMP_DECL(142)  AGB_TRAMP_DECL(143)
AGB_TRAMP_DECL(144)  AGB_TRAMP_DECL(145)  AGB_TRAMP_DECL(146)  AGB_TRAMP_DECL(147)
AGB_TRAMP_DECL(148)  AGB_TRAMP_DECL(149)  AGB_TRAMP_DECL(150)  AGB_TRAMP_DECL(151)
AGB_TRAMP_DECL(152)  AGB_TRAMP_DECL(153)  AGB_TRAMP_DECL(154)  AGB_TRAMP_DECL(155)
AGB_TRAMP_DECL(156)  AGB_TRAMP_DECL(157)  AGB_TRAMP_DECL(158)  AGB_TRAMP_DECL(159)
AGB_TRAMP_DECL(160)  AGB_TRAMP_DECL(161)  AGB_TRAMP_DECL(162)  AGB_TRAMP_DECL(163)
AGB_TRAMP_DECL(164)  AGB_TRAMP_DECL(165)  AGB_TRAMP_DECL(166)  AGB_TRAMP_DECL(167)
AGB_TRAMP_DECL(168)  AGB_TRAMP_DECL(169)  AGB_TRAMP_DECL(170)  AGB_TRAMP_DECL(171)
AGB_TRAMP_DECL(172)  AGB_TRAMP_DECL(173)  AGB_TRAMP_DECL(174)  AGB_TRAMP_DECL(175)
AGB_TRAMP_DECL(176)  AGB_TRAMP_DECL(177)  AGB_TRAMP_DECL(178)  AGB_TRAMP_DECL(179)
AGB_TRAMP_DECL(180)  AGB_TRAMP_DECL(181)  AGB_TRAMP_DECL(182)  AGB_TRAMP_DECL(183)
AGB_TRAMP_DECL(184)  AGB_TRAMP_DECL(185)  AGB_TRAMP_DECL(186)  AGB_TRAMP_DECL(187)
AGB_TRAMP_DECL(188)  AGB_TRAMP_DECL(189)  AGB_TRAMP_DECL(190)  AGB_TRAMP_DECL(191)
AGB_TRAMP_DECL(192)  AGB_TRAMP_DECL(193)  AGB_TRAMP_DECL(194)  AGB_TRAMP_DECL(195)
AGB_TRAMP_DECL(196)  AGB_TRAMP_DECL(197)  AGB_TRAMP_DECL(198)  AGB_TRAMP_DECL(199)
AGB_TRAMP_DECL(200)  AGB_TRAMP_DECL(201)  AGB_TRAMP_DECL(202)  AGB_TRAMP_DECL(203)
AGB_TRAMP_DECL(204)  AGB_TRAMP_DECL(205)  AGB_TRAMP_DECL(206)  AGB_TRAMP_DECL(207)
AGB_TRAMP_DECL(208)  AGB_TRAMP_DECL(209)  AGB_TRAMP_DECL(210)  AGB_TRAMP_DECL(211)
AGB_TRAMP_DECL(212)  AGB_TRAMP_DECL(213)  AGB_TRAMP_DECL(214)  AGB_TRAMP_DECL(215)
AGB_TRAMP_DECL(216)  AGB_TRAMP_DECL(217)  AGB_TRAMP_DECL(218)  AGB_TRAMP_DECL(219)
AGB_TRAMP_DECL(220)  AGB_TRAMP_DECL(221)  AGB_TRAMP_DECL(222)  AGB_TRAMP_DECL(223)
AGB_TRAMP_DECL(224)  AGB_TRAMP_DECL(225)  AGB_TRAMP_DECL(226)  AGB_TRAMP_DECL(227)
AGB_TRAMP_DECL(228)  AGB_TRAMP_DECL(229)  AGB_TRAMP_DECL(230)  AGB_TRAMP_DECL(231)
AGB_TRAMP_DECL(232)  AGB_TRAMP_DECL(233)  AGB_TRAMP_DECL(234)  AGB_TRAMP_DECL(235)
AGB_TRAMP_DECL(236)  AGB_TRAMP_DECL(237)  AGB_TRAMP_DECL(238)  AGB_TRAMP_DECL(239)
AGB_TRAMP_DECL(240)  AGB_TRAMP_DECL(241)  AGB_TRAMP_DECL(242)  AGB_TRAMP_DECL(243)
AGB_TRAMP_DECL(244)  AGB_TRAMP_DECL(245)  AGB_TRAMP_DECL(246)  AGB_TRAMP_DECL(247)
AGB_TRAMP_DECL(248)  AGB_TRAMP_DECL(249)  AGB_TRAMP_DECL(250)  AGB_TRAMP_DECL(251)
AGB_TRAMP_DECL(252)  AGB_TRAMP_DECL(253)  AGB_TRAMP_DECL(254)  AGB_TRAMP_DECL(255)
AGB_TRAMP_DECL(256)
AGB_TRAMP_DECL(257)
AGB_TRAMP_DECL(258)
AGB_TRAMP_DECL(259)

AGB_TRAMP_DECL(260)
AGB_TRAMP_DECL(261)
AGB_TRAMP_DECL(262)
AGB_TRAMP_DECL(263)

AGB_TRAMP_DECL(264)
AGB_TRAMP_DECL(265)
AGB_TRAMP_DECL(266)
AGB_TRAMP_DECL(267)

AGB_TRAMP_DECL(268)
AGB_TRAMP_DECL(269)
AGB_TRAMP_DECL(270)
AGB_TRAMP_DECL(271)

AGB_TRAMP_DECL(272)
AGB_TRAMP_DECL(273)
AGB_TRAMP_DECL(274)
AGB_TRAMP_DECL(275)

AGB_TRAMP_DECL(276)
AGB_TRAMP_DECL(277)
AGB_TRAMP_DECL(278)
AGB_TRAMP_DECL(279)

AGB_TRAMP_DECL(280)
AGB_TRAMP_DECL(281)
AGB_TRAMP_DECL(282)
AGB_TRAMP_DECL(283)

AGB_TRAMP_DECL(284)
AGB_TRAMP_DECL(285)
AGB_TRAMP_DECL(286)
AGB_TRAMP_DECL(287)

AGB_TRAMP_DECL(288)
AGB_TRAMP_DECL(289)
AGB_TRAMP_DECL(290)
AGB_TRAMP_DECL(291)

AGB_TRAMP_DECL(292)
AGB_TRAMP_DECL(293)
AGB_TRAMP_DECL(294)
AGB_TRAMP_DECL(295)

AGB_TRAMP_DECL(296)
AGB_TRAMP_DECL(297)
AGB_TRAMP_DECL(298)
AGB_TRAMP_DECL(299)

AGB_TRAMP_DECL(300)
AGB_TRAMP_DECL(301)
AGB_TRAMP_DECL(302)
AGB_TRAMP_DECL(303)

AGB_TRAMP_DECL(304)
AGB_TRAMP_DECL(305)
AGB_TRAMP_DECL(306)
AGB_TRAMP_DECL(307)

AGB_TRAMP_DECL(308)
AGB_TRAMP_DECL(309)
AGB_TRAMP_DECL(310)
AGB_TRAMP_DECL(311)

AGB_TRAMP_DECL(312)
AGB_TRAMP_DECL(313)
AGB_TRAMP_DECL(314)
AGB_TRAMP_DECL(315)

AGB_TRAMP_DECL(316)
AGB_TRAMP_DECL(317)
AGB_TRAMP_DECL(318)
AGB_TRAMP_DECL(319)

AGB_TRAMP_DECL(320)
AGB_TRAMP_DECL(321)
AGB_TRAMP_DECL(322)
AGB_TRAMP_DECL(323)

AGB_TRAMP_DECL(324)
AGB_TRAMP_DECL(325)
AGB_TRAMP_DECL(326)
AGB_TRAMP_DECL(327)

AGB_TRAMP_DECL(328)
AGB_TRAMP_DECL(329)
AGB_TRAMP_DECL(330)
AGB_TRAMP_DECL(331)

AGB_TRAMP_DECL(332)
AGB_TRAMP_DECL(333)
AGB_TRAMP_DECL(334)
AGB_TRAMP_DECL(335)

AGB_TRAMP_DECL(336)
AGB_TRAMP_DECL(337)
AGB_TRAMP_DECL(338)
AGB_TRAMP_DECL(339)

AGB_TRAMP_DECL(340)
AGB_TRAMP_DECL(341)
AGB_TRAMP_DECL(342)
AGB_TRAMP_DECL(343)

AGB_TRAMP_DECL(344)
AGB_TRAMP_DECL(345)
AGB_TRAMP_DECL(346)
AGB_TRAMP_DECL(347)

AGB_TRAMP_DECL(348)
AGB_TRAMP_DECL(349)
AGB_TRAMP_DECL(350)
AGB_TRAMP_DECL(351)

AGB_TRAMP_DECL(352)
AGB_TRAMP_DECL(353)
AGB_TRAMP_DECL(354)
AGB_TRAMP_DECL(355)

AGB_TRAMP_DECL(356)
AGB_TRAMP_DECL(357)
AGB_TRAMP_DECL(358)
AGB_TRAMP_DECL(359)

AGB_TRAMP_DECL(360)
AGB_TRAMP_DECL(361)
AGB_TRAMP_DECL(362)
AGB_TRAMP_DECL(363)

AGB_TRAMP_DECL(364)
AGB_TRAMP_DECL(365)
AGB_TRAMP_DECL(366)
AGB_TRAMP_DECL(367)

AGB_TRAMP_DECL(368)
AGB_TRAMP_DECL(369)
AGB_TRAMP_DECL(370)
AGB_TRAMP_DECL(371)

AGB_TRAMP_DECL(372)
AGB_TRAMP_DECL(373)
AGB_TRAMP_DECL(374)
AGB_TRAMP_DECL(375)

AGB_TRAMP_DECL(376)
AGB_TRAMP_DECL(377)
AGB_TRAMP_DECL(378)
AGB_TRAMP_DECL(379)

AGB_TRAMP_DECL(380)
AGB_TRAMP_DECL(381)
AGB_TRAMP_DECL(382)
AGB_TRAMP_DECL(383)

AGB_TRAMP_DECL(384)
AGB_TRAMP_DECL(385)
AGB_TRAMP_DECL(386)
AGB_TRAMP_DECL(387)

AGB_TRAMP_DECL(388)
AGB_TRAMP_DECL(389)
AGB_TRAMP_DECL(390)
AGB_TRAMP_DECL(391)

AGB_TRAMP_DECL(392)
AGB_TRAMP_DECL(393)
AGB_TRAMP_DECL(394)
AGB_TRAMP_DECL(395)

AGB_TRAMP_DECL(396)
AGB_TRAMP_DECL(397)
AGB_TRAMP_DECL(398)
AGB_TRAMP_DECL(399)

AGB_TRAMP_DECL(400)
AGB_TRAMP_DECL(401)
AGB_TRAMP_DECL(402)
AGB_TRAMP_DECL(403)

AGB_TRAMP_DECL(404)
AGB_TRAMP_DECL(405)
AGB_TRAMP_DECL(406)
AGB_TRAMP_DECL(407)

AGB_TRAMP_DECL(408)
AGB_TRAMP_DECL(409)
AGB_TRAMP_DECL(410)
AGB_TRAMP_DECL(411)

AGB_TRAMP_DECL(412)
AGB_TRAMP_DECL(413)
AGB_TRAMP_DECL(414)
AGB_TRAMP_DECL(415)

AGB_TRAMP_DECL(416)
AGB_TRAMP_DECL(417)
AGB_TRAMP_DECL(418)
AGB_TRAMP_DECL(419)

AGB_TRAMP_DECL(420)
AGB_TRAMP_DECL(421)
AGB_TRAMP_DECL(422)
AGB_TRAMP_DECL(423)

AGB_TRAMP_DECL(424)
AGB_TRAMP_DECL(425)
AGB_TRAMP_DECL(426)
AGB_TRAMP_DECL(427)

AGB_TRAMP_DECL(428)
AGB_TRAMP_DECL(429)
AGB_TRAMP_DECL(430)
AGB_TRAMP_DECL(431)

AGB_TRAMP_DECL(432)
AGB_TRAMP_DECL(433)
AGB_TRAMP_DECL(434)
AGB_TRAMP_DECL(435)

AGB_TRAMP_DECL(436)
AGB_TRAMP_DECL(437)
AGB_TRAMP_DECL(438)
AGB_TRAMP_DECL(439)

AGB_TRAMP_DECL(440)
AGB_TRAMP_DECL(441)
AGB_TRAMP_DECL(442)
AGB_TRAMP_DECL(443)

AGB_TRAMP_DECL(444)
AGB_TRAMP_DECL(445)
AGB_TRAMP_DECL(446)
AGB_TRAMP_DECL(447)

AGB_TRAMP_DECL(448)
AGB_TRAMP_DECL(449)
AGB_TRAMP_DECL(450)
AGB_TRAMP_DECL(451)

AGB_TRAMP_DECL(452)
AGB_TRAMP_DECL(453)
AGB_TRAMP_DECL(454)
AGB_TRAMP_DECL(455)

AGB_TRAMP_DECL(456)
AGB_TRAMP_DECL(457)
AGB_TRAMP_DECL(458)
AGB_TRAMP_DECL(459)

AGB_TRAMP_DECL(460)
AGB_TRAMP_DECL(461)
AGB_TRAMP_DECL(462)
AGB_TRAMP_DECL(463)

AGB_TRAMP_DECL(464)
AGB_TRAMP_DECL(465)
AGB_TRAMP_DECL(466)
AGB_TRAMP_DECL(467)

AGB_TRAMP_DECL(468)
AGB_TRAMP_DECL(469)
AGB_TRAMP_DECL(470)
AGB_TRAMP_DECL(471)

AGB_TRAMP_DECL(472)
AGB_TRAMP_DECL(473)
AGB_TRAMP_DECL(474)
AGB_TRAMP_DECL(475)

AGB_TRAMP_DECL(476)
AGB_TRAMP_DECL(477)
AGB_TRAMP_DECL(478)
AGB_TRAMP_DECL(479)

AGB_TRAMP_DECL(480)
AGB_TRAMP_DECL(481)
AGB_TRAMP_DECL(482)
AGB_TRAMP_DECL(483)

AGB_TRAMP_DECL(484)
AGB_TRAMP_DECL(485)
AGB_TRAMP_DECL(486)
AGB_TRAMP_DECL(487)

AGB_TRAMP_DECL(488)
AGB_TRAMP_DECL(489)
AGB_TRAMP_DECL(490)
AGB_TRAMP_DECL(491)

AGB_TRAMP_DECL(492)
AGB_TRAMP_DECL(493)
AGB_TRAMP_DECL(494)
AGB_TRAMP_DECL(495)

AGB_TRAMP_DECL(496)
AGB_TRAMP_DECL(497)
AGB_TRAMP_DECL(498)
AGB_TRAMP_DECL(499)

AGB_TRAMP_DECL(500)
AGB_TRAMP_DECL(501)
AGB_TRAMP_DECL(502)
AGB_TRAMP_DECL(503)

AGB_TRAMP_DECL(504)
AGB_TRAMP_DECL(505)
AGB_TRAMP_DECL(506)
AGB_TRAMP_DECL(507)

AGB_TRAMP_DECL(508)
AGB_TRAMP_DECL(509)
AGB_TRAMP_DECL(510)
AGB_TRAMP_DECL(511)

AGB_TRAMP_DECL(512)
AGB_TRAMP_DECL(513)
AGB_TRAMP_DECL(514)
AGB_TRAMP_DECL(515)

AGB_TRAMP_DECL(516)
AGB_TRAMP_DECL(517)
AGB_TRAMP_DECL(518)
AGB_TRAMP_DECL(519)

AGB_TRAMP_DECL(520)
AGB_TRAMP_DECL(521)
AGB_TRAMP_DECL(522)
AGB_TRAMP_DECL(523)

AGB_TRAMP_DECL(524)
AGB_TRAMP_DECL(525)
AGB_TRAMP_DECL(526)
AGB_TRAMP_DECL(527)

AGB_TRAMP_DECL(528)
AGB_TRAMP_DECL(529)
AGB_TRAMP_DECL(530)
AGB_TRAMP_DECL(531)

AGB_TRAMP_DECL(532)
AGB_TRAMP_DECL(533)
AGB_TRAMP_DECL(534)
AGB_TRAMP_DECL(535)

AGB_TRAMP_DECL(536)
AGB_TRAMP_DECL(537)
AGB_TRAMP_DECL(538)
AGB_TRAMP_DECL(539)

AGB_TRAMP_DECL(540)
AGB_TRAMP_DECL(541)
AGB_TRAMP_DECL(542)
AGB_TRAMP_DECL(543)

AGB_TRAMP_DECL(544)
AGB_TRAMP_DECL(545)
AGB_TRAMP_DECL(546)
AGB_TRAMP_DECL(547)

AGB_TRAMP_DECL(548)
AGB_TRAMP_DECL(549)
AGB_TRAMP_DECL(550)
AGB_TRAMP_DECL(551)

AGB_TRAMP_DECL(552)
AGB_TRAMP_DECL(553)
AGB_TRAMP_DECL(554)
AGB_TRAMP_DECL(555)

AGB_TRAMP_DECL(556)
AGB_TRAMP_DECL(557)
AGB_TRAMP_DECL(558)
AGB_TRAMP_DECL(559)

AGB_TRAMP_DECL(560)
AGB_TRAMP_DECL(561)
AGB_TRAMP_DECL(562)
AGB_TRAMP_DECL(563)

AGB_TRAMP_DECL(564)
AGB_TRAMP_DECL(565)
AGB_TRAMP_DECL(566)
AGB_TRAMP_DECL(567)

AGB_TRAMP_DECL(568)
AGB_TRAMP_DECL(569)
AGB_TRAMP_DECL(570)
AGB_TRAMP_DECL(571)

AGB_TRAMP_DECL(572)
AGB_TRAMP_DECL(573)
AGB_TRAMP_DECL(574)
AGB_TRAMP_DECL(575)

AGB_TRAMP_DECL(576)
AGB_TRAMP_DECL(577)
AGB_TRAMP_DECL(578)
AGB_TRAMP_DECL(579)

AGB_TRAMP_DECL(580)
AGB_TRAMP_DECL(581)
AGB_TRAMP_DECL(582)
AGB_TRAMP_DECL(583)

AGB_TRAMP_DECL(584)
AGB_TRAMP_DECL(585)
AGB_TRAMP_DECL(586)
AGB_TRAMP_DECL(587)

AGB_TRAMP_DECL(588)
AGB_TRAMP_DECL(589)
AGB_TRAMP_DECL(590)
AGB_TRAMP_DECL(591)

AGB_TRAMP_DECL(592)
AGB_TRAMP_DECL(593)
AGB_TRAMP_DECL(594)
AGB_TRAMP_DECL(595)

AGB_TRAMP_DECL(596)
AGB_TRAMP_DECL(597)
AGB_TRAMP_DECL(598)
AGB_TRAMP_DECL(599)

AGB_TRAMP_DECL(600)
AGB_TRAMP_DECL(601)
AGB_TRAMP_DECL(602)
AGB_TRAMP_DECL(603)

AGB_TRAMP_DECL(604)
AGB_TRAMP_DECL(605)
AGB_TRAMP_DECL(606)
AGB_TRAMP_DECL(607)

AGB_TRAMP_DECL(608)
AGB_TRAMP_DECL(609)
AGB_TRAMP_DECL(610)
AGB_TRAMP_DECL(611)

AGB_TRAMP_DECL(612)
AGB_TRAMP_DECL(613)
AGB_TRAMP_DECL(614)
AGB_TRAMP_DECL(615)

AGB_TRAMP_DECL(616)
AGB_TRAMP_DECL(617)
AGB_TRAMP_DECL(618)
AGB_TRAMP_DECL(619)

AGB_TRAMP_DECL(620)
AGB_TRAMP_DECL(621)
AGB_TRAMP_DECL(622)
AGB_TRAMP_DECL(623)

AGB_TRAMP_DECL(624)
AGB_TRAMP_DECL(625)
AGB_TRAMP_DECL(626)
AGB_TRAMP_DECL(627)

AGB_TRAMP_DECL(628)
AGB_TRAMP_DECL(629)
AGB_TRAMP_DECL(630)
AGB_TRAMP_DECL(631)

AGB_TRAMP_DECL(632)
AGB_TRAMP_DECL(633)
AGB_TRAMP_DECL(634)
AGB_TRAMP_DECL(635)

AGB_TRAMP_DECL(636)
AGB_TRAMP_DECL(637)
AGB_TRAMP_DECL(638)
AGB_TRAMP_DECL(639)

AGB_TRAMP_DECL(640)
AGB_TRAMP_DECL(641)
AGB_TRAMP_DECL(642)
AGB_TRAMP_DECL(643)

AGB_TRAMP_DECL(644)
AGB_TRAMP_DECL(645)
AGB_TRAMP_DECL(646)
AGB_TRAMP_DECL(647)

AGB_TRAMP_DECL(648)
AGB_TRAMP_DECL(649)
AGB_TRAMP_DECL(650)
AGB_TRAMP_DECL(651)

AGB_TRAMP_DECL(652)
AGB_TRAMP_DECL(653)
AGB_TRAMP_DECL(654)
AGB_TRAMP_DECL(655)

AGB_TRAMP_DECL(656)
AGB_TRAMP_DECL(657)
AGB_TRAMP_DECL(658)
AGB_TRAMP_DECL(659)

AGB_TRAMP_DECL(660)
AGB_TRAMP_DECL(661)
AGB_TRAMP_DECL(662)
AGB_TRAMP_DECL(663)

AGB_TRAMP_DECL(664)
AGB_TRAMP_DECL(665)
AGB_TRAMP_DECL(666)
AGB_TRAMP_DECL(667)

AGB_TRAMP_DECL(668)
AGB_TRAMP_DECL(669)
AGB_TRAMP_DECL(670)
AGB_TRAMP_DECL(671)

AGB_TRAMP_DECL(672)
AGB_TRAMP_DECL(673)
AGB_TRAMP_DECL(674)
AGB_TRAMP_DECL(675)

AGB_TRAMP_DECL(676)
AGB_TRAMP_DECL(677)
AGB_TRAMP_DECL(678)
AGB_TRAMP_DECL(679)

AGB_TRAMP_DECL(680)
AGB_TRAMP_DECL(681)
AGB_TRAMP_DECL(682)
AGB_TRAMP_DECL(683)

AGB_TRAMP_DECL(684)
AGB_TRAMP_DECL(685)
AGB_TRAMP_DECL(686)
AGB_TRAMP_DECL(687)

AGB_TRAMP_DECL(688)
AGB_TRAMP_DECL(689)
AGB_TRAMP_DECL(690)
AGB_TRAMP_DECL(691)

AGB_TRAMP_DECL(692)
AGB_TRAMP_DECL(693)
AGB_TRAMP_DECL(694)
AGB_TRAMP_DECL(695)

AGB_TRAMP_DECL(696)
AGB_TRAMP_DECL(697)
AGB_TRAMP_DECL(698)
AGB_TRAMP_DECL(699)

AGB_TRAMP_DECL(700)
AGB_TRAMP_DECL(701)
AGB_TRAMP_DECL(702)
AGB_TRAMP_DECL(703)

AGB_TRAMP_DECL(704)
AGB_TRAMP_DECL(705)
AGB_TRAMP_DECL(706)
AGB_TRAMP_DECL(707)

AGB_TRAMP_DECL(708)
AGB_TRAMP_DECL(709)
AGB_TRAMP_DECL(710)
AGB_TRAMP_DECL(711)

AGB_TRAMP_DECL(712)
AGB_TRAMP_DECL(713)
AGB_TRAMP_DECL(714)
AGB_TRAMP_DECL(715)

AGB_TRAMP_DECL(716)
AGB_TRAMP_DECL(717)
AGB_TRAMP_DECL(718)
AGB_TRAMP_DECL(719)

AGB_TRAMP_DECL(720)
AGB_TRAMP_DECL(721)
AGB_TRAMP_DECL(722)
AGB_TRAMP_DECL(723)

AGB_TRAMP_DECL(724)
AGB_TRAMP_DECL(725)
AGB_TRAMP_DECL(726)
AGB_TRAMP_DECL(727)

AGB_TRAMP_DECL(728)
AGB_TRAMP_DECL(729)
AGB_TRAMP_DECL(730)
AGB_TRAMP_DECL(731)

AGB_TRAMP_DECL(732)
AGB_TRAMP_DECL(733)
AGB_TRAMP_DECL(734)
AGB_TRAMP_DECL(735)

AGB_TRAMP_DECL(736)
AGB_TRAMP_DECL(737)
AGB_TRAMP_DECL(738)
AGB_TRAMP_DECL(739)

AGB_TRAMP_DECL(740)
AGB_TRAMP_DECL(741)
AGB_TRAMP_DECL(742)
AGB_TRAMP_DECL(743)

AGB_TRAMP_DECL(744)
AGB_TRAMP_DECL(745)
AGB_TRAMP_DECL(746)
AGB_TRAMP_DECL(747)

AGB_TRAMP_DECL(748)
AGB_TRAMP_DECL(749)
AGB_TRAMP_DECL(750)
AGB_TRAMP_DECL(751)

AGB_TRAMP_DECL(752)
AGB_TRAMP_DECL(753)
AGB_TRAMP_DECL(754)
AGB_TRAMP_DECL(755)

AGB_TRAMP_DECL(756)
AGB_TRAMP_DECL(757)
AGB_TRAMP_DECL(758)
AGB_TRAMP_DECL(759)

AGB_TRAMP_DECL(760)
AGB_TRAMP_DECL(761)
AGB_TRAMP_DECL(762)
AGB_TRAMP_DECL(763)

AGB_TRAMP_DECL(764)
AGB_TRAMP_DECL(765)
AGB_TRAMP_DECL(766)
AGB_TRAMP_DECL(767)

AGB_TRAMP_DECL(768)
AGB_TRAMP_DECL(769)
AGB_TRAMP_DECL(770)
AGB_TRAMP_DECL(771)

AGB_TRAMP_DECL(772)
AGB_TRAMP_DECL(773)
AGB_TRAMP_DECL(774)
AGB_TRAMP_DECL(775)

AGB_TRAMP_DECL(776)
AGB_TRAMP_DECL(777)
AGB_TRAMP_DECL(778)
AGB_TRAMP_DECL(779)

AGB_TRAMP_DECL(780)
AGB_TRAMP_DECL(781)
AGB_TRAMP_DECL(782)
AGB_TRAMP_DECL(783)

AGB_TRAMP_DECL(784)
AGB_TRAMP_DECL(785)
AGB_TRAMP_DECL(786)
AGB_TRAMP_DECL(787)

AGB_TRAMP_DECL(788)
AGB_TRAMP_DECL(789)
AGB_TRAMP_DECL(790)
AGB_TRAMP_DECL(791)

AGB_TRAMP_DECL(792)
AGB_TRAMP_DECL(793)
AGB_TRAMP_DECL(794)
AGB_TRAMP_DECL(795)

AGB_TRAMP_DECL(796)
AGB_TRAMP_DECL(797)
AGB_TRAMP_DECL(798)
AGB_TRAMP_DECL(799)

AGB_TRAMP_DECL(800)
AGB_TRAMP_DECL(801)
AGB_TRAMP_DECL(802)
AGB_TRAMP_DECL(803)

AGB_TRAMP_DECL(804)
AGB_TRAMP_DECL(805)
AGB_TRAMP_DECL(806)
AGB_TRAMP_DECL(807)

AGB_TRAMP_DECL(808)
AGB_TRAMP_DECL(809)
AGB_TRAMP_DECL(810)
AGB_TRAMP_DECL(811)

AGB_TRAMP_DECL(812)
AGB_TRAMP_DECL(813)
AGB_TRAMP_DECL(814)
AGB_TRAMP_DECL(815)

AGB_TRAMP_DECL(816)
AGB_TRAMP_DECL(817)
AGB_TRAMP_DECL(818)
AGB_TRAMP_DECL(819)

AGB_TRAMP_DECL(820)
AGB_TRAMP_DECL(821)
AGB_TRAMP_DECL(822)
AGB_TRAMP_DECL(823)

AGB_TRAMP_DECL(824)
AGB_TRAMP_DECL(825)
AGB_TRAMP_DECL(826)
AGB_TRAMP_DECL(827)

AGB_TRAMP_DECL(828)
AGB_TRAMP_DECL(829)
AGB_TRAMP_DECL(830)
AGB_TRAMP_DECL(831)

AGB_TRAMP_DECL(832)
AGB_TRAMP_DECL(833)
AGB_TRAMP_DECL(834)
AGB_TRAMP_DECL(835)

AGB_TRAMP_DECL(836)
AGB_TRAMP_DECL(837)
AGB_TRAMP_DECL(838)
AGB_TRAMP_DECL(839)

AGB_TRAMP_DECL(840)
AGB_TRAMP_DECL(841)
AGB_TRAMP_DECL(842)
AGB_TRAMP_DECL(843)

AGB_TRAMP_DECL(844)
AGB_TRAMP_DECL(845)
AGB_TRAMP_DECL(846)
AGB_TRAMP_DECL(847)

AGB_TRAMP_DECL(848)
AGB_TRAMP_DECL(849)
AGB_TRAMP_DECL(850)
AGB_TRAMP_DECL(851)

AGB_TRAMP_DECL(852)
AGB_TRAMP_DECL(853)
AGB_TRAMP_DECL(854)
AGB_TRAMP_DECL(855)

AGB_TRAMP_DECL(856)
AGB_TRAMP_DECL(857)
AGB_TRAMP_DECL(858)
AGB_TRAMP_DECL(859)

AGB_TRAMP_DECL(860)
AGB_TRAMP_DECL(861)
AGB_TRAMP_DECL(862)
AGB_TRAMP_DECL(863)

AGB_TRAMP_DECL(864)
AGB_TRAMP_DECL(865)
AGB_TRAMP_DECL(866)
AGB_TRAMP_DECL(867)

AGB_TRAMP_DECL(868)
AGB_TRAMP_DECL(869)
AGB_TRAMP_DECL(870)
AGB_TRAMP_DECL(871)

AGB_TRAMP_DECL(872)
AGB_TRAMP_DECL(873)
AGB_TRAMP_DECL(874)
AGB_TRAMP_DECL(875)

AGB_TRAMP_DECL(876)
AGB_TRAMP_DECL(877)
AGB_TRAMP_DECL(878)
AGB_TRAMP_DECL(879)

AGB_TRAMP_DECL(880)
AGB_TRAMP_DECL(881)
AGB_TRAMP_DECL(882)
AGB_TRAMP_DECL(883)

AGB_TRAMP_DECL(884)
AGB_TRAMP_DECL(885)
AGB_TRAMP_DECL(886)
AGB_TRAMP_DECL(887)

AGB_TRAMP_DECL(888)
AGB_TRAMP_DECL(889)
AGB_TRAMP_DECL(890)
AGB_TRAMP_DECL(891)

AGB_TRAMP_DECL(892)
AGB_TRAMP_DECL(893)
AGB_TRAMP_DECL(894)
AGB_TRAMP_DECL(895)

AGB_TRAMP_DECL(896)
AGB_TRAMP_DECL(897)
AGB_TRAMP_DECL(898)
AGB_TRAMP_DECL(899)

AGB_TRAMP_DECL(900)
AGB_TRAMP_DECL(901)
AGB_TRAMP_DECL(902)
AGB_TRAMP_DECL(903)

AGB_TRAMP_DECL(904)
AGB_TRAMP_DECL(905)
AGB_TRAMP_DECL(906)
AGB_TRAMP_DECL(907)

AGB_TRAMP_DECL(908)
AGB_TRAMP_DECL(909)
AGB_TRAMP_DECL(910)
AGB_TRAMP_DECL(911)

AGB_TRAMP_DECL(912)
AGB_TRAMP_DECL(913)
AGB_TRAMP_DECL(914)
AGB_TRAMP_DECL(915)

AGB_TRAMP_DECL(916)
AGB_TRAMP_DECL(917)
AGB_TRAMP_DECL(918)
AGB_TRAMP_DECL(919)

AGB_TRAMP_DECL(920)
AGB_TRAMP_DECL(921)
AGB_TRAMP_DECL(922)
AGB_TRAMP_DECL(923)

AGB_TRAMP_DECL(924)
AGB_TRAMP_DECL(925)
AGB_TRAMP_DECL(926)
AGB_TRAMP_DECL(927)

AGB_TRAMP_DECL(928)
AGB_TRAMP_DECL(929)
AGB_TRAMP_DECL(930)
AGB_TRAMP_DECL(931)

AGB_TRAMP_DECL(932)
AGB_TRAMP_DECL(933)
AGB_TRAMP_DECL(934)
AGB_TRAMP_DECL(935)

AGB_TRAMP_DECL(936)
AGB_TRAMP_DECL(937)
AGB_TRAMP_DECL(938)
AGB_TRAMP_DECL(939)

AGB_TRAMP_DECL(940)
AGB_TRAMP_DECL(941)
AGB_TRAMP_DECL(942)
AGB_TRAMP_DECL(943)

AGB_TRAMP_DECL(944)
AGB_TRAMP_DECL(945)
AGB_TRAMP_DECL(946)
AGB_TRAMP_DECL(947)

AGB_TRAMP_DECL(948)
AGB_TRAMP_DECL(949)
AGB_TRAMP_DECL(950)
AGB_TRAMP_DECL(951)

AGB_TRAMP_DECL(952)
AGB_TRAMP_DECL(953)
AGB_TRAMP_DECL(954)
AGB_TRAMP_DECL(955)

AGB_TRAMP_DECL(956)
AGB_TRAMP_DECL(957)
AGB_TRAMP_DECL(958)
AGB_TRAMP_DECL(959)

AGB_TRAMP_DECL(960)
AGB_TRAMP_DECL(961)
AGB_TRAMP_DECL(962)
AGB_TRAMP_DECL(963)

AGB_TRAMP_DECL(964)
AGB_TRAMP_DECL(965)
AGB_TRAMP_DECL(966)
AGB_TRAMP_DECL(967)

AGB_TRAMP_DECL(968)
AGB_TRAMP_DECL(969)
AGB_TRAMP_DECL(970)
AGB_TRAMP_DECL(971)

AGB_TRAMP_DECL(972)
AGB_TRAMP_DECL(973)
AGB_TRAMP_DECL(974)
AGB_TRAMP_DECL(975)

AGB_TRAMP_DECL(976)
AGB_TRAMP_DECL(977)
AGB_TRAMP_DECL(978)
AGB_TRAMP_DECL(979)

AGB_TRAMP_DECL(980)
AGB_TRAMP_DECL(981)
AGB_TRAMP_DECL(982)
AGB_TRAMP_DECL(983)

AGB_TRAMP_DECL(984)
AGB_TRAMP_DECL(985)
AGB_TRAMP_DECL(986)
AGB_TRAMP_DECL(987)

AGB_TRAMP_DECL(988)
AGB_TRAMP_DECL(989)
AGB_TRAMP_DECL(990)
AGB_TRAMP_DECL(991)

AGB_TRAMP_DECL(992)
AGB_TRAMP_DECL(993)
AGB_TRAMP_DECL(994)
AGB_TRAMP_DECL(995)

AGB_TRAMP_DECL(996)
AGB_TRAMP_DECL(997)
AGB_TRAMP_DECL(998)
AGB_TRAMP_DECL(999)

AGB_TRAMP_DECL(1000)
AGB_TRAMP_DECL(1001)
AGB_TRAMP_DECL(1002)
AGB_TRAMP_DECL(1003)

AGB_TRAMP_DECL(1004)
AGB_TRAMP_DECL(1005)
AGB_TRAMP_DECL(1006)
AGB_TRAMP_DECL(1007)

AGB_TRAMP_DECL(1008)
AGB_TRAMP_DECL(1009)
AGB_TRAMP_DECL(1010)
AGB_TRAMP_DECL(1011)

AGB_TRAMP_DECL(1012)
AGB_TRAMP_DECL(1013)
AGB_TRAMP_DECL(1014)
AGB_TRAMP_DECL(1015)

AGB_TRAMP_DECL(1016)
AGB_TRAMP_DECL(1017)
AGB_TRAMP_DECL(1018)
AGB_TRAMP_DECL(1019)

AGB_TRAMP_DECL(1020)
AGB_TRAMP_DECL(1021)
AGB_TRAMP_DECL(1022)
AGB_TRAMP_DECL(1023)


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

    if (!e->fn(e->engine, self_p, arg_ptrs, n, &out_v, e->user))
        return js_mkundef();

    return out_v.v;
}

void antb_define_method(AntbEngine* engine, AntbValue* obj, const char* name,
                        AntbNativeFn fn, void* user)
{
    agb_method_entry* e;
    ant_value_t f;
    int slot;

    if (!engine || !obj || !name || !fn)
        return;
    if (g_method_count >= AGB_TRAMPOLINE_COUNT)
        return;

    slot = g_method_count++;
    e = &g_methods[slot];
    e->engine = engine;
    e->js   = engine->js;
    e->obj  = obj->v;
    e->fn   = fn;
    e->user = user;
    strncpy(e->name, name, sizeof(e->name) - 1);

    f = js_mkfun_dyn(g_tramps[slot]);
    /* js_set_function_name needs a real object; T_CFUNC rejects it */
    js_setprop(engine->js, obj->v, js_mkstr(engine->js, name, strlen(name)), f);
}

/* --- accessor properties ------------------------------------------------- */

/* defined further down (class property tables) */
static int agb_class_lookup_prop(AntbClass* k, const char* name,
                                 AntbGetterFn* fn, void** user);

static ant_value_t agb_getter_trampoline(ant_t* js, ant_value_t obj,
                                         const char* key, size_t klen)
{
    agb_getter_entry* entry = (agb_getter_entry*)js_get_native(obj, AGB_TAG_GETTER);
    AntbValue  self_v;
    AntbValue  out_v;
    AntbGetterFn fn = NULL;
    void*      user = NULL;
    char name[128];

    if (!entry)
        return js_mkundef();

    if (klen >= sizeof(name))
        klen = sizeof(name) - 1;
    memcpy(name, key, klen);
    name[klen] = '\0';

    if (entry->klass) {
        if (!agb_class_lookup_prop(entry->klass, name, &fn, &user))
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
    char name[128];

    if (!entry || !entry->setter)
        return false;

    if (klen >= sizeof(name))
        klen = sizeof(name) - 1;
    memcpy(name, key, klen);
    name[klen] = '\0';

    self_v.js = js;
    self_v.v  = obj;
    val_v.js  = js;
    val_v.v   = value;

    return entry->setter(entry->engine, &self_v, name, &val_v, entry->user) != 0;
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

    if (getenv("AGB_CALL_DEBUG"))
        fprintf(stderr, "[call] enter fn=%p argc=%d\n", (void*)fn->v, argc);

    r = js_call_value(engine->js, fn->v, this_v, argv, argc);

    if (getenv("AGB_CALL_DEBUG"))
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
    void*       ptr;
    unsigned    tag;
    ant_value_t value;   /* GC root: registered with gc_register_root() */
    int         used;
    int         next;    /* hash chain index+1 */
} agb_wrapper_entry;

static agb_wrapper_entry g_wrappers[AGB_MAX_WRAPPERS];
#define AGB_WRAPPER_HASH_SIZE 65521   /* prime */

static int                g_wrapperHash[AGB_WRAPPER_HASH_SIZE];   /* index+1 */
static int                g_wrapper_count = 0;

static unsigned agbWrapperBucket(ant_t* js, void* ptr, unsigned tag)
{
    unsigned long long h = ((unsigned long long)(size_t)ptr >> 4) ^ tag;

    h ^= (unsigned long long)(size_t)js >> 4;
    h *= 1099511628211ULL;
    return (unsigned)(h % AGB_WRAPPER_HASH_SIZE);
}

AntbValue* antb_wrap_native(AntbEngine* engine, void* ptr, unsigned tag)
{
    int i;

    if (!engine || !engine->js || !ptr)
        return NULL;

    /* O(1) lookup: the old linear scan over every wrapper turned node-heavy
     * pages into O(n^2) (163.com spent minutes here). */
    {
        unsigned bucket = agbWrapperBucket(engine->js, ptr, tag);
        int idx = g_wrapperHash[bucket];

        while (idx) {
            agb_wrapper_entry* e = &g_wrappers[idx - 1];

            if (e->used && e->js == engine->js && e->ptr == ptr && e->tag == tag)
                return alloc_value(engine->js, e->value);
            idx = e->next;
        }
    }

    if (g_wrapper_count >= AGB_MAX_WRAPPERS)
        return NULL;

    {
        agb_wrapper_entry* e = &g_wrappers[g_wrapper_count];
        ant_value_t obj = mkobj(engine->js, 0);

        js_set_native(obj, ptr, (uint32_t)tag);

        e->js    = engine->js;
        e->ptr   = ptr;
        e->tag   = tag;
        e->value = obj;
        e->used  = 1;

        /* keep the wrapper alive for the lifetime of the engine */
        gc_pin_permanent(e->js, e->value);

        {
            unsigned bucket = agbWrapperBucket(e->js, e->ptr, e->tag);

            e->next = g_wrapperHash[bucket];
            g_wrapperHash[bucket] = g_wrapper_count + 1;
        }
        g_wrapper_count++;

        return alloc_value(engine->js, obj);
    }
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
/* --- classes / prototypes ------------------------------------------------ */

#define AGB_MAX_CLASSES 128

struct AntbClass {
    AntbEngine* engine;
    ant_t*      js;
    ant_value_t proto;      /* GC root */
    char        name[64];
};

static struct AntbClass* g_classes[AGB_MAX_CLASSES];
static int               g_class_count = 0;

AntbClass* antb_class_create(AntbEngine* engine, const char* name)
{
    struct AntbClass* k;

    if (!engine || !engine->js || !name)
        return NULL;
    if (g_class_count >= AGB_MAX_CLASSES)
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

    g_classes[g_class_count++] = k;
    return k;
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

AntbValue* antb_wrap_native_as(AntbEngine* engine, AntbClass* klass,
                               void* ptr, unsigned tag)
{
    AntbValue* v;

    if (!engine || !klass || !ptr)
        return NULL;

    v = antb_wrap_native(engine, ptr, tag);
    if (!v)
        return NULL;

    js_set_proto(engine->js, v->v, klass->proto);

    /* one dispatching getter per instance for the class' properties; only on
     * first wrap (the cache may hand back an already prepared wrapper) */
    if (!js_get_native(v->v, AGB_TAG_GETTER))
        antb_class_install_properties(engine, klass, v);

    return v;
}
/* --- class property tables ----------------------------------------------- *
 * ant does not run an object's exotic getter when the property is found on a
 * prototype, so class level properties are installed as one dispatching
 * getter per instance instead. */

#define AGB_MAX_CLASS_PROPS 32

typedef struct {
    char         name[64];
    AntbGetterFn fn;
    void*        user;
} agb_class_prop;

static agb_class_prop g_class_props[AGB_MAX_CLASSES][AGB_MAX_CLASS_PROPS];
static int            g_class_prop_count[AGB_MAX_CLASSES];

static int agb_class_index(AntbClass* k)
{
    int i;
    for (i = 0; i < g_class_count; i++)
        if (g_classes[i] == k)
            return i;
    return -1;
}

static int agb_class_lookup_prop(AntbClass* k, const char* name,
                                 AntbGetterFn* fn, void** user)
{
    int idx = agb_class_index(k);
    int i;

    if (idx < 0)
        return 0;

    for (i = 0; i < g_class_prop_count[idx]; i++) {
        if (strcmp(g_class_props[idx][i].name, name) == 0) {
            *fn   = g_class_props[idx][i].fn;
            *user = g_class_props[idx][i].user;
            return 1;
        }
    }
    return 0;
}

int antb_class_set_property(AntbEngine* engine, AntbClass* klass,
                            const char* name, AntbGetterFn fn, void* user)
{
    int idx;
    int n;

    if (!engine || !klass || !name || !fn)
        return 0;

    idx = agb_class_index(klass);
    if (idx < 0)
        return 0;

    n = g_class_prop_count[idx];
    if (n >= AGB_MAX_CLASS_PROPS)
        return 0;

    strncpy(g_class_props[idx][n].name, name, sizeof(g_class_props[idx][n].name) - 1);
    g_class_props[idx][n].fn   = fn;
    g_class_props[idx][n].user = user;
    g_class_prop_count[idx] = n + 1;
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
}

