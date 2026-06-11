"""
em-x11 Emscripten port.

Usage:
    emcc myapp.c -sUSE_EM_X11 -o myapp.html
    emcc myapp.c --use-port=em_x11 -o myapp.html

The port links against em-x11's static archives (libX11.a, libXext.a,
libXft.a, ...) and injects the JS-side bridge library
(native/src/lib/library_em-x11.js) so C code calling Xlib APIs works
in the browser with zero user JS glue.

Set EM_X11_SRC to point at the em-x11 clone if the port script isn't
co-located with the source tree.
"""

import os
import logging
import subprocess

VERSION = '0.0.1'
LICENSE = 'MIT'

logger = logging.getLogger('em_x11')

# All static archives the port provides, in link order (higher-level first).
# Order matches a standard Xaw program's link line:
#   Xaw -> Xmu -> Xt -> Xpm -> SM -> ICE -> xtrans -> Xft -> Xrender -> fontconfig -> GLX -> Xext -> X11
_PORT_LIBS = [
    'libXaw.a',
    'libXmu.a',
    'libXpm.a',
    'libXt.a',
    'libSM.a',
    'libICE.a',
    'libxtrans.a',
    'libXft.a',
    'libXrender.a',
    'libfontconfig.a',
    'libGLX.a',
    'libXext.a',
    'libX11.a',
]


def needed(settings):
    """Return True when -sUSE_EM_X11 or --use-port=em_x11 is active.

    Emscripten >= 5.0.0 asserts in settings.__getattr__ when an unknown
    key is read in limited-settings mode, so the usual getattr default
    doesn't help.  Catch the assertion and treat it as False.
    """
    try:
        return getattr(settings, 'USE_EM_X11', False)
    except AssertionError:
        return False


def get_lib_name(settings):
    return 'libem_x11.a'


def find_em_x11_root():
    """Walk up from this script to find the em-x11 project root."""
    env = os.environ.get('EM_X11_SRC', '')
    if env:
        root = os.path.abspath(env)
        if os.path.isdir(root):
            return root
        logger.warning('EM_X11_SRC is set but not a directory: %s', root)

    # Script is at <root>/tools/ports/em_x11.py
    script_dir = os.path.dirname(os.path.abspath(__file__))
    root = os.path.normpath(os.path.join(script_dir, '..', '..'))
    if os.path.isdir(os.path.join(root, 'native')):
        return root

    # If the port was copied into emscripten's own tools/ports/, the
    # relative path won't work.  Fall back to the current directory.
    cwd = os.getcwd()
    if os.path.isdir(os.path.join(cwd, 'native')):
        return cwd

    return None


def _find_artifacts(em_x11_root):
    """Return the path to the artifacts directory, or None.

    Prefers the main cmake build output (build/artifacts/) which is
    always present when building in-tree (pnpm build:native).  Falls
    back to a dedicated port-build directory.
    """
    # Main build — the fast path. Already built by pnpm build:native.
    main = os.path.join(em_x11_root, 'build', 'artifacts')
    if os.path.isdir(main) and os.path.exists(os.path.join(main, 'libX11.a')):
        return main

    # Port-specific build — only used by external projects that point
    # at a clean em-x11 clone.
    port_build = os.path.join(em_x11_root, 'build', 'port-build', 'artifacts')
    if os.path.isdir(port_build) and os.path.exists(os.path.join(port_build, 'libX11.a')):
        return port_build

    return None


def _ensure_built(em_x11_root, artifacts_dir):
    """Build em-x11 via cmake if artifacts don't exist yet."""
    if artifacts_dir and os.path.exists(os.path.join(artifacts_dir, 'libX11.a')):
        return artifacts_dir

    # Check third-party sources
    third_party = os.path.join(em_x11_root, 'ignored-area', 'third-party')
    if not os.path.isdir(os.path.join(third_party, 'libXt')):
        script = os.path.join(em_x11_root, 'scripts', 'fetch-third-party.sh')
        if os.path.isfile(script):
            logger.info('Third-party sources missing; running fetch-third-party.sh ...')
            subprocess.run(['bash', script], cwd=em_x11_root, check=True)

    build_dir = os.path.join(em_x11_root, 'build', 'port-build')
    out = os.path.join(build_dir, 'artifacts')
    marker = os.path.join(out, '.em-x11-port-built')

    # Check freshness
    need_build = not os.path.exists(marker)
    if not need_build:
        marker_mtime = os.path.getmtime(marker)
        native_dir = os.path.join(em_x11_root, 'native')
        for dirpath, _dirnames, filenames in os.walk(native_dir):
            for fn in filenames:
                if fn.endswith(('.c', '.h')):
                    if os.path.getmtime(os.path.join(dirpath, fn)) > marker_mtime:
                        need_build = True
                        break
            if need_build:
                break

    if need_build:
        cmake_cmd = [
            'emcmake', 'cmake',
            '-S', em_x11_root,
            '-B', build_dir,
            '-DCMAKE_BUILD_TYPE=Release',
            '-DEM_X11_BUILD_SIDE_MODULE=OFF',
        ]
        logger.info('Configuring em-x11: %s', ' '.join(cmake_cmd))
        subprocess.run(cmake_cmd, cwd=em_x11_root, check=True)

        build_cmd = ['cmake', '--build', build_dir, '-j']
        logger.info('Building em-x11: %s', ' '.join(build_cmd))
        subprocess.run(build_cmd, cwd=em_x11_root, check=True)

        open(marker, 'w').close()

    return out


