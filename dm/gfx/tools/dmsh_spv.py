#!/usr/bin/env python3
# Linux (nxasahi): the game's shaders as SPIR-V for the Vulkan backend, from the same translation as the Metal chain, stopped before SPIRV-Cross.
# Usage: DM_GAME=<id> dmsh_spv.py <Switch package dir> <output dir> [key...]   (as dmsh_build.py; output <output dir>/spv-<chain version>/<key>.spv, .res.json, .job.json)
# It runs the unmodified dmsh_build.py, so the jobs (stage, facts, constant buffer 1 contents, pairs) are exactly the Metal chain's, and changes nothing the chain version
# covers (dmsh_chain.FILES: editing dmsh_build.py or dmsh_chain.py would make every Mac and iOS library stale). Three hooks, all here:
#   - the tool check covers the translator only (its pinned dlls and embedded source, dmsh_chain.lock): spirv-cross and the Metal compilers are not part of this chain
#   - the translator runs through dotnet (its dmsh_xlate apphost is a macOS executable; the dlls are the pinned ones)
#   - each translation's outputs are collected as it returns, and the build stops at its first SPIRV-Cross call, where the Metal-only part begins
import json, os, runpy, shutil, subprocess, sys, threading
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import dmsh_chain   # before XLATE is set: XLATE_DIR stays the pinned build in dmsh_xlate_bin

if len(sys.argv) < 3: sys.exit('usage: DM_GAME=<id> dmsh_spv.py <Switch package dir> <output dir> [key...]')
DLL = os.path.join(dmsh_chain.XLATE_DIR, 'dmsh_xlate.dll')
OUT = os.path.join(sys.argv[2], 'spv-' + dmsh_chain.version()); os.makedirs(OUT, exist_ok=True)
os.makedirs(sys.argv[2], exist_ok=True)
wrapper = os.path.join(sys.argv[2], 'dmsh_xlate_dotnet')
with open(wrapper, 'w') as f: f.write(f'#!/bin/sh\nexec dotnet "{DLL}" "$@"\n')
os.chmod(wrapper, 0o755); os.environ['XLATE'] = wrapper

def check_tools():
    want = dict(l.rstrip('\n').split('\t', 1) for l in open(dmsh_chain.LOCK) if '\t' in l and not l.startswith('#'))
    bad = [f'{d}: lock {want.get(d)} / actual {dmsh_chain.sha16(os.path.join(dmsh_chain.XLATE_DIR, d))}' for d in ('dmsh_xlate.dll', 'Ryujinx.Graphics.Shader.dll', 'Ryujinx.Common.dll')
           if want.get(d) != dmsh_chain.sha16(os.path.join(dmsh_chain.XLATE_DIR, d))]
    src = subprocess.run(['dotnet', DLL, '--source-sha'], capture_output=True, text=True).stdout.strip()
    if src != want.get('xlate_source') or src != dmsh_chain.xlate_source_sha(): bad.append(f'translator source: embedded {src} / lock {want.get("xlate_source")} / repository {dmsh_chain.xlate_source_sha()}')
    return bad
dmsh_chain.check_tools = check_tools

