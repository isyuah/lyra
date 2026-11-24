const assert = require('node:assert/strict');
const { writeFile } = require('node:fs/promises');
const vscode = require('vscode');

function waitForDiagnostics(uri, predicate, description) {
    return new Promise((resolve, reject) => {
        const subscription = vscode.languages.onDidChangeDiagnostics(event => {
            if (event.uris.some(changed => changed.toString() === uri.toString())) check();
        });
        const timer = setTimeout(() => finish(() => reject(new Error(
            `${description}: ${JSON.stringify(vscode.languages.getDiagnostics(uri))}`,
        ))), 15000);
        const finish = callback => {
            clearTimeout(timer);
            subscription.dispose();
            callback();
        };
        const check = () => {
            const diagnostics = vscode.languages.getDiagnostics(uri);
            if (predicate(diagnostics)) finish(() => resolve(diagnostics));
        };
        check();
    });
}

async function checkSmoke() {
    const [project] = vscode.workspace.workspaceFolders;
    const source = vscode.Uri.joinPath(project.uri, 'stats', 'src', 'main.ly');
    const library = vscode.Uri.joinPath(project.uri, 'metrics', 'src', 'metrics.ly');
    const document = await vscode.workspace.openTextDocument(source);
    assert.equal(document.languageId, 'lyra');
    const extension = vscode.extensions.getExtension('lyra-lang.lyra-language');
    assert.ok(extension, 'Lyra extension was not loaded');
    await extension.activate();
    const editor = await vscode.window.showTextDocument(document);
    const original = await vscode.workspace.fs.readFile(source);

    const call = document.getText().indexOf('parseLine(line.value)');
    assert.notEqual(call, -1);
    const callPosition = document.positionAt(call + 2);
    const definitions = await vscode.commands.executeCommand('vscode.executeDefinitionProvider', source, callPosition);
    assert.ok(definitions.length > 0, 'definition provider did not index parseLine');
    assert.ok(!vscode.workspace.textDocuments.some(open => open.uri.toString() === library.toString()));
    const originalLibrary = await vscode.workspace.fs.readFile(library);
    const libraryText = new TextDecoder('utf-8', { fatal: true }).decode(originalLibrary);
    const changedLibrary = libraryText.replace(
        'pub fn parseLine(line: String): Result<Reading,String> { decodeReading(line) }',
        'pub fn parseLine(line: Bool): Result<Reading,String> { decodeReading("{}") }',
    );
    assert.notEqual(changedLibrary, libraryText);
    await writeFile(library.fsPath, changedLibrary, 'utf-8');
    try {
        await waitForDiagnostics(source, listing => listing.some(item =>
            item.severity === vscode.DiagnosticSeverity.Error && item.range.start.line === callPosition.line,
        ), 'dependency change did not diagnose parseLine in the open caller');
    } finally {
        await writeFile(library.fsPath, originalLibrary);
    }
    await waitForDiagnostics(source, listing => listing.length === 0, 'dependency diagnostic did not clear after restoring the file');

    editor.selection = new vscode.Selection(callPosition, callPosition);
    await vscode.commands.executeCommand('editor.action.revealDefinition');
    const target = vscode.window.activeTextEditor;
    assert.equal(target.document.uri.toString(), library.toString());
    const declaration = 'pub fn parseLine(';
    const declarationOffset = target.document.getText().indexOf(declaration);
    assert.notEqual(declarationOffset, -1);
    const nameStart = target.document.positionAt(declarationOffset + 'pub fn '.length);
    assert.ok(target.selection.start.isEqual(nameStart), 'definition jump did not land on parseLine');

    const sourceEditor = await vscode.window.showTextDocument(document);
    const probe = 'fn diagnosticProbe(): Unit { print("中文🙂"); val value: Int = #identitySyntax("wrong"); }';
    const inserted = document.positionAt(document.getText().length);
    const endOfLine = document.eol === vscode.EndOfLine.CRLF ? '\r\n' : '\n';
    assert.ok(await sourceEditor.edit(edit => edit.insert(inserted, endOfLine + probe + endOfLine)));
    try {
        const diagnostics = await waitForDiagnostics(source, listing => listing.some(item => item.code === 'LYT0001'), 'missing unsaved macro diagnostic');
        const diagnostic = diagnostics.find(item => item.code === 'LYT0001');
        const expectedColumn = probe.indexOf('"wrong"');
        assert.equal(diagnostic.range.start.line, inserted.line + 1);
        assert.equal(diagnostic.range.start.character, expectedColumn);
        assert.equal(diagnostic.range.end.character, expectedColumn + '"wrong"'.length);
        assert.equal(document.getText(diagnostic.range), '"wrong"');
        assert.ok(document.isDirty);
        assert.deepEqual(await vscode.workspace.fs.readFile(source), original);
    } finally {
        await vscode.commands.executeCommand('workbench.action.files.revert');
    }
    await waitForDiagnostics(source, listing => listing.length === 0, 'diagnostic did not clear after reverting');
    assert.ok(!document.isDirty);
}

async function run() {
    const report = process.env.LYRA_SMOKE_RESULT;
    try {
        await checkSmoke();
        await writeFile(report, `PASS VS Code ${vscode.version}: cross-package definition and disk updates, unsaved Unicode macro diagnostic, diagnostics cleared\n`);
    } catch (error) {
        await writeFile(report, `${error.stack}\n`);
        throw error;
    }
}

module.exports = { run };
