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

sys.argv = [os.path.join(HERE, 'dmsh_build.py')] + sys.argv[1:]
try: runpy.run_path(sys.argv[0], run_name='__main__')
except Translated: pass
print(f'SPIR-V: {_state["spv"]} of {_state["jobs"]} translated shaders written to {OUT}; translation failed {len(_state["failed"])}; storage image capabilities added to {_state["caps"]}')
for key, msg in list(_state['failed'].items())[:10]: print(f'  [RED] {key}: {msg}')
sys.exit(0 if _state['jobs'] else 1)
