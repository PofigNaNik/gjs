// Copyright 2012 Giovanni Campagna
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to
// deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
// sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.

/**
 * This module provides a set of convenience APIs for building packaged
 * applications.
 */

const GLib = imports.gi.GLib;
const GIRepository = imports.gi.GIRepository;
const Gio = imports.gi.Gio;
const System = imports.system;

const Gettext = imports.gettext;

/*< public >*/
var name;
var version;
var prefix;
var datadir;
var libdir;
var pkgdatadir;
var pkglibdir;
var moduledir;
var localedir;

/*< private >*/
let _base;
let _requires;
let _dumpRequires = false;

function _runningFromSource(name) {
    if (System.version >= 13600) {
        let fileName = System.programInvocationName;

        let binary = Gio.File.new_for_path(fileName);
        let cwd = Gio.File.new_for_path('.');
        return binary.has_prefix(cwd);
    } else {
        return GLib.file_test(name + '.doap',
                              GLib.FileTest.EXISTS);
    }
}

/**
 * init:
 * @params: package parameters
 *
 * Initialize directories and global variables. Must be called
 * before any of other API in Package is used.
 * @params must be an object with at least the following keys:
 *  - name: the package name ($(PACKAGE_NAME) in autotools)
 *  - version: the package version
 *  - prefix: the installation prefix
 *
 * init() will take care to check if the program is running from
 * the source directory or not, by looking for a 'src' directory.
 *
 * At the end, the global variable 'pkg' will contain the
 * Package module (imports.package). Additionally, the following
 * module variables will be available:
 *  - name, version: same as in @params
 *  - prefix: the installation prefix (as passed in @params)
 *  - datadir, libdir: the final datadir and libdir when installed;
 *                     usually, these would be prefix + '/share' and
 *                     and prefix + '/lib' (or '/lib64')
 *  - pkgdatadir: the directory to look for private data files, such as
 *                images, stylesheets and UI definitions;
 *                this will be datadir + name when installed and
 *                './data' when running from the source tree
 *  - pkglibdir: the directory to look for private typelibs and C
 *               libraries;
 *               this will be libdir + name when installed and
 *               './lib' when running from the source tree
 *  - moduledir: the directory to look for JS modules;
 *               this will be pkglibdir when installed and
 *               './src' when running from the source tree
 *  - localedir: the directory containing gettext translation files;
 *               this will be datadir + '/locale' when installed
 *               and './po' in the source tree
 *
 * All paths are absolute and will not end with '/'.
 *
 * As a side effect, init() calls GLib.set_prgname().
 */
function init(params) {
    window.pkg = imports.package;
    name = params.name;
    version = params.version;

    // Must call it first, because it can only be called
    // once, and other library calls might have it as a
    // side effect
    GLib.set_prgname(name);

    prefix = params.prefix;
    libdir = params.libdir;
    datadir = GLib.build_filenamev([prefix, 'share']);
    let libpath, girpath;

    if (_runningFromSource(name)) {
        log('Running from source tree, using local files');
        // Running from source directory
        _base = GLib.get_current_dir();
        pkglibdir = GLib.build_filenamev([_base, 'lib']);
        libpath = GLib.build_filenamev([pkglibdir, '.libs']);
        girpath = pkglibdir;
        pkgdatadir = GLib.build_filenamev([_base, 'data']);
        localedir = GLib.build_filenamev([_base, 'po']);
        moduledir = GLib.build_filenamev([_base, 'src']);
    } else {
        _base = prefix;
        pkglibdir = GLib.build_filenamev([libdir, name]);
        libpath = pkglibdir;
        girpath = GLib.build_filenamev([pkglibdir, 'girepository-1.0']);
        pkgdatadir = GLib.build_filenamev([datadir, name]);
        localedir = GLib.build_filenamev([datadir, 'locale']);
        moduledir = pkgdatadir;
    }

    imports.searchPath.unshift(moduledir);
    GIRepository.Repository.prepend_search_path(girpath);
    GIRepository.Repository.prepend_library_path(libpath);

    _parseArgs();
}

/**
 * start:
 * @params: see init()
 *
 * This is a convenience function if your package has a
 * single entry point.
 * You must define a main(ARGV) function inside a main.js
 * module in moduledir.
 */
function start(params) {
    init(params);

    return imports.main.main(ARGV);
}

/**
 * require:
 * @libs: the external dependencies to import
 *
 * Mark a dependency on a specific version of one or more
 * external GI typelibs.
 * @libs must be an object whose keys are a typelib name,
 * and values are the respective version. The empty string
 * indicates any version.
 */
function require(libs) {
    _requires = libs;

    if (_dumpRequires) {
        dumpRequires();
        System.exit(0);
    }

    for (let l in libs) {
        let version = libs[l];

        if (version != '')
            imports.gi.versions[l] = version;

        try {
            // Import the package to check the version
            imports.gi[l];
        } catch(e) {
            printerr('Unsatisfied dependency: ' + e.message);
            System.exit(1);
        }
    }
}

function dumpRequires() {
    print(JSON.stringify(_requires));
}

function initGettext() {
    Gettext.bindtextdomain(name, localedir);
    Gettext.textdomain(name);

    let gettext = imports.gettext;
    window._ = gettext.gettext;
    window.C_ = gettext.pgettext;
    window.N_ = function(x) { return x; }
}

function initFormat() {
    let format = imports.format;
    String.prototype.format = format.format;
}

function initSubmodule(name) {
    if (moduledir != pkgdatadir) {
        // Running from source tree, add './name' to search paths

        let submoduledir = GLib.build_filenamev([_base, name]);
        let libpath = GLib.build_filenamev([submoduledir, '.libs']);
        GIRepository.Repository.prepend_search_path(submoduledir);
        GIRepository.Repository.prepend_library_path(libpath);
    } else {
        // Running installed, submodule is in $(pkglibdir), nothing to do
    }
}

function loadResource(name) {
    name = (name || this.name) + '.gresource';

    Gio.Resource.load(GLib.build_filenamev([pkg.pkgdatadir, name]))._register();
}

function _spawnGDB(debugIndex) {
    ARGV.splice(debugIndex, 1);

    try {
        System.exec(['gdb', '--args', 'gjs', System.programInvocationName].concat(ARGV));
    } catch(e) {
        print('Failed to launch debugger: ' + e.message);
        System.exit(1);
    }
}

function _parseArgs() {
    for (let i = 0; i < ARGV.length; i++) {
        if (ARGV[i] == '--debug') {
            _spawnGDB(i);
        } else if (ARGV[i] == '--requires') {
            _dumpRequires = true;
        }
    }
}
