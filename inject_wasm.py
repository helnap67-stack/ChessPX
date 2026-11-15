#!/usr/bin/env python3
"""Embed px_engine.wasm (base64) into the chesspx HTML file.

usage: python3 inject_wasm.py chesspx43_engine.html px_engine.wasm chesspx43_final.html

Replaces the content of <script type="application/octet-stream" id="engine-wasm-b64"> ... </script>.
Safe to run again on its own output (the slot is simply overwritten).
"""
import base64, re, sys

def leb(b, i):
    r = s = 0
    while True:
        x = b[i]; i += 1; r |= (x & 0x7f) << s; s += 7
        if not x & 0x80: return r, i

def list_imports(b):
    """Return [(module, name, kind)] from the wasm import section (so you can see what the worker has to stub)."""
    if b[:4] != b'\0asm': raise SystemExit('not a wasm file (bad magic)')
    i, out = 8, []
    while i < len(b):
        sid = b[i]; i += 1; size, i = leb(b, i); end = i + size
        if sid == 2:
            n, j = leb(b, i)
            for _ in range(n):
                l, j = leb(b, j); mod = b[j:j+l].decode(); j += l
                l, j = leb(b, j); nm = b[j:j+l].decode(); j += l
                kind = b[j]; j += 1
                if kind == 0: _, j = leb(b, j)
                elif kind == 1:
                    j += 1; f = b[j]; j += 1; _, j = leb(b, j)
                    if f & 1: _, j = leb(b, j)
                elif kind == 2:
                    f = b[j]; j += 1; _, j = leb(b, j)
                    if f & 1: _, j = leb(b, j)
                elif kind == 3: j += 2
                out.append((mod, nm, ['func', 'table', 'memory', 'global'][kind]))
        i = end
    return out

def main():
    if len(sys.argv) != 4: raise SystemExit(__doc__)
    html_path, wasm_path, out_path = sys.argv[1:]
    wasm = open(wasm_path, 'rb').read()
    imps = list_imports(wasm)
    print('wasm: %d bytes, imports: %s' % (len(wasm), ', '.join('%s.%s' % (m, n) for m, n, _ in imps) or '(none)'))
    html = open(html_path, encoding='utf-8').read()
    pat = re.compile(r'(<script type="application/octet-stream" id="engine-wasm-b64">)(.*?)(</script>)', re.S)
    if not pat.search(html): raise SystemExit('slot <script id="engine-wasm-b64"> not found in ' + html_path)
    b64 = base64.b64encode(wasm).decode()
    html = pat.sub(lambda m: m.group(1) + b64 + m.group(3), html, count=1)
    open(out_path, 'w', encoding='utf-8').write(html)
    print('wrote %s (%.0f KB)' % (out_path, len(html) / 1024))

main()
