/*
 * Every bridged native callback must be bracketed by the host's
 * contextActivate/contextLeave hooks.
 *
 * ant switches realms per function call. The host switches its derived state
 * (which document is current) in the "activate" hook, and ant restores its own
 * realm after the call - but the host cannot see that moment. Without a paired
 * "leave" hook the host stays on the *callee's* realm/document: agave installs
 * the catch-all window getter on each realm's global, and a child frame that
 * called one node method (a method of the main realm's class) left the current
 * document on the parent until the next callback, so document-scoped host
 * functions ran against the wrong document (roadmap P1-aa / R148).
 *
 * The hooks must also stay paired when a native returns "not handled" and when
 * it leaves a value behind.
 */
#include <stdio.h>
#include <string.h>

#include "AntBridge.h"

static int g_enters = 0;
static int g_leaves = 0;
static int g_unbalanced = 0;

static void hostActivate(AntbEngine* engine)
{
    (void)engine;
    g_enters++;
}

static void hostLeave(AntbEngine* engine)
{
    (void)engine;
    g_leaves++;
    if (g_leaves > g_enters)
        g_unbalanced = 1;
}

static int ping(AntbEngine* engine, AntbValue* self, AntbValue** args,
                int argc, AntbValue* out, void* user)
{
    AntbValue* v;

    (void)self;
    (void)args;
    (void)argc;
    (void)user;

    v = antb_make_number(engine, 7.0);
    if (!v)
        return 0;

    antb_value_assign(out, v);
    antb_value_release(v);
    return 1;
}

/* Returns "not handled" (the bridge turns it into undefined): the leave hook
 * still has to run, or every later callback would be off by one. */
static int quiet(AntbEngine* engine, AntbValue* self, AntbValue** args,
                 int argc, AntbValue* out, void* user)
{
    (void)engine;
    (void)self;
    (void)args;
    (void)argc;
    (void)out;
    (void)user;
    return 0;
}

static int check(AntbEngine* engine, const char* script, double want, const char* label)
{
    char* err = NULL;
    AntbValue* result = antb_eval(engine, script, (int)strlen(script), &err);
    int ok = result && antb_is_number(result) && antb_to_number(result) == want;
    int before = g_enters - g_leaves;

    if (!ok) {
        char* text = result ? antb_to_utf8(result) : NULL;

        printf("FAIL %s: -> number=%d value=%g text=%s%s%s\n",
               label, result ? antb_is_number(result) : -1,
               result ? antb_to_number(result) : 0.0,
               text ? text : "(null)",
               err ? " err=" : "", err ? err : "");
        antb_free(text);
    }
    if (before != 0) {
        printf("FAIL %s: enter/leave unbalanced by %d after the call\n", label, before);
        ok = 0;
    }
    if (err) {
        printf("FAIL %s: error %s\n", label, err);
        ok = 0;
    }

    if (result)
        antb_value_release(result);
    antb_free(err);

    return ok;
}

int main(void)
{
    volatile char stack_anchor = 0;
    AntbEngine* engine = antb_engine_create((const void*)&stack_anchor);
    AntbValue* global;
    int failures = 0;

    if (!engine) {
        printf("FAIL: engine create failed\n");
        return 1;
    }

    antb_set_context_activate(hostActivate);
    antb_set_context_leave(hostLeave);

    global = antb_global(engine);
    if (!global) {
        printf("FAIL: no global object\n");
        antb_engine_destroy(engine);
        return 1;
    }

    antb_define_method(engine, global, "ping", ping, NULL);
    antb_define_method(engine, global, "quiet", quiet, NULL);
    antb_value_release(global);

    if (!check(engine, "ping()", 7.0, "single call"))
        failures++;
    if (!check(engine, "ping() + ping()", 14.0, "two calls"))
        failures++;
    if (!check(engine, "quiet() === undefined ? 1 : 0", 1.0, "not-handled native"))
        failures++;

    /* A throw out of the script after a callback must not leave the pair open
     * either: the hooks are per callback, not per evaluation. */
    if (!check(engine, "var s = 0; for (var i = 0; i < 3; i++) s += ping(); s", 21.0, "loop"))
        failures++;
    if (!check(engine, "try { ping(); } catch (e) { 0 } ; 1", 1.0, "after try"))
        failures++;

    if (g_unbalanced) {
        printf("FAIL: leave hook ran more often than the activate hook\n");
        failures++;
    }
    if (g_enters != g_leaves) {
        printf("FAIL: enters=%d leaves=%d (must match)\n", g_enters, g_leaves);
        failures++;
    }
    if (g_enters < 7) {
        printf("FAIL: expected at least 7 callbacks, saw %d\n", g_enters);
        failures++;
    }

    antb_set_context_activate(NULL);
    antb_set_context_leave(NULL);
    antb_engine_destroy(engine);

    if (failures) {
        printf("BRIDGE CONTEXT HOOK CHECKS FAILED (%d)\n", failures);
        return 1;
    }

    printf("ALL BRIDGE CONTEXT HOOK CHECKS PASSED (enters=%d leaves=%d)\n",
           g_enters, g_leaves);
    return 0;
}
