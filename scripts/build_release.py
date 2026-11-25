"""Build Coco's fixed evaluator with original network bytes embedded by incbin."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
CPP = ['main', 'board', 'movegen', 'tt', 'search', 'search_policy', 'evaluate',
       'uci', 'nnue', 'datagen', 'ravager_eval', 'embedded_network']
C = ['src/ravager_bridge.c', 'third_party/ravager/nnue/inference.c',
     'third_party/ravager/src/bitboard.c', 'Fathom/src/tbprobe.c']
ARCH = {
    'native': ['-march=native'],
    'x86-64-popcnt': ['-mpopcnt', '-msse4.1'],
    'x86-64-avx2': ['-mpopcnt', '-msse4.1', '-mavx2', '-mfma'],
    'x86-64-bmi2': ['-mpopcnt', '-msse4.1', '-mavx2', '-mfma', '-mbmi2'],
    'x86-64-avx512': ['-mpopcnt', '-msse4.1', '-mavx2', '-mfma', '-mbmi2', '-mavx512f', '-mavx512bw', '-mavx512dq', '-mavx512vl'],
    'apple-silicon': ['-march=armv8.2-a+crypto+dotprod'],
    'armv8': ['-march=armv8-a'],
    'armv8-dotprod': ['-march=armv8.2-a+dotprod'],
}


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch', choices=ARCH, default='native')
    parser.add_argument('--compiler', choices=['gcc', 'clang', 'mingw'], default='gcc')
    parser.add_argument('--cxx')
    parser.add_argument('--cc')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--scalar', action='store_true')
    parser.add_argument('--verify-frames', action='store_true')
    args = parser.parse_args()
    prefix = 'x86_64-w64-mingw32-' if args.compiler == 'mingw' else ''
    default_cxx = prefix + ('clang++' if args.compiler == 'clang' else 'g++' + ('-posix' if prefix else ''))
    default_cc = prefix + ('clang' if args.compiler == 'clang' else 'gcc' + ('-posix' if prefix else ''))
    cxx = args.cxx or os.environ.get('CXX') or default_cxx
    cc = args.cc or os.environ.get('CC') or default_cc
    windows = os.name == 'nt' or args.compiler == 'mingw'
    output = (args.output or Path('coco-chess.exe' if windows else 'coco-chess')).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    config = json.loads((ROOT / 'release.json').read_text())
    net = ROOT / config['nnue_file']
    if sha(net).upper() != config['nnue_sha256'] or net.stat().st_size != config['nnue_bytes']:
        raise RuntimeError('Selected release network identity mismatch')
    subprocess.run([sys.executable, str(ROOT / 'scripts/make_build_info.py'),
                    '--arch', args.arch, '--compiler', args.compiler], cwd=ROOT, check=True)
    flags = ['-O3', '-DNDEBUG', '-DL1_SIZE=512', '-DCOCO_FIXED_RAVAGER', '-pthread',
             '-Isrc', '-IFathom/src', '-Ithird_party/ravager/src', '-Ithird_party/ravager/nnue', *ARCH[args.arch]]
    if windows: flags += ['-mstackrealign']
    if args.scalar: flags += ['-DCOCO_FORCE_SCALAR']
    if args.verify_frames: flags += ['-DRAVAGER_VERIFY_FRAMES']
    flags += shlex.split(os.environ.get('COCO_EXTRA_FLAGS', ''))
    key = hashlib.sha256(json.dumps([args.arch, cxx, cc, flags]).encode()).hexdigest()[:16]
    build = ROOT / 'build_cache/release-2.0' / key
    build.mkdir(parents=True, exist_ok=True)
    units = [(f'src/{name}.cpp', cxx, '-std=c++20') for name in CPP]
    units += [(name, cc, '-std=c11') for name in C]
    headers = list((ROOT / 'src').glob('*.h'))
    headers += list((ROOT / 'third_party/ravager').rglob('*.h')) + list((ROOT / 'Fathom/src').glob('*.h'))
    dependencies = {str(p.relative_to(ROOT)): sha(p) for p in headers if p.name != 'nnue_data.h'}
    def compile_unit(unit):
        name, compiler, standard = unit
        obj = build / (name.replace('/', '_') + '.o')
        command = [compiler, standard, *flags]
        if name in C[:3]: command += ['-Drook_attacks=rv_rook_attacks', '-Dbishop_attacks=rv_bishop_attacks']
        signature = hashlib.sha256(json.dumps([command, sha(ROOT / name), dependencies, sha(net) if 'embedded_network' in name else '']).encode()).hexdigest()
        stamp = obj.with_suffix('.signature')
        if not obj.exists() or not stamp.exists() or stamp.read_text() != signature:
            subprocess.run([*command, '-c', str(ROOT / name), '-o', str(obj)], cwd=ROOT, check=True)
            stamp.write_text(signature)
        return obj
    with ThreadPoolExecutor(max_workers=max(1, min(args.jobs, 8))) as pool:
        objects = list(pool.map(compile_unit, units))
    subprocess.run([cxx, '-pthread', *(['-static'] if windows else []), *map(str, objects),
                    '-o', str(output)], cwd=ROOT, check=True)
    record = {'architecture': args.arch, 'compiler': args.compiler, 'flags': flags,
              'network_sha256': sha(net), 'network_bytes': net.stat().st_size,
              'artifact_sha256': sha(output), 'source_files': {name: sha(ROOT / name) for name, _, _ in units},
              'headers': dependencies, 'scalar': args.scalar, 'verify_frames': args.verify_frames}
    output.with_suffix(output.suffix + '.build.json').write_text(json.dumps(record, indent=2) + '\n')
    print(f'Built {output.name}; embedded {net.stat().st_size} original bytes; sha256 {record["artifact_sha256"]}')


if __name__ == '__main__':
    main()
