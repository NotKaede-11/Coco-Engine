#!/usr/bin/env python3
"""Packaging must reject incomplete or inconsistent release sets before upload."""
import argparse
import contextlib
import copy
import io
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import check_release
import release_manifest


class ReleasePipelineTest(unittest.TestCase):
    def setUp(self):
        (ROOT / 'scratch').mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=ROOT / 'scratch')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.config = json.loads((ROOT / 'release.json').read_text())
        for name in ('release.json', 'coco.nnue', 'src/uci.cpp', self.config['notes']):
            target = self.root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, target)

    def save_config(self):
        (self.root / 'release.json').write_text(json.dumps(self.config))

    def test_readiness_and_tag_gate(self):
        check_release.validate(self.root, tag='v' + self.config['version'])
        self.config['release_ready'] = True
        self.config['readiness_checks']['gauntlet_completed_and_reviewed'] = False
        self.save_config()
        with self.assertRaisesRegex(ValueError, 'not ready'):
            check_release.validate(self.root, ready=True)
        self.config['readiness_checks'] = dict.fromkeys(self.config['readiness_checks'], True)
        self.save_config()
        check_release.validate(self.root, ready=True)
        with self.assertRaisesRegex(ValueError, 'does not match'):
            check_release.validate(self.root, ready=True, tag='v0.0.0')

    def test_changed_network_is_rejected(self):
        with (self.root / 'coco.nnue').open('ab') as stream:
            stream.write(b'changed')
        with self.assertRaisesRegex(ValueError, 'network hash/size'):
            check_release.validate(self.root)

    def make_dist(self):
        dist = self.root / 'dist'
        dist.mkdir()
        for name in self.config['artifacts']:
            engine = dist / name
            engine.write_bytes(b'fixture:' + name.encode())
            record = dict(artifact=name, artifact_sha256=release_manifest.sha256(engine),
                          artifact_bytes=engine.stat().st_size, source_commit='test-commit',
                          nnue_sha256=self.config['nnue_sha256'], nnue_bytes=self.config['nnue_bytes'],
                          fixed_signature_nodes=self.config['fixed_signature_nodes'],
                          fixed_signature_verified=False, runtime_identity=None)
            (dist / (name + '.metadata.json')).write_text(json.dumps(record))
        return dist

    def assemble(self, dist):
        with contextlib.redirect_stdout(io.StringIO()):
            return release_manifest.assemble(argparse.Namespace(
                dist=dist, tag='v' + self.config['version'], source_commit='test-commit',
                output=dist / 'release-manifest.json'))

    def test_complete_set_and_corruption(self):
        dist = self.make_dist()
        self.assertEqual(self.assemble(dist), 0)
        engine = dist / self.config['artifacts'][0]
        engine.write_bytes(b'corrupt')
        with self.assertRaisesRegex(RuntimeError, 'hash mismatch'):
            self.assemble(dist)

    def test_missing_duplicate_and_wrong_provenance(self):
        dist = self.make_dist()
        path = dist / (self.config['artifacts'][0] + '.metadata.json')
        original = path.read_text()
        record = json.loads(original)
        path.unlink()
        with self.assertRaisesRegex(RuntimeError, 'incomplete'):
            self.assemble(dist)
        path.write_text(original)
        duplicate = dist / 'duplicate.metadata.json'
        duplicate.write_text(original)
        with self.assertRaisesRegex(RuntimeError, 'duplicate'):
            self.assemble(dist)
        duplicate.unlink()
        for key, value in [('source_commit', 'wrong'), ('nnue_sha256', '0' * 64),
                           ('fixed_signature_nodes', 731322), ('artifact', '../outside')]:
            with self.subTest(key=key):
                bad = copy.deepcopy(record)
                bad[key] = value
                path.write_text(json.dumps(bad))
                with self.assertRaises(RuntimeError):
                    self.assemble(dist)
        path.write_text(original)


if __name__ == '__main__':
    unittest.main(verbosity=2)
