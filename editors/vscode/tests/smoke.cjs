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

    const callPrefix = 'jsonlReader(input, ';
    const call = document.getText().indexOf(callPrefix + 'parseLine)');
    assert.notEqual(call, -1);
    const callPosition = document.positionAt(call + callPrefix.length + 2);
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

    const addImport = 'fn autoImportProbe(): Unit { val _ = Clo.now() }';
    const end = document.positionAt(document.getText().length);
    assert.ok(await sourceEditor.edit(edit => edit.insert(end, endOfLine + addImport + endOfLine)));
    try {
        const position = end.translate(1, addImport.indexOf('Clo') + 'Clo'.length);
        await waitForDiagnostics(source, listing => listing.some(item => item.code === 'LYN0001'), 'missing unresolved auto-import probe');
        const completions = await vscode.commands.executeCommand('vscode.executeCompletionItemProvider', source, position);
        const clock = completions.items.find(item => (item.label.label ?? item.label) === 'Clock');
        assert.equal(clock.kind, vscode.CompletionItemKind.Class);
        assert.equal(clock.additionalTextEdits.length, 1);
        assert.equal(clock.additionalTextEdits[0].newText, `import std.time.Clock${endOfLine}`);
        assert.ok(!document.getText().includes('import std.time.Clock'));
        const edit = new vscode.WorkspaceEdit();
        edit.set(source, [...clock.additionalTextEdits, vscode.TextEdit.replace(clock.range.replacing ?? clock.range, clock.insertText)]);
        assert.ok(await vscode.workspace.applyEdit(edit));
        assert.ok(document.getText().includes('Clock.now()'));
        await waitForDiagnostics(source, listing => !listing.some(item => item.severity === vscode.DiagnosticSeverity.Error),
            'auto import did not make Clock.now resolve');
    } finally {
        await vscode.commands.executeCommand('workbench.action.files.revert');
    }
}

async function checkTyping() {
    const source = vscode.Uri.file(process.env.LYRA_SMOKE_HTTP);
    const document = await vscode.workspace.openTextDocument(source);
    const editor = await vscode.window.showTextDocument(document);
    const original = document.getText();
    const serveOffset = original.indexOf('await router.serve(listener)');
    assert.notEqual(serveOffset, -1);
    const inserted = document.lineAt(document.positionAt(serveOffset)).rangeIncludingLineBreak.end;
    const use = document.positionAt(serveOffset + 'await ro'.length);
    const logger = document.positionAt(original.indexOf('val logger') + 'val logger'.length);
    const router = document.positionAt(original.indexOf('val router') + 'val router'.length);
    const available = new vscode.Range(new vscode.Position(0, 0), inserted);
    const inlays = () => vscode.commands.executeCommand('vscode.executeInlayHintProvider', source, available);
    const expectTypes = hints => {
        assert.ok(hints.some(hint => hint.position.isEqual(logger) && hint.label === ': Logger'));
        assert.ok(hints.some(hint => hint.position.isEqual(router) && hint.label === ': Router'));
    };
    const complete = position => vscode.commands.executeCommand('vscode.executeCompletionItemProvider', source, position);
    const endOfLine = document.eol === vscode.EndOfLine.CRLF ? '\r\n' : '\n';
    expectTypes(await inlays());
    const start = inserted.translate(0, 4);
    assert.ok(await editor.edit(edit => edit.insert(inserted, '    ' + endOfLine)));
    try {
        editor.selection = new vscode.Selection(start, start);
        await vscode.commands.executeCommand('default:type', { text: 'v' });
        await waitForDiagnostics(source, listing => listing.some(item => item.code === 'LYN0001'), 'missing unfinished-name diagnostic');
        const keywords = (await complete(start.translate(0, 1))).items.map(item => item.label);
        assert.ok(keywords.includes('val') && keywords.includes('var'), `keyword completions: ${JSON.stringify(keywords)}`);
        const generated = document.getText().indexOf('createUserRoute(router)');
        assert.notEqual(generated, -1);
        const routeItems = (await complete(document.positionAt(generated + 'createUserRo'.length))).items;
        assert.ok(routeItems.some(item => item.label === 'createUserRoute' && item.kind === vscode.CompletionItemKind.Function),
            `route completions: ${JSON.stringify(routeItems.map(item => [item.label, item.kind]))}`);
        expectTypes(await inlays());
        const known = await vscode.commands.executeCommand('vscode.executeDefinitionProvider', source, use);
        assert.ok(known.some(item => item.range.end.isEqual(router)));
        for (const method of ['Definition', 'Hover', 'Definition']) {
            const result = await vscode.commands.executeCommand(`vscode.execute${method}Provider`, source, start);
            assert.equal(result?.length ?? 0, 0);
        }
        assert.ok(await editor.edit(edit => edit.replace(new vscode.Range(start, start.translate(0, 1)), 'ro')));
        const names = (await complete(start.translate(0, 2))).items.map(item => item.label);
        assert.ok(names.includes('router'), `visible name completions: ${JSON.stringify(names)}`);
        assert.ok(await editor.edit(edit => edit.replace(new vscode.Range(start, start.translate(0, 2)), 'router.')));
        const members = (await complete(start.translate(0, 'router.'.length))).items;
        assert.ok(members.some(item => item.label === 'serve' && item.kind === vscode.CompletionItemKind.Method));
        const call = 'val _ = listen(8080, ho';
        assert.ok(await editor.edit(edit => edit.replace(new vscode.Range(start, start.translate(0, 'router.'.length)), call)));
        const signature = await vscode.commands.executeCommand('vscode.executeSignatureHelpProvider', source, start.translate(0, call.length));
        assert.ok(signature.signatures.some(item => item.parameters.some(parameter => parameter.label.includes('host:'))));
        assert.ok((await complete(start.translate(0, call.length))).items.some(item => item.label === 'host:'));
        assert.ok(document.isDirty);
        assert.ok(document.getText().includes('val router = Router()'));
    } finally {
        await vscode.commands.executeCommand('workbench.action.files.revert');
    }
    await waitForDiagnostics(source, listing => listing.length === 0, 'unfinished-name diagnostic did not clear after reverting');
    assert.equal(document.getText(), original);
}

async function run() {
    const report = process.env.LYRA_SMOKE_RESULT;
    try {
        await checkSmoke();
        await checkTyping();
        await writeFile(report, `PASS VS Code ${vscode.version}: cross-package definitions, auto import, Unicode macro diagnostics, member completion, signatures and inlay hints\n`);
    } catch (error) {
        await writeFile(report, `${error.stack}\n`);
        throw error;
    }
}

module.exports = { run };
