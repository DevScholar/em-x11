#!/usr/bin/env python3
"""Generate library_em-x11.js from bridges.c + preamble + custom files.

Parses EM_JS declarations from bridges.c, extracts function signatures,
and generates minimal stub implementations for any bridge function that
does NOT already have a manual implementation in the custom file.

Usage: python3 tools/generate-js-library.py \\
           native/em_x11/bridges.c \\
           native/src/lib/library_em-x11.preamble.js \\
           native/src/lib/library_em-x11.custom.js \\
           native/src/lib/library_em-x11.js
"""

import re
import sys
import os


def extract_em_js_signatures(content):
    """Parse EM_JS declarations from bridges.c.

    Returns list of dicts with keys:
      ret_type, name, params (list of (type, name) tuples)
    """
    results = []
    # Match EM_JS and EM_ASYNC_JS declarations — DOTALL for multi-line
    pattern = (
        r'EM_(?:ASYNC_)?JS\s*\(\s*'
        r'(\w+(?:\s+\w+)?(?:\s*\*)?)\s*,\s*'  # return type (may have *)
        r'(em_x11_js_\w+)\s*,\s*'              # function name
        r'\(([^)]*)\)\s*,\s*\{'                # parameter list
    )
    for m in re.finditer(pattern, content, re.DOTALL):
        ret_type = m.group(1).strip()
        name = m.group(2).strip()
        params_str = m.group(3).strip()
        params = _parse_params(params_str)
        results.append({
            'ret_type': ret_type,
            'name': name,
            'params': params,
        })
    return results


def _parse_params(params_str):
    """Parse C parameter list like 'unsigned int id, int x, int y'."""
    if not params_str:
        return []
    params = []
    for part in params_str.split(','):
        part = part.strip()
        if not part:
            continue
        # Split into type and name: last word is name, rest is type
        tokens = part.split()
        if len(tokens) < 2:
            continue
        param_name = tokens[-1]
        param_type = ' '.join(tokens[:-1])
        # Remove trailing whitespace/newlines from type
        param_type = re.sub(r'\s+', ' ', param_type).strip()
        params.append((param_type, param_name))
    return params


def sig_char(c_type):
    """Map C type to Emscripten sig character."""
    t = c_type.strip()
    if t == 'void':
        return 'v'
    # int, unsigned int, unsigned, Bool, char* all map to 'i'
    return 'i'


def build_sig(ret_type, params):
    """Build __sig string like 'viiii'."""
    return sig_char(ret_type) + ''.join(sig_char(t) for t, _ in params)


def js_coerce(param_type, param_name):
    """Generate JS expression with appropriate coercion."""
    t = param_type.strip()
    if t in ('unsigned int', 'unsigned'):
        return f'{param_name} >>> 0'
    # int, Bool, or anything else
    return f'{param_name} | 0'


def bridge_to_method(name):
    """Convert bridge name to host method name.

    em_x11_js_clear_area -> onClearArea
    em_x11_js_get_window_attrs -> onGetWindowAttrs
    em_x11_js_glx_create_context -> onGlxCreateContext
    """
    stem = name.replace('em_x11_js_', '', 1)
    parts = stem.split('_')
    camel = ''.join(p[0].upper() + p[1:] for p in parts if p)
    return 'on' + camel


def generate_stub(func):
    """Generate a JS library stub for one bridge function."""
    name = func['name']
    ret = func['ret_type']
    params = func['params']
    sig = build_sig(ret, params)
    method = bridge_to_method(name)

    param_names = [pn for _, pn in params]
    param_list = ', '.join(param_names)

    lines = []
    lines.append(f"  {name}__sig: '{sig}',")

    if ret == 'void':
        lines.append(f'  {name}: function({param_list}) {{')
        lines.append(f'    var h = EmX11Host.get();')
        if param_names:
            coerced = ', '.join(js_coerce(pt, pn) for pt, pn in params)
            lines.append(f'    if (h) h.{method}({coerced});')
        else:
            lines.append(f'    if (h) h.{method}();')
        lines.append('  },')
    else:
        lines.append(f'  {name}: function({param_list}) {{')
        lines.append(f'    var h = EmX11Host.get();')
        lines.append(f'    if (!h) return 0;')
        if param_names:
            coerced = ', '.join(js_coerce(pt, pn) for pt, pn in params)
            lines.append(f'    return h.{method}({coerced}) | 0;')
        else:
            lines.append(f'    return h.{method}() | 0;')
        lines.append('  },')

    return '\n'.join(lines)


def extract_manual_names(custom_content):
    """Extract em_x11_js_* function names already defined in the custom file."""
    all_names = set(re.findall(r'\b(em_x11_js_\w+)\b', custom_content))
    # Exclude __sig and __deps annotations
    return {n for n in all_names if '__sig' not in n and '__deps' not in n}


def main():
    if len(sys.argv) != 5:
        prog = os.path.basename(sys.argv[0])
        print(f'Usage: {prog} <bridges.c> <preamble.js> <custom.js> <output.js>',
              file=sys.stderr)
        sys.exit(1)

    bridges_path = sys.argv[1]
    preamble_path = sys.argv[2]
    custom_path = sys.argv[3]
    output_path = sys.argv[4]

    with open(bridges_path, 'r', encoding='utf-8') as f:
        bridges_content = f.read()
    with open(preamble_path, 'r', encoding='utf-8') as f:
        preamble = f.read()
    with open(custom_path, 'r', encoding='utf-8') as f:
        custom = f.read()

    funcs = extract_em_js_signatures(bridges_content)
    manual_names = extract_manual_names(custom)

    # Generate stubs for functions NOT manually defined
    missing = [f for f in funcs if f['name'] not in manual_names]
    stubs = []
    if missing:
        stubs.append('')
        stubs.append('  /* === Auto-generated stubs === */')
        stubs.append('')
        for func in missing:
            stubs.append(generate_stub(func))
            stubs.append('')

    # Assemble output
    trailer = '\n};\n\nautoAddDeps(LibraryEmX11, \'$EmX11Host\');\naddToLibrary(LibraryEmX11);\n'

    with open(output_path, 'w', encoding='utf-8') as f:
        f.write(preamble)
        f.write('\n')
        f.write('\n'.join(stubs))
        f.write('\n')
        f.write(custom)
        f.write('\n')
        f.write(trailer)

    # Report
    total = len(funcs)
    manual_count = len([f for f in funcs if f['name'] in manual_names])
    generated_count = len(missing)
    extra_custom = [n for n in sorted(manual_names)
                    if not any(f['name'] == n for f in funcs)]
    print(f'Bridges: {total} total, {manual_count} manual, {generated_count} generated')
    if extra_custom:
        print(f'Warning: {len(extra_custom)} custom entries not in bridges.c:')
        for n in extra_custom:
            print(f'  {n}')
    if generated_count > 0:
        print('Generated stubs:')
        for func in missing:
            print(f'  {func["name"]}')


if __name__ == '__main__':
    main()
