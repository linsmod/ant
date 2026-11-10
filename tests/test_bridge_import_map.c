/*
 * Browser import maps: the embedder resolves *bare* module specifiers through
 * the document's <script type="importmap">, before any Node-style package
 * lookup.
 *
 * This is what makes GitHub's bundled pages load: their HTML declares
 *
 *   <script type="importmap">{"imports":{"react":"https://.../react.js"}}</script>
 *
 * and the module graph then writes `import ... from "react"`. Without the hook
 * the bare specifier came back from esm_resolve_url() unchanged and the loader
 * tried to open a *file* named "react" ("Cannot open module: react"), taking
 * down every module that imports React.
 *
 * The hook contract checked here:
 *   1. an exact bare specifier maps through the host resolver;
 *   2. a trailing-slash prefix mapping resolves the suffix too;
 *   3. relative specifiers never reach the resolver (HTML resolves those
 *      against the base URL, the map is not consulted);
 *   4. an unmapped bare specifier still fails as before - the hook must not
 *      turn a miss into a silent success;
 *   5. a bare specifier on a file/package base (no URL) is also offered to the
 *      map first, with Node-style resolution as the fallback.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "AntBridge.h"

static int g_calls = 0;
static int g_relativeCalls = 0;
static char g_dir[512];

static char* join(const char* name)
{
    char path[1024];

    snprintf(path, sizeof(path), "%s/%s", g_dir, name);
    return strdup(path);
}

static char* resolver(const char* specifier, const char* base)
{
    (void)base;
    g_calls++;

    if (specifier[0] == '.')
        g_relativeCalls++;

    if (strcmp(specifier, "mylib") == 0)
        return join("ant-import-map-lib.mjs");

    /* Prefix mapping: "lib/" -> <dir>/ (spec: a key ending in '/' matches a
     * prefix and the remainder is appended to the target). */
    if (strncmp(specifier, "lib/", 4) == 0) {
        char path[1024];

        snprintf(path, sizeof(path), "%s/%s", g_dir, specifier + 4);
        return strdup(path);
    }

    return NULL;
}

static int writeFile(const char* name, const char* text)
{
    char path[1024];
    FILE* file;

    snprintf(path, sizeof(path), "%s/%s", g_dir, name);
    file = fopen(path, "wb");
    if (!file)
        return 0;

    fwrite(text, 1, strlen(text), file);
    fclose(file);
    return 1;
}

/* Evaluates `src` as a module whose URL is `base`. A module's completion value
 * is its namespace object, so the cases below stash the answer on the global
 * and read it back with evalNumber(). Returns 0 when the evaluation failed. */
static int evalModule(AntbEngine* engine, const char* src, const char* base)
{
    char* err = NULL;
    AntbValue* value = antb_eval_module(engine, src, (int)strlen(src), base, &err);
    int ok = value != NULL;

    if (value)
        antb_value_release(value);
    if (err) {
        printf("  (module error: %s)\n", err);
        antb_free(err);
    }
    return ok;
}

/* Evaluates a plain expression and returns it as a number (-1 on failure). */
static double evalNumber(AntbEngine* engine, const char* expr)
{
    char* err = NULL;
    AntbValue* value = antb_eval(engine, expr, (int)strlen(expr), &err);
    double result = -1.0;

    if (value) {
        if (antb_is_number(value))
            result = antb_to_number(value);
        antb_value_release(value);
    }
    if (err) {
        printf("  (eval error: %s)\n", err);
        antb_free(err);
    }
    return result;
}

