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

#include <glib.h>
#include <glib/gstdio.h>
#include <gjs/gjs.h>
#include <util/crash.h>
#include <locale.h>

#include <string.h>

typedef struct {
    GjsContext *context;
} GjsTestJSFixture;

static char *top_srcdir;


static void
setup(GjsTestJSFixture *fix,
      gconstpointer     test_data)
{
    gboolean success;
    GError *error = NULL;
    int code;
    char *filename;
    char *search_path[2];

    search_path[0] = g_build_filename(top_srcdir, "test", "modules", NULL);
    search_path[1] = NULL;

    fix->context = gjs_context_new_with_search_path(search_path);
    g_free(search_path[0]);

    /* Load jsUnit.js directly into global scope, rather than
     * requiring each test to import it as a module, among other
     * things this lets us test importing modules without relying on
     * importing a module, but also it's just less typing to have
     * "assert*" without a prefix.
     */
    filename = g_build_filename(top_srcdir, "modules", "jsUnit.js", NULL);
    success = gjs_context_eval_file(fix->context, filename, &code, &error);
    g_free(filename);

    if (!success)
        g_error("%s", error->message);
}

static void
teardown(GjsTestJSFixture *fix,
         gconstpointer     test_data)
{
    gjs_memory_report("before destroying context", FALSE);
    g_object_unref(fix->context);
    gjs_memory_report("after destroying context", TRUE);
}

static void
test(GjsTestJSFixture *fix,
     gconstpointer     test_data)
{
    GError *error = NULL;
    gboolean success;
    int code;

    success = gjs_context_eval_file(fix->context, (const char*)test_data, &code, &error);
    if (!success)
        g_error("%s", error->message);
    g_assert(error == NULL);
    g_assert_cmpint(code, ==, 0);
}

static GSList *
read_all_dir_sorted (const char *dirpath)
{
    GSList *result = NULL;
    GDir *dir;
    const char *name;

    dir = g_dir_open(dirpath, 0, NULL);
    g_assert(dir != NULL);

    while ((name = g_dir_read_name(dir)) != NULL)
        result = g_slist_prepend (result, g_strdup (name));
    result = g_slist_sort(result, (GCompareFunc) strcmp);

    g_dir_close(dir);
    return result;
}

int
main(int argc, char **argv)
{
    /* These are relative to top_builddir */
    const char * const path_directories[] = {
        GJS_TOP_SRCDIR"/modules",
        GJS_TOP_SRCDIR"/test/js/modules",
        ".libs:",
        NULL
    };

    char *js_test_dir;
    char *working_dir;
    char *gjs_unit_path;
    char *gjs_unit_dir;
    char *top_builddir;
    char *data_home;
    GString *path;
    size_t i;
    GSList *all_tests, *iter;

    working_dir = g_get_current_dir();

    if(g_path_is_absolute(argv[0]))
        gjs_unit_path = g_strdup(argv[0]);
    else
        gjs_unit_path = g_build_filename(working_dir, argv[0], NULL);

    gjs_unit_dir = g_path_get_dirname(gjs_unit_path);
    /* the gjs-unit executable will be in <top_builddir>/.libs */
    top_builddir = g_build_filename(gjs_unit_dir, "..", NULL);
    top_srcdir = g_build_filename(top_builddir, GJS_TOP_SRCDIR, NULL);

    /* Normalize, not strictly necessary */
    g_chdir(top_builddir);
    g_free(top_builddir);
    top_builddir = g_get_current_dir();

    g_chdir(top_srcdir);
    g_free(top_srcdir);
    top_srcdir = g_get_current_dir();

    g_chdir(working_dir);

    /* we're always going to use uninstalled files, set up necessary
     * environment variables, but don't overwrite if already set */

    data_home = g_build_filename(top_builddir, "test_user_data", NULL);
    path = g_string_new(NULL);
    for(i = 0; i < G_N_ELEMENTS(path_directories); i++) {
        char *directory;

        if (i != 0)
            g_string_append_c(path, ':');

        directory = g_build_filename(top_builddir, path_directories[i], NULL);
        g_string_append(path, directory);
        g_free(directory);
    }

    g_setenv("TOP_SRCDIR", top_srcdir, FALSE);
    g_setenv("BUILDDIR", top_builddir, FALSE);
    g_setenv("XDG_DATA_HOME", data_home, FALSE);
    g_setenv("GJS_PATH", path->str, FALSE);

    gjs_init_sleep_on_crash();

    setlocale(LC_ALL, "");
    g_test_init(&argc, &argv, NULL);

    g_type_init();

    /* iterate through all 'test*.js' files in ${top_srcdir}/test/js */
    js_test_dir = g_build_filename(top_srcdir, "test", "js", NULL);

    all_tests = read_all_dir_sorted(js_test_dir);
    for (iter = all_tests; iter; iter = iter->next) {
        char *name = iter->data;
        char *test_name;
        char *file_name;

        if (!(g_str_has_prefix(name, "test") &&
              g_str_has_suffix(name, ".js")))
            continue;

        /* pretty print, drop 'test' prefix and '.js' suffix from test name */
        test_name = g_strconcat("/js/", name + 4, NULL);
        test_name[strlen(test_name)-3] = '\0';

        file_name = g_build_filename(js_test_dir, name, NULL);
        g_test_add(test_name, GjsTestJSFixture, file_name, setup, test, teardown);
        g_free(name);
        g_free(test_name);
        /* not freeing file_name as it's needed while running the test */
    }
    g_slist_free(all_tests);

    return g_test_run ();
}
