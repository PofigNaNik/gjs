/* -*- mode: C; c-basic-offset: 4; indent-tabs-mode: nil; -*- */
/*
 * Copyright (c) 2008  litl, LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <config.h>

#include <string.h>
#include <limits.h>
#include <util/log.h>

#include "closure.h"
#include "keep-alive.h"
#include <gjs/gjs.h>
#include <gjs/compat.h>

typedef struct {
    GClosure base;
    JSRuntime *runtime;
    JSContext *context;
    JSObject *obj;
    guint unref_on_global_object_finalized : 1;
} Closure;

/*
 * The closure can be destroyed in several cases:
 * - invalidation by unref, e.g. when a signal is disconnected, closure is unref'd
 * - invalidation because we were invoked while the context was dead
 * - invalidation through finalization (we were garbage collected)
 *
 * These don't have to happen in the same order; garbage collection can
 * be either before, or after, context destruction.
 *
 */

static void
invalidate_js_pointers(Closure *c)
{
    if (c->obj == NULL)
        return;

    c->obj = NULL;
    c->context = NULL;
    c->runtime = NULL;

    /* Notify any closure reference holders they
     * may want to drop references.
     */
    g_closure_invalidate(&c->base);
}

static void
global_context_finalized(JSObject *obj,
                         void     *data)
{
    Closure *c;
    gboolean need_unref;

    c = data;

    gjs_debug_closure("Context global object destroy notifier on closure %p "
                      "which calls object %p",
                      c, c->obj);

    /* invalidate_js_pointers() could free us so check flag now to avoid
     * invalid memory access
     */
    need_unref = c->unref_on_global_object_finalized;
    c->unref_on_global_object_finalized = FALSE;

    if (c->obj != NULL) {
        g_assert(c->obj == obj);

        invalidate_js_pointers(c);
    }

    if (need_unref) {
        g_closure_unref(&c->base);
    }
}

static void
check_context_valid(Closure *c)
{
    JSContext *a_context;
    JSContext *iter;

    if (c->runtime == NULL)
        return;

    iter = NULL;
    while ((a_context = JS_ContextIterator(c->runtime,
                                           &iter)) != NULL) {
        if (a_context == c->context) {
            return;
        }
    }

    gjs_debug_closure("Context %p no longer exists, invalidating "
                      "closure %p which calls object %p",
                      c->context, c, c->obj);

    /* Did not find the context. */
    invalidate_js_pointers(c);
}

/* Invalidation is like "dispose" - it is guaranteed to happen at
 * finalize, but may happen before finalize. Normally, g_closure_invalidate()
 * is called when the "target" of the closure becomes invalid, so that the
 * source (the signal connection, say can be removed.) The usage above
 * in invalidate_js_pointers() is typical. Since the target of the closure
 * is under our control, it's unlikely that g_closure_invalidate() will ever
 * be called by anyone else, but in case it ever does, it's slightly better
 * to remove the "keep alive" here rather than in the finalize notifier.
 */
static void
closure_invalidated(gpointer data,
                    GClosure *closure)
{
    Closure *c;

    c = (Closure*) closure;

    gjs_debug_closure("Invalidating closure %p which calls object %p",
                      closure, c->obj);

    if (c->obj == NULL) {
        gjs_debug_closure("   (closure %p already dead, nothing to do)",
                          closure);
        return;
    }

    /* this will set c->obj to null if the context is dead
     */
    check_context_valid(c);

    if (c->obj == NULL) {
        /* Context is dead here. This happens if, as a side effect of
         * tearing down the context, the closure was invalidated,
         * say be some other finalized object that had a ref to
         * the closure dropping said ref.
         *
         * Because c->obj was not NULL at the start of
         * closure_invalidated, we know that
         * global_context_finalized() has not been called.  So we know
         * we are not being invalidated from inside
         * global_context_finalized().
         *
         * That means global_context_finalized() has yet to be called,
         * but we know it will be called, because the context is dead
         * and thus its global object should be finalized.
         *
         * We can't call gjs_keep_alive_remove_global_child() because
         * the context is invalid memory and we can't get to the
         * global object that stores the keep alive.
         *
         * So global_context_finalized() could be called on an
         * already-finalized closure. To avoid this, we temporarily
         * ref ourselves, and set a flag to remove this ref
         * in global_context_finalized().
         */
        gjs_debug_closure("   (closure %p's context was dead, holding ref "
                          "until global object finalize)",
                          closure);

        c->unref_on_global_object_finalized = TRUE;
        g_closure_ref(&c->base);
    } else {
        /* If the context still exists, then remove our destroy
         * notifier.  Otherwise we would call the destroy notifier on
         * an already-freed closure.
         *
         * This happens in the normal case, when the closure is
         * invalidated for some reason other than destruction of the
         * JSContext.
         */
        gjs_debug_closure("   (closure %p's context was alive, "
                          "removing our destroy notifier on global object)",
                          closure);
        gjs_keep_alive_remove_global_child(c->context,
                                           global_context_finalized,
                                           c->obj,
                                           c);

        c->obj = NULL;
        c->context = NULL;
        c->runtime = NULL;
    }
}

