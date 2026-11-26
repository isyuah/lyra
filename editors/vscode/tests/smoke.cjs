const assert = require('node:assert/strict');
const { writeFile } = require('node:fs/promises');
const { basename, dirname, resolve } = require('node:path');
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
    const discoveredTests = await vscode.commands.executeCommand('lyra.tests.refresh');
    assert.ok(discoveredTests > 0, 'Test Explorer did not discover package tests');
    const editor = await vscode.window.showTextDocument(document);
    const original = await vscode.workspace.fs.readFile(source);

    const callPrefix = 'jsonlReader(input, ';
    const call = document.getText().indexOf(callPrefix + 'parseLine)');
    assert.notEqual(call, -1);
    const callPosition = document.positionAt(call + callPrefix.length + 2);
    const definitions = await vscode.commands.executeCommand('vscode.executeDefinitionProvider', source, callPosition);
    assert.ok(definitions.length > 0, 'definition provider did not index parseLine');
    const outline = await vscode.commands.executeCommand('vscode.executeDocumentSymbolProvider', source);
    assert.ok(outline.some(item => item.name === 'aggregate') && outline.some(item => item.name === 'main'));
    const symbols = await vscode.commands.executeCommand('vscode.executeWorkspaceSymbolProvider', 'parseLine');
    assert.ok(symbols.some(item => item.name === 'parseLine' && item.location.uri.toString() === library.toString()));
    const field = document.positionAt(document.getText().indexOf('reading.delta') + 2);
    const types = await vscode.commands.executeCommand('vscode.executeTypeDefinitionProvider', source, field);
    assert.ok(types.some(item => (item.targetUri ?? item.uri).toString() === library.toString()));
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

    const addImport = 'fn importProbe(): Unit { val _ = Clock.now() }';
    const end = document.positionAt(document.getText().length);
    assert.ok(await sourceEditor.edit(edit => edit.insert(end, endOfLine + addImport + endOfLine)));
    try {
        const start = end.translate(1, addImport.indexOf('Clock'));
        const diagnostics = await waitForDiagnostics(source, listing => listing.some(item => item.code === 'LYN0001'), 'missing unresolved import probe');
        const completions = await vscode.commands.executeCommand('vscode.executeCompletionItemProvider', source, start.translate(0, 3));
        assert.ok(!completions.items.some(item => (item.label.label ?? item.label) === 'Clock'));
        const range = diagnostics.find(item => item.code === 'LYN0001').range;
        const actions = await vscode.commands.executeCommand('vscode.executeCodeActionProvider', source, range, 'quickfix');
        const action = actions.find(item => item.edit && item.title.includes('import std.time'));
        assert.ok(action, `missing module import action: ${JSON.stringify(actions.map(item => item.title))}`);
        assert.ok(!document.getText().includes('import std.time'));
        assert.ok(await vscode.workspace.applyEdit(action.edit));
        assert.ok(document.getText().includes(`import std.time${endOfLine}`));
        assert.ok(document.getText().includes('time.Clock.now()'));
        await waitForDiagnostics(source, listing => !listing.some(item => item.severity === vscode.DiagnosticSeverity.Error),
            'module import action did not make Clock.now resolve');
    } finally {
        await vscode.commands.executeCommand('workbench.action.files.revert');
    }
}

async function checkManifest() {
    const [project] = vscode.workspace.workspaceFolders;
    const uri = vscode.Uri.joinPath(project.uri, 'stats', 'lyra.json5');
    const document = await vscode.workspace.openTextDocument(uri);
    assert.equal(document.languageId, 'lyra-manifest');
    const editor = await vscode.window.showTextDocument(document);
    const original = await vscode.workspace.fs.readFile(uri);
    const modified = "{name: 'example/stats', version: '0.1.0', kind: 'l'}";
    assert.ok(await editor.edit(edit => edit.replace(new vscode.Range(document.positionAt(0), document.positionAt(document.getText().length)), modified)));
    try {
        const issues = await waitForDiagnostics(uri, listing => listing.some(item => item.code === 'LYK0001'), 'missing manifest kind diagnostic');
        assert.equal(document.getText(issues.find(item => item.code === 'LYK0001').range), "'l'");
        const cursor = document.positionAt(modified.indexOf("'l'") + 2);
        const result = await vscode.commands.executeCommand('vscode.executeCompletionItemProvider', uri, cursor);
        assert.ok(result.items.some(item => item.label === 'lib' && item.kind === vscode.CompletionItemKind.EnumMember));
        assert.deepEqual(await vscode.workspace.fs.readFile(uri), original);
    } finally {
        await vscode.commands.executeCommand('workbench.action.files.revert');
    }
    await waitForDiagnostics(uri, listing => listing.length === 0, 'manifest diagnostic did not clear after reverting');
}

