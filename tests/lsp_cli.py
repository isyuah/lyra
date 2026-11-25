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


def responses(output):
    result = []
    offset = 0
    while offset < len(output):
        separator = output.index(b'\r\n\r\n', offset)
        headers = output[offset:separator].decode('ascii').split('\r\n')
        length = next(int(line.split(':', 1)[1]) for line in headers if line.lower().startswith('content-length:'))
        offset = separator + 4
        result.append(json.loads(output[offset:offset + length]))
        offset += length
    return result


def run_server(documents, messages):
    environment = {**os.environ, 'LYRA_CACHE': str(documents[0][0].parents[1] / 'cache')}
    transcript = [
        {'jsonrpc': '2.0', 'id': 1, 'method': 'initialize', 'params': {'capabilities': {'general': {'positionEncodings': ['utf-16']}}}},
        {'jsonrpc': '2.0', 'method': 'initialized', 'params': {}},
    ]
    for path, text in documents:
        transcript.append({'jsonrpc': '2.0', 'method': 'textDocument/didOpen', 'params': {'textDocument': {'uri': uri(path), 'languageId': 'lyra', 'version': 1, 'text': text}}})
    transcript.extend(messages)
    transcript.extend([
        {'jsonrpc': '2.0', 'id': 90, 'method': 'shutdown', 'params': None},
        {'jsonrpc': '2.0', 'method': 'exit', 'params': None},
    ])
    process = subprocess.run([COMPILER, 'lsp', '--stdio'], input=b''.join(map(message, transcript)), capture_output=True, timeout=60, env=environment)
    if process.returncode:
        raise AssertionError(process.stderr.decode('utf-8', errors='replace'))
    return responses(process.stdout)


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
    def test_incomplete_source_completes_keywords_imports_and_only_visible_names(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'src/main.ly'
            source.parent.mkdir()
            (root / 'lyra.json5').write_text('{name:"app",version:"0.1.0"}', encoding='utf-8')
            (source.parent / 'sibling.ly').write_text('fn rowOutsideFile(): Int { 1 }\n', encoding='utf-8')
            text = ('import std.http.Router as WebRouter\n'
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
            self.assertEqual({item['label'] for item in results[3]['items']}, {'route', 'router'})
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
            self.assertEqual(len(results[11]['changes'][uri(source)]), 2)
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
            self.assertEqual(hover['contents']['value'], '```lyra\nfn read(): Unit\n    throws Closed, Exception\n```')

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
            self.assertEqual(results[4]['result']['contents']['value'], '```lyra\nfn greet(name: String): String { "你好 😀 " + name }\n```')
            self.assertTrue(any(item['label'] == 'greet' for item in results[5]['result']['items']))
            self.assertIn('newText', results[6]['result'][0])
            rename = results[7]['result']['changes'][uri(source)]
            self.assertEqual(len(rename), 2)
            self.assertTrue(all(item['newText'] == 'welcome' for item in rename))

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
            text = 'import dependency.select\nfn main(): Unit { print(select(1)); }\n'
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
            text = 'import library.select\nfn main(): Unit { print(select(1)); }\n'
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

    def test_watched_disk_edits_refresh_closed_dependency_and_macro_input(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'app/src/main.ly'
            library = root / 'library/src/main.ly'
            macro = root / 'app/src/.macros/declare.ly'
            macro_input = root / 'app/inputs/value.txt'
            source_text = ('import library.setting\n#declare\nfn main(): Unit {\n'
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
            text = 'import self.model.Pair\nimport self.model.PairOps\nfn main(): Unit { print(Pair("Lyra").combined) }\n'
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
            text = 'import std.io.readLine\nfn main(): Unit { val _ = readLine() }\n'
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
