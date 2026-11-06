/*
 * Native-method slots must be handed back when an engine is destroyed.
 *
 * The bridge keeps one process-wide trampoline table for antb_define_method().
 * Each document realm installs ~170 bridged methods, so a browser that creates
 * a fresh engine per reload exhausts a monotonic table after a handful of
 * reloads; every later definition silently disappears and the page gets a
 * document object without methods (the "reload until the page goes blank" bug).
 *
 * This test creates and destroys more engines than the table has slots for and
 * checks on every round that a method defined late in the batch is callable.
 */
#include <stdio.h>
#include <string.h>

#include "AntBridge.h"

#define ROUNDS      12
#define METHODS     300   /* 12 * 300 > 1024 trampolines */

static int answer(AntbEngine* engine, AntbValue* self, AntbValue** args,
                  int argc, AntbValue* out, void* user)
{
    AntbValue* v;

    (void)self;
    (void)args;
    (void)argc;
    (void)user;

    v = antb_make_number(engine, 42.0);
    if (!v)
        return 0;

    antb_value_assign(out, v);
    antb_value_release(v);
    return 1;
}

int main(void)
{
    int round;
    int failures = 0;

    for (round = 0; round < ROUNDS; ++round) {
        volatile char stack_anchor = 0;
        AntbEngine* engine = antb_engine_create((const void*)&stack_anchor);
        AntbValue* global;
        AntbValue* result;
        char* err = NULL;
        char name[32];
        int i;

        if (!engine) {
            printf("FAIL round %d: engine create failed\n", round);
            return 1;
        }

        global = antb_global(engine);
        if (!global) {
            printf("FAIL round %d: no global object\n", round);
            antb_engine_destroy(engine);
            return 1;
        }

        for (i = 0; i < METHODS; ++i) {
            snprintf(name, sizeof(name), "m%d", i);
            antb_define_method(engine, global, name, answer, NULL);
        }

        result = antb_eval(engine, "m299()", 6, &err);
        if (!result || !antb_is_number(result) || antb_to_number(result) != 42.0) {
            char* text = result ? antb_to_utf8(result) : NULL;

            printf("FAIL round %d: m299() -> number=%d value=%g text=%s%s%s\n",
                   round, result ? antb_is_number(result) : -1,
                   result ? antb_to_number(result) : 0.0,
                   text ? text : "(null)",
                   err ? " err=" : "", err ? err : "");
            antb_free(text);
            failures++;
        }

        if (result)
            antb_value_release(result);
        antb_value_release(global);
        antb_free(err);
        antb_engine_destroy(engine);
    }

    if (failures) {
        printf("%d round(s) could not call a late-defined native method\n", failures);
        return 1;
    }

    printf("OK: %d engines x %d methods, all late methods callable\n", ROUNDS, METHODS);
    return 0;
}