async function checkTesting() {
    const [project] = vscode.workspace.workspaceFolders;
    const tests = await vscode.workspace.findFiles('**/tests/*.ly');
    const expected = vscode.Uri.joinPath(project.uri, 'metrics', 'tests', 'readings.ly');
    assert.ok(tests.some(uri => uri.toString() === expected.toString()), 'workspace test files were not exposed to VS Code');
}

function debugStopTracker(name) {
    const events = [];
    const waiters = [];
    const registration = vscode.debug.registerDebugAdapterTrackerFactory('lyra', {
        createDebugAdapterTracker(session) {
            if (session.configuration.name !== name) return {};
            return {
                onDidSendMessage(message) {
                    if (message.type !== 'event' || message.event !== 'stopped') return;
                    const waiter = waiters.shift();
                    if (waiter) waiter(message.body);
                    else events.push(message.body);
                },
            };
        },
    });
    return {
        next() {
            if (events.length) return Promise.resolve(events.shift());
            return new Promise(resolve => waiters.push(resolve));
        },
        dispose() { registration.dispose(); },
    };
}

async function debugFrameAt(session, threadId, sourcePath, targetLine) {
    const { stackFrames } = await session.customRequest('stackTrace', { threadId, startFrame: 0, levels: 1 });
    const frame = stackFrames?.[0];
    assert.ok(frame, `No stack frame at ${basename(sourcePath)}:${targetLine}`);
    const expected = resolve(sourcePath);
    const actual = resolve(frame.source?.path ?? '');
    const matches = process.platform === 'win32' ? actual.toLowerCase() === expected.toLowerCase() : actual === expected;
    assert.ok(matches && frame.line === targetLine, `Unexpected stopped frame: ${JSON.stringify(frame)}`);
    return frame;
}

async function debugLocals(session, frame) {
    const deadline = Date.now() + 5000;
    while (Date.now() < deadline) {
        try {
            const { scopes } = await session.customRequest('scopes', { frameId: frame.id });
            const localScope = scopes.find(scope => scope.name === 'Locals');
            assert.ok(localScope, `No Locals scope: ${JSON.stringify(scopes)}`);
            const { variables } = await session.customRequest('variables', { variablesReference: localScope.variablesReference });
            return new Map(variables.map(variable => [variable.name, variable.value]));
        } catch (error) {
            if (!String(error.message).includes('notStopped')) throw error;
            await new Promise(resolve => setTimeout(resolve, 30));
        }
    }
    throw new Error('Debugger did not pause before locals were inspected');
}

