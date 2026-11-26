from contextlib import contextmanager
import json
import os
from pathlib import Path
from queue import Queue
import subprocess
import sys
import tempfile
from threading import Thread
import unittest
from urllib.parse import quote


COMPILER = Path(sys.argv.pop(1)).resolve()


def uri(path):
    value = path.resolve().as_posix()
    if len(value) > 1 and value[1] == ':':
        value = '/' + value
    return 'file://' + quote(value, safe='/:')


def position_at(text, offset):
    before = text[:offset]
    return {'line': before.count('\n'), 'character': len(before.rsplit('\n', 1)[-1].encode('utf-16-le')) // 2}


def document_request(number, path, operation, **params):
    return {'jsonrpc': '2.0', 'id': number, 'method': 'textDocument/' + operation,
            'params': {'textDocument': {'uri': uri(path)}, **params}}


def message(value):
    body = json.dumps(value, ensure_ascii=False, separators=(',', ':')).encode('utf-8')
    return b'Content-Length: ' + str(len(body)).encode() + b'\r\n\r\n' + body


def run_server(documents, messages, folders=()):
    output = []
    transcript = [
        {'jsonrpc': '2.0', 'id': 1, 'method': 'initialize', 'params': {'capabilities': {'general': {'positionEncodings': ['utf-16']}},
                                                               'workspaceFolders': [{'uri': uri(folder), 'name': folder.name} for folder in folders]}},
        {'jsonrpc': '2.0', 'method': 'initialized', 'params': {}},
    ]
    for path, text in documents:
        language = 'lyra-manifest' if path.name == 'lyra.json5' else 'lyra'
        transcript.append({'jsonrpc': '2.0', 'method': 'textDocument/didOpen', 'params': {'textDocument': {'uri': uri(path), 'languageId': language, 'version': 1, 'text': text}}})
    transcript.extend(messages)
    open_versions = {}
    with interactive_server(documents[0][0].parents[1]) as (send, receive):
        for request in transcript:
            send(request)
            method = request['method']
            if 'id' in request:
                while True:
                    reply = receive()
                    output.append(reply)
                    if reply.get('id') == request['id']:
                        break
            elif method in ('textDocument/didOpen', 'textDocument/didChange', 'textDocument/didClose'):
                document = request['params']['textDocument']
                key = document['uri']
                if method == 'textDocument/didChange' and document['version'] <= open_versions[key]:
                    continue
                if method == 'textDocument/didClose':
                    open_versions.pop(key)
                    pending = {**open_versions, key: None}
                else:
                    open_versions[key] = document['version']
                    pending = dict(open_versions)
                while pending:
                    reply = receive()
                    output.append(reply)
                    if reply.get('method') == 'textDocument/publishDiagnostics':
                        by_uri = reply['params']
                        if by_uri['uri'] in pending and pending[by_uri['uri']] == by_uri.get('version'):
                            pending.pop(by_uri['uri'])
    return output


@contextmanager
def interactive_server(root):
    environment = {**os.environ, 'LYRA_CACHE': str(root / 'cache')}
    with subprocess.Popen([COMPILER, 'lsp', '--stdio'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, env=environment) as process:
        incoming = Queue()

        def read_messages():
            try:
                for header in iter(process.stdout.readline, b''):
                    length = int(header.split(b':', 1)[1])
                    assert process.stdout.readline() == b'\r\n'
                    incoming.put(json.loads(process.stdout.read(length)))
            except Exception as error:
                incoming.put(error)
            finally:
                incoming.put(EOFError('Lyra LSP closed stdout'))

        reader = Thread(target=read_messages, daemon=True)
        reader.start()

        def send(value):
            process.stdin.write(message({'jsonrpc': '2.0', **value}))
            process.stdin.flush()

        def receive():
            value = incoming.get(timeout=20)
            if isinstance(value, Exception):
                raise value
            return value

        try:
            yield send, receive
            send({'id': 90, 'method': 'shutdown'})
            send({'method': 'exit'})
            assert process.wait(timeout=10) == 0, process.stderr.read().decode('utf-8', errors='replace')
        finally:
            if process.poll() is None:
                process.kill()
            process.wait(timeout=10)
            reader.join(timeout=10)


class LanguageServer(unittest.TestCase):
    def test_tests_have_separate_analysis_contexts_and_see_unsaved_sources(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            api = root / 'src/api.ly'
            healthy = root / 'tests/healthy.ly'
            broken = root / 'tests/broken.ly'
            api_text = 'pub fn answer(): Int { 42 }\nfn main(): Unit {}\n'
            healthy_text = 'import self.{answer}\nfn main(): Unit { val outcome = answer(); print(outcome) }\n'
            broken_text = 'import self.{answer}\nfn main(): Unit { print(missingName) }\n'
            for path, text in [(root / 'lyra.json5', '{name:"kit",version:"0.1.0"}'), (api, api_text),
                               (healthy, healthy_text), (broken, 'fn main(): Unit {}\n')]:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text, encoding='utf-8')
            site = position_at(healthy_text, healthy_text.rindex('answer'))
            requests = [document_request(2, healthy, 'hover', position=site), document_request(3, healthy, 'definition', position=site),
                        document_request(4, healthy, 'inlayHint', range={'start': {'line': 0, 'character': 0}, 'end': {'line': 2, 'character': 0}})]
            output = run_server([(healthy, healthy_text), (broken, broken_text), (api, api_text)], requests)
            results = {item['id']: item['result'] for item in output if 'id' in item}
            self.assertIn('fn answer(): Int', results[2]['contents']['value'])
            self.assertEqual(results[3][0]['uri'], uri(api))
            self.assertEqual([hint['label'] for hint in results[4]], [': Int'])
            diagnostics = {item['params']['uri']: item['params']['diagnostics'] for item in output if item.get('method') == 'textDocument/publishDiagnostics'}
            self.assertEqual(diagnostics[uri(healthy)], [])
            self.assertEqual(diagnostics[uri(api)], [])
            self.assertEqual([error['code'] for error in diagnostics[uri(broken)]], ['LYN0001'])
            self.assertIn('missingName', diagnostics[uri(broken)][0]['message'])
            self.assertFalse((root / 'lyra.lock').exists())

    def test_incomplete_decorated_body_does_not_hide_independent_errors(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'src/main.ly'
            text = '@trace\nfn decorated(): Unit { val broken = ; print(1) }\nfn main(): Unit { print(unbound) }\n'
            source.parent.mkdir()
            source.write_text(text, encoding='utf-8')
            (root / 'lyra.json5').write_text('{name:"kit",version:"0.1.0"}', encoding='utf-8')
            output = run_server([(source, text)], [])
            errors = next(item['params']['diagnostics'] for item in output if item.get('method') == 'textDocument/publishDiagnostics')
            self.assertTrue(any(item['range']['start']['line'] == 1 for item in errors), errors)
            self.assertTrue(any(item['code'] == 'LYN0001' and 'unbound' in item['message'] for item in errors), errors)

    def test_navigation_and_semantics_use_current_symbol_identity(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'app/src/main.ly'
            library = root / 'model/src/api.ly'
            unopened = root / 'tools/src/tools.ly'
            model = ('pub class User(pub val id: Int) {\n'
                     '    pub fn getId(): Int { id }\n'
                     '}\npub type UserView = User\n')
            text = ('import model as records\nimport model.{UserView as Profile}\n'
                    '/// Displays the user.\n/// This is folded as a group.\n'
                    'fn getUser(value: Profile): Profile { value }\n'
                    'fn main(): Unit {\n'
                    '    val result = records.User(7)\n'
                    '    /* 中文🙂 */ val view: Profile = getUser(result)\n'
                    '    if true { val result = "inner"; print(result) }\n'
                    '    print(view.getId())\n'
                    '    print(result.id)\n}\n')
            for path, content in [
                (root / 'lyra.json5', "{workspace:{members:['app','model','tools']}}"),
                (root / 'app/lyra.json5', "{name:'app',version:'0.1.0',dependencies:{model:{name:'model',version:'0.1.0',path:'../model'}}}"),
                (root / 'model/lyra.json5', "{name:'model',version:'0.1.0',kind:'lib'}"),
                (root / 'tools/lyra.json5', "{name:'tools',version:'0.1.0',kind:'lib'}"),
                (root / 'cache/hidden/lyra.json5', "{name:'cached',version:'0.1.0',kind:'lib'}"),
                (root / 'cache/hidden/src/hidden.ly', 'pub fn findWidget(): Unit {}'),
                (unopened, 'pub fn findWidget(): Unit {}'),
                (library, model), (source, text),
            ]:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content, encoding='utf-8')
            target = position_at(text, text.index('Profile =') + 2)
            break_edit = text.replace('print(result.id)', 'val writing = ;')
            output = run_server([(source, text)], [
                document_request(2, source, 'semanticTokens/full'),
                document_request(3, source, 'documentHighlight', position=position_at(text, text.index('result.id') + 1)),
                document_request(4, source, 'typeDefinition', position=position_at(text, text.index('view.getId') + 1)),
                document_request(5, source, 'typeDefinition', position=target),
                document_request(6, source, 'definition', position=target),
                {'id': 7, 'method': 'workspace/symbol', 'params': {'query': 'findwidget'}},
                {'id': 8, 'method': 'workspace/symbol', 'params': {'query': 'User.getId'}},
                {'method': 'textDocument/didChange', 'params': {'textDocument': {'uri': uri(source), 'version': 2}, 'contentChanges': [{'text': break_edit}]}},
                document_request(9, source, 'documentSymbol'),
                document_request(10, source, 'foldingRange'),
                document_request(11, source, 'semanticTokens/full'),
            ], folders=(root,))
            replies = {item['id']: item['result'] for item in output if 'id' in item}
            legend = replies[1]['capabilities']['semanticTokensProvider']['legend']['tokenTypes']

            def tokens(number):
                result = {}
                line = column = 0
                data = iter(replies[number]['data'])
                for row, col, length, kind, modifiers in zip(data, data, data, data, data, strict=True):
                    line += row
                    column = col if row else column + col
                    result[line, column] = (legend[kind], length, modifiers)
                return result

            resolved = tokens(2)
            for marker, name, kind, declaration in [
                ('value: Profile', 'value', 'parameter', True), ('value }', 'value', 'parameter', False),
                ('records.User', 'records', 'namespace', False), ('User(7)', 'User', 'class', False),
                ('view: Profile', 'view', 'variable', True), ('Profile =', 'Profile', 'type', False),
                ('getUser(result)', 'getUser', 'function', False), ('getId()', 'getId', 'method', False),
                ('id)', 'id', 'property', False),
            ]:
                where = position_at(text, text.index(marker))
                actual, length, modifiers = resolved[where['line'], where['character']]
                self.assertEqual((actual, length, modifiers & 1), (kind, len(name), declaration))
            self.assertEqual([item['range']['start'] for item in replies[3]],
                             [position_at(text, text.index(marker)) for marker in ('result = records', 'result)\n    if', 'result.id')])
            for number, name in ((4, 'User'), (5, 'User'), (6, 'UserView')):
                location, = replies[number]
                start = model.index(name)
                self.assertEqual(location, {'uri': uri(library), 'range': {'start': position_at(model, start), 'end': position_at(model, start + len(name))}})
            self.assertEqual([item['location']['uri'] for item in replies[7]], [uri(unopened)])
            member, = replies[8]
            self.assertEqual((member['name'], member['containerName'], member['location']['uri']), ('getId', 'User', uri(library)))
            self.assertEqual([item['name'] for item in replies[9]], ['getUser', 'main'])
            self.assertEqual(replies[9][1]['selectionRange']['start'], position_at(break_edit, break_edit.index('main')))
            self.assertIn({'startLine': 2, 'endLine': 3, 'kind': 'comment'}, replies[10])
            self.assertIn({'startLine': 0, 'endLine': 1, 'kind': 'imports'}, replies[10])
            self.assertIn({'startLine': 5, 'endLine': 11}, replies[10])
            partial = tokens(11)
            where = position_at(text, text.index('value: Profile'))
            self.assertIn((where['line'], where['character']), partial)
            where = position_at(text, text.index('view.getId'))
            self.assertNotIn((where['line'], where['character']), partial)

    def test_unsaved_manifest_completion_and_diagnostics_leave_disk_resolution_unchanged(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = root / 'app/lyra.json5'
            workspace = root / 'lyra.json5'
            source = manifest.parent / 'src/main.ly'
            library = root / 'model/src/api.ly'
            text = 'import model.{available}\nfn main(): Unit { available() }\n'
            saved = "{name:'app',version:'0.1.0',dependencies:{model:{name:'model',version:'0.1.0',path:'../model'}}}"
            packages = "{workspace:{members:['app','model']}}"
            for path, content in [(manifest, saved), (workspace, packages), (source, text), (library, 'pub fn available(): Unit {}'),
                                  (root / 'model/lyra.json5', "{name:'model',version:'0.1.0',kind:'lib'}")]:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content, encoding='utf-8')
            edited = "{name:'app', version:'0.1.0', /* 中文🙂 */ kind: 'l', }"
            nested = "{name:'app', version:'0.1.0', dependencies:{model:{name:'model', version:'^0.1.0', pat}}}"
            editing_workspace = "{workspace:{ mem: ['app'] }}"
            missing_member = "{workspace:{members:['app','absent']}}"

            def change(path, version, content):
                return {'method': 'textDocument/didChange', 'params': {'textDocument': {'uri': uri(path), 'version': version},
                                                                      'contentChanges': [{'text': content}]}}

            output = run_server([(source, text), (manifest, edited), (workspace, editing_workspace)], [
                document_request(2, manifest, 'completion', position=position_at(edited, edited.index("'l'") + 2)),
                document_request(3, workspace, 'completion', position=position_at(editing_workspace, editing_workspace.index('mem') + 3)),
                change(manifest, 2, nested),
                document_request(4, manifest, 'completion', position=position_at(nested, nested.index('pat') + 3)),
                change(workspace, 2, missing_member),
                change(manifest, 3, saved),
            ])
            replies = {item['id']: item['result'] for item in output if 'id' in item}
            lib, = replies[2]['items']
            value = edited.index("'l'")
            value_range = {'start': position_at(edited, value), 'end': position_at(edited, value + 3)}
            self.assertEqual(lib['textEdit'], {'range': value_range, 'newText': "'lib'"})
            self.assertEqual([(item['label'], item['textEdit']['newText']) for item in replies[3]['items']], [('members', 'members')])
            self.assertEqual([(item['label'], item['kind']) for item in replies[4]['items']], [('path', 10)])
            diagnostics = {(item['params']['uri'], item['params'].get('version')): item['params']['diagnostics'] for item in output
                           if item.get('method') == 'textDocument/publishDiagnostics'}
            self.assertEqual(diagnostics[uri(source), 1], [])
            self.assertEqual(diagnostics[uri(manifest), 1][0]['range'], value_range)
            self.assertEqual(diagnostics[uri(manifest), 1][0]['code'], 'LYK0001')
            self.assertEqual(diagnostics[uri(manifest), 3], [])
            self.assertEqual(diagnostics[uri(workspace), 2][0]['range']['start'], position_at(missing_member, missing_member.index("'absent'")))
            self.assertEqual(manifest.read_text(encoding='utf-8'), saved)
            self.assertEqual(workspace.read_text(encoding='utf-8'), packages)

    def test_recovery_keeps_other_functions_checking_and_versions_precise_fixes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'src/main.ly'
            source.parent.mkdir()
            other = source.with_name('other.ly')
            definitions = ('fn brokenSyntax(): Int { val value = ; 0 }\n'
                           'fn choose(value: Int8): Int { 1 }\n'
                           'fn choose(value: UInt8): Int { 2 }\n')
            other.write_text(definitions, encoding='utf-8')
            text = ('fn first(): Int { /* 中文🙂 */ 1; }\r\n'
                    'fn second(): Int { 2; }\r\n'
                    'fn ambiguous(): Int { choose(1) }\r\n'
                    'fn main(): Unit { val stable = 7; print(stable) }\r\n')
            source.write_bytes(text.encode())

            def selected(first, last):
                return {'start': position_at(text, first), 'end': position_at(text, last)}

            range_first = selected(text.index(';'), text.index(';') + 1)
            whole = selected(0, len(text))
            output = run_server([(source, text)], [
                document_request(2, source, 'codeAction', range=range_first, context={'diagnostics': [], 'only': ['quickfix']}),
                document_request(3, source, 'codeAction', range=whole, context={'diagnostics': [], 'only': ['source.fixAll.lyra']}),
                document_request(4, source, 'inlayHint', range=whole),
            ])
            diagnostics = [item['params'] for item in output if item.get('method') == 'textDocument/publishDiagnostics']
            current = next(item['diagnostics'] for item in diagnostics if item['uri'] == uri(source))
            self.assertEqual([item['code'] for item in current], ['LYT0001', 'LYT0001', 'LYT0019'])
            self.assertEqual(current[0]['range'], range_first)
            self.assertEqual({(note['location']['uri'], note['location']['range']['start']['line']) for note in current[2]['relatedInformation']},
                             {(uri(other), 1), (uri(other), 2)})
            closed = next(item['diagnostics'] for item in diagnostics if item['uri'] == uri(other))
            self.assertEqual([item['code'] for item in closed], ['LYP0001'])
            results = {item['id']: item['result'] for item in output if 'id' in item}
            quick, = results[2]
            edit, = quick['edit']['documentChanges']
            self.assertEqual(edit, {'textDocument': {'uri': uri(source), 'version': 1}, 'edits': [{'range': range_first, 'newText': ''}]})
            fix_all, = results[3]
            self.assertEqual([change['range']['start']['line'] for change in fix_all['edit']['documentChanges'][0]['edits']], [0, 1])
            self.assertEqual([item['label'] for item in results[4]], [': Int'])

    def test_context_completion_distinguishes_declarations_values_types_and_macro_families(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'src/main.ly'
            source.parent.mkdir()
            (root / 'lyra.json5').write_text('{name:"app",version:"0.1.0"}', encoding='utf-8')
            text = ('vHere\n#\n@\nfn sample(request: String): Unit {\n'
                    '    val text: StrHere = "#doc"\n    prHere\n    imHere\n    #\n}\n')
            source.write_text('', encoding='utf-8')
            positions = [1, text.index('#') + 1, text.index('@') + 1, text.index('StrHere') + 3,
                         text.index('prHere') + 2, text.index('imHere') + 2, text.rindex('#') + 1,
                         text.index('#doc') + 2]
            output = run_server([(source, text)], [document_request(index, source, 'completion', position=position_at(text, offset))
                                                    for index, offset in enumerate(positions, 2)])
            items = {item['id']: {entry['label']: entry['kind'] for entry in item['result']['items']}
                     for item in output if item.get('id', 0) > 1}
            self.assertEqual(items[2], {})
            self.assertEqual(items[3], {'derive': 3, 'post': 3})
            self.assertEqual(items[4], {'trace': 23})
            self.assertEqual(items[5], {'String': 7})
            self.assertEqual(items[6]['print'], 3)
            self.assertNotIn('import', items[7])
            self.assertNotIn('implicit', items[7])
            self.assertEqual(items[8], {'assert': 3})
            self.assertEqual(items[9], {})

    def test_generated_completion_preserves_only_necessary_escapes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'src/main.ly'
            macro = root / 'src/.macros/emit.ly'
            macro.parent.mkdir(parents=True)
            (root / 'lyra.json5').write_text('{name:"app",version:"0.1.0"}', encoding='utf-8')
            text = '#emit\nfn main(): Unit { routeName(); `val`(); unfinished }\n'
            source.write_text(text, encoding='utf-8')
            declarations = 'fn `routeName`(): Unit {}\nfn `val`(): Unit {}\n'
            macro.write_text(f'fn emit(target: String, raw: String): Syntax {{ syntaxCode({json.dumps(declarations)}) }}', encoding='utf-8')
            offsets = [text.index('routeName') + len('routeNa'), text.index('`val`') + len('`val')]
            output = run_server([(source, text)], [document_request(index, source, 'completion', position=position_at(text, offset))
                                                    for index, offset in enumerate(offsets, 2)])
            results = {item['id']: [(entry['label'], entry['kind'], entry['textEdit']['newText'])
                                    for entry in item['result']['items']]
                       for item in output if item.get('id', 0) > 1}
            self.assertEqual(results, {2: [('routeName', 3, 'routeName')], 3: [('`val`', 3, '`val`')]})

    def test_module_and_group_completion_preserve_the_file_scope(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            app = root / 'client'
            source = app / 'src/main.ly'
            sibling = app / 'src/sibling.ly'
            export = app / 'src/api/exports.ly'
            declaration = root / 'kit/src/sub/implementation.ly'
            for path, text in [
                (app / 'lyra.json5', '{name:"client",version:"0.1.0",dependencies:{kit:{name:"kit",version:"0.1.0",path:"../kit"},'
                                    'pending:{name:"missing",version:"0.1.0",registry:"https://invalid.invalid"}}}'),
                (root / 'kit/lyra.json5', '{name:"kit",version:"0.1.0",kind:"lib"}'),
                (declaration, '/// Builds a heading.\npub fn renderTitle(): String { "hello" }\nfn hiddenTitle(): String { "private" }\n'),
                (export, 'pub import kit.sub.{renderTitle as title}\n'),
                (sibling, 'import std.http.{Router as WebRouter}\n'),
                (source, ''),
            ]:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text, encoding='utf-8')
            text = ('// 文件头\r\n\r\nimport \r\nimport std.ht\r\nimport std.http\r\nimport std.http.{Request, }\r\n'
                    'import kit.sub as net\r\nimport self.api as api\r\nimport kit.su\r\n'
                    'pub import kit.sub.{ }\r\n'
                    'fn typed(value: http.Req): Unit {}\r\n'
                    'fn main(): Unit { val _ = "中文🙂"; net.renderTi; http.Ro; api.tit; renderTi; Web; tit; htt }\r\n')
            markers = ['import ', 'std.ht', 'std.http.{Request, ', 'kit.su\r', 'pub import kit.sub.{',
                       'net.renderTi', 'http.Ro', 'api.tit', '; renderTi', 'Web', '; tit', 'http.Req', '; htt']
            offsets = [text.index(marker) + len(marker.rstrip('\r')) for marker in markers]
            output = run_server([(source, text)], [document_request(index, source, 'completion', position=position_at(text, offset))
                                                    for index, offset in enumerate(offsets, 2)])
            items = {item['id']: {entry['label']: entry for entry in item['result']['items']}
                     for item in output if item.get('id', 0) > 1}
            self.assertTrue({'kit', 'pending', 'self', 'std'} <= items[2].keys())
            self.assertTrue({'http', 'httpClient', 'httpTls'} <= items[3].keys())
            self.assertIn('Router', items[4])
            self.assertNotIn('Request', items[4])
            self.assertEqual(set(items[5]), {'sub'})
            self.assertEqual(set(items[6]), {'renderTitle'})
            member = items[7]['renderTitle']
            self.assertEqual(member['documentation']['value'], 'Builds a heading.')
            self.assertNotIn('additionalTextEdits', member)
            self.assertEqual(member['textEdit']['newText'], 'renderTitle')
            self.assertIn('Router', items[8])
            self.assertEqual(set(items[9]), {'title'})
            self.assertEqual(items[10], {})
            self.assertEqual(items[11], {})
            self.assertEqual(items[12], {})
            self.assertIn('Request', items[13])
            self.assertEqual(items[14]['http']['kind'], 9)
            self.assertFalse((app / 'lyra.lock').exists())
            self.assertEqual(source.read_bytes(), b'')

    def test_namespace_alias_navigation_respects_overloads_and_local_shadowing(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            app = root / 'app'
            source = app / 'src/main.ly'
            api = root / 'kit/src/net/api.ly'
            api.parent.mkdir(parents=True)
            source.parent.mkdir(parents=True)
            (app / 'lyra.json5').write_text('{name:"app",version:"0.1.0",dependencies:{kit:{name:"kit",version:"0.1.0",path:"../kit"}}}', encoding='utf-8')
            (root / 'kit/lyra.json5').write_text('{name:"kit",version:"0.1.0",kind:"lib"}', encoding='utf-8')
            library = ('pub struct Box(pub val value: Int)\n'
                       'pub fn render(count: Int): String { count.toString() }\n'
                       'pub fn render(text: String): String { text }\n')
            api.write_text(library, encoding='utf-8')
            text = ('import kit.net\nfn take(box: net.Box): Int { box.value }\n'
                    'fn main(): Unit {\n    print(net.render(7)); print(net.render("text"))\n'
                    '    { val net = [1]; print(net.size) }\n}\n')
            source.write_text(text, encoding='utf-8')
            integer = position_at(text, text.index('net.render(7)') + len('net.r'))
            string = position_at(text, text.index('net.render("text")') + len('net.r'))
            alias = position_at(text, text.index('net.Box') + 1)
            local = position_at(text, text.index('net.size') + len('net.'))
            output = run_server([(source, text)], [
                document_request(2, source, 'definition', position=integer),
                document_request(3, source, 'definition', position=string),
                document_request(4, source, 'definition', position=alias),
                document_request(5, source, 'rename', position=alias, newName='network'),
                document_request(6, source, 'completion', position=local),
            ])
            results = {item['id']: item['result'] for item in output if 'id' in item}
            for index, signature in ((2, 'render(count'), (3, 'render(text')):
                location, = results[index]
                self.assertEqual(location['uri'], uri(api))
                self.assertEqual(location['range']['start'], position_at(library, library.index(signature)))
            defined, = results[4]
            imported = position_at(text, len('import kit.'))
            self.assertEqual(defined['range']['start'], imported)
            alias_end = position_at(text, len('import kit.net'))
            document, = results[5]['documentChanges']
            self.assertEqual(document['textDocument'], {'uri': uri(source), 'version': 1})
            edits = document['edits']
            self.assertIn({'range': {'start': alias_end, 'end': alias_end}, 'newText': ' as network'}, edits)
            self.assertEqual({edit['range']['start']['line'] for edit in edits if edit['newText'] == 'network'}, {1, 3})
            self.assertEqual(len(edits), 4)
            members = {item['label'] for item in results[6]['items']}
            self.assertIn('size', members)
            self.assertFalse({'render', 'Box'} & members)

    def test_import_quick_fix_reuses_alias_and_keeps_multiple_sources_separate(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            app = root / 'app'
            source = app / 'src/main.ly'
            for path, text in [
                (app / 'lyra.json5', '{name:"app",version:"0.1.0",dependencies:{kit:{name:"kit",version:"0.1.0",path:"../kit"}}}'),
                (root / 'kit/lyra.json5', '{name:"kit",version:"0.1.0",kind:"lib"}'),
                (root / 'kit/src/net/api.ly', 'pub fn renderTitle(): String { "kit" }\n'),
                (app / 'src/net/api.ly', 'pub fn renderTitle(): String { "self" }\n'),
                (source, ''),
            ]:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text, encoding='utf-8')

            original = '// 中文🙂\r\nimport std.time as times\r\nfn main(): Unit { val _ = Clock.now() }\r\n'
            changed = ('// 中文🙂\r\nimport kit.net as kitNet\r\n'
                       'fn main(): Unit { val net = 7; print("中文🙂"); print(renderTitle()) }\r\n')

            def request(number, text, name, **context):
                start = text.index(name)
                return document_request(number, source, 'codeAction', range={
                    'start': position_at(text, start), 'end': position_at(text, start + len(name)),
                }, context={'diagnostics': [], **context})

            output = run_server([(source, original)], [
                request(2, original, 'Clock', only=['quickfix']),
                request(3, original, 'Clock', only=['refactor']),
                request(4, original, 'main'),
                {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                    'textDocument': {'uri': uri(source), 'version': 2}, 'contentChanges': [{'text': changed}],
                }},
                request(5, changed, 'renderTitle'),
            ])
            results = {item['id']: item['result'] for item in output if 'id' in item}
            self.assertIn('codeActionProvider', results[1]['capabilities'])
            self.assertEqual(results[3], [])
            self.assertEqual(results[4], [])

            def versioned_edits(action, version):
                self.assertEqual(action['kind'], 'quickfix')
                document, = action['edit']['documentChanges']
                self.assertEqual(document['textDocument'], {'uri': uri(source), 'version': version})
                return document['edits']

            reuse, = results[2]
            self.assertEqual([edit['newText'] for edit in versioned_edits(reuse, 1)], ['times.Clock'])
            reuse, add = results[5]
            self.assertEqual([edit['newText'] for edit in versioned_edits(reuse, 2)], ['kitNet.renderTitle'])
            replace, insert = versioned_edits(add, 2)
            self.assertEqual(replace['range'], request(6, changed, 'renderTitle')['params']['range'])
            self.assertEqual(replace['newText'], 'net2.renderTitle')
            self.assertEqual(insert, {'range': {'start': {'line': 1, 'character': 0}, 'end': {'line': 1, 'character': 0}},
                                      'newText': 'import self.net as net2\r\n'})

    def test_member_completion_and_overload_signatures_use_current_types(self):
        with tempfile.TemporaryDirectory() as temporary:
            app = Path(temporary)
            source = app / 'src/main.ly'
            api = app / 'src/api/box.ly'
            api.parent.mkdir(parents=True)
            (app / 'lyra.json5').write_text('{name:"app",version:"0.1.0"}', encoding='utf-8')
            api.write_text('pub class Box<T>(pub val value: T, val secret: Int = 0) {\n'
                           '    pub static fn one(value: T): Box<T> { Box(value) }\n'
                           '    pub fn get(): T { value }\n    fn internal(): T { value }\n}\n'
                           'pub enum State { Closed, Opened(Int) }\n'
                           'pub extension BoxOps<T> for Box<T> { fn unwrap(): T { this.value } }\n'
                           'pub extension OtherOps<T> for Box<T> { fn unwanted(): T { this.value } }\n', encoding='utf-8')
            text = ('import self.api.{Box, BoxOps, State}\nimport std.http\n'
                    '/// Integer lookup.\nfn lookup(count: Int, limit: Int): Int { count + limit }\n'
                    'fn lookup(count: Int, label: String): String { label }\n'
                    'fn lookup(text: String, other: Bool): Bool { other }\n'
                    'fn valueProbe(): Unit { val first = Box(5); first. }\n'
                    'fn optionProbe(): Unit { val optional: Box<Int>? = Box(5); optional?. }\n'
                    'fn staticProbe(): Unit { Box<Int>. }\nfn enumProbe(): Unit { State. }\n'
                    'fn listProbe(): Unit { [1, 2]. }\nfn overloadProbe(): Unit { lookup(1, li) }\n'
                    'fn main(): Unit { http.listen(8080, ho\n}\n')
            source.write_text('', encoding='utf-8')
            points = [(word, 'completion') for word in ['first.', 'optional?.', 'Box<Int>.', 'State.', '[1, 2].', 'lookup(1, li', 'listen(8080, ho']]
            points.extend([('lookup(1, li', 'signatureHelp'), ('listen(8080, ho', 'signatureHelp')])
            output = run_server([(source, text)], [document_request(index, source, method, position=position_at(text, text.index(word) + len(word)))
                                                    for index, (word, method) in enumerate(points, 2)])
            result = {item['id']: item['result'] for item in output if 'id' in item}
            self.assertIn('signatureHelpProvider', result[1]['capabilities'])
            for index in (2, 3):
                members = {item['label']: item for item in result[index]['items']}
                self.assertEqual(members['value']['kind'], 5)
                self.assertIn(': Int', members['get']['detail'])
                self.assertIn('unwrap', members)
                self.assertFalse({'other', 'secret', 'internal', 'one', 'unwanted'} & members.keys())
            self.assertEqual({item['label'] for item in result[4]['items']}, {'one'})
            self.assertEqual({item['label']: item['kind'] for item in result[5]['items']}, {'Closed': 20, 'Opened': 20})
            self.assertTrue({'add', 'slice', 'size'} <= {item['label'] for item in result[6]['items']})
            self.assertIn('limit:', {item['label'] for item in result[7]['items']})
            self.assertIn('host:', {item['label'] for item in result[8]['items']})
            self.assertEqual(result[9]['activeParameter'], 1)
            self.assertEqual([item['label'] for item in result[9]['signatures']],
                             ['fn lookup(count: Int, limit: Int): Int', 'fn lookup(count: Int, label: String): String'])
            self.assertEqual(result[9]['signatures'][0]['documentation']['value'], 'Integer lookup.')
            self.assertIn('host: String', result[10]['signatures'][0]['label'])

    def test_latest_edit_wins_during_rapid_changes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'src/main.ly'
            source.parent.mkdir()
            original = 'fn main(): Unit { print(1) }\n'
            source.write_text(original, encoding='utf-8')
            with interactive_server(root) as (send, receive):
                send({'id': 1, 'method': 'initialize'})
                self.assertEqual(receive()['id'], 1)
                send({'method': 'textDocument/didOpen', 'params': {'textDocument': {
                    'uri': uri(source), 'languageId': 'lyra', 'version': 1, 'text': original,
                }}})
                self.assertEqual(receive()['params']['version'], 1)
                for version in range(2, 20):
                    send({'method': 'textDocument/didChange', 'params': {
                        'textDocument': {'uri': uri(source), 'version': version},
                        'contentChanges': [{'text': original if version == 19 else 'fn main(): Unit { unknown() }\n'}],
                    }})
                published = [1]
                while published[-1] < 19:
                    latest = receive()
                    self.assertEqual(latest['method'], 'textDocument/publishDiagnostics')
                    self.assertGreater(latest['params']['version'], published[-1])
                    published.append(latest['params']['version'])
                self.assertEqual(published[-1], 19)
                self.assertLess(len(published), 19)
                self.assertEqual(latest['params']['diagnostics'], [])

    def test_macro_completion_and_definition_follow_compiletime_reexports(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'client/src/main.ly'
            macro = root / 'kit/src/hooks/.expressions/value.ly'
            decorator = root / 'kit/src/hooks/.decorators/audit.ly'
            text = ('import macro kit.checked as guard\nimport decorator kit.report as traced\n'
                    'pub import macro kit.ch\npub import decorator kit.re\n'
                    '@traced\nfn example(): Int { #guard() }\n')
            implementation = '/// Expands to the checked value.\npub fn value(target: String, argument: String): Syntax { quote { 1 } }\n'
            for path, body in [
                (root / 'client/lyra.json5', '{name:"client",version:"0.1.0",dependencies:{kit:{name:"kit",version:"0.1.0",path:"../kit"}}}'),
                (root / 'kit/lyra.json5', '{name:"kit",version:"0.1.0",kind:"lib"}'),
                (root / 'kit/src/exports.ly', 'pub import macro self.hooks.value as checked\npub import decorator self.hooks.audit as report\n'),
                (macro, implementation),
                (decorator, 'pub fn audit(target: String, argument: String): Syntax { syntaxAround(syntaxCode(""), syntaxCode(""), syntaxCode("")) }'),
                (source, text),
            ]:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(body, encoding='utf-8')
            output = run_server([(source, text)], [
                document_request(2, source, 'completion', position=position_at(text, text.index('kit.ch\n') + len('kit.ch'))),
                document_request(3, source, 'completion', position=position_at(text, text.index('kit.re\n') + len('kit.re'))),
                document_request(4, source, 'definition', position=position_at(text, text.rindex('guard') + 2)),
                document_request(5, source, 'definition', position=position_at(text, text.rindex('traced') + 2)),
                document_request(6, source, 'definition', position=position_at(text, text.index('checked') + 2)),
                document_request(7, source, 'hover', position=position_at(text, text.rindex('guard') + 2)),
            ])
            results = {item['id']: item['result'] for item in output if 'id' in item}
            self.assertEqual([item['label'] for item in results[2]['items']], ['checked'])
            self.assertEqual([item['label'] for item in results[3]['items']], ['report'])
            self.assertEqual(results[4][0]['uri'], uri(macro))
            self.assertEqual(results[4][0]['range']['start'], position_at(implementation, implementation.index('fn value') + 3))
            self.assertEqual(results[5][0]['uri'], uri(decorator))
            self.assertEqual(results[6], results[4])
            self.assertIn('Expands to the checked value.', results[7]['contents']['value'])

    def test_incomplete_source_completes_keywords_imports_and_only_visible_names(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'src/main.ly'
            source.parent.mkdir()
            (root / 'lyra.json5').write_text('{name:"app",version:"0.1.0"}', encoding='utf-8')
            (source.parent / 'sibling.ly').write_text('fn rowOutsideFile(): Int { 1 }\n', encoding='utf-8')
            text = ('import std.http.{Router as WebRouter}\n'
                    'fn unrelated(): Unit { val rogue = 0; }\n'
                    'fn serve(route: String): Unit {\n'
                    '    val router = WebRouter()\n'
                    '    if true { val rotten = 42; }\n'
                    '    val _ = "中文🙂"; roSuffix\n'
                    '    Web\n'
                    '    v // router\n'
                    '    val roomy =\n'
                    '}\n')
            source.write_text(text, encoding='utf-8')
            keyword = text.index('v //')
            local = text.index('roSuffix')
            output = run_server([(source, text)], [
                document_request(2, source, 'completion', position=position_at(text, keyword + 1)),
                document_request(3, source, 'completion', position=position_at(text, local + 2)),
                document_request(4, source, 'completion', position=position_at(text, text.index('    Web\n') + len('    Web'))),
                document_request(5, source, 'completion', position=position_at(text, keyword + len('v // roo'))),
            ])
            results = {item['id']: item['result'] for item in output if item.get('id') in range(2, 6)}
            self.assertEqual({(item['label'], item['kind']) for item in results[2]['items']}, {('val', 14), ('var', 14)})
            self.assertEqual({item['label'] for item in results[3]['items']}, {'route', 'router', 'rowOutsideFile'})
            router = next(item for item in results[3]['items'] if item['label'] == 'router')
            self.assertEqual(router['textEdit'], {'newText': 'router', 'range': {
                'start': position_at(text, local), 'end': position_at(text, local + len('roSuffix')),
            }})
            self.assertEqual([item['label'] for item in results[4]['items']], ['WebRouter'])
            self.assertEqual(results[5]['items'], [])
            diagnostics = next(item['params']['diagnostics'] for item in output if item.get('method') == 'textDocument/publishDiagnostics')
            self.assertEqual([item['code'] for item in diagnostics], ['LYP0001'])

    def test_unsaved_prefix_preserves_checked_navigation_and_hints_without_request_errors(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'src/main.ly'
            source.parent.mkdir()
            (root / 'lyra.json5').write_text('{name:"app",version:"0.1.0"}', encoding='utf-8')
            text = 'fn main(): Int {\n    val _ = "中文🙂"; val router = [1, 2]\n    print(router.size)\n    0\n}\n'
            source.write_text(text, encoding='utf-8')
            router_start = text.index('router =')
            use = position_at(text, text.index('router.size') + 1)
            inserted = text.index('    0')
            prior = position_at(text, inserted)
            broken = text[:inserted] + '    v\n' + text[inserted:]
            prefix = position_at(broken, inserted + len('    v'))
            key = {'uri': uri(source)}
            change = {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                'textDocument': {**key, 'version': 2},
                'contentChanges': [{'range': {'start': prior, 'end': prior}, 'text': '    v\n'}],
            }}
            clear = {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                'textDocument': {**key, 'version': 3}, 'contentChanges': [{'text': text}],
            }}
            visible = {'start': {'line': 0, 'character': 0}, 'end': position_at(text, inserted)}
            messages = [document_request(2, source, 'inlayHint', range=visible), change,
                        document_request(3, source, 'completion', position=prefix),
                        document_request(4, source, 'inlayHint', range=visible),
                        document_request(5, source, 'hover', position=use),
                        document_request(6, source, 'definition', position=use),
                        document_request(7, source, 'hover', position={**prefix, 'character': prefix['character'] - 1}),
                        document_request(8, source, 'definition', position={**prefix, 'character': prefix['character'] - 1}),
                        document_request(9, source, 'definition', position={**prefix, 'character': prefix['character'] - 1}),
                        document_request(10, source, 'rename', position=use, newName='items'), clear,
                        document_request(11, source, 'rename', position=use, newName='items')]
            output = run_server([(source, text)], messages)
            results = {item['id']: item['result'] for item in output if item.get('id') in range(1, 12)}
            self.assertTrue(results[1]['capabilities']['inlayHintProvider'])
            self.assertEqual({item['label'] for item in results[3]['items']}, {'val', 'var'})
            hints = [{'position': position_at(text, router_start + len('router')), 'label': ': List<Int>', 'kind': 1}]
            self.assertEqual(results[2], hints)
            self.assertEqual(results[4], hints)
            self.assertIn('val router', results[5]['contents']['value'])
            self.assertEqual(results[6][0]['range']['start'], position_at(text, router_start))
            self.assertEqual([results[number] for number in range(7, 11)], [None] * 4)
            document, = results[11]['documentChanges']
            self.assertEqual(document['textDocument'], {'uri': uri(source), 'version': 3})
            self.assertEqual(len(document['edits']), 2)
            diagnostics = [item['params']['diagnostics'] for item in output if item.get('method') == 'textDocument/publishDiagnostics']
            self.assertEqual([[item['code'] for item in listing] for listing in diagnostics], [[], ['LYN0001'], []])
            self.assertEqual(diagnostics[1][0]['range']['end'], prefix)

    def test_inlay_hints_use_inferred_function_method_and_generic_binding_types(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / 'src/main.ly'
            source.parent.mkdir()
            text = ('class Box<Value>(val item: Value) { fn get() { item } }\n'
                    'fn generic<Value>(value: Value) { val copy = value; copy }\n'
                    'fn main(): Unit {\n'
                    '    print("中文🙂"); val inferred = Box("text").get()\n'
                    '    val explicit: Int = 7\n'
                    '    var `as` = generic(7)\n'
                    '    val _ = generic(true)\n'
                    '}\n')
            source.write_text(text, encoding='utf-8')
            whole = {'start': position_at(text, 0), 'end': position_at(text, len(text))}
            literals = {'start': position_at(text, text.index('    print')), 'end': position_at(text, text.index('    var'))}
            output = run_server([(source, text)], [
                document_request(2, source, 'inlayHint', range=whole),
                document_request(3, source, 'inlayHint', range=literals),
            ])
            results = {item['id']: item['result'] for item in output if item.get('id') in (2, 3)}
            expected = [{'position': position_at(text, text.index(anchor) + len(anchor)), 'label': ': ' + label, 'kind': 1}
                        for anchor, label in [('fn get()', 'Value'), ('fn generic<Value>(value: Value)', 'Value'),
                                              ('val copy', 'Value'), ('val inferred', 'String'), ('var `as`', 'Int')]]
            self.assertEqual(results[2], expected)
            self.assertEqual(results[3], [expected[3]])
            diagnostics = next(item['params']['diagnostics'] for item in output if item.get('method') == 'textDocument/publishDiagnostics')
            self.assertEqual(diagnostics, [])

    def test_hover_includes_a_multiline_throws_clause(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / 'src/main.ly'
            source.parent.mkdir()
            text = 'exception Closed\nfn read(): Unit\n    throws Closed, Exception {}\nfn main(): Unit { read() }\n'
            source.write_text(text, encoding='utf-8')
            output = run_server([(source, text)], [
                {'jsonrpc': '2.0', 'id': 2, 'method': 'textDocument/hover', 'params': {
                    'textDocument': {'uri': uri(source)}, 'position': {'line': 3, 'character': 19},
                }},
            ])
            hover = next(item['result'] for item in output if item.get('id') == 2)
            self.assertEqual(hover['contents']['value'], '```lyra\nfn read(): Unit throws Closed, Exception\n```')

    def test_formatting_uses_unsaved_source_and_preserves_raw_strings_despite_type_errors(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / 'src' / 'main.ly'
            source.parent.mkdir()
            disk = b'fn main(): Unit {}\n'
            source.write_bytes(disk)
            text = 'fn main(): Unit {\r\n val text = r#"中文 😀  \r\n keep \t  \r\nlast"#  \r\n  unknown(text) \r\n}\r\n'
            result = run_server([(source, text)], [
                {'jsonrpc': '2.0', 'id': 2, 'method': 'textDocument/formatting', 'params': {
                    'textDocument': {'uri': uri(source)}, 'options': {'tabSize': 2, 'insertSpaces': False},
                }},
            ])
            response = next(item for item in result if item.get('id') == 2)
            edit, = response['result']
            self.assertEqual(edit['newText'], 'fn main(): Unit {\r\n\tval text = r#"中文 😀  \r\n keep \t  \r\nlast"#\r\n\tunknown(text)\r\n}\r\n')
            self.assertEqual(edit['range']['end'], {'line': 6, 'character': 0})
            self.assertEqual(source.read_bytes(), disk)

    def test_utf16_navigation_completion_rename_and_formatting(self):
        with tempfile.TemporaryDirectory() as temporary:
            package = Path(temporary) / 'app'
            source = package / 'src/main.ly'
            source.parent.mkdir(parents=True)
            text = 'fn greet(name: String): String { "你好 😀 " + name }\nfn main(): Unit {\n    print("😀"); greet("世界"); }   \n'
            source.write_text(text, encoding='utf-8')
            line = text.splitlines()[2]
            call = line.index('greet')
            utf16 = len(line[:call].encode('utf-16-le')) // 2
            messages = [
                {'jsonrpc': '2.0', 'id': 2, 'method': 'textDocument/definition', 'params': {'textDocument': {'uri': uri(source)}, 'position': {'line': 2, 'character': utf16 + 2}}},
                {'jsonrpc': '2.0', 'id': 3, 'method': 'textDocument/references', 'params': {'textDocument': {'uri': uri(source)}, 'position': {'line': 2, 'character': utf16 + 2}, 'context': {'includeDeclaration': True}}},
                {'jsonrpc': '2.0', 'id': 4, 'method': 'textDocument/hover', 'params': {'textDocument': {'uri': uri(source)}, 'position': {'line': 2, 'character': utf16 + 2}}},
                {'jsonrpc': '2.0', 'id': 5, 'method': 'textDocument/completion', 'params': {'textDocument': {'uri': uri(source)}, 'position': {'line': 2, 'character': utf16 + 3}}},
                {'jsonrpc': '2.0', 'id': 6, 'method': 'textDocument/formatting', 'params': {'textDocument': {'uri': uri(source)}, 'options': {'tabSize': 4, 'insertSpaces': True}}},
                {'jsonrpc': '2.0', 'id': 7, 'method': 'textDocument/rename', 'params': {'textDocument': {'uri': uri(source)}, 'position': {'line': 2, 'character': utf16 + 2}, 'newName': 'welcome'}},
            ]
            output = run_server([(source, text)], messages)
            results = {item.get('id'): item for item in output if 'id' in item}
            self.assertEqual(results[1]['result']['capabilities']['positionEncoding'], 'utf-16')
            definition = results[2]['result'][0]
            self.assertEqual(definition['uri'], uri(source))
            self.assertEqual(definition['range']['start'], {'line': 0, 'character': 3})
            references = results[3]['result']
            self.assertEqual(len(references), 2)
            usage = next(item for item in references if item['range']['start']['line'] == 2)
            self.assertEqual(usage['range']['start']['character'], utf16)
            self.assertEqual(results[4]['result']['contents']['value'], '```lyra\nfn greet(name: String): String\n```')
            self.assertTrue(any(item['label'] == 'greet' for item in results[5]['result']['items']))
            self.assertIn('newText', results[6]['result'][0])
            document, = results[7]['result']['documentChanges']
            self.assertEqual(document['textDocument'], {'uri': uri(source), 'version': 1})
            rename = document['edits']
            self.assertEqual(len(rename), 2)
            self.assertTrue(all(item['newText'] == 'welcome' for item in rename))

    def test_rename_rejects_both_directions_of_capture_without_rejecting_shadowing(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / 'src/main.ly'
            source.parent.mkdir()
            text = ('fn main(): Unit {\n'
                    '    val outside = 1\n'
                    '    if true { val inside = 2; print(outside); print(inside) }\n'
                    '    if true { val separate = 3; print(separate) }\n'
                    '}\n')
            source.write_text(text, encoding='utf-8')
            outside = position_at(text, text.index('outside'))
            inside = position_at(text, text.index('inside'))
            messages = [document_request(2, source, 'rename', position=outside, newName='inside'),
                        document_request(3, source, 'rename', position=inside, newName='outside'),
                        document_request(4, source, 'rename', position=inside, newName='separate'),
                        document_request(5, source, 'rename', position=position_at(text, text.index('main')), newName='start')]
            results = {item['id']: item['result'] for item in run_server([(source, text)], messages) if 'id' in item}
            self.assertEqual([results[2], results[3], results[5]], [None, None, None])
            document, = results[4]['documentChanges']
            self.assertEqual(document['textDocument'], {'uri': uri(source), 'version': 1})
            self.assertEqual([edit['newText'] for edit in document['edits']], ['separate', 'separate'])
            self.assertEqual({edit['range']['start']['line'] for edit in document['edits']}, {2})
            self.assertEqual(source.read_text(encoding='utf-8'), text)

    def test_exported_rename_includes_unopened_workspace_consumers_and_tests(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            library = root / 'kit/src/library.ly'
            client = root / 'client/src/main.ly'
            unopened = root / 'other/src/main.ly'
            test = root / 'kit/tests/check.ly'
            api = 'pub fn choose(value: Int): Int { value }\n'
            main = 'import kit\nfn main(): Unit { print(kit.choose(1)) }\n'
            case = 'import self.{choose}\nfn main(): Unit { print(choose(7)) }\n'
            dependency = 'dependencies:{kit:{name:"kit",version:"0.1.0",path:"../kit"}}'
            for path, content in [
                (root / 'lyra.json5', '{workspace:{members:["kit","client","other"]}}'),
                (root / 'kit/lyra.json5', '{name:"kit",version:"0.1.0",kind:"lib"}'),
                (root / 'client/lyra.json5', '{name:"client",version:"0.1.0",' + dependency + '}'),
                (root / 'other/lyra.json5', '{name:"other",version:"0.1.0",' + dependency + '}'),
                (library, api), (client, main), (unopened, main), (test, case),
            ]:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content, encoding='utf-8')
            results = run_server([(client, main), (library, api)], [document_request(2, client, 'rename',
                position=position_at(main, main.index('choose')), newName='selectValue')], folders=[root])
            changes = next(item['result']['documentChanges'] for item in results if item.get('id') == 2)
            by_file = {edit['textDocument']['uri']: edit for edit in changes}
            self.assertEqual(set(by_file), {uri(library), uri(client), uri(unopened), uri(test)})
            for path, version, edits in [(library, 1, 1), (client, 1, 1), (unopened, None, 1), (test, None, 2)]:
                document = by_file[uri(path)]
                self.assertEqual(document['textDocument']['version'], version)
                self.assertEqual([edit['newText'] for edit in document['edits']], ['selectValue'] * edits)
            self.assertFalse((root / 'lyra.lock').exists())

    def test_named_argument_rename_follows_the_selected_overload(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / 'src/main.ly'
            source.parent.mkdir()
            text = ('fn choose<T>(value: T, count: Int): Int { count }\n'
                    'fn choose(count: Int): Int { count }\n'
                    'fn main(): Unit { print(choose("😀", count: 3)); print(choose(count: 5)) }\n')
            source.write_text(text, encoding='utf-8')
            selected = position_at(text, text.index('count: 3'))
            output = run_server([(source, text)], [document_request(2, source, 'rename', position=selected, newName='limit')])
            rename = next(item['result'] for item in output if item.get('id') == 2)
            document, = rename['documentChanges']
            positions = [position_at(text, text.index('count: Int')), position_at(text, text.index('{ count }') + 2), selected]
            self.assertEqual([item['range']['start'] for item in document['edits']], positions)
            self.assertEqual([item['newText'] for item in document['edits']], ['limit'] * 3)

    def test_rename_declines_external_and_generated_symbols(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            dependency = root / 'kit/src/library.ly'
            source = root / 'app/src/main.ly'
            macro = root / 'app/src/.macros/emit.ly'
            text = ('import kit\n#emit\nfn shared(): Int { 1 }\n'
                    'fn main(): Unit { print(kit.read()); print(created()); print(shared()) }\n')
            output = 'fn created(): Int { shared() }'
            for path, content in [
                (root / 'app/lyra.json5', '{name:"app",version:"0.1.0",dependencies:{kit:{name:"kit",version:"0.1.0",path:"../kit"}}}'),
                (root / 'kit/lyra.json5', '{name:"kit",version:"0.1.0",kind:"lib"}'),
                (dependency, 'pub fn read(): Int { 7 }\n'),
                (macro, f'fn emit(target: String, raw: String): Syntax {{ syntaxCode({json.dumps(output)}) }}'),
                (source, text),
            ]:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content, encoding='utf-8')
            messages = [document_request(index, source, 'rename', position=position_at(text, text.rindex(name)), newName='replacement')
                        for index, name in enumerate(('read()', 'created()', 'shared()'), 2)]
            results = run_server([(source, text)], messages)
            self.assertEqual([item['result'] for item in results if item.get('id') in (2, 3, 4)], [None, None, None])
            diagnostics = next(item for item in results if item.get('method') == 'textDocument/publishDiagnostics')
            self.assertEqual(diagnostics['params']['diagnostics'], [])

    def test_cross_package_definition_targets_the_selected_overload(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            package = root / 'app'
            dependency = root / 'dependency'
            (package / 'src').mkdir(parents=True)
            (dependency / 'src').mkdir(parents=True)
            (dependency / 'lyra.json5').write_text('{name:"test/dependency",version:"0.1.0",kind:"lib"}', encoding='utf-8')
            dependency_source = dependency / 'src/main.ly'
            dependency_text = 'pub fn select(value: Int): Int { value }\npub fn select(value: String): String { value }\n'
            dependency_source.write_text(dependency_text, encoding='utf-8')
            (package / 'lyra.json5').write_text(
                '{name:"test/app",version:"0.1.0",dependencies:{dependency:{name:"test/dependency",version:"0.1.0",path:"../dependency"}}}',
                encoding='utf-8',
            )
            source = package / 'src/main.ly'
            text = 'import dependency.{select}\nfn main(): Unit { print(select(1)); }\n'
            source.write_text(text, encoding='utf-8')
            line = text.splitlines()[1]
            call = line.index('select(')
            definition = {'jsonrpc': '2.0', 'id': 2, 'method': 'textDocument/definition',
                          'params': {'textDocument': {'uri': uri(source)}, 'position': {'line': 1, 'character': call + 2}}}
            def change(version, text):
                return {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                    'textDocument': {'uri': uri(dependency_source), 'version': version},
                    'contentChanges': [{'text': text}],
                }}
            output = run_server([(source, text), (dependency_source, dependency_text)], [
                definition, change(3, '\n' + dependency_text), {**definition, 'id': 3},
                change(2, dependency_text), {**definition, 'id': 4},
                {'jsonrpc': '2.0', 'method': 'textDocument/didClose', 'params': {
                    'textDocument': {'uri': uri(dependency_source)},
                }}, {**definition, 'id': 5},
            ])
            for request, line_number in [(2, 0), (3, 1), (4, 1), (5, 0)]:
                result = next(item for item in output if item.get('id') == request)
                self.assertNotIn('error', result, result)
                location = result['result'][0]
                self.assertEqual(location['uri'], uri(dependency_source))
                self.assertEqual(location['range']['start'], {'line': line_number, 'character': 7})

    def test_package_snapshots_invalidate_dependents_not_unrelated_members(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'app' / 'src/main.ly'
            library = root / 'library' / 'src/api.ly'
            tool = root / 'tool' / 'src/main.ly'
            initial = 'pub fn select(value: Int): Int { value }\npub fn select(value: String): String { value }\n'
            text = 'import library.{select}\nfn main(): Unit { print(select(1)); }\n'
            tool_text = 'fn main(): Unit { print(0) }\n'
            for path, content in ((source, text), (library, initial), (tool, tool_text)):
                path.parent.mkdir(parents=True)
                path.write_text(content, encoding='utf-8')
            (root / 'lyra.json5').write_text('{workspace:{members:["app","library","tool"]}}', encoding='utf-8')
            (root / 'app/lyra.json5').write_text(
                '{name:"app",version:"0.1.0",dependencies:{library:{name:"library",version:"0.1.0",path:"../library"}}}',
                encoding='utf-8',
            )
            (root / 'library/lyra.json5').write_text('{name:"library",version:"0.1.0",kind:"lib"}', encoding='utf-8')
            (root / 'tool/lyra.json5').write_text('{name:"tool",version:"0.1.0"}', encoding='utf-8')

            def change(path, version, updated):
                return {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                    'textDocument': {'uri': uri(path), 'version': version}, 'contentChanges': [{'text': updated}],
                }}

            private_overload = initial.removeprefix('pub ')
            reordered = initial.splitlines(keepends=True)[1] + initial.splitlines(keepends=True)[0]
            request = {'jsonrpc': '2.0', 'id': 2, 'method': 'textDocument/definition', 'params': {
                'textDocument': {'uri': uri(source)},
                'position': {'line': 1, 'character': text.splitlines()[1].index('select(') + 2},
            }}
            output = run_server([(source, text), (library, initial), (tool, tool_text)], [
                {'jsonrpc': '2.0', 'method': '$/setTrace', 'params': {'value': 'verbose'}},
                change(tool, 2, tool_text.replace('0', '1')),
                change(library, 2, private_overload),
                change(library, 3, reordered),
                request,
                {'jsonrpc': '2.0', 'method': 'textDocument/didClose', 'params': {'textDocument': {'uri': uri(library)}}},
                {**request, 'id': 3},
            ])
            app_diagnostics = [item['params']['diagnostics'] for item in output
                               if item.get('method') == 'textDocument/publishDiagnostics' and item['params']['uri'] == uri(source)]
            self.assertEqual([len(items) for items in app_diagnostics], [0, 0, 0, 0, 1, 0, 0])
            self.assertEqual(app_diagnostics[-3][0]['code'], 'LYT0001')
            analyses = [Path(item['params']['message'].split(' for ', 1)[1]).name for item in output
                        if item.get('method') == '$/logTrace' and 'reran semantic checks' in item['params']['message']]
            self.assertEqual(analyses, ['tool', 'app', 'library', 'app', 'library', 'app'])
            for identifier, line in [(2, 1), (3, 0)]:
                result = next(item for item in output if item.get('id') == identifier)
                self.assertEqual(result['result'][0]['uri'], uri(library))
                self.assertEqual(result['result'][0]['range']['start'], {'line': line, 'character': 7})

    def test_diagnostics_track_unsaved_changes_and_macro_ranges(self):
        with tempfile.TemporaryDirectory() as temporary:
            package = Path(temporary) / 'app'
            source = package / 'src/main.ly'
            macro = package / 'src/.expressions/fail.ly'
            macro.parent.mkdir(parents=True)
            source.write_text('', encoding='utf-8')
            (package / 'lyra.json5').write_text(json.dumps({'name': 'app', 'version': '0.1.0'}), encoding='utf-8')
            macro.write_text('fn fail(target: String, raw: String): Syntax { syntaxArgument(0) }', encoding='utf-8')
            text = 'fn main(): Unit { val value: Int = #fail("bad"); }'
            source.write_text(text, encoding='utf-8')
            output = run_server([(source, text)], [])
            diagnostic = next(item for item in output if item.get('method') == 'textDocument/publishDiagnostics')
            error = diagnostic['params']['diagnostics'][0]
            self.assertEqual(error['code'], 'LYT0001')
            self.assertEqual(error['range']['start']['character'], text.index('"bad"'))

            replacement = 'fn main(): Unit { val value = 42; }'
            change = {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {'textDocument': {'uri': uri(source), 'version': 2}, 'contentChanges': [{'text': replacement}]}}
            output = run_server([(source, text)], [change])
            diagnostics = [item for item in output if item.get('method') == 'textDocument/publishDiagnostics']
            self.assertEqual(diagnostics[-1]['params']['diagnostics'], [])

            macro_text = 'fn fail(target: String, raw: String): Syntax { syntaxCode("1 + false") }'
            macro.write_text(macro_text, encoding='utf-8')
            generated = run_server([(source, text)], [document_request(2, source, 'semanticTokens/full')])
            details = next(item['params']['diagnostics'][0] for item in generated if item.get('method') == 'textDocument/publishDiagnostics')
            self.assertEqual(details['range']['start'], position_at(text, text.index('fail')))
            note, = details['relatedInformation']
            self.assertEqual(note['location'], {'uri': uri(macro), 'range': {
                'start': position_at(macro_text, macro_text.index('fail')), 'end': position_at(macro_text, macro_text.index('fail') + 4)}})
            replies = {item['id']: item['result'] for item in generated if 'id' in item}
            legend = replies[1]['capabilities']['semanticTokensProvider']['legend']['tokenTypes']
            data = iter(replies[2]['data'])
            column = 0
            tokens = {}
            for row, distance, length, kind, _ in zip(data, data, data, data, data, strict=True):
                self.assertEqual(row, 0)
                column += distance
                tokens[column] = (legend[kind], length)
            self.assertEqual(tokens[text.index('fail')], ('macro', len('fail')))

    def test_watched_disk_edits_refresh_closed_dependency_and_macro_input(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'app/src/main.ly'
            library = root / 'library/src/main.ly'
            macro = root / 'app/src/.macros/declare.ly'
            macro_input = root / 'app/inputs/value.txt'
            source_text = ('import library.{setting}\n#declare\nfn main(): Unit {\n'
                           '    val current: Int = setting(); print(current + configured());\n}\n')
            library_text = 'pub fn setting(): Int { 1 }\n'
            for path, content in (
                (source, source_text), (library, library_text), (macro_input, '2'),
                (macro, 'fn declare(target: String, raw: String): Syntax { '
                        'syntaxCode("fn configured(): Int { " + macroInput("value") + " }") }'),
                (root / 'app/lyra.json5', '{name:"app",version:"0.1.0",dependencies:{library:{name:"library",'
                                        'version:"0.1.0",path:"../library"}},macroInputs:{declare:{value:"inputs/value.txt"}}}'),
                (root / 'library/lyra.json5', '{name:"library",version:"0.1.0",kind:"lib"}'),
            ):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content, encoding='utf-8')

            with interactive_server(root) as (send, receive):
                send({'id': 1, 'method': 'initialize', 'params': {'capabilities': {'workspace': {
                    'didChangeWatchedFiles': {'dynamicRegistration': True, 'relativePatternSupport': True},
                }}}})
                self.assertEqual(receive()['id'], 1)
                send({'method': 'initialized'})
                send({'method': 'textDocument/didOpen', 'params': {'textDocument': {
                    'uri': uri(source), 'languageId': 'lyra', 'version': 1, 'text': source_text,
                }}})
                registration = receive()
                self.assertEqual(registration['method'], 'client/registerCapability')
                watches = [watch['globPattern'] for item in registration['params']['registrations']
                           for watch in item['registerOptions']['watchers']]
                self.assertIn({'baseUri': uri(root / 'library'), 'pattern': 'src/**/*.ly'}, watches)
                self.assertIn({'baseUri': uri(macro_input.parent), 'pattern': 'value.txt'}, watches)
                send({'id': registration['id'], 'result': None})

                def diagnostics():
                    while True:
                        notification = receive()
                        if notification.get('method') == 'textDocument/publishDiagnostics':
                            self.assertEqual(notification['params']['uri'], uri(source))
                            return [item['code'] for item in notification['params']['diagnostics']]
                        self.assertNotIn('error', notification)

                def changed(path, kind):
                    send({'method': 'workspace/didChangeWatchedFiles', 'params': {
                        'changes': [{'uri': uri(path), 'type': kind}],
                    }})

                self.assertEqual(diagnostics(), [])
                library.write_text('pub fn setting(): String { "wrong" }', encoding='utf-8')
                changed(library, 2)
                self.assertEqual(diagnostics(), ['LYT0001'])
                library.write_text(library_text, encoding='utf-8')
                changed(library, 2)
                self.assertEqual(diagnostics(), [])
                macro_input.unlink()
                changed(macro_input, 3)
                self.assertEqual(diagnostics(), ['LYM0002'])
                macro_input.write_text('3', encoding='utf-8')
                changed(macro_input, 1)
                self.assertEqual(diagnostics(), [])

    def test_incremental_parse_cache_rebases_field_default_helpers(self):
        with tempfile.TemporaryDirectory() as temporary:
            package = Path(temporary) / 'app'
            (package / 'src/model').mkdir(parents=True)
            (package / 'lyra.json5').write_text('{"name":"app","version":"0.1.0"}', encoding='utf-8')
            model = package / 'src/model/types.ly'
            model_text = (
                'pub struct Pair(pub val first: String, pub val second: String = first + "!")\n'
                'pub extension PairOps for Pair {\n'
                '    val combined: String get() { this.second }\n'
                '}\n'
            )
            model.write_text(model_text, encoding='utf-8')
            source = package / 'src/main.ly'
            text = 'import self.model.{Pair}\nimport self.model.{PairOps}\nfn main(): Unit { print(Pair("Lyra").combined) }\n'
            source.write_text(text, encoding='utf-8')
            prefix = package / 'src/a.ly'
            prefix_text = '// prefix\n'
            prefix.write_text(prefix_text, encoding='utf-8')
            update = {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                'textDocument': {'uri': uri(prefix), 'version': 2},
                'contentChanges': [{'text': 'fn addedBeforeModel(): Unit {}\n'}],
            }}
            output = run_server([(source, text), (model, model_text), (prefix, prefix_text)], [update])
            notifications = [item for item in output if item.get('method') == 'textDocument/publishDiagnostics'
                             and item['params']['uri'] == uri(source)]
            self.assertGreaterEqual(len(notifications), 2)
            self.assertEqual(notifications[-1]['params']['diagnostics'], [])

    def test_unsaved_macro_input_invalidates_analysis(self):
        with tempfile.TemporaryDirectory() as temporary:
            package = Path(temporary) / 'app'
            source = package / 'src/main.ly'
            macro = package / 'src/.macros/declare.ly'
            macro.parent.mkdir(parents=True)
            source.parent.mkdir(parents=True, exist_ok=True)
            source_text = '#declare\nfn main(): Unit { print(title()) }\n'
            source.write_text(source_text, encoding='utf-8')
            (package / 'lyra.json5').write_text(json.dumps({
                'name': 'app', 'version': '0.1.0', 'macroInputs': {'declare': {'title': 'inputs/title.txt'}},
            }), encoding='utf-8')
            macro.write_text(
                'fn declare(target: String, argument: String): Syntax { syntaxCode("fn title(): String { " + macroInput("title") + " }") }',
                encoding='utf-8',
            )
            macro_input = package / 'inputs/title.txt'
            macro_input.parent.mkdir(parents=True)
            initial = '"initial"'
            macro_input.write_text(initial, encoding='utf-8')
            change = {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                'textDocument': {'uri': uri(macro_input), 'version': 2},
                'contentChanges': [{'text': '1'}],
            }}
            output = run_server([(source, source_text), (macro_input, initial)], [change,
                {'jsonrpc': '2.0', 'method': 'textDocument/didClose', 'params': {'textDocument': {'uri': uri(macro_input)}}},
            ])
            notifications = [item for item in output if item.get('method') == 'textDocument/publishDiagnostics'
                             and item['params']['uri'] == uri(source)]
            self.assertEqual(notifications[-2]['params']['diagnostics'][0]['code'], 'LYT0001')
            self.assertEqual(notifications[-1]['params']['diagnostics'], [])

    def test_navigation_in_a_file_with_an_expression_expansion_uses_original_utf16(self):
        with tempfile.TemporaryDirectory() as temporary:
            package = Path(temporary) / 'app'
            source = package / 'src/main.ly'
            macro = package / 'src/.expressions/marker.ly'
            macro.parent.mkdir(parents=True)
            (package / 'lyra.json5').write_text('{name:"app",version:"0.1.0"}', encoding='utf-8')
            macro.write_text('fn marker(target: String, raw: String): Syntax { syntaxCode("{ val macroOnly = 0; macroOnly }") }', encoding='utf-8')
            text = 'fn target(value: Int): Int { value }\nfn main(): Unit { val prefix = #marker(); print("你好 😀"); val result = target(42) }\n'
            source.write_text(text, encoding='utf-8')
            line = text.splitlines()[1]
            character = len(line[:line.index('target(')].encode('utf-16-le')) // 2
            params = {'textDocument': {'uri': uri(source)}, 'position': {'line': 1, 'character': character + 1}}
            output = run_server([(source, text)], [
                {'jsonrpc': '2.0', 'id': 2, 'method': 'textDocument/definition', 'params': params},
                {'jsonrpc': '2.0', 'id': 3, 'method': 'textDocument/references', 'params': {**params, 'context': {'includeDeclaration': True}}},
                document_request(4, source, 'inlayHint', range={'start': position_at(text, 0), 'end': position_at(text, len(text))}),
            ])
            definition = next(item for item in output if item.get('id') == 2)['result'][0]
            self.assertEqual(definition['range']['start'], {'line': 0, 'character': 3})
            references = next(item for item in output if item.get('id') == 3)['result']
            self.assertEqual(len(references), 2)
            self.assertEqual(references[1]['range']['start'], {'line': 1, 'character': character})
            hints = next(item['result'] for item in output if item.get('id') == 4)
            self.assertEqual(hints, [{'position': position_at(text, text.index(anchor) + len(anchor)), 'label': ': Int', 'kind': 1}
                                     for anchor in ('val prefix', 'val result')])

    def test_editing_a_nested_imported_macro_refreshes_the_callers_diagnostics(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'app/src/main.ly'
            macro = root / 'kit/src/tools/.expressions/value.ly'
            source.parent.mkdir(parents=True)
            macro.parent.mkdir(parents=True)
            (root / 'app/lyra.json5').write_text('{name:"app",version:"0.1.0",dependencies:{kit:{name:"kit",version:"^0.1.0",path:"../kit"}}}', encoding='utf-8')
            (root / 'kit/lyra.json5').write_text('{name:"kit",version:"0.1.0",kind:"lib"}', encoding='utf-8')
            text = 'import macro kit.tools.value\nfn main(): Unit { val result: Int = #value() }\n'
            implementation = 'pub fn value(target: String, argument: String): Syntax { quote { 1 } }'
            source.write_text(text, encoding='utf-8')
            macro.write_text(implementation, encoding='utf-8')
            change = {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                'textDocument': {'uri': uri(macro), 'version': 2},
                'contentChanges': [{'text': implementation.replace('quote { 1 }', 'quote { false }')}],
            }}
            output = run_server([(source, text), (macro, implementation)], [change,
                {'jsonrpc': '2.0', 'method': 'textDocument/didClose', 'params': {'textDocument': {'uri': uri(macro)}}},
            ])
            diagnostics = [item['params']['diagnostics'] for item in output if item.get('method') == 'textDocument/publishDiagnostics'
                           and item['params']['uri'] == uri(source)]
            error, = diagnostics[-2]
            self.assertEqual(error['code'], 'LYT0001')
            self.assertEqual(diagnostics[-1], [])

    def test_unsaved_standard_library_overlay_is_used(self):
        with tempfile.TemporaryDirectory() as temporary:
            package = Path(temporary) / 'app'
            source = package / 'src/main.ly'
            source.parent.mkdir(parents=True)
            (package / 'lyra.json5').write_text('{"name":"app","version":"0.1.0"}', encoding='utf-8')
            text = 'import std.io.{readLine}\nfn main(): Unit { val _ = readLine() }\n'
            source.write_text(text, encoding='utf-8')
            standard_source = COMPILER.parent.parent / 'share' / 'lyra' / 'std' / 'io' / 'src' / 'io.ly'
            standard_text = standard_source.read_text(encoding='utf-8')
            replacement = standard_text.replace('pub fn readLine(', 'pub fn renamedReadLine(', 1)
            self.assertNotEqual(replacement, standard_text)
            update = {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                'textDocument': {'uri': uri(source), 'version': 2},
                'contentChanges': [{'text': text}],
            }}
            output = run_server([(source, text), (standard_source, replacement)], [update])
            notifications = [item for item in output if item.get('method') == 'textDocument/publishDiagnostics'
                             and item['params']['uri'] == uri(source)]
            self.assertTrue(notifications)
            self.assertEqual(notifications[-1]['params']['diagnostics'][0]['code'], 'LYN0003')

    def test_trivia_only_edit_reuses_semantics_but_code_edit_rechecks(self):
        with tempfile.TemporaryDirectory() as temporary:
            package = Path(temporary) / 'app'
            source = package / 'src/main.ly'
            source.parent.mkdir(parents=True)
            (package / 'lyra.json5').write_text('{"name":"app","version":"0.1.0"}', encoding='utf-8')
            initial = 'fn main(): Unit { print(1) } // aa\n'
            source.write_text(initial, encoding='utf-8')

            def change(version, text):
                return {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                    'textDocument': {'uri': uri(source), 'version': version},
                    'contentChanges': [{'text': text}],
                }}

            trivia_edit = initial.replace('// aa', '// bb')
            invalid_edit = 'fn main(): Unit { val value: Int = "wrong"; } // bb\n'
            output = run_server([(source, initial)], [
                {'jsonrpc': '2.0', 'method': '$/setTrace', 'params': {'value': 'verbose'}},
                change(2, trivia_edit),
                change(3, invalid_edit),
            ])
            traces = [item['params']['message'] for item in output if item.get('method') == '$/logTrace']
            self.assertTrue(any('reused checked workspace' in message for message in traces), traces)
            self.assertGreaterEqual(sum('reran semantic checks' in message for message in traces), 1, traces)
            diagnostics = [item['params']['diagnostics'] for item in output
                           if item.get('method') == 'textDocument/publishDiagnostics' and item['params']['uri'] == uri(source)]
            self.assertEqual(diagnostics[-1][0]['code'], 'LYT0001')
            self.assertIn('String', diagnostics[-1][0]['message'])
            self.assertEqual(diagnostics[-1][0]['range']['start']['character'], invalid_edit.index('"wrong"'))


if __name__ == '__main__':
    unittest.main()
