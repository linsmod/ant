#ifndef ANT_VM_MODULE_H
#define ANT_VM_MODULE_H

#include "types.h"
#include "silver/engine.h"

ant_value_t vm_library(ant_t *js);
ant_value_t js_vm_dynamic_import(ant_t *js, sv_code_unit_t *unit, ant_value_t specifier, ant_value_t options);

/* Dynamic import with an explicit base URL (classic scripts / eval code have no
 * import callback but still resolve relative specifiers against their own URL). */
ant_value_t js_dynamic_import_from(ant_t *js, ant_value_t specifier, ant_value_t options, const char *base_path);

#endif
