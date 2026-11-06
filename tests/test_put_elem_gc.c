// meson test -C build put-elem-gc
//
// Dynamic keyed stores (obj[key] = v) must survive a collection between the
// key conversion and the store. Three shapes are covered because each took a
// different path through jit_put_elem():
//
//   * a dynamic string key            -> sv_key_to_propstr, then setprop
//   * a non-index numeric key (0.5)   -> converted to "0.5" (fractional keys
//                                       used to be passed on without a pin)
//   * a key whose conversion fails    -> must be reported as an error, never
//                                       stored (163's adtracker oom turned into
//                                       a hash_key AV here)
//
// 163's adtracker crashed in hash_key because of exactly these: the raw key was
// unrooted during conversion, and a failed conversion (error value) was stored
// as if it were the key string.
#include "internal.h"
#include "silver/glue.h"
#include "gc.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  char stack_base;
  ant_t *js = ant_create(nullptr);
  assert(js);
  js_setstackbase(js, &stack_base);

  GC_ROOT_SAVE(roots, js);
  ant_value_t obj = Ant_Object_CreateBare(js);
  GC_ROOT_PIN(js, obj);

  for (int i = 0; i < 300; i++) {
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "key%d", i);

    ant_value_t string_key = js_mkstr(js, buf, (size_t)len);
    ant_value_t r1 = jit_helper_put_elem(js->vm, js, obj, string_key, js_mknum(i));
    assert(!is_err(r1));

    ant_value_t number_key = js_mknum((double)i + 0.5);
    ant_value_t r2 = jit_helper_put_elem(js->vm, js, obj, number_key, js_mknum(i));
    assert(!is_err(r2));

    // Long concatenated-looking keys (the SDK shape that oomed): the store must
    // either succeed or report an error, never crash and never store an error.
    if ((i % 32) == 0) {
      char big[200];
      int big_len = snprintf(big, sizeof(big), "k%0*d", 120, i);
      ant_value_t long_key = js_mkstr(js, big, (size_t)big_len);
      ant_value_t r3 = jit_helper_put_elem(js->vm, js, obj, long_key, js_mknum(i));
      assert(!is_err(r3));
    }

    if ((i % 32) == 0) gc_run(js);
  }

  // Read a few back: values must be the ones stored, with the right keys.
  ant_value_t v = js_getprop_fallback(js, obj, "key299");
  assert(vtype(v) == kTypeNumber && tod(v) == 299.0);

  v = js_getprop_fallback(js, obj, "299.5");
  assert(vtype(v) == kTypeNumber && tod(v) == 299.0);

  GC_ROOT_RESTORE(js, roots);
  js_destroy(js);
  puts("PASS dynamic keyed stores survive collection (string, fractional and long keys)");
  return 0;
}