async function checkDebugging() {
    const sourcePath = process.env.LYRA_SMOKE_DEBUG_SOURCE;
    const program = process.env.LYRA_SMOKE_DEBUG_APP;
    const source = vscode.Uri.file(sourcePath);
    const text = new TextDecoder('utf-8', { fatal: true }).decode(await vscode.workspace.fs.readFile(source));
    const lines = text.split(/\r?\n/);
    const beforeLine = lines.findIndex(line => line.includes('await sleepMs')) + 1;
    const afterLine = lines.findIndex(line => line.trim() === 'afterWait') + 1;
    assert.ok(beforeLine > 0 && afterLine > beforeLine);
    const breakpoints = [beforeLine, afterLine].map(line => new vscode.SourceBreakpoint(
        new vscode.Location(source, new vscode.Range(line - 1, 0, line - 1, 0)),
    ));
    const workspaceFolder = vscode.workspace.workspaceFolders[0];
    let session;
    const name = 'Lyra async debugger smoke';
    const stops = debugStopTracker(name);
    vscode.debug.addBreakpoints(breakpoints);
    try {
        const started = new Promise((resolve, reject) => {
            const subscription = vscode.debug.onDidStartDebugSession(current => {
                if (current.type !== 'lyra' || current.configuration.name !== name) return;
                subscription.dispose();
                resolve(current);
            });
            setTimeout(() => { subscription.dispose(); reject(new Error('Lyra debug session did not start')); }, 10000);
        });
        const beforeStopped = stops.next();
        assert.ok(await vscode.debug.startDebugging(workspaceFolder, {
            type: 'lyra', request: 'launch', name, program, cwd: dirname(program), args: [], stopOnEntry: false,
        }), 'VS Code rejected the Lyra debug configuration');
        session = await started;
        const beforeStop = await beforeStopped;
        const beforeFrame = await debugFrameAt(session, beforeStop.threadId, sourcePath, beforeLine);
        const beforeLocals = await debugLocals(session, beforeFrame);
        assert.equal(beforeLocals.get('base'), '40', `before-await locals: ${JSON.stringify([...beforeLocals])}`);
        assert.equal(beforeLocals.get('beforeWait'), '41', `before-await locals: ${JSON.stringify([...beforeLocals])}`);
        const afterStopped = stops.next();
        await session.customRequest('continue', { threadId: beforeStop.threadId });
        const afterStop = await afterStopped;
        const afterFrame = await debugFrameAt(session, afterStop.threadId, sourcePath, afterLine);
        const afterLocals = await debugLocals(session, afterFrame);
        assert.equal(afterLocals.get('beforeWait'), '41', `after-await locals: ${JSON.stringify([...afterLocals])}`);
        assert.equal(afterLocals.get('afterWait'), '42', `after-await locals: ${JSON.stringify([...afterLocals])}`);
    } finally {
        if (session) await vscode.debug.stopDebugging(session);
        stops.dispose();
        vscode.debug.removeBreakpoints(breakpoints);
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
    const localBindingEnd = name => {
        const declaration = `val ${name}`;
        const offset = original.indexOf(`${declaration} =`);
        assert.notEqual(offset, -1, `missing local ${declaration}`);
        return document.positionAt(offset + declaration.length);
    };
    const logger = localBindingEnd('logger');
    const router = localBindingEnd('router');
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
        const mount = document.getText().indexOf('#mountRoutes(router,');
        assert.notEqual(mount, -1);
        const routeItems = (await complete(document.positionAt(mount + '#mountRo'.length))).items;
        assert.ok(routeItems.some(item => item.label === 'mountRoutes' && item.kind === vscode.CompletionItemKind.Function),
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
        const module = 'http.';
        assert.ok(await editor.edit(edit => edit.replace(new vscode.Range(start, start.translate(0, 'router.'.length)), module)));
        const moduleMembers = (await complete(start.translate(0, module.length))).items;
        assert.ok(moduleMembers.some(item => item.label === 'Router' && item.kind === vscode.CompletionItemKind.Class));
        assert.ok(moduleMembers.some(item => item.label === 'listen' && item.kind === vscode.CompletionItemKind.Function));
        const call = 'val _ = http.listen(8080, ho';
        assert.ok(await editor.edit(edit => edit.replace(new vscode.Range(start, start.translate(0, module.length)), call)));
        const signature = await vscode.commands.executeCommand('vscode.executeSignatureHelpProvider', source, start.translate(0, call.length));
        assert.ok(signature.signatures.some(item => item.parameters.some(parameter => parameter.label.includes('host:'))));
        assert.ok((await complete(start.translate(0, call.length))).items.some(item => item.label === 'host:'));
        assert.ok(document.isDirty);
        assert.ok(document.getText().includes('val router = http.Router()'));
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
        await checkManifest();
        await checkTyping();
        await checkTesting();
        await checkDebugging();
        await writeFile(report, `PASS VS Code ${vscode.version}: language navigation, diagnostics, Test Explorer and async debug locals across await\n`);
    } catch (error) {
        await writeFile(report, `${error.stack}\n`);
        throw error;
    }
}

module.exports = { run };