# Storage images without a declared format (OpTypeImage Sampled 2, Format Unknown) need StorageImageRead/WriteWithoutFormat in Vulkan; the translator's SPIR-V
# leaves them out (Metal never asks; Ryujinx's own Vulkan backend adds them from the device's features). Inserted right after the header, where capabilities go
def fix_caps(path):
    import struct
    b = open(path, 'rb').read(); w = list(struct.unpack('<%dI' % (len(b) // 4), b)); i, caps, unk = 5, set(), False
    while i < len(w):
        op, n = w[i] & 0xFFFF, w[i] >> 16
        if n == 0: break
        if op == 17: caps.add(w[i + 1])                                   # OpCapability
        if op == 25 and w[i + 7] == 2 and w[i + 8] == 0: unk = True         # OpTypeImage, Sampled 2 (storage), ImageFormat Unknown
        i += n
    add = [c for c in (55, 56) if unk and c not in caps]                  # StorageImageReadWithoutFormat, StorageImageWriteWithoutFormat
    if add: open(path, 'wb').write(struct.pack('<%dI' % (len(w) + 2 * len(add)), *(w[:5] + [x for c in add for x in ((2 << 16) | 17, c)] + w[5:])))
    return len(add)

class Translated(Exception): pass
_run, _lock, _state = subprocess.run, threading.Lock(), {'jobs': 0, 'spv': 0, 'failed': {}, 'caps': 0}
def run(args, *a, **k):
    if isinstance(args, (list, tuple)) and args and args[0] == wrapper and len(args) == 3 and not args[1].startswith('-'):
        r = _run(args, *a, **k)
        jobs, w = json.load(open(args[1])), args[2]
        with _lock:
            for line in (r.stderr or '').splitlines():
                if line.startswith('[RED] ') and ':' in line: key, msg = line[6:].split(':', 1); _state['failed'][key.strip()] = msg.strip()[:200]
            for job in jobs:
                key = job['key']; _state['jobs'] += 1
                if not os.path.exists(os.path.join(w, key + '.spv')): continue
                for ext in ('.spv', '.res.json', '.reader.spv'):
                    if os.path.exists(os.path.join(w, key + ext)): shutil.copyfile(os.path.join(w, key + ext), os.path.join(OUT, key + ext))
                _state['caps'] += bool(fix_caps(os.path.join(OUT, key + '.spv')))
                json.dump(job, open(os.path.join(OUT, key + '.job.json'), 'w'))
                _state['spv'] += 1
        return r
    if isinstance(args, (list, tuple)) and args and args[0] == 'spirv-cross' and _state['jobs']: raise Translated()   # the Metal part starts here
    if isinstance(args, (list, tuple)) and args and args[0] == 'xcrun':   # the Metal library cache asks for the compiler's version at setup: there is none
        if _state['jobs']: raise Translated()
        return subprocess.CompletedProcess(args, 1, '', 'xcrun: not on Linux')
    return _run(args, *a, **k)
subprocess.run = run

# ---- Vulkan reflection: <key>.vk.json ----
# What each SPIR-V binding is to the replay (a uniform slot, a texture slot, a storage buffer and where its address is, the support buffer …) is decided by the Metal chain's
# reflect() in dmsh_build.py from the resources' names. It is taken from there unchanged: its function definitions and constant tables are lifted out of the parsed source
# (nothing of the script runs), and it is handed a stand-in for SPIRV-Cross's argument buffer struct made from the SPIR-V: one member per resource, a combined image-sampler as
# the texture and its "Smplr" sampler, as SPIRV-Cross declares them. Its answers keep the member ids, which are the SPIR-V bindings
import ast, struct
def _lift_build():
    src = open(os.path.join(HERE, 'dmsh_build.py')).read(); tree = ast.parse(src); keep = []
    for n in tree.body:
        if isinstance(n, ast.FunctionDef): keep.append(n)
        elif isinstance(n, ast.Assign) and all(isinstance(t, (ast.Name, ast.Tuple)) for t in n.targets):
            names = [x.id for t in n.targets for x in ([t] if isinstance(t, ast.Name) else t.elts) if isinstance(x, ast.Name)]
            if names and all(x.isupper() for x in names):
                try: ast.literal_eval(n.value); keep.append(n)
                except ValueError: pass
    import re as _re, dmsh_geometry as _geo, collections as _col
    ns = {'re': _re, 'os': os, 'json': json, 'dmsh_geometry': _geo, 'collections': _col, '__name__': 'dmsh_build_lifted'}
    exec(compile(ast.Module(body=keep, type_ignores=[]), 'dmsh_build.py (lifted)', 'exec'), ns)
    return ns
_B = None
def _spv(path):
    b = open(path, 'rb').read(); w = struct.unpack('<%dI' % (len(b) // 4), b); i, ins = 5, []
    while i < len(w):
        op, n = w[i] & 0xFFFF, w[i] >> 16
        if not n: break
        ins.append((op, w[i + 1:i + n])); i += n
    return ins
def _cstr(ws):
    return b''.join(struct.pack('<I', x) for x in ws).split(b'\0', 1)[0].decode('utf-8', 'replace')
def vk_reflect(key):
    global _B
    if _B is None: _B = _lift_build()
    spv, res = os.path.join(OUT, key + '.spv'), json.load(open(os.path.join(OUT, key + '.res.json')))
    ins = _spv(spv)
    name = {a[0]: _cstr(a[1:]) for op, a in ins if op == 5}
    bind = {a[0]: a[2] for op, a in ins if op == 71 and a[1] == 33}
    dset = {a[0]: a[2] for op, a in ins if op == 71 and a[1] == 34}
    types = {a[0]: (op, a) for op, a in ins if op in (25, 26, 27, 28, 29, 30, 32)}
    const = {a[1]: a[2] for op, a in ins if op == 43 and len(a) > 2}
    members, kinds = [], {}
    for op, a in ins:
        if op != 59 or a[1] not in bind: continue
        var, sc = a[1], a[2]
        t = types.get(a[0], (0, ()))[1]; t = t[2] if len(t) > 2 else None   # pointee
        arr = None
        while t in types and types[t][0] in (28, 29):
            ta = types[t][1]; arr = const.get(ta[2], 0) if types[t][0] == 28 else 0; t = ta[1]
        top = types.get(t, (0, ()))[0]
        nm = name.get(var, '_%d' % var)
        if top in (25, 27):
            img = types[t][1] if top == 25 else types[types[t][1][1]][1]   # OpTypeImage operands: id, sampled type, dim, depth, arrayed, ms, sampled, format
            dim, sampled = img[2], img[6]
            ty = 'texture_buffer<float>' if dim == 5 else ('texture2d<float, access::read_write>' if sampled == 2 else 'texture2d<float>')
            if arr is not None: ty = 'array<%s, %d>' % (ty, arr or 1)
            members.append((ty, nm, bind[var])); kinds[bind[var]] = 'image' if sampled == 2 else 'texel_buffer' if dim == 5 else 'texture'
            if top == 27: members.append(('array<sampler, %d>' % (arr or 1) if arr is not None else 'sampler', nm + 'Smplr', bind[var]))
        elif top == 26: members.append(('sampler', nm, bind[var])); kinds[bind[var]] = 'sampler'
        else: members.append(('device void*', nm, bind[var])); kinds[bind[var]] = 'storage' if sc == 12 or (sc == 2 and any(op2 == 71 and a2[0] == t and a2[1] == 3 for op2, a2 in ins)) else 'uniform'
    msl = 'struct spvDescriptorSetBuffer0\n{\n' + ''.join('    %s %s [[id(%d)]];\n' % m for m in members) + '};\n'
    binds = _B['reflect'](key, res['stage'], msl, res)
    for e in binds:
        e['binding'] = e.pop('arg_id'); e['set'] = 0; e['vk'] = kinds.get(e['binding'], '?')
        e['name'] = e.pop('msl_name'); e.pop('msl_type', None)
    out = {'key': key, 'stage': res['stage'], 'bindings': binds, 'frag_out_map': res.get('frag_out_map'), 'uses_frag_coord': res.get('uses_frag_coord'),
           'inputs': sorted(a[2] for op, a in ins if op == 71 and a[1] == 30 and any(op2 == 59 and a2[1] == a[0] and a2[2] == 1 for op2, a2 in ins)),
           'outputs': sorted(a[2] for op, a in ins if op == 71 and a[1] == 30 and any(op2 == 59 and a2[1] == a[0] and a2[2] == 3 for op2, a2 in ins))}
    json.dump(out, open(os.path.join(OUT, key + '.vk.json'), 'w'))
    return binds

# ---- canonical bindings: <key>.vk.spv and <key>.vkb ----
# Shader objects of different stages are bound together, so their descriptor set layouts must agree. Every stage gets a set of its own (vertex 0, pixel 1, geometry 2,
# hull 3, domain 4; compute 0 in its own layout), and each resource a fixed binding by what it is (the table above vk_reflect's answers): the host then has one set layout
# per stage for every shader. <key>.vk.spv is the SPIR-V with its DescriptorSet / Binding decorations rewritten so; <key>.vkb lists the bindings for the host, one per line:
#   binding <TAB> descriptor (u uniform, s storage, t combined texture, i storage image, b texel buffer, p sampler) <TAB> kind <TAB> reg <TAB> off <TAB> sampler kind <TAB> sampler reg/id <TAB> sampler off
#   <TAB> dim (1 1D 2 2D 3 3D 4 cube 5 buffer) <TAB> arrayed <TAB> multisampled <TAB> depth compare <TAB> array count
STAGE_SET = {'vs': 0, 'ps': 1, 'gs': 2, 'hs': 3, 'ds': 4, 'cs': 0}
CANON = {'support': 0, 'c1': 15}
def canon_binding(e, seq):
    k, r = e['kind'], e.get('reg')
    if k in CANON: return CANON[k]
    if k == 'cbuf' and r is not None and 0 <= r < 14: return 1 + r
    if k == 'drvcb': return {0: 16, 2: 17}.get(e.get('bank'), 18)
    if k == 'sbuf' and r is not None and 0 <= r < 16: return 20 + r
    if k == 'ubo_ptr' and r is not None and 0 <= r < 14: return 36 + r
    if k in ('sbuf', 'sbuf_ubo'): return seq('s', 50, 14)
    if k == 'tex' and r is not None and 0 <= r < 256: return 64 + r
    if k == 'tex_ubo': return seq('u', 320, 32)
    if k == 'tex_c2a': return seq('a', 352, 16)
    if k == 'tbuf' and r is not None and 0 <= r < 32: return 400 + r
    if k == 'img' and r is not None and 0 <= r < 16: return 460 + r
    if k == 'geometry' and r is not None: return 480 + r
    return seq('x', 500, 64)
def vk_canon(key):
    vk = json.load(open(os.path.join(OUT, key + '.vk.json')))
    ins = _spv(os.path.join(OUT, key + '.spv'))
    bind = {a[0]: a[2] for op, a in ins if op == 71 and a[1] == 33}
    by_binding = {}
    for e in vk['bindings']: by_binding.setdefault(e['binding'], []).append(e)
    used, nxt = {}, {}
    def seq(tag, base, n):
        i = nxt.get(tag, 0); nxt[tag] = i + 1
        if i >= n: raise ValueError(f'more than {n} bindings of kind {tag}')
        return base + i
    # image types of the resources: dim, arrayed, ms, depth (OpTypeImage), and array counts
    types = {a[0]: (op, a) for op, a in ins if op in (25, 26, 27, 28, 29, 32)}
    const = {a[1]: a[2] for op, a in ins if op == 43 and len(a) > 2}
    info = {}
    for op, a in ins:
        if op != 59 or a[1] not in bind: continue
        t = types.get(a[0], (0, ()))[1]; t = t[2] if len(t) > 2 else None; cnt = 1
        while t in types and types[t][0] in (28, 29):
            ta = types[t][1]; cnt = const.get(ta[2], 1) if types[t][0] == 28 else 0; t = ta[1]
        img = None
        if t in types and types[t][0] == 27: t = types[t][1][1]
        if t in types and types[t][0] == 25: img = types[t][1]
        dim = {0: 1, 1: 2, 2: 3, 3: 4, 5: 5}.get(img[2], 2) if img else 0
        info[bind[a[1]]] = (dim, img[4] if img else 0, img[5] if img else 0, img[3] if img else 0, cnt)
    remap, rows = {}, []
    for b, es in sorted(by_binding.items()):
        main = next((e for e in es if not e['name'].endswith('Smplr')), es[0])
        smp = next((e for e in es if e['name'].endswith('Smplr')), None)
        cb = canon_binding(main, seq)
        if cb in used: raise ValueError(f'bindings {used[cb]} and {b} both map to canonical binding {cb} ({main["kind"]} reg {main.get("reg")})')
        used[cb] = b; remap[b] = cb
        desc = {'uniform': 'u', 'storage': 's', 'texture': 't', 'image': 'i', 'texel_buffer': 'b', 'sampler': 'p'}.get(main['vk'], '?')
        dim, arr, ms, dep, cnt = info.get(b, (0, 0, 0, 0, 1))
        sreg = smp.get('reg') if smp else ''
        rows.append('\t'.join(str(x) for x in (cb, desc, main['kind'], '' if main.get('reg') is None else main['reg'], main.get('off', '') if main.get('off') is not None else '',
                                                 smp['kind'] if smp else '', '' if sreg is None else (sreg if not isinstance(sreg, (list, tuple)) else sreg[1]),
                                                 smp.get('off', '') if smp and smp.get('off') is not None else '', dim, arr, ms, dep, cnt)))
    st = STAGE_SET.get(vk['stage'], 0)
    b = open(os.path.join(OUT, key + '.spv'), 'rb').read(); w = list(struct.unpack('<%dI' % (len(b) // 4), b)); i = 5
    while i < len(w):
        op, n = w[i] & 0xFFFF, w[i] >> 16
        if not n: break
        if op == 71 and w[i + 1] in bind:
            if w[i + 2] == 34: w[i + 3] = st
            elif w[i + 2] == 33: w[i + 3] = remap.get(w[i + 3], w[i + 3])
        i += n
    open(os.path.join(OUT, key + '.vk.spv'), 'wb').write(struct.pack('<%dI' % len(w), *w))
    open(os.path.join(OUT, key + '.vkb'), 'w').write(f'# {key} {vk["stage"]} set {st}\n' + ''.join(r + '\n' for r in rows))

sys.argv = [os.path.join(HERE, 'dmsh_build.py')] + sys.argv[1:]
try: runpy.run_path(sys.argv[0], run_name='__main__')
except Translated: pass
import collections as _c
_kinds, _noreg = _c.Counter(), 0
for f in sorted(os.listdir(OUT)):
    if not f.endswith('.spv') or '.reader.' in f: continue
    try:
        bs = vk_reflect(f[:-4])
        vk_canon(f[:-4])
        for b in bs: _kinds[b['kind']] += 1; _noreg += b['kind'] in ('cbuf', 'tex', 'img', 'sbuf', 'samp', 'samp_tex', 'tbuf') and b.get('reg') is None
    except Exception as e: print(f'  [RED] {f}: reflection failed: {e}'); _state['failed'][f[:-4]] = 'reflection: ' + str(e)
print('Vulkan reflection (.vk.json): bindings by kind ' + ', '.join(f'{k} {v}' for k, v in _kinds.most_common()) + f'; without a slot {_noreg}')
print(f'SPIR-V: {_state["spv"]} of {_state["jobs"]} translated shaders written to {OUT}; translation failed {len(_state["failed"])}; storage image capabilities added to {_state["caps"]}')
for key, msg in list(_state['failed'].items())[:10]: print(f'  [RED] {key}: {msg}')
sys.exit(0 if _state['jobs'] else 1)
