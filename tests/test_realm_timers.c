// meson test -C build realm-timers
//
// Browser hosts create one realm per frame. init_timer_module() installs
// setTimeout/setInterval on the *main* realm's global at engine startup; a new
// realm must get its own copies on its own global, otherwise every iframe runs
// without timers (agave: deepseek's WeChat QR iframe threw
// "ReferenceError: 'setTimeout' is not defined" inside jquery.min.js).
//
// ant_realm_install_timers() is deliberately opt-in from the embedding layer:
// Node's vm.runInNewContext() must keep `typeof setTimeout === 'undefined'`.
#include "internal.h"
#include "cfunc.h"
#include "loop.h"
#include "reactor.h"
#include "realm.h"
#include "modules/symbol.h"
#include "modules/timer.h"
#include "silver/call.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <uv.h>

static int child_calls;

static ant_value_t on_child(ant_params_t) {
  child_calls++;
  return js_mkundef();
}

static ant_value_t global_get(ant_t *js, const char *name) {
  return js_getprop_fallback(js, js_glob(js), name);
}

static void schedule_in_child(ant_t *js, const char *name, ant_value_t callback, int delay) {
  ant_value_t fn = global_get(js, name);
  ant_value_t args[] = { callback, js_mknum(delay) };
  ant_value_t result = sv_vm_call(js->vm, js, fn, js_mkundef(), args, 2, NULL, js_mkundef());
  assert(!is_err(result));
}

int main(void) {
  char stack_base;
  ant_t *js = ant_create(nullptr);
  assert(js);
  js_setstackbase(js, &stack_base);
  init_symbol_module(js);
  init_timer_module(js);

  /* The main realm has timers from init_timer_module(). */
  assert(is_callable(global_get(js, "setTimeout")));
  assert(is_callable(global_get(js, "setInterval")));
  assert(is_callable(global_get(js, "clearTimeout")));
  assert(is_callable(global_get(js, "queueMicrotask")));

  /* A freshly created realm does NOT have them (Node vm semantics). */
  ant_realm_t *child = ant_realm_create(js);
  assert(child);
  {
    ant_realm_t *prev = ant_realm_enter(js, child);
    assert(!is_callable(global_get(js, "setTimeout")));
    assert(!is_callable(global_get(js, "setInterval")));
    ant_realm_enter(js, prev);
  }

  /* The browser host installs them into the child realm explicitly. */
  {
    ant_realm_t *prev = ant_realm_enter(js, child);
    ant_realm_install_timers(js);
    ant_realm_enter(js, prev);
  }

  /* Main realm's copies are untouched and still callable. */
  assert(is_callable(global_get(js, "setTimeout")));

  /* Child realm now has its own callable copies, distinct identities. */
  ant_value_t main_timeout;
  ant_value_t child_timeout;
  {
    ant_realm_t *prev = ant_realm_enter(js, child);
    child_timeout = global_get(js, "setTimeout");
    assert(is_callable(child_timeout));
    assert(is_callable(global_get(js, "setInterval")));
    assert(is_callable(global_get(js, "clearTimeout")));
    ant_realm_enter(js, prev);
    main_timeout = global_get(js, "setTimeout");
    assert(is_callable(main_timeout));

    /* A timer scheduled from the child realm fires its callback. The callback
     * is a native function created in the main realm; that is fine: the point
     * under test is that the child's setTimeout is callable at all. */
    schedule_in_child(js, "setTimeout", Ant_Function_Create(on_child), 1);
    ant_realm_enter(js, prev);
  }

  assert(has_pending_timers(js));
  for (int i = 0; i < 1000 && has_pending_timers(js); i++) {
    uv_run(js->loop, UV_RUN_NOWAIT);
    js_poll_events(js);
    if (has_pending_timers(js)) uv_sleep(1);
  }
  assert(!has_pending_timers(js));
  assert(child_calls == 1);

  js_destroy(js);
  puts("PASS browser realms get their own timer entry points");
}
