"""Compare real PVF resources using production C and isolated reference reader classes.

Does not connect to a database or start the reference applications. Install zhconv
for the references' real traditional-to-simplified conversion; no replacement decoder.
"""
import argparse
import ast
from collections import Counter
import gc
import hashlib
import json
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
import sys

from db_integration import OUT, ROOT

PROBE = OUT / 'resource_probe.exe'
sys.path.insert(0, str(OUT / 'pydeps'))


def build():
    OUT.mkdir(parents=True, exist_ok=True)
    zig = os.environ.get('ZIG') or shutil.which('zig') or r'C:\zig\zig.exe'
    cmd = [zig, 'cc', '-target', 'x86_64-windows-gnu', '-std=c11', '-O2',
           '-DUNICODE', '-D_UNICODE', '-Wall', '-Wextra', '-Wshadow', '-Wconversion',
           '-Wpedantic', '-Werror', '-Isrc', 'tools/resource_probe.c']
    cmd += ['src/' + s + '.c' for s in ('pvf', 'npk', 'zlib', 'util')]
    cmd += ['-luser32', '-lgdi32', '-ladvapi32', '-o', str(PROBE)]
    subprocess.run(cmd, cwd=ROOT, check=True)


def read_manifest(path):
    data = {'leafs': {}, 'items': {}, 'exp': [], 'samples': [], 'hashes': {}}
    with path.open(encoding='utf-8') as f:
        for line in f:
            row = line.rstrip('\n').split('\t')
            if row[0] == 'META':
                data['meta'] = list(map(int, row[1:]))
            elif row[0] == 'NPK':
                data['npk'] = list(map(int, row[1:]))
            elif row[0] == 'EXP':
                data['exp'].append(int(row[2]))
            elif row[0] == 'LEAF':
                data['leafs'][bytes.fromhex(row[4])] = tuple(map(int, row[1:4]))
            elif row[0] == 'HASH':
                data['hashes'][bytes.fromhex(row[2])] = row[1]
            elif row[0] == 'ITEM':
                data['items'][int(row[1])] = (int(row[2]), int(row[3]),
                    bytes.fromhex(row[4]).decode('utf-8'), bytes.fromhex(row[5]).decode('utf-8'))
            elif row[0] == 'SAMPLE':
                data['samples'].append(row[1:])
    return data


def reference_reader(path, pvf, encoding):
    from zhconv import convert
    source = ast.parse(path.read_text(encoding='utf-8-sig'))
    keep = {'decrypt_Bytes', 'PVFHeader', 'StringTable', 'Str', 'Lst_lite2',
            'TinyPVF', 'get_exp_table2', 'detect_pvf_encoding'}
    nodes = [n for n in source.body if isinstance(n, (ast.ClassDef, ast.FunctionDef)) and n.name in keep]
    namespace = {'struct': struct, 'unpack': struct.unpack, 'convert': convert,
                 'print': lambda *a, **kw: None}
    exec(compile(ast.Module(nodes, type_ignores=[]), str(path), 'exec'), namespace)
    header = namespace['PVFHeader'](str(pvf))
    reader = namespace['TinyPVF'](header, encode=encoding)
    reader.load_Leafs()
    return reader, namespace


def check(reference, pvf, c):
    from zhconv import convert
    reader, namespace = reference_reader(reference, pvf, 'gbk' if c['meta'][2] == 936 else 'big5')
    try:
        leafs = {}
        raw_paths = {}
        for leaf in reader.fileTreeDict.values():
            path = leaf['filePath_bytes'].lower().lstrip(b'/')
            raw_paths[path] = leaf['filePath']
            leafs[path] = (struct.unpack('<I', leaf['fileLength_bytes'])[0],
                           leaf['fileCrc32'], leaf['relativeOffset'])
        assert leafs == c['leafs'], 'file-tree metadata differ'
        exp = namespace['get_exp_table2'](reader)
        assert [n & 0xffffffff for n in exp] == c['exp'], 'experience table bits differ'
        for path, md5 in c['hashes'].items():
            raw = reader.read_File_In_Decrypted_Bin(raw_paths[path])
            assert hashlib.md5(raw).hexdigest() == md5, ('decrypted bytes differ', path)
        rng = random.Random(20261112)
        chosen = {int(s[0]) for s in c['samples']}
        chosen.update(rng.sample(sorted(c['items']), min(240, len(c['items']))))
        original_name_differences = []
        for item_id in sorted(chosen):
            kind, limit, name, path = c['items'][item_id]
            values = reader.read_File_In_List2(path)[1]
            assert '[name]' in values, (item_id, 'reference cannot find name')
            expected = values[values.index('[name]') + 1]
            assert convert(name, 'zh-cn') == expected, (item_id, name, expected)
            if name != expected:
                original_name_differences.append(item_id)
            expected_limit = values[values.index('[stack limit]') + 1] if '[stack limit]' in values else 0
            assert limit == expected_limit, (item_id, 'stack limit', limit, expected_limit)
            if '[equipment type]' in values:
                tag = values[values.index('[equipment type]') + 1]
                expected_kind = 6 if tag == '[creature]' else 7 if tag in (
                    '[artifact red]', '[artifact blue]', '[artifact green]') else 5 if 'avatar' in tag else 1
                assert kind == expected_kind, (item_id, 'equipment type', tag, kind)
        return {'reference': str(reference), 'leaves': len(leafs), 'decrypted_files': len(c['hashes']),
                'experience_entries': len(exp), 'reference_negative_experience_indices': [i for i, n in enumerate(exp) if n < 0],
                'sampled_items': len(chosen), 'names_requiring_simplified_conversion': original_name_differences}
    finally:
        reader.pvfHeader.fp.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pvf', type=Path, required=True)
    parser.add_argument('--images', type=Path)
    parser.add_argument('--build', action='store_true')
    parser.add_argument('--reference', action='append', type=Path)
    args = parser.parse_args()
    if args.build:
        build()
    references = args.reference or [ROOT.parent / 'DNF_pvf_python/dnfpkgtool/pvfReader.py',
                                    *ROOT.parent.glob('*/2026.3.14*/dnfpkgtool/pvfReader.py')]
    manifests = []
    for run in ('first', 'cached'):
        path = OUT / ('resources-' + run + '.tsv')
        cmd = [str(PROBE), 'inspect', str(args.pvf), str(path)]
        if args.images:
            cmd.append(str(args.images))
        subprocess.run(cmd, check=True)
        manifests.append(read_manifest(path))
    first, cached = manifests
    for field in ('leafs', 'items', 'exp', 'samples', 'hashes'):
        assert first[field] == cached[field], ('cache changes results', field)
    if args.images:
        assert first['npk'][0] == 1, 'NPK index failed'
        assert all(s[2] == '1' and s[5] == '1' and int(s[6]) > 0 and int(s[7]) > 0
                   and s[8] == '1' for s in first['samples']), 'an icon/detail sample failed'
    result = {'pvf': str(args.pvf), 'leafs': len(first['leafs']), 'items': len(first['items']),
              'kind_counts': dict(Counter(i[0] for i in first['items'].values())),
              'load_ms': [first['meta'][3], cached['meta'][3]],
              'npk': first.get('npk'), 'icon_samples': len(first['samples']), 'references': []}
    del cached, manifests
    for reference in references:
        result['references'].append(check(reference, args.pvf, first))
        print('PASS', reference, result['references'][-1], flush=True)
        gc.collect()
    (OUT / 'resources-check.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
