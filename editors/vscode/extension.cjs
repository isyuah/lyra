const { workspace } = require('vscode');
const { LanguageClient } = require('vscode-languageclient/node');

let client;

async function activate() {
    const command = workspace.getConfiguration('lyra').get('serverPath');
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
