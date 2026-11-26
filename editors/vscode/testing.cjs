const { execFile } = require('node:child_process');
const { basename, dirname } = require('node:path');
const vscode = require('vscode');

function invokeTests(command, directory, args) {
    return new Promise(resolve => {
        execFile(command, ['test', directory, '--format=json', ...args], {
            cwd: directory, windowsHide: true, encoding: 'utf8', maxBuffer: 8 * 1024 * 1024,
        }, (error, stdout, stderr) => {
            let tests;
            try {
                tests = JSON.parse(stdout).tests;
                if (!Array.isArray(tests)) throw new Error('Lyra did not return a test list.');
            }
            catch (failure) { resolve({ error: stderr.trim() || error?.message || failure.message }); return; }
            resolve({ tests, error: stderr.trim() || error?.message });
        });
    });
}

function registerTesting(context, command) {
    const controller = vscode.tests.createTestController('lyra.tests', 'Lyra');
    const output = vscode.window.createOutputChannel('Lyra Tests');
    const cases = new WeakMap();
    let discovered;
    let timer;
    let disposed = false;

    async function discover() {
        const manifests = await vscode.workspace.findFiles('**/lyra.json5', '**/{build,.lyra,.git,node_modules}/**');
        const directories = [...new Set(manifests.map(uri => dirname(uri.fsPath)))].sort((left, right) => left.localeCompare(right));
        const seen = new Set();
        const packages = new Map();
        for (const directory of directories) {
            if (disposed) return;
            const result = await invokeTests(command, directory, ['--list']);
            if (!result.tests) { output.appendLine(result.error); continue; }
            for (const entry of result.tests) {
                const uri = vscode.Uri.file(entry.file);
                if (seen.has(uri.toString())) continue;
                seen.add(uri.toString());
                const root = vscode.Uri.file(dirname(dirname(entry.file)));
                let group = packages.get(root.toString());
                if (!group) {
                    group = controller.createTestItem(root.toString(), entry.name.slice(0, entry.name.lastIndexOf('/')), root);
                    packages.set(root.toString(), group);
                }
                const item = controller.createTestItem(uri.toString(), basename(entry.file), uri);
                cases.set(item, { directory, id: entry.id });
                group.children.add(item);
            }
        }
        if (!disposed) controller.items.replace([...packages.values()]);
    }

    function refresh() {
        if (!discovered) discovered = discover().catch(error => output.appendLine(error.message)).finally(() => { discovered = undefined; });
        return discovered;
    }

    function scheduleRefresh() {
        clearTimeout(timer);
        timer = setTimeout(() => {
            if (discovered) void discovered.then(() => { if (!disposed) void refresh(); });
            else if (!disposed) void refresh();
        }, 150);
    }

    controller.resolveHandler = async item => { if (!item) await refresh(); };
    controller.refreshHandler = refresh;
    context.subscriptions.push(vscode.commands.registerCommand('lyra.tests.refresh', async () => {
        await refresh();
        let count = 0;
        function visit(items) {
            items.forEach(item => {
                if (cases.has(item)) count++;
                else visit(item.children);
            });
        }
        visit(controller.items);
        return count;
    }));
    const manifests = vscode.workspace.createFileSystemWatcher('**/lyra.json5');
    const tests = vscode.workspace.createFileSystemWatcher('**/tests/*.ly', false, true, false);
    for (const watcher of [manifests, tests]) {
        context.subscriptions.push(watcher, watcher.onDidCreate(scheduleRefresh), watcher.onDidChange(scheduleRefresh), watcher.onDidDelete(scheduleRefresh));
    }
    context.subscriptions.push(vscode.workspace.onDidChangeWorkspaceFolders(scheduleRefresh), output, controller, {
        dispose() { disposed = true; clearTimeout(timer); },
    });

    const profile = controller.createRunProfile('Run Lyra Tests', vscode.TestRunProfileKind.Run, async (request, cancellation) => {
        if (!request.include && !controller.items.size) await refresh();
        const selected = [];
        const excluded = new Set(request.exclude?.map(item => item.id));
        function visit(item) {
            if (excluded.has(item.id)) return;
            if (cases.has(item)) selected.push(item);
            else item.children.forEach(visit);
        }
        if (request.include) request.include.forEach(visit);
        else controller.items.forEach(visit);
        const run = controller.createTestRun(request);
        try {
            selected.forEach(item => run.enqueued(item));
            for (const item of selected) {
                if (cancellation.isCancellationRequested) { run.skipped(item); continue; }
                run.started(item);
                const { directory, id } = cases.get(item);
                const { tests, error } = await invokeTests(command, directory, ['--offline', '--case', id, '--timeout-seconds', '60']);
                const result = tests?.find(entry => entry.id === id);
                if (!result) {
                    run.errored(item, new vscode.TestMessage(error || 'Lyra returned no result for this test.'));
                    continue;
                }
                for (const text of [result.stdout, result.stderr]) if (text)
                    run.appendOutput(text.replace(/\r?\n/g, '\r\n'), undefined, item);
                if (result.outputTruncated) run.appendOutput('Lyra truncated test output.\r\n', undefined, item);
                if (result.status === 'passed') run.passed(item, result.durationMs);
                else {
                    const failure = new vscode.TestMessage(result.error?.message || `Test exited with code ${result.exitCode}.`);
                    if (result.error?.file && result.error.line) {
                        const line = result.error.line - 1;
                        failure.location = new vscode.Location(vscode.Uri.file(result.error.file), new vscode.Range(line, 0, line, 0));
                    }
                    run.failed(item, failure, result.durationMs);
                }
            }
        } finally { run.end(); }
    }, true);
    context.subscriptions.push(profile);
}

module.exports = { registerTesting };
