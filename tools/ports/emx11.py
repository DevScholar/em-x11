"""
em-x11 Emscripten port.

The port is the canonical way to link against em-x11. It returns the full
filesystem paths to the static archives, bypassing emscripten's built-in
-lX11 -> libxlib.js hijack. The em-x11 examples use it; external projects
can too by passing --use-port=<path>/emx11.py to emcc.

The port reuses the main cmake build artifacts (build/artifacts/) when they
exist — no separate build step. If they don't exist (e.g. an external
project pointing at a clean em-x11 clone), the port runs the cmake build
on demand. Set EMX11_SRC to tell the port where the em-x11 root is.
"""

import os
import logging
import subprocess

VERSION = '0.0.1'
LICENSE = 'MIT'

logger = logging.getLogger('emx11')

# All static archives the port provides, in link order (higher-level first).
# Order matches what a standard Xaw program's link line looks like:
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


def find_emx11_root():
    """Walk up from this script to find the em-x11 project root."""
    env = os.environ.get('EMX11_SRC', '')
    if env:
        root = os.path.abspath(env)
        if os.path.isdir(root):
            return root
        logger.warning('EMX11_SRC is set but not a directory: %s', root)

    # Script is at <root>/tools/ports/emx11.py
    script_dir = os.path.dirname(os.path.abspath(__file__))
    root = os.path.normpath(os.path.join(script_dir, '..', '..'))
    if os.path.isdir(os.path.join(root, 'native')):
        return root

    # If the port was copied into emscripten's own tools/ports/, the
    # relative path won't work. Fall back to the current directory.
    cwd = os.getcwd()
    if os.path.isdir(os.path.join(cwd, 'native')):
        return cwd

    return None


def _find_artifacts(emx11_root):
    """Return the path to the artifacts directory, or None.

    Prefers the main cmake build output (build/artifacts/) which is always
    present when building in-tree (pnpm build:native). Falls back to a
    dedicated port-build directory.
    """
    # Main build — the fast path. Already built by pnpm build:native.
    main = os.path.join(emx11_root, 'build', 'artifacts')
    if os.path.isdir(main) and os.path.exists(os.path.join(main, 'libX11.a')):
        return main

    # Port-specific build — only used by external projects that point at
    # a clean em-x11 clone.
    port_build = os.path.join(emx11_root, 'build', 'port-build', 'artifacts')
    if os.path.isdir(port_build) and os.path.exists(os.path.join(port_build, 'libX11.a')):
        return port_build

    return None


def _ensure_built(emx11_root, artifacts_dir):
    """Build em-x11 via cmake if artifacts don't exist yet."""
    if artifacts_dir and os.path.exists(os.path.join(artifacts_dir, 'libX11.a')):
        return artifacts_dir

    # Check third-party sources
    third_party = os.path.join(emx11_root, 'ignored-area', 'third-party')
    if not os.path.isdir(os.path.join(third_party, 'libXt')):
        script = os.path.join(emx11_root, 'scripts', 'fetch-third-party.sh')
        if os.path.isfile(script):
            logger.info('Third-party sources missing; running fetch-third-party.sh ...')
            subprocess.run(['bash', script], cwd=emx11_root, check=True)

    build_dir = os.path.join(emx11_root, 'build', 'port-build')
    out = os.path.join(build_dir, 'artifacts')
    marker = os.path.join(out, '.emx11-port-built')

    # Check freshness
    need_build = not os.path.exists(marker)
    if not need_build:
        marker_mtime = os.path.getmtime(marker)
        native_dir = os.path.join(emx11_root, 'native')
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
            '-S', emx11_root,
            '-B', build_dir,
            '-DCMAKE_BUILD_TYPE=Release',
            '-DEMX11_BUILD_SIDE_MODULE=OFF',
        ]
        logger.info('Configuring em-x11: %s', ' '.join(cmake_cmd))
        subprocess.run(cmake_cmd, cwd=emx11_root, check=True)

        build_cmd = ['cmake', '--build', build_dir, '-j']
        logger.info('Building em-x11: %s', ' '.join(build_cmd))
        subprocess.run(build_cmd, cwd=emx11_root, check=True)

        open(marker, 'w').close()

    return out


def get(ports, settings, shared):
    emx11_root = find_emx11_root()
    if not emx11_root:
        logger.error(
            'Cannot find em-x11 source root. '
            'Set EMX11_SRC=/path/to/em-x11 or place this script under '
            '<emx11>/tools/ports/emx11.py'
        )
        return []

    artifacts_dir = _find_artifacts(emx11_root)
    if artifacts_dir:
        logger.debug('Using pre-built artifacts: %s', artifacts_dir)
    else:
        artifacts_dir = _ensure_built(emx11_root, artifacts_dir)

    # Return the full archive paths so emcc passes them through to wasm-ld
    # without triggering the map_to_js_libs hijack on -lX11 / -lGL.
    lib_paths = []
    for lib in _PORT_LIBS:
        p = os.path.join(artifacts_dir, lib)
        if os.path.exists(p):
            lib_paths.append(p)
        else:
            logger.debug('Skipping %s (not built)', lib)

    return lib_paths


def clear(ports, settings, shared):
    emx11_root = find_emx11_root()
    if not emx11_root:
        return
    import shutil
    port_build = os.path.join(emx11_root, 'build', 'port-build')
    if os.path.isdir(port_build):
        logger.info('Clearing em-x11 port build: %s', port_build)
        shutil.rmtree(port_build)


def process_args(ports):
    """Return compiler flags so that #include <X11/Xlib.h> resolves."""
    emx11_root = find_emx11_root()
    if not emx11_root:
        return []
    include_dir = os.path.join(emx11_root, 'native', 'include')
    return ['-I' + include_dir]


def process_dependencies(settings):
    pass


def show():
    return 'em-x11 (--use-port=emx11; %s license)' % LICENSE
