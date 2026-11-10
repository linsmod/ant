/*
 * Interface bindings: a class that carries the WebIDL shape.
 *
 * A plain bridge class only gives wrappers a prototype object. An interface
 * adds the parent chain, the constructor object (with the interface name), the
 * @@toStringTag and a class-level getter/setter pair, so wrappers answer
 * instanceof / Object.getPrototypeOf / constructor / Object.prototype.toString
 * the way a browser does.
 *
 * The test builds EventTarget <- Node <- Element <- HTMLElement <-
 * HTMLDivElement, wraps a native object as the leaf, and checks the JS-visible
 * semantics of the chain, the constructor (including "Illegal constructor"),
 * static constants, and property dispatch (getter, setter, catch-all).
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "AntBridge.h"

#define INTERFACE_TAG 0x494E5446u  /* 'INTF' */

static double g_seven = 7.0;

static int iface_get(AntbEngine* engine, AntbValue* self, const char* name,
                     AntbValue* out, void* user)
{
    AntbValue* v;

    (void)self;
    (void)user;

    if (!name || strcmp(name, "seven") != 0)
        return 0;

    v = antb_make_number(engine, g_seven);
    if (!v)
        return 0;

    antb_value_assign(out, v);
    antb_value_release(v);
    return 1;
}

static int iface_set(AntbEngine* engine, AntbValue* self, const char* name,
                     const AntbValue* value, void* user)
{
    (void)engine;
    (void)self;
    (void)user;

    if (!name || strcmp(name, "seven") != 0)
        return 0;

    g_seven = antb_to_number(value);
    return 1;
}

static int catch_all_get(AntbEngine* engine, AntbValue* self, const char* name,
                         AntbValue* out, void* user)
{
    AntbValue* v;

    (void)self;
    (void)user;

    /* A catch-all only answers the names the facade owns: everything else must
     * fall through to the prototype chain (constructor, toString, ...), which
     * is exactly the rule nodeGetter follows. */
    if (!name || strcmp(name, "unknownName") != 0)
        return 0;

    v = antb_make_number(engine, 1234.0);
    if (!v)
        return 0;

    antb_value_assign(out, v);
    antb_value_release(v);
    return 1;
}

static const char kScript[] =
    "var r = [];"
    "r.push(Object.getPrototypeOf(div) === ProbeHTMLDivElement.prototype);"
    "r.push(Object.getPrototypeOf(ProbeHTMLDivElement.prototype) === ProbeHTMLElement.prototype);"
    "r.push(Object.getPrototypeOf(ProbeHTMLElement.prototype) === ProbeElement.prototype);"
    "r.push(Object.getPrototypeOf(ProbeElement.prototype) === ProbeNode.prototype);"
    "r.push(Object.getPrototypeOf(ProbeNode.prototype) === ProbeEventTarget.prototype);"
    "r.push(Object.getPrototypeOf(ProbeEventTarget.prototype) === Object.prototype);"
    "r.push(div instanceof ProbeHTMLDivElement);"
    "r.push(div instanceof ProbeHTMLElement);"
    "r.push(div instanceof ProbeElement);"
    "r.push(div instanceof ProbeNode);"
    "r.push(div instanceof ProbeEventTarget);"
    "r.push(div instanceof Object);"
    "r.push(div.constructor === ProbeHTMLDivElement);"
    "r.push(div.constructor.name === 'ProbeHTMLDivElement');"
    "r.push(Object.prototype.toString.call(div) === '[object ProbeHTMLDivElement]');"
    "r.push(ProbeHTMLDivElement.ELEMENT_NODE === 1);"
    "r.push(ProbeHTMLDivElement.TEXT_NODE === 3);"
    "r.push(ProbeHTMLDivElement.prototype[Symbol.toStringTag] === 'ProbeHTMLDivElement');"
    "var pd = Object.getOwnPropertyDescriptor(ProbeHTMLElement, 'prototype');"
    "r.push(pd.writable === false && pd.enumerable === false && pd.configurable === false);"
    "var cd = Object.getOwnPropertyDescriptor(ProbeHTMLDivElement.prototype, 'constructor');"
    "r.push(cd.writable === true && cd.enumerable === false && cd.configurable === true);"
    "var illegal = false;"
    "try { new ProbeHTMLElement(); } catch (e) { illegal = e instanceof TypeError; }"
    "r.push(illegal);"
    "r.push(div.seven === 7);"
    "div.seven = 9;"
    "r.push(div.seven === 9);"
    "r.push(div.unknownName === 1234);"
    "r.push(typeof div.toString === 'function' && div.toString() === '[object ProbeHTMLDivElement]');"
    "var bad = [];"
    "r.forEach(function (x, i) { if (x !== true) bad.push(i); });"
    "var bad = [];"
    "r.forEach(function (x, i) { if (x !== true) bad.push(i); });"
    "bad.length === 0 || bad.join('|');";

int main(void)
{
    volatile char stack_anchor = 0;
    AntbEngine* engine = antb_engine_create((const void*)&stack_anchor);
    AntbValue* global;
    AntbValue* wrapper;
    AntbClass* event_target;
    AntbClass* node;
    AntbClass* element;
    AntbClass* html_element;
    AntbClass* div_element;
    char* err = NULL;
    AntbValue* result;
    int failures = 0;

    if (!engine) {
        printf("FAIL: engine create failed\n");
        return 1;
    }

    global = antb_global(engine);
    if (!global) {
        printf("FAIL: no global object\n");
        return 1;
    }

    event_target = antb_class_create_iface(engine, "ProbeEventTarget", NULL);
    node         = antb_class_create_iface(engine, "ProbeNode", event_target);
    element      = antb_class_create_iface(engine, "ProbeElement", node);
    html_element = antb_class_create_iface(engine, "ProbeHTMLElement", element);
    div_element  = antb_class_create_iface(engine, "ProbeHTMLDivElement", html_element);

    if (!event_target || !node || !element || !html_element || !div_element) {
        printf("FAIL: interface create failed\n");
        antb_engine_destroy(engine);
        return 1;
    }

    antb_class_add_constant(engine, node, "ELEMENT_NODE", 1.0);
    antb_class_add_constant(engine, node, "TEXT_NODE", 3.0);

    /* Property dispatch: explicit name first, then the catch-all. */
    if (!antb_class_set_property_rw(engine, node, "seven", iface_get, iface_set, NULL)) {
        printf("FAIL: class property registration failed\n");
        failures++;
    }
    antb_class_set_property(engine, node, "", catch_all_get, NULL);

    wrapper = antb_wrap_native_as(engine, div_element, (void*)&g_seven, INTERFACE_TAG);
    if (!wrapper) {
        printf("FAIL: wrap failed\n");
        antb_engine_destroy(engine);
        return 1;
    }
    antb_set_prop(engine, global, "div", wrapper);
    antb_value_release(wrapper);

    antb_expose_classes(engine);

    result = antb_eval(engine, kScript, (int)strlen(kScript), &err);

    if (!result || !antb_is_boolean(result) || !antb_to_boolean(result)) {
        char* text = result ? antb_to_utf8(result) : NULL;

        printf("FAIL: interface semantics: result=%s%s%s\n",
               text ? text : "(null)", err ? " err=" : "", err ? err : "");
        antb_free(text);
        failures++;
    }

    antb_value_release(result);
    antb_free(err);
    antb_value_release(global);
    antb_engine_destroy(engine);

    if (failures) {
        printf("BRIDGE INTERFACE CHECKS FAILED (%d)\n", failures);
        return 1;
    }

    printf("ALL BRIDGE INTERFACE CHECKS PASSED\n");
    return 0;
}