int main(void)
{
    volatile char stack_anchor = 0;
    AntbEngine* engine;
    const char* tmp = getenv("TEMP");
    int failures = 0;
    double got;

    if (!tmp || !tmp[0])
        tmp = getenv("TMP");
    if (!tmp || !tmp[0])
        tmp = ".";

    snprintf(g_dir, sizeof(g_dir), "%s", tmp);
    {
        /* The engine joins with '/' and fopen() accepts it on Windows too. */
        size_t i;

        for (i = 0; g_dir[i]; ++i)
            if (g_dir[i] == '\\')
                g_dir[i] = '/';
    }

    if (!writeFile("ant-import-map-lib.mjs", "export const answer = 42;\n")) {
        printf("FAIL: cannot write the mapped module under %s\n", g_dir);
        return 1;
    }
    if (!writeFile("ant-import-map-dep.mjs", "export const answer = 7;\n")) {
        printf("FAIL: cannot write the prefix-mapped module under %s\n", g_dir);
        return 1;
    }

    engine = antb_engine_create((const void*)&stack_anchor);
    if (!engine) {
        printf("FAIL: engine create failed\n");
        return 1;
    }

    antb_set_import_map_resolver(resolver);

    /* 1. Exact bare specifier on a URL base (the GitHub shape). */
    g_calls = 0;
    if (!evalModule(engine,
            "import { answer } from 'mylib'; globalThis.__imap = answer;",
            "https://example.test/assets/app.js")) {
        printf("FAIL exact mapping: module evaluation failed\n");
        failures++;
    }
    got = evalNumber(engine, "globalThis.__imap");
    if (got != 42.0) {
        printf("FAIL exact mapping: got %g, want 42\n", got);
        failures++;
    } else {
        printf("PASS exact mapping (mylib -> %s/ant-import-map-lib.mjs)\n", g_dir);
    }
    if (g_calls < 1) {
        printf("FAIL exact mapping: the resolver was never called\n");
        failures++;
    }

    /* 2. Prefix mapping ("lib/" -> <dir>/). */
    if (!evalModule(engine,
            "import { answer } from 'lib/ant-import-map-dep.mjs'; globalThis.__imap = answer;",
            "https://example.test/assets/app.js")) {
        printf("FAIL prefix mapping: module evaluation failed\n");
        failures++;
    }
    got = evalNumber(engine, "globalThis.__imap");
    if (got != 7.0) {
        printf("FAIL prefix mapping: got %g, want 7\n", got);
        failures++;
    } else {
        printf("PASS prefix mapping (lib/x -> <dir>/x)\n");
    }

    /* 3. Relative specifiers must not consult the map. */
    g_relativeCalls = 0;
    {
        /* A relative import that cannot resolve: the map is not asked, and the
         * failure is local. */
        evalModule(engine,
            "import './definitely-missing.mjs'; 1",
            "https://example.test/assets/app.js");
    }
    if (g_relativeCalls != 0) {
        printf("FAIL relative specifier consulted the import map (%d calls)\n", g_relativeCalls);
        failures++;
    } else {
        printf("PASS relative specifiers bypass the import map\n");
    }

    /* 4. An unmapped bare specifier still fails; the map miss is not a success. */
    if (evalModule(engine,
            "import x from 'not-mapped-anywhere'; 1",
            "https://example.test/assets/app.js")) {
        printf("FAIL unmapped bare specifier: expected failure, got success\n");
        failures++;
    } else {
        printf("PASS unmapped bare specifier still fails (no silent success)\n");
    }

    /* 5. A file-style base reaches the hook too (the loader's non-URL path). */
    {
        char entry[1024];

        snprintf(entry, sizeof(entry), "%s/ant-import-map-entry.mjs", g_dir);
        evalNumber(engine, "globalThis.__imap = 0");   /* reset: module is cached */
        if (!evalModule(engine,
                "import { answer } from 'mylib'; globalThis.__imap = answer;",
                entry)) {
            printf("FAIL file-base mapping: module evaluation failed\n");
            failures++;
        }
        got = evalNumber(engine, "globalThis.__imap");
        if (got != 42.0) {
            printf("FAIL file-base mapping: got %g, want 42\n", got);
            failures++;
        } else {
            printf("PASS file-base mapping (bare specifier via import map)\n");
        }
    }

    antb_set_import_map_resolver(NULL);
    antb_engine_destroy(engine);

    {
        char path[1024];

        snprintf(path, sizeof(path), "%s/ant-import-map-lib.mjs", g_dir);
        remove(path);
        snprintf(path, sizeof(path), "%s/ant-import-map-dep.mjs", g_dir);
        remove(path);
    }

    if (failures) {
        printf("BRIDGE IMPORT MAP CHECKS FAILED (%d)\n", failures);
        return 1;
    }

    printf("ALL BRIDGE IMPORT MAP CHECKS PASSED (resolver calls=%d)\n", g_calls);
    return 0;
}
