/*
 * The bridge class registry is per engine and dynamic.
 *
 * A browser realm now needs ~120 interface classes (one per HTML element
 * interface) and interfaces can declare more than 32 attributes. The registry
 * used to be a process-wide array of 128 classes with a 32-entry property table
 * per class: the first engine filled it up, and the second tab silently lost
 * its DOM classes (antb_class_create returned NULL) and its attributes beyond
 * the 32nd.
 *
 * This test fills several engines at once with more classes than the old table
 * held, gives one class more properties than the old per-class cap, wraps a
 * native object and checks the dispatching getter and a method. Engines are
 * destroyed and recreated to prove the per-engine registry is released.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "AntBridge.h"

#define ENGINES              6
#define CLASSES_PER_ENGINE   60   /* 6 * 60 = 360 > the old 128 slots */
#define PROPS_PER_CLASS      40   /* > the old 32-entry property cap     */
#define ROUNDS               2

static int prop_get(AntbEngine* engine, AntbValue* self, const char* name,
                    AntbValue* out, void* user)
{
    AntbValue* v;

    (void)self;
    (void)name;

    v = antb_make_number(engine, 1000.0 + (double)(intptr_t)user);
    if (!v)
        return 0;

    antb_value_assign(out, v);
    antb_value_release(v);
    return 1;
}

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

static int run_engine_round(int round)
{
    AntbEngine* engines[ENGINES];
    int failures = 0;
    int e;

    memset(engines, 0, sizeof(engines));

    for (e = 0; e < ENGINES; ++e) {
        volatile char stack_anchor = 0;
        AntbEngine* engine = antb_engine_create((const void*)&stack_anchor);
        AntbValue* global;
        int c;

        if (!engine) {
            printf("FAIL round %d: engine %d create failed\n", round, e);
            return 1;
        }
        engines[e] = engine;

        global = antb_global(engine);
        if (!global) {
            printf("FAIL round %d: engine %d has no global\n", round, e);
            return 1;
        }

        for (c = 0; c < CLASSES_PER_ENGINE; ++c) {
            char class_name[48];
            AntbClass* klass;
            int p;

            snprintf(class_name, sizeof(class_name), "Round%dEngine%dClass%d",
                     round, e, c);

            klass = antb_class_create(engine, class_name);
            if (!klass) {
                printf("FAIL round %d: engine %d class %d create failed\n",
                       round, e, c);
                antb_value_release(global);
                return 1;
            }

            for (p = 0; p < PROPS_PER_CLASS; ++p) {
                char prop_name[24];

                snprintf(prop_name, sizeof(prop_name), "p%d", p);
                if (!antb_class_set_property(engine, klass, prop_name, prop_get,
                                             (void*)(intptr_t)p)) {
                    printf("FAIL round %d: engine %d class %d property %d rejected\n",
                           round, e, c, p);
                    antb_value_release(global);
                    return 1;
                }
            }

            antb_class_add_method(engine, klass, "m", answer, NULL);

            /* The last class of each engine gets the wrapper: p0/p39 must
             * come back from the dispatching getter, m() from the method. */
            if (c == CLASSES_PER_ENGINE - 1) {
                AntbValue* wrapper = antb_wrap_native_as(engine, klass,
                                                         (void*)&engines[e], 0x54455354u);

                if (!wrapper) {
                    printf("FAIL round %d: wrap failed\n", round);
                    antb_value_release(global);
                    return 1;
                }

                antb_set_prop(engine, global, "probe", wrapper);
                antb_value_release(wrapper);
            }
        }

        antb_value_release(global);
    }

    /* Every engine is alive at the same time: 360 classes and 6 wrapped
     * engines would both have failed on the old static tables. */
    for (e = 0; e < ENGINES; ++e) {
        static const char expr[] =
            "probe.p0 === 1000 && probe.p39 === 1039 && probe.m() === 42";
        char* err = NULL;
        AntbValue* result = antb_eval(engines[e], expr, (int)strlen(expr), &err);

        if (!result || !antb_is_boolean(result) || !antb_to_boolean(result)) {
            char* text = result ? antb_to_utf8(result) : NULL;

            printf("FAIL round %d: engine %d class dispatch: result=%s%s%s\n",
                   round, e, text ? text : "(null)",
                   err ? " err=" : "", err ? err : "");
            antb_free(text);
            failures++;
        }
        antb_value_release(result);
        antb_free(err);
    }

    for (e = 0; e < ENGINES; ++e) {
        if (engines[e])
            antb_engine_destroy(engines[e]);
    }

    return failures;
}

int main(void)
{
    int failures = 0;
    int round;

    for (round = 0; round < ROUNDS; ++round)
        failures += run_engine_round(round);

    if (failures) {
        printf("BRIDGE CLASS TABLE CHECKS FAILED (%d)\n", failures);
        return 1;
    }

    printf("ALL BRIDGE CLASS TABLE CHECKS PASSED\n");
    return 0;
}
