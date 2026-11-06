// meson test -C build index-elem
//
// Element access fast paths (P1-g). Two things are covered:
//
//   * spelling: JS semantics must not change - dense and sparse (element stored
//     as an interned index key) reads/writes, holes, length, deleted indices,
//     accessor indices and index keys inherited from the prototype;
//   * the fast path itself: js_index_get_fast() may only answer when it cannot
//     allocate or run JS (dense slot or an own data property), and must decline
//     accessor / prototype / deleted cases so the caller falls back.
//
// The sparse cases are the ones that hang real sites: a bytecode VM addressing
// slots in the thousands makes `arr[i]` take the generic property path once per
// opcode, which is ~40x slower than a dense read.
#include "internal.h"
#include "silver/glue.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static ant_value_t run(ant_t *js, const char *source) {
  ant_value_t result = js_eval_bytecode(js, source, strlen(source));
  assert(!is_err(result));
  return result;
}

static void check(ant_t *js, const char *source, const char *what) {
  ant_value_t result = js_eval_bytecode(js, source, strlen(source));
  if (is_err(result)) {
    fprintf(stderr, "FAIL %s\n", what);
    exit(1);
  }
}

/* Fetch `a` after the script above defined it. */
static ant_value_t global_named(ant_t *js, const char *name) {
  ant_value_t value = js_getprop_fallback(js, js->global, name);
  assert(!is_err(value));
  return value;
}

int main(void) {
  char stack_base;
  ant_t *js = ant_create(nullptr);
  assert(js);
  js_setstackbase(js, &stack_base);

  /* ---- semantics: dense, sparse, holes, length --------------------------- */
  check(js,
    "var a = [];"
    "for (var i = 0; i < 8; i++) a[1000 + i] = i * 3;"
    "if (a[1003] !== 9) throw new Error('sparse read');"
    "if (a.length !== 1008) throw new Error('sparse length');"
    "a[1003] = 99;"
    "if (a[1003] !== 99) throw new Error('sparse write');"
    "if (a[7] !== undefined) throw new Error('hole must be undefined');"
    "if (('7' in a)) throw new Error('hole must not be in');"
    "a.length = 1;"
    "if (a[1003] !== undefined) throw new Error('truncated read');"
    "if (a.length !== 1) throw new Error('truncated length');"
    "a[1003] = 5;"
    "if (a[1003] !== 5 || a.length !== 1004) throw new Error('re-extend');",
    "sparse basics");

  /* dense array keeps working (the fast path must not shadow it) */
  check(js,
    "var d = [1, 2, 3, 4];"
    "if (d[2] !== 3) throw new Error('dense read');"
    "d[2] = 30;"
    "if (d[2] !== 30 || d.length !== 4) throw new Error('dense write');",
    "dense basics");

  /* ---- fast path unit checks -------------------------------------------- */
  ant_value_t arr = global_named(js, "a");
  assert(vtype(arr) == kTypeArray);

  ant_value_t fast = js_mkundef();
  assert(js_index_get_fast(js, arr, 1003, &fast));
  assert(vtype(fast) == kTypeNumber && tod(fast) == 5.0);

  /* hole: the fast path has nothing to answer with */
  fast = js_mkundef();
  assert(!js_index_get_fast(js, arr, 7, &fast));

  /* out of range: also declined (the caller must produce undefined) */
  assert(!js_index_get_fast(js, arr, 5000, &fast));

  /* deleted element: must not be resurrected by a stale shape slot */
  check(js, "delete a[1003]; if (a[1003] !== undefined) throw new Error('delete');", "delete");
  assert(!js_index_get_fast(js, arr, 1003, &fast));

  /* accessor on an index: reading runs the getter, so the fast path declines */
  check(js, "Object.defineProperty(a, 1005, { get: function () { return 42; }, configurable: true });", "accessor define");
  assert(!js_index_get_fast(js, arr, 1005, &fast));
  check(js, "if (a[1005] !== 42) throw new Error('accessor read');", "accessor read");

  /* index key on the prototype: fast path must not answer, js_arr_get must */
  check(js, "Array.prototype[7] = 777; var e = []; e[0] = 1;", "proto define");
  ant_value_t empty = global_named(js, "e");
  assert(vtype(empty) == kTypeArray);
  assert(!js_index_get_fast(js, empty, 7, &fast));
  /* Known deviation (recorded in agave docs/arch-capability-gaps.md): an array
   * read past its length returns undefined without consulting index keys on the
   * prototype, so this is NOT 777 today. Pinned here so a future fix flips this
   * assertion deliberately instead of silently changing the fast-path contract. */
  ant_value_t inherited = js_arr_get(js, empty, 7);
  assert(vtype(inherited) == kTypeUndefined);
  check(js, "delete Array.prototype[7];", "proto delete");

  /* ---- huge indices: the hash memo path, not the linear one -------------- */
  check(js,
    "var h = []; h[700000] = 1234; h[262144] = 99;"
    "if (h[700000] !== 1234 || h[262144] !== 99) throw new Error('huge read');"
    "h[700000] = 4321;"
    "if (h[700000] !== 4321) throw new Error('huge write');",
    "huge indices");
  {
    ant_value_t huge = global_named(js, "h");
    assert(vtype(huge) == kTypeArray);
    ant_value_t v = js_mkundef();
    assert(js_index_get_fast(js, huge, 700000, &v));
    assert(vtype(v) == kTypeNumber && tod(v) == 4321.0);
    assert(js_index_get_fast(js, huge, 262144, &v));
    assert(vtype(v) == kTypeNumber && tod(v) == 99.0);
  }

  /* ---- many slots, read back through the element opcode ------------------ */
  check(js,
    "var wide = [];"
    "for (var i = 0; i < 5000; i++) wide[i * 2] = i;"
    "var sum = 0;"
    "for (var i = 0; i < 5000; i++) sum += wide[i * 2];"
    "if (sum !== 12497500) throw new Error('wide sum ' + sum);"
    "if (wide.length !== 9999) throw new Error('wide length ' + wide.length);",
    "wide sparse");

  js_destroy(js);
  puts("PASS dense/sparse element access keeps JS semantics and the fast path declines what it cannot answer");
  return 0;
}
