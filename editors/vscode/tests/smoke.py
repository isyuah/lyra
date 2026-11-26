import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile


TESTS = Path(__file__).resolve().parent
EXTENSION = TESTS.parent
EXAMPLES = EXTENSION.parents[1] / 'examples'


def main():
    parser = argparse.ArgumentParser(description='Run the Lyra smoke in an isolated VS Code extension host.')
    parser.add_argument('vscode', type=Path, help='path to the native desktop executable (Code.exe or code)')
    parser.add_argument('lyra', type=Path, help='path to a built or installed Lyra executable')
    parser.add_argument('--vsix', type=Path, help='verify the packaged extension instead of the source tree')
    options = parser.parse_args()
    vscode, lyra = options.vscode.resolve(), options.lyra.resolve()
    if not vscode.is_file() or not lyra.is_file():
        parser.error('expected paths to the VS Code and Lyra executables')
    with tempfile.TemporaryDirectory(prefix='lyra-vscode-smoke-') as temporary:
        root = Path(temporary)
        extension = EXTENSION
        if options.vsix is not None:
            with zipfile.ZipFile(options.vsix) as archive:
                files = [name for name in archive.namelist() if name.startswith('extension/')]
                archive.extractall(root / 'vsix', files)
            extension = root / 'vsix' / 'extension'
        if not (extension / 'node_modules' / 'vscode-languageclient' / 'package.json').is_file():
            parser.error('VSIX is missing vscode-languageclient' if options.vsix else 'run npm ci in editors/vscode first')
        project = root / 'JSONL workspace'
        http = root / 'HTTP workspace'
        debug = root / 'Debug workspace'
        ignores = shutil.ignore_patterns('build', '.lyra', '.lyra-cache')
        shutil.copytree(EXAMPLES / 'jsonlines', project, ignore=ignores)
        shutil.copytree(EXAMPLES / 'http-sqlite', http, ignore=ignores)
        (debug / 'src').mkdir(parents=True)
        (debug / 'lyra.json5').write_text('{name:"debug/smoke",version:"0.1.0"}', encoding='utf-8')
        debug_source = debug / 'src' / 'main.ly'
        debug_source.write_text(
            'async fn calculate(base: Int): Int {\n'
            '    val beforeWait = base + 1\n'
            '    await sleepMs(30)\n'
            '    val afterWait = beforeWait + 1\n'
            '    afterWait\n'
            '}\n'
            'async fn main(): Unit { print(await calculate(40)) }\n',
            encoding='utf-8',
        )
        built = subprocess.run([lyra, 'build', debug, '--debug-info'], capture_output=True, text=True, timeout=45)
        if built.returncode:
            parser.exit(1, f'Could not build the debugger smoke program:\n{built.stderr}\n')
        debug_app = debug / 'build' / 'debug' / ('app.exe' if os.name == 'nt' else 'app')
        macro = project / 'stats' / 'src' / '.expressions' / 'identitySyntax.ly'
        macro.parent.mkdir()
        macro.write_text('fn identitySyntax(target: String, raw: String): Syntax { syntaxArgument(0) }\n', encoding='utf-8')

        profile = root / 'profile'
        (profile / 'User').mkdir(parents=True)
        (profile / 'User' / 'settings.json').write_text(json.dumps({
            'lyra.serverPath': str(lyra),
            'lyra.debugAdapterPath': shutil.which('lldb-dap') or 'lldb-dap',
            'chat.disableAIFeatures': True,
            'files.autoSave': 'off',
            'editor.wordBasedSuggestions': 'off',
            'update.mode': 'none',
            'extensions.autoCheckUpdates': False,
            'extensions.autoUpdate': False,
            'telemetry.telemetryLevel': 'off',
            'workbench.startupEditor': 'none',
        }), encoding='utf-8')
        report, log = root / 'result.txt', root / 'vscode.log'
        environment = {**os.environ, 'LYRA_CACHE': str(root / 'cache'), 'LYRA_SMOKE_RESULT': str(report),
                       'LYRA_SMOKE_HTTP': str(http / 'src' / 'main.ly'),
                       'LYRA_SMOKE_DEBUG_APP': str(debug_app), 'LYRA_SMOKE_DEBUG_SOURCE': str(debug_source)}
        for key in ('ELECTRON_RUN_AS_NODE', 'VSCODE_IPC_HOOK_CLI', 'VSCODE_PORTABLE'):
            environment.pop(key, None)
        arguments = [
            vscode, project, '--extensionDevelopmentPath', extension, '--extensionTestsPath', TESTS / 'smoke.cjs',
            '--user-data-dir', profile, '--extensions-dir', root / 'extensions', '--disable-extensions',
            '--disable-workspace-trust', '--disable-gpu', '--skip-welcome', '--skip-release-notes',
        ]
        failure = None
        with log.open('wb') as output:
            try:
                process = subprocess.run(arguments, env=environment, stdout=output, stderr=subprocess.STDOUT, timeout=90)
                if process.returncode:
                    failure = f'VS Code exited with status {process.returncode}'
            except (OSError, subprocess.TimeoutExpired) as error:
                failure = str(error)
        result = report.read_text(encoding='utf-8') if report.is_file() else ''
        channels = '\n'.join(path.read_text(encoding='utf-8', errors='replace') for path in profile.rglob('*Lyra Language Server.log'))
        if any(f'Request textDocument/{method} failed.' in channels for method in ('definition', 'hover', 'completion', 'inlayHint', 'signatureHelp')):
            failure = 'An editing request reported an error instead of using diagnostics'
        if failure or not result.startswith('PASS '):
            output = '\n'.join(log.read_text(encoding='utf-8', errors='replace').splitlines()[-50:])
            parser.exit(1, f'{failure or "VS Code test did not report success"}\n{result}\n{channels}\n{output}\n')
        print(result.rstrip())


if __name__ == '__main__':
    main()