static void
closure_finalized(gpointer data,
                  GClosure *closure)
{
    GJS_DEC_COUNTER(closure);
}

void
gjs_closure_invoke(GClosure *closure,
                   int       argc,
                   jsval    *argv,
                   jsval    *retval)
{
    Closure *c;
    JSContext *context;

    c = (Closure*) closure;

    check_context_valid(c);
    context = c->context;

    if (c->obj == NULL) {
        /* We were destroyed; become a no-op */
        c->context = NULL;
        return;
    }

    JS_BeginRequest(context);

    if (JS_IsExceptionPending(context)) {
        gjs_debug_closure("Exception was pending before invoking callback??? "
                          "Not expected");
        gjs_log_exception(c->context, NULL);
    }

    if (!gjs_call_function_value(context,
                                 NULL, /* "this" object; NULL is some kind of default presumably */
                                 OBJECT_TO_JSVAL(c->obj),
                                 argc,
                                 argv,
                                 retval)) {
        /* Exception thrown... */
        gjs_debug_closure("Closure invocation failed (exception should "
                          "have been thrown) closure %p callable %p",
                          closure, c->obj);
        if (!gjs_log_exception(context, NULL))
            gjs_debug_closure("Closure invocation failed but no exception was set?");
        goto out;
    }

    if (gjs_log_exception(context, NULL)) {
        gjs_debug_closure("Closure invocation succeeded but an exception was set");
    }

 out:
    JS_EndRequest(context);
}

gboolean
gjs_closure_invoke_simple(JSContext   *context,
                          GClosure    *closure,
                          jsval       *retval,
                          const gchar *format,
                          ...)
{
    va_list ap;
    int argc;
    void *stack_space;
    jsval *argv;
    int i;

    JS_BeginRequest(context);

    va_start(ap, format);
    argv = JS_PushArgumentsVA(context, &stack_space, format, ap);
    va_end(ap);
    if (!argv)
        return FALSE;

    argc = (int)strlen(format);
    for (i = 0; i < argc; i++)
        JS_AddRoot(context, &argv[i]);
    JS_AddRoot(context, retval);

    gjs_closure_invoke(closure, argc, argv, retval);

    for (i = 0; i < argc; i++)
        JS_RemoveRoot(context, &argv[i]);
    JS_RemoveRoot(context, retval);

    JS_PopArguments(context, stack_space);

    JS_EndRequest(context);

    return TRUE;
}

JSContext*
gjs_closure_get_context(GClosure *closure)
{
    Closure *c;

    c = (Closure*) closure;

    check_context_valid(c);

    return c->context;
}

JSObject*
gjs_closure_get_callable(GClosure *closure)
{
    Closure *c;

    c = (Closure*) closure;

    return c->obj;
}

GClosure*
gjs_closure_new(JSContext  *context,
                   JSObject   *callable,
                   const char *description)
{
    Closure *c;

    c = (Closure*) g_closure_new_simple(sizeof(Closure), NULL);
    c->runtime = JS_GetRuntime(context);
    c->context = context;
    JS_BeginRequest(c->context);

    c->obj = callable;
    c->unref_on_global_object_finalized = FALSE;

    GJS_INC_COUNTER(closure);
    /* the finalize notifier right now is purely to track the counter
     * of how many closures are alive.
     */
    g_closure_add_finalize_notifier(&c->base, NULL, closure_finalized);

    gjs_keep_alive_add_global_child(c->context,
                                    global_context_finalized,
                                    c->obj,
                                    c);

    g_closure_add_invalidate_notifier(&c->base, NULL, closure_invalidated);

    gjs_debug_closure("Create closure %p which calls object %p '%s'",
                      c, c->obj, description);

    JS_EndRequest(c->context);

    return &c->base;
}

