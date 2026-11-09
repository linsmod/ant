#ifdef ANT_REALM_FIELD
ANT_REALM_FIELD(global)
ANT_REALM_FIELD(global_this)
ANT_REALM_FIELD(global_sandbox)
ANT_REALM_FIELD(microtask_realm)
ANT_REALM_FIELD(codegen_blocked)
ANT_REALM_FIELD(global_lexicals)
ANT_REALM_FIELD(global_lexical_count)
ANT_REALM_FIELD(global_lexical_cap)
ANT_REALM_FIELD(global_lexical_index)
ANT_REALM_FIELD(global_lexical_index_cap)
ANT_REALM_FIELD(array_chain_plain_epoch)
ANT_REALM_FIELD(promise_constructor_protector_invalid)
ANT_REALM_FIELD(promise_resolve_lookup_protector_invalid)
ANT_REALM_FIELD(promise_species_protector_invalid)
ANT_REALM_FIELD(array_species_protector_invalid)
ANT_REALM_FIELD(array_iteration_protector_invalid)
ANT_REALM_FIELD(promise_then_protector_invalid)
ANT_REALM_FIELD(regexp)
ANT_REALM_FIELD(iterators)
/* The module world belongs to the realm: ESM registry (js->modules.state), the
 * require/import caches and the module evaluation stack are per realm, so two
 * frames importing the same URL each get their own module instance (the HTML
 * spec's module map is per environment settings object). */
ANT_REALM_FIELD(modules)
#undef ANT_REALM_FIELD
#endif