def get(ports, settings, shared):
    """Return static archive paths for linking.

    Returns the full filesystem paths so emcc passes them through to
    wasm-ld without triggering the map_to_js_libs hijack on -lX11 / -lGL.

    TODO: wrap with shared.cache.get_lib() once the port lands in
    upstream emscripten and can use ports.install_file / install_header_dir.
    """
    em_x11_root = find_em_x11_root()
    if not em_x11_root:
        logger.error(
            'Cannot find em-x11 source root. '
            'Set EM_X11_SRC=/path/to/em-x11 or place this script under '
            '<em_x11>/tools/ports/em_x11.py'
        )
        return []

    artifacts_dir = _find_artifacts(em_x11_root)
    if artifacts_dir:
        logger.debug('Using pre-built artifacts: %s', artifacts_dir)
    else:
        artifacts_dir = _ensure_built(em_x11_root, artifacts_dir)

    lib_paths = []
    for lib in _PORT_LIBS:
        p = os.path.join(artifacts_dir, lib)
        if os.path.exists(p):
            lib_paths.append(p)
        else:
            logger.debug('Skipping %s (not built)', lib)

    return lib_paths


def clear(ports, settings, shared):
    shared.cache.erase_lib(get_lib_name(settings))
    em_x11_root = find_em_x11_root()
    if not em_x11_root:
        return
    import shutil
    port_build = os.path.join(em_x11_root, 'build', 'port-build')
    if os.path.isdir(port_build):
        logger.info('Clearing em-x11 port build: %s', port_build)
        shutil.rmtree(port_build)


def process_args(ports):
    """Return compile flags: include path, JS library, and default Host.

    --js-library injects the bridge functions.  --pre-js injects the
    default Host IIFE so Layer 1 (zero JS) mode auto-creates a Host
    on Module.canvas.  Users who want a custom Host can set
    Module['emX11NoAutoStart'] = true and call initEmX11() manually.

    In the SIDE_MODULE path (Pyodide dlopen) the EM_JS bodies carry
    the implementations; the JS library and default host are only
    needed for the static-link path.
    """
    em_x11_root = find_em_x11_root()
    if not em_x11_root:
        return []
    include_dir = os.path.join(em_x11_root, 'native', 'include')
    js_library = os.path.join(em_x11_root, 'native', 'src', 'lib', 'library_em-x11.js')
    default_host = os.path.join(em_x11_root, 'build', 'artifacts', 'em-x11-default-host.js')
    args = ['-I' + include_dir]
    if os.path.exists(js_library):
        args.append('--js-library=' + js_library)
    shell_file = os.path.join(em_x11_root, 'tools', 'ports', 'shell.html')
    if os.path.exists(default_host):
        args.append('--pre-js=' + default_host)
    if os.path.exists(shell_file):
        args.append('--shell-file=' + shell_file)
    return args


def process_dependencies(settings):
    """Ensure runtime symbols the JS library needs are exported.

    Emscripten >= 5.0.0 may call this during the compile phase where
    settings is in limited mode and rejects writes.  Silently skip in
    that case — the symbols will be set during the link phase instead.
    """
    try:
        settings.EXPORTED_RUNTIME_METHODS.append('ccall')
        settings.EXPORTED_RUNTIME_METHODS.append('cwrap')
        settings.EXPORTED_RUNTIME_METHODS.append('UTF8ToString')
        settings.EXPORTED_RUNTIME_METHODS.append('stringToUTF8')
        settings.EXPORTED_RUNTIME_METHODS.append('stringToNewUTF8')
        settings.EXPORTED_RUNTIME_METHODS.append('FS')
        settings.EXPORTED_RUNTIME_METHODS.append('specialHTMLTargets')
    except AssertionError:
        pass


def show():
    return 'em-x11 (-sUSE_EM_X11 or --use-port=em_x11; %s license)' % LICENSE
