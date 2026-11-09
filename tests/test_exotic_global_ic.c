/*
 * The global property inline cache must keep working when the global object is
 * exotic.
 *
 * A browser window (agave, R148) installs a catch-all getter on the realm's
 * global object to answer the window's named/index properties. That makes the
 * global `is_exotic`, and both sv_global_ic_try_get_hit() and
 * sv_global_ic_try_fill() used to refuse such an object, so every global read
 * in a loop fell back to the full property walk: 5 ns -> 470 ns per read.
 *
 * The IC only ever caches a name that resolves to a plain data slot on the
 * global, and js_getprop_fallback_len() looks the own slot up *before* it
 * consults the exotic getter, so caching that slot is sound. A name the object
 * lacks is never cached - the getter keeps deciding it.
 *
 * meson test -C build-zig exotic-global-ic
 */
#include "internal.h"
#include "silver/engine.h"
#include "silver/ops/globals.h"
#include "debug.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Answers every name the global does not own, like a window getter does. */
static ant_value_t catch_all_getter(
    ant_t *js, ant_value_t obj, const char *key, size_t key_len
) {
  (void)obj;
  (void)key;
  (void)key_len;
  return js_mkstr(js, "from-getter", 11);
}

static void run(ant_t *js, const char *source) {
  ant_value_t result = js_eval_bytecode(js, source, strlen(source));
  assert(!is_err(result));
}

int main(void) {
  char stack_base;
  ant_t *js = ant_create(NULL);

  assert(js);
  js_setstackbase(js, &stack_base);

  run(js,
      "var hotValue = 7;"
      "function spin(n) {"
      "  var s = 0;"
      "  for (var i = 0; i < n; i++) s += hotValue;"
      "  return s;"
      "}"
      "spin(1); spin(2);");

  /* The global now answers misses: exactly the agave window case. */
  js_set_getter(js->global, catch_all_getter);
  assert(js_obj_ptr(js->global)->flags.is_exotic);

  /* An owned data slot still wins over the getter. */
  ant_value_t direct = js_get(js, js->global, "hotValue");
  assert(vtype(direct) == kTypeNumber && tod(direct) == 7.0);

  /* ... and the loop keeps reading the slot, not the getter's string. */
  run(js, "var spinResult = spin(1000);");
  ant_value_t result = js_get(js, js->global, "spinResult");
  assert(!is_err(result));
  assert(vtype(result) == kTypeNumber);
  assert(tod(result) == 7000.0);

  /* A missing name is not cached and goes through the getter. */
  ant_value_t missing = js_get(js, js->global, "notDeclaredAnywhere");
  assert(vtype(missing) == kTypeString);

  /* The IC entry for hotValue is usable on an exotic global: this is the
   * assertion that regressed (the guard used to bail on is_exotic). */
  ant_value_t spin = js_get(js, js->global, "spin");
  sv_closure_t *closure = js_func_closure(spin);
  assert(closure && closure->func && closure->func->ic_slots);

  ant_object_t *global = sv_global_obj_ptr(js->global);
  const char *hot = intern_string(js, "hotValue", 8);
  assert(hot);

  sv_ic_entry_t *slot = NULL;
  for (int i = 0; i < closure->func->ic_count; i++) {
    sv_ic_entry_t *ic = &closure->func->ic_slots[i];
    if (ic->cached_holder == global) {
      slot = ic;
      break;
    }
  }
  assert(slot && "spin() must record a global IC entry");

  ant_value_t cached = js_mkundef();
  assert(sv_global_ic_try_get_hit(js, js->global, slot, hot, &cached));
  assert(vtype(cached) == kTypeNumber && tod(cached) == 7.0);

  printf("ALL EXOTIC-GLOBAL-IC CHECKS PASSED\n");
  return 0;
}
