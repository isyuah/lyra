const { DebugAdapterExecutable, debug, workspace, window } = require('vscode');
const { existsSync } = require('node:fs');
const { join } = require('node:path');
const { LanguageClient } = require('vscode-languageclient/node');
const { registerTesting } = require('./testing.cjs');

let client;

function registerDebugging(context) {
    context.subscriptions.push(debug.registerDebugConfigurationProvider('lyra', {
        resolveDebugConfiguration(folder, configuration) {
            let program = configuration.program;
            if (!program && folder) {
                const executable = process.platform === 'win32' ? 'app.exe' : 'app';
                program = join(folder.uri.fsPath, 'build', 'debug', executable);
            }
            if (!program) {
                void window.showErrorMessage('Set a Lyra executable in the launch configuration.');
                return undefined;
            }
            if (process.platform === 'win32' && !program.includes('${') && !existsSync(program) && existsSync(program + '.exe'))
                configuration.program = program + '.exe';
            else configuration.program = program;
            if (process.platform === 'win32' && !configuration.program.includes('${')) {
                const symbols = configuration.program.replace(/\.[^./\\]+$/, '.pdb');
                if (existsSync(symbols)) {
                    const command = `target symbols add "${symbols.replaceAll('"', '\\"')}"`;
                    configuration.preRunCommands = [...(Array.isArray(configuration.preRunCommands) ? configuration.preRunCommands : []), command];
                }
            }
            configuration.cwd ??= folder?.uri.fsPath;
            return configuration;
        },
    }));
    context.subscriptions.push(debug.registerDebugAdapterDescriptorFactory('lyra', {
        createDebugAdapterDescriptor(session) {
            const configured = workspace.getConfiguration('lyra').get('debugAdapterPath');
            const command = configured || process.env.LYRA_LLDB_DAP || 'lldb-dap';
            return new DebugAdapterExecutable(command, [], {
                cwd: session.workspaceFolder?.uri.fsPath,
                windowsHide: true,
            });
        },
    }));
}

async function activate(context) {
    const command = workspace.getConfiguration('lyra').get('serverPath');
    registerTesting(context, command);
    registerDebugging(context);
    client = new LanguageClient('lyra', 'Lyra Language Server', {
        command,
        args: ['lsp', '--stdio'],
        options: { cwd: __dirname, shell: false, windowsHide: true },
    }, {
        documentSelector: [{ scheme: 'file', language: 'lyra' }, { scheme: 'file', pattern: '**/lyra.json5' }],
    });
    await client.start();
}

async function deactivate() {
    await client?.dispose();
}

module.exports = { activate, deactivate };
