# Faster SARC shader scanning

Status: **implemented; full-game output comparison pending**. The [shader reference](reference/shaders.md#sarc-filename-scans-copy-the-archive-tail) records the observed bottleneck, its cause and the general reader fix. No game build was restarted or launched to apply this patch.

## Validation

`dm/tools/tests/test_sarc_members.py` covers named and unnamed entries, order and payload preservation, shared name offsets, empty archives/names, UTF-8 replacement, nested shader scanning and malformed filename bounds/terminators. Its deterministic performance regression check forbids suffix copies and filename searches beyond the name table; it does not depend on wall-clock thresholds.

The new checks reproduced the old reader's suffix-copy behavior before the fix and pass afterward. The focused parser, incremental-build and translation-chain checks passed (59 tests); the full tool suite ran 270 tests with 48 optional/platform-dependent skips and no failures, and 10 additional container-reader tests passed. Existing incremental-build tests confirm that editing `shader_split.py` invalidates the split's cached inputs. The translation-chain header remains current; no translation rule or shader key convention changed.

### Synthetic microbenchmark

Measured on the user's M1 Max under Fedora ARM64 / Python 3.14. The input has 129 named members: 128 one-byte payloads and one 4 MiB zero-filled payload. Input creation is excluded from the measurement; `tracemalloc` is enabled for both readers.

| Reader | Elapsed | Peak traced Python allocations |
|---|---:|---:|
| Previous suffix-splitting reader | 4.750839 s | 37.20 MiB |
| Bounded filename search | 0.002913 s | 4.02 MiB |

Both produced identical names, entry order and payload bytes. This intentionally stresses the old implementation's archive-wide NUL splitting; it is **not** a measured whole-game build speedup or process-RSS measurement.

Reproduce from the repository root:

```sh
.venv/bin/python -m unittest -v dm/tools/tests/test_sarc_members.py
.venv/bin/python - <<'PY'
import gc, sys, time, tracemalloc
sys.path.insert(0, 'dm/tools/tests')
from test_sarc_members import sarc, legacy_members, ss
entries = [(f'file{i}.bin', b'x') for i in range(128)] + [('large.bin', bytes(4 << 20))]
data = sarc(entries)
for name, reader in [('before', legacy_members), ('after', ss.sarc_members)]:
    gc.collect(); tracemalloc.start(); start = time.perf_counter()
    result = list(reader(data))
    elapsed = time.perf_counter() - start
    _, peak = tracemalloc.get_traced_memory(); tracemalloc.stop()
    assert result == entries
    print(f'{name}: {elapsed:.6f} s, peak Python allocation {peak / 1048576:.2f} MiB')
PY
```

## Remaining validation and separate follow-up

- On a later user-approved rebuild, compare shader keys, package bytes and generated tables with the prior output. Keep all game-derived data outside the repository. Synthetic valid-input comparisons pass, but full-game package/table equality has not been rechecked.
- Periodic shader-scan progress remains a separate, unimplemented improvement.
- Running Python workers do not pick up an on-disk edit. Do not cancel a working build just to apply the patch; it takes effect on a subsequent import/build.
