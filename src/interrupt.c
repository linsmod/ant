#include "interrupt.h"
#include "errors.h"
#include <stdatomic.h>

ant_value_t js_interrupt_raise(ant_t *js) {
  /* Consume the request: it is one abort, not a mode. Leaving it set made every
   * later safepoint raise again, so the *next* page's scripts were killed too -
   * a browser that showed a blank page on every reload after one abort. The
   * result still carries SLOT_VM_TERMINATION, so the unwinding host can tell
   * this apart from an ordinary error. */
  atomic_store_explicit(&js->interrupt_pending, 0, memory_order_relaxed);
  js->terminating = true;
  
  ant_value_t error = js_take_thrown(js, js_mkerr(js, "Script execution interrupted"));
  if (is_object_type(error)) js_set_slot(error, SLOT_VM_TERMINATION, js_true);
  
  return js_throw(js, error);
}

bool js_is_termination(ant_value_t value) {
  return is_object_type(value) && js_get_slot(value, SLOT_VM_TERMINATION) == js_true;
}

bool js_termination_pending(ant_t *js) {
  return Ant_Exception_Pending(js) && js_is_termination(Ant_Exception_Value(js, Ant_Exception_Peek(js)));
}
