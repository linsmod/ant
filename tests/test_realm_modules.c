/*
 * The ES module registry belongs to the realm.
 *
 * The HTML spec's module map is per environment settings object, so two
 * iframes/vm contexts importing the same URL must each evaluate the module once
 * and get their own instance. Before this, js->modules was part of the isolate
 * and shared by every realm: the second realm importing a URL the first one had
 * already loaded got the *first* realm's namespace (its body never ran there).
 *
 * The test writes a small module to disk, then in each realm does
 * `import('./shared_impl.js')` twice (relative to a set filename) and checks
 * that realm's own global counter: 1 (one instance, second import cached), and
 * 0 in a realm that has not imported it. It also hammers realm create/destroy
 * with registries so the per-realm registry is released (run under ASan to see
 * the free side).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "AntBridge.h"

#ifdef _WIN32
#define TEST_SEP "\\"
#else
#define TEST_SEP "/"
#endif

#define TEST_PATH_MAX 512

static const char* kImplSource =
    "globalThis.__sharedLoads = (globalThis.__sharedLoads || 0) + 1;\n"
    "export const n = globalThis.__sharedLoads;\n";

static int failures = 0;
static char g_entryPath[TEST_PATH_MAX];
static char g_implPath[TEST_PATH_MAX];
static const char* g_implName = "shared_impl.js";

static int writeFile(const char* path, const char* text)
{
    FILE* f = fopen(path, "wb");

    if (!f)
        return 0;

    fwrite(text, 1, strlen(text), f);
    fclose(f);
    return 1;
}

static int evalClassic(AntbEngine* engine, const char* src, char** out)
{
    char* err = NULL;
    AntbValue* v = antb_eval(engine, src, (int)strlen(src), &err);

    if (out)
        *out = NULL;

    if (!v) {
        printf("FAIL: eval \"%.40s\" failed: %s\n", src, err ? err : "(no message)");
        free(err);
        return 0;
    }

    if (out)
        *out = antb_to_utf8(v);

    antb_value_release(v);
    return 1;
}

/* One dynamic import in the current realm, resolved against the entry file next
 * to the module on disk. The promise settles during the pump (one pass is
 * enough for the file: loader, which reads synchronously). */
static int importShared(AntbEngine* engine)
{
    char src[256];
    char* out = NULL;
    int ok;

    snprintf(src, sizeof(src),
             "import('./%s').then(function(){ globalThis.__impOk = (globalThis.__impOk || 0) + 1; },"
             " function(e){ globalThis.__impErr = String(e); })",
             g_implName);

    antb_set_filename(engine, g_entryPath);
    ok = evalClassic(engine, src, &out);

    free(out);

    if (ok) {
        int i;

        for (i = 0; i < 4; ++i)
            antb_pump(engine);
    }

    return ok;
}

/* Debug helper: "ok|err" of the last import in this realm. */

/* Reads globalThis.__sharedLoads from the current realm; 0 when it was never
 * set there (which is exactly what a shared registry produces). */
static int realmLoads(AntbEngine* engine)
{
    char* out = NULL;
    int n;

    if (!evalClassic(engine, "String(globalThis.__sharedLoads)", &out))
        return -1;

    n = out ? (int)strtol(out, NULL, 10) : 0;
    free(out);
    return n;
}

static void expectLoads(AntbEngine* engine, int want, const char* where)
{
    int got = realmLoads(engine);

    if (got != want) {
        printf("FAIL %s: __sharedLoads=%d (want %d)\n", where, got, want);
        failures++;
    } else {
        printf("ok %s: __sharedLoads=%d\n", where, got);
    }
}

int main(void)
{
    volatile char stack_anchor = 0;
    AntbEngine* engine;
    AntbRealm* realmA;
    AntbRealm* realmB;
    AntbRealm* mainRealm;
    AntbRealm* prev;
    char dir[TEST_PATH_MAX];
    const char* tmp;
    int i;

    tmp = getenv("TEMP");
    if (!tmp || !tmp[0]) tmp = getenv("TMP");
    if (!tmp || !tmp[0]) tmp = ".";

    snprintf(dir, sizeof(dir), "%s%sant-realm-modules-%lu-%lu", tmp, TEST_SEP,
             (unsigned long)time(NULL), (unsigned long)rand());

#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0700);
#endif

    /* The module base is a plain filesystem path (what ant's resolver and file
     * loader work with); agave's browser layer masquerades file: URLs, this
     * test does not need that. */
    snprintf(g_entryPath, sizeof(g_entryPath), "%s%sentry.js", dir, TEST_SEP);
    snprintf(g_implPath, sizeof(g_implPath), "%s%s%s", dir, TEST_SEP, g_implName);

    if (!writeFile(g_implPath, kImplSource)) {
        printf("FAIL: cannot write %s\n", g_implPath);
        return 1;
    }

    engine = antb_engine_create((const void*)&stack_anchor);
    if (!engine) {
        printf("FAIL: engine create failed\n");
        return 1;
    }

    mainRealm = antb_realm_current(engine);

    /* Main realm: two imports of the same URL are one instance. */
    if (!importShared(engine) || !importShared(engine))
        failures++;
    else
        expectLoads(engine, 1, "main realm after two imports");

    /* Realm A: a fresh module world - its own counter starts empty and its own
     * instance of the module runs exactly once. */
    realmA = antb_realm_create(engine);
    if (!realmA) {
        printf("FAIL: realm A create failed\n");
        return 1;
    }

    prev = antb_realm_enter(engine, realmA);
    expectLoads(engine, 0, "realm A before import");
    if (!importShared(engine) || !importShared(engine))
        failures++;
    else {
        expectLoads(engine, 1, "realm A after two imports");
    }

    /* Back to the main realm: its own instance is untouched. */
    antb_realm_enter(engine, prev);
    expectLoads(engine, 1, "main realm after realm A ran");

    /* Realm A again: cached in *A's* registry, body does not run again. */
    prev = antb_realm_enter(engine, realmA);
    expectLoads(engine, 1, "realm A re-entered");
    if (!importShared(engine))
        failures++;
    else
        expectLoads(engine, 1, "realm A third import (cached)");

    antb_realm_enter(engine, mainRealm);

    /* Realm B: yet another instance. */
    realmB = antb_realm_create(engine);
    if (!realmB) {
        printf("FAIL: realm B create failed\n");
        return 1;
    }

    prev = antb_realm_enter(engine, realmB);
    if (!importShared(engine))
        failures++;
    else
        expectLoads(engine, 1, "realm B after import");
    antb_realm_enter(engine, prev);

    /* Realm registries are released with the realm: 50 short-lived realms each
     * load the module. (Leaks/UAFs show up under ASan.) */
    for (i = 0; i < 50; ++i) {
        AntbRealm* r = antb_realm_create(engine);
        AntbRealm* back;

        if (!r) {
            printf("FAIL: realm create failed in the loop (round %d)\n", i);
            failures++;
            break;
        }

        back = antb_realm_enter(engine, r);
        if (!importShared(engine))
            failures++;
        antb_realm_enter(engine, back);

        if ((i % 10) == 9) {
            antb_gc(engine);
            antb_realm_collect(engine);
        }
    }

    antb_gc(engine);
    antb_realm_collect(engine);

    if (failures == 0)
        printf("ok: per-realm module registries (50 short-lived realms)\n");

    antb_engine_destroy(engine);

    if (failures) {
        printf("FAILURES: %d\n", failures);
        return 1;
    }

    printf("ok\n");
    return 0;
}