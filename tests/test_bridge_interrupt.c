/*
 * The host abort path must be safe from another thread, and it must actually
 * interrupt a blocked script.
 *
 * Two gaps this pins down (P2 "ant interrupt path cleanup"):
 *
 *   1. Atomics.wait() without a timeout used a plain pthread_cond_wait(). The
 *      host's abort only sets interrupt_pending from its own thread and nothing
 *      signals that condvar, so the wait was truly uninterruptible.
 *
 *   2. antb_engine_destroy() used to free the isolate while the watchdog/UI
 *      thread was iterating g_engines[] (antb_engines_js_depth() /
 *      antb_interrupt_all_engines()): a narrow use-after-free. The table is now
 *      guarded by a lock and destroy unlinks before tearing down, so hammering
 *      create/destroy from one thread while the other scans must not crash.
 *
 * The test runs the engine on its own thread (ant engines are thread affine:
 * the stack bounds come from the creating thread) and drives the abort path
 * from the main thread.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
#else
#include <time.h>
static void sleep_ms(int ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
#endif

#include "AntBridge.h"

#define WAIT_SCRIPT \
    "Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0);"

#define CHURN_MS 400

static volatile int g_evalDone = 0;
static volatile int g_evalInterrupted = 0;
static volatile unsigned int g_depthInWait = 0;

static void* evalThread(void* arg)
{
    volatile char stack_anchor = 0;
    AntbEngine* engine;
    char* err = NULL;
    AntbValue* v;

    (void)arg;

    engine = antb_engine_create((const void*)&stack_anchor);

    if (!engine) {
        g_evalDone = 1;
        return NULL;
    }

    v = antb_eval(engine, WAIT_SCRIPT, (int)strlen(WAIT_SCRIPT), &err);
    g_evalInterrupted = (!v && err && (strstr(err, "interrupted") || strstr(err, "terminat"))) ? 1 : 0;

    if (err)
        free(err);
    if (v)
        antb_value_release(v);

    /* Destroy on the thread that owns the engine, like the browser does. */
    antb_engine_destroy(engine);
    engine = NULL;

    g_evalDone = 1;
    return NULL;
}

/* Part 2: one thread creates/destroys engines while the other scans the table. */
static volatile int g_churnStop = 0;
static volatile int g_churnEngines = 0;

static void* churnThread(void* arg)
{
    (void)arg;

    while (!g_churnStop) {
        volatile char stack_anchor = 0;
        AntbEngine* engine = antb_engine_create((const void*)&stack_anchor);

        g_churnEngines++;
        antb_engine_destroy(engine);
    }

    return NULL;
}

static void* scanThread(void* arg)
{
    (void)arg;

    while (!g_churnStop) {
        (void)antb_engines_js_depth();
        antb_interrupt_all_engines();
    }

    return NULL;
}

int main(void)
{
    int failures = 0;
    pthread_t evalTid;
    int waited = 0;

    /* ---- 1. Atomics.wait(no timeout) is interrupted from another thread ---- */
    if (pthread_create(&evalTid, NULL, evalThread, NULL) != 0) {
        printf("FAIL: cannot start the eval thread\n");
        return 1;
    }

    while (!g_evalDone && waited < 3000) {
        unsigned int depth = antb_engines_js_depth();

        if (depth > g_depthInWait)
            g_depthInWait = depth;

        sleep_ms(20);
        waited += 20;
    }

    if (g_evalDone) {
        printf("FAIL: Atomics.wait() returned within %d ms without an abort\n", waited);
        failures++;
    } else {
        antb_interrupt_all_engines();

        waited = 0;
        while (!g_evalDone && waited < 5000) {
            sleep_ms(20);
            waited += 20;
        }

        if (!g_evalDone) {
            /* Do not pthread_join a thread that is still blocked: fail now, so
             * the harness sees a clean failure instead of its own timeout. */
            printf("FAIL: Atomics.wait() was not interrupted within 5 s\n");
            exit(1);
        } else if (!g_evalInterrupted) {
            printf("FAIL: eval finished but not with a termination error\n");
            failures++;
        } else {
            printf("ok: Atomics.wait() interrupted after the host abort\n");
        }
    }

    pthread_join(evalTid, NULL);

    if (g_depthInWait == 0) {
        printf("FAIL: antb_engines_js_depth() reported 0 while JS was on the stack\n");
        failures++;
    } else {
        printf("ok: js depth while waiting = %u\n", g_depthInWait);
    }

    if (antb_engines_js_depth() != 0) {
        printf("FAIL: depth is not 0 after the engine was destroyed\n");
        failures++;
    }

    /* ---- 2. create/destroy vs depth/interrupt from another thread ---- */
    {
        pthread_t churnTid;
        pthread_t scanTid;

        g_churnStop = 0;

        if (pthread_create(&churnTid, NULL, churnThread, NULL) != 0 ||
            pthread_create(&scanTid, NULL, scanThread, NULL) != 0) {
            printf("FAIL: cannot start the churn threads\n");
            return 1;
        }

        sleep_ms(CHURN_MS);
        g_churnStop = 1;
        pthread_join(churnTid, NULL);
        pthread_join(scanTid, NULL);

        printf("ok: %d engines created/destroyed while another thread scanned\n", g_churnEngines);

        if (antb_engines_js_depth() != 0) {
            printf("FAIL: depth is not 0 after all engines were destroyed\n");
            failures++;
        }
    }

    if (failures) {
        printf("FAILURES: %d\n", failures);
        return 1;
    }

    printf("ok\n");
    return 0;
}
