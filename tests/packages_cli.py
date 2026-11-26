import functools
import hashlib
import http.server
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest


COMPILER = Path(sys.argv.pop(1)).resolve()


class RegistryHandler(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        self.server.requests.append(self.path)
        super().do_GET()

    def log_message(self, format, *args):
        pass


class Packages(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.registry = self.root / 'registry'
        self.cache = self.root / 'cache'
        self.env = {**os.environ, 'LYRA_CACHE': str(self.cache)}

    def cli(self, *args, good=True, env=None):
        result = subprocess.run([COMPILER, *args], capture_output=True, text=True, env=env or self.env,
                                timeout=45, cwd=self.root)
        if good:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def pkg(self, directory, name, version='1.0.0', deps=None, **fields):
        root = self.root / directory
        (root / 'src').mkdir(parents=True, exist_ok=True)
        (root / 'src/main.ly').write_text('fn main(): Unit { print(42) }\n')
        (root / 'lyra.json5').write_text(json.dumps(dict(name=name, version=version, dependencies=deps or {}, **fields)))
        return root

    def publish(self, name, ver, deps=None):
        root = self.pkg('producer/' + name, name, ver, deps)
        self.cli('pack', root, '--registry', self.registry)
        return root

    def dep(self, name, version, registry=None):
        return dict(name=name, version=version, registry=str(registry or self.registry))

    def locked(self, root, name):
        entries = json.loads((root / 'lyra.lock').read_text())['packages']
        return next(entry for entry in entries if entry['name'] == name)

    def test_new_deterministic_pack_and_immutable_release(self):
        root = self.root / 'library with space'
        self.cli('new', root, '--lib', '--name', 'acme/notes')
        self.assertIn('pub fn', (root / 'src/lib.ly').read_text())
        manifest = json.loads((root / 'lyra.json5').read_text())
        self.assertEqual((manifest['name'], manifest['kind']), ('acme/notes', 'lib'))
        self.cli('new', root, '--name', 'changed', good=False)
        binary = root / 'src/data.bin'
        binary.write_bytes(bytes(range(256)))
        output = self.root / 'release1.lypkg'
        self.cli('pack', root, '-o', output)
        (root / '.env').write_text('private-local-setting')
        (root / 'unused.o').write_bytes(b'not a source')
        self.cli('pack', root, '-o', self.root / 'release2.lypkg')
        self.assertEqual(output.read_bytes(), (self.root / 'release2.lypkg').read_bytes())
        self.assertNotIn(b'private-local-setting', output.read_bytes())
        self.cli('pack', root, '--registry', self.registry)
        index = self.registry / 'index/acme/notes.json'
        digest = json.loads(index.read_text())['versions'][0]['sha256']
        self.assertEqual(hashlib.sha256(output.read_bytes()).hexdigest(), digest)
        before = index.read_bytes()
        binary.write_bytes(b'changed source')
        self.assertIn('LYK0004', self.cli('pack', root, '--registry', self.registry, good=False).stderr)
        self.assertEqual(index.read_bytes(), before)
        binary.unlink()
        binary.symlink_to(root / '.env')
        self.assertIn('LYK0001', self.cli('pack', root, good=False).stderr)

    def test_local_workspace_json5_lock_refresh_and_cycles(self):
        workspace = self.root / 'workspace'
        app = self.pkg('workspace/app', 'acme/app', deps={'cfg': {'name': 'acme/cfg', 'path': '../cfg', 'version': '^1.0.0'}})
        cfg = self.pkg('workspace/cfg', 'acme/cfg')
        (workspace / 'lyra.json5').write_text("{/* local project */ workspace: {members: ['app', 'cfg',],},} // end\n")
        self.assertIn('LYK0003', self.cli('fetch', app, '--locked', good=False).stderr)
        self.cli('lock', app, '--offline')
        self.assertFalse((app / 'lyra.lock').exists())
        entry = self.locked(workspace, 'acme/cfg')
        self.assertEqual(self.locked(workspace, 'acme/app')['dependencies']['cfg'], entry['id'])
        previous = (workspace / 'lyra.lock').read_bytes()
        (cfg / 'src/main.ly').write_text('fn new_content(): Int { 9 }')
        self.cli('fetch', workspace, '--locked', '--offline')
        self.assertEqual((workspace / 'lyra.lock').read_bytes(), previous)
        self.pkg('workspace/cfg', 'acme/cfg', '1.1.0')
        self.assertIn('LYK0003', self.cli('lock', app, '--locked', good=False).stderr)
        self.cli('lock', workspace)
        self.assertEqual(self.locked(workspace, 'acme/cfg')['version'], '1.1.0')
        self.assertIn('LYK0001', self.cli('pack', app, good=False).stderr)
        self.pkg('workspace/cfg', 'acme/cfg', deps={'app': {'name': 'acme/app', 'path': '../app', 'version': '1.0.0'}})
        error = self.cli('lock', workspace, good=False).stderr
        self.assertIn('LYK0002', error)
        self.assertIn('acme/cfg', error)
        self.assertIn('acme/app', error)

    def test_compatible_pin_update_and_explicit_prerelease(self):
        self.publish('acme/text', '0.2.3')
        app = self.pkg('app', 'app', deps={'text': self.dep('acme/text', '^0.2.3')})
        self.cli('fetch', app)
        for ver in ('0.2.5', '0.3.0', '0.2.9-rc.1'):
            self.publish('acme/text', ver)
        self.cli('lock', app)
        self.assertEqual(self.locked(app, 'acme/text')['version'], '0.2.3')
        self.cli('lock', app, '--update')
        self.assertEqual(self.locked(app, 'acme/text')['version'], '0.2.5')
        self.pkg('app', 'app', deps={'text': self.dep('acme/text', '0.2.9-rc.1')})
        self.cli('lock', app)
        self.assertEqual(self.locked(app, 'acme/text')['version'], '0.2.9-rc.1')

    def test_backtracking_and_conflict_show_both_paths(self):
        self.publish('acme/text', '1.0.0')
        self.publish('acme/text', '2.0.0')
        self.publish('acme/reader', '1.0.0', {'text': self.dep('acme/text', '1.0.0', 'self')})
        self.publish('acme/reader', '1.1.0', {'alt': self.dep('acme/text', '2.0.0', 'self')})
        self.publish('acme/writer', '1.0.0', {'another': self.dep('acme/text', '1.0.0', 'self')})
        deps = {'reader': self.dep('acme/reader', '^1.0.0'), 'writer': self.dep('acme/writer', '1.0.0')}
        app = self.pkg('app', 'app', deps=deps)
        self.cli('lock', app)
        self.assertEqual(self.locked(app, 'acme/reader')['version'], '1.0.0')
        self.assertEqual(len(json.loads((app / 'lyra.lock').read_text())['packages']), 4)
        deps['reader']['version'] = '1.1.0'
        self.pkg('app', 'app', deps=deps)
        previous = (app / 'lyra.lock').read_bytes()
        error = self.cli('lock', app, good=False).stderr
        for text in ('LYK0002', 'acme/writer', 'acme/reader', 'acme/text 1.0.0', 'acme/text 2.0.0'):
            self.assertIn(text, error)
        self.assertEqual((app / 'lyra.lock').read_bytes(), previous)

    def test_yank_keep_existing_lock_verify_hash_and_skip_new_resolve(self):
        self.publish('acme/serve', '1.0.0')
        self.publish('acme/serve', '1.1.0')
        deps = {'server': self.dep('acme/serve', '^1.0.0')}
        app = self.pkg('app', 'acme/app', deps=deps)
        self.cli('lock', app)
        chosen = self.locked(app, 'acme/serve')
        self.assertEqual(chosen['version'], '1.1.0')
        index = self.registry / 'index/acme/serve.json'
        data = json.loads(index.read_text())
        data['versions'][1]['yanked'] = True
        index.write_text(json.dumps(data))
        result = self.cli('fetch', app, '--locked')
        self.assertIn('acme/serve@1.1.0', result.stdout + result.stderr)
        new_app = self.pkg('new-solve', 'fresh', deps=deps)
        self.cli('lock', new_app)
        self.assertEqual(self.locked(new_app, 'acme/serve')['version'], '1.0.0')
        (self.registry / 'artifacts' / (chosen['sha256'] + '.lypkg')).write_bytes(b'tampered')
        self.assertIn('LYK0005', self.cli('fetch', app, '--locked', good=False).stderr)
        data['versions'][1]['sha256'] = '0' * 64
        index.write_text(json.dumps(data))
        self.assertIn('LYK0005', self.cli('fetch', app, '--locked', good=False).stderr)

    def test_http_fetch_locked_offline_and_no_hidden_scripts(self):
        marker = self.root / 'should-not-exist'
        library = self.pkg('library', 'acme/read', scripts={'prepare': {'command': f'touch {marker}', 'outputs': ['out/ready']}})
        self.cli('pack', library, '--registry', self.registry)
        handler = functools.partial(RegistryHandler, directory=self.registry)
        httpd = http.server.ThreadingHTTPServer(('127.0.0.1', 0), handler)
        httpd.requests = []
        thread = threading.Thread(target=httpd.serve_forever)
        thread.start()
        self.addCleanup(lambda: (httpd.shutdown(), thread.join(), httpd.server_close()))
        base = f'http://127.0.0.1:{httpd.server_port}'
        app = self.pkg('app', 'client', deps={'read': self.dep('acme/read', '^1.0.0', base)})
        forbidden = {**self.env, 'PATH': ''}
        warnings = self.cli('fetch', app, env=forbidden)
        self.assertIn('prepare', warnings.stdout + warnings.stderr)
        self.assertFalse(marker.exists())
        pin = self.locked(app, 'acme/read')
        self.assertEqual(pin['source'], base)
        self.assertEqual(httpd.requests, ['/index/acme/read.json', '/artifacts/' + pin['sha256'] + '.lypkg'])
        self.assertEqual((self.cache / 'sources' / pin['sha256'] / 'src/main.ly').read_bytes(), (library / 'src/main.ly').read_bytes())
        self.cli('fetch', app, '--locked', env=forbidden)
        self.cli('fetch', app, env=forbidden)
        self.cli('fetch', app, '--offline', '--locked', env=forbidden)
        self.assertEqual(len(httpd.requests), 2)
        previous = (app / 'lyra.lock').read_bytes()
        shutil.rmtree(self.cache)
        missing = self.cli('fetch', app, '--offline', '--locked', good=False, env=forbidden)
        self.assertIn('acme/read@1.0.0', missing.stderr)
        self.assertIn(pin['sha256'], missing.stderr)
        self.assertEqual(len(httpd.requests), 2)
        self.cli('fetch', app, '--locked', env=forbidden)
        self.assertEqual(httpd.requests[2:], ['/artifacts/' + pin['sha256'] + '.lypkg'])
        self.assertEqual((app / 'lyra.lock').read_bytes(), previous)
        self.cli('lock', app, '--update', env=forbidden)
        self.assertEqual(httpd.requests[3:], ['/index/acme/read.json'])
        self.assertFalse(marker.exists())

    def test_invalid_manifest_or_incomplete_archive_is_rejected(self):
        app = self.pkg('app', 'client', deps={'invalid': self.dep('acme//notes', '^1.0.0')})
        self.assertIn('LYK0001', self.cli('lock', app, good=False).stderr)
        self.assertFalse((app / 'lyra.lock').exists())
        name = b'src/notes.ly'
        contents = b'LYRA-PACK-1\n' + len(name).to_bytes(4, 'big') + (1).to_bytes(4, 'big') + name + b'X'
        digest = hashlib.sha256(contents).hexdigest()
        (self.registry / 'index').mkdir(parents=True)
        (self.registry / 'artifacts').mkdir()
        (self.registry / 'index/lib.json').write_text(json.dumps(dict(name='lib', versions=[dict(version='1.0.0', sha256=digest)])))
        (self.registry / 'artifacts' / (digest + '.lypkg')).write_bytes(contents)
        self.pkg('app', 'client', deps={'safe': self.dep('lib', '1.0.0')})
        self.assertIn('LYK0001', self.cli('fetch', app, good=False).stderr)
        self.assertFalse((app / 'lyra.lock').exists())
        self.assertFalse((self.cache / 'sources').exists())


if __name__ == '__main__':
    unittest.main()
