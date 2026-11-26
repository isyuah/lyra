# Lyra Language for VS Code

为 `.ly` 和 `lyra.json5` 提供编辑支持。诊断、补全、语义高亮、导航和格式化由原生 SDK 的 `lyra lsp --stdio` 提供。扩展不下载 SDK 或包依赖，不编译或运行项目，也不启动准备脚本。

## 本地打包和安装

在此目录中使用 Node.js 22 或更新版本执行：

```sh
npm ci
npm run check
npm exec --yes --package=@vscode/vsce@4.1.1 -- vsce package --allow-missing-repository --skip-license -o lyra-language.vsix
code --install-extension lyra-language.vsix --force
```

也可用 VS Code 的 **Extensions: Install from VSIX...** 选择生成的文件。需要 VS Code 1.100 或更新版本。更新时同时使用包含相应语言服务的新 SDK，并执行 **Developer: Reload Window**。复制 SDK 时保留 `bin`、`lib` 和 `share` 的相对位置。

## 配置 SDK

默认从 VS Code 扩展宿主的 `PATH` 查找 `lyra`。也可以在用户设置中配置 SDK 的绝对路径，例如 Windows：

```json
{
  "lyra.serverPath": "E:/SDKs/Lyra/bin/lyra.exe"
}
```

在 Linux 上可改为 `/home/me/lyra-sdk/bin/lyra`。配置值只填路径，不添加额外引号或命令行参数。使用 WSL 或 SSH 时，在远程扩展宿主安装扩展，并配置远程的路径或 `PATH`。修改设置后执行 **Developer: Reload Window**。在受信任的工作区打开 `.ly` 或 `lyra.json5` 即可启动语言服务；启动报错可在 **Output → Lyra Language Server** 看日志。

打开包含 `lyra.json5` 的包目录或其 workspace；对已有锁的外部依赖，先在终端执行 `lyra fetch <包目录> --locked`，让语言服务使用已下载的源码。多根工作区使用同一个语言进程，由服务端按当前文件所属的包分析。

语言服务按入口包和可达依赖缓存语义分析。修改无关包不会重检当前包；修改相关包时会重跑模块链接和类型检查，并复用未变且未展开宏的文件的解析结果。VS Code 会监听这些包的源码、相关清单和锁文件，以及显式授权的宏输入；即使依赖文件未打开，磁盘修改也会刷新诊断。

补全按当前位置区分顶层声明、函数体、类型、成员和导入。函数体输入 `v` 可补全 `val`、`var`，输入 `pr` 可找到内置 `print`；顶层输入 `#` 可找到 `derive`、`post`，表达式里的 `#` 可找到 `assert`，`@` 可找到 `trace` 和可见装饰器。`router.` 或 `value?.` 提示实际类型上可访问的成员及已导入的扩展。自动弹出遵循 VS Code 的 `editor.quickSuggestions`，也可按 `Ctrl+Space` 手动触发。

输入 `import std.`、`import self.` 或 `import <依赖简称>.` 选择模块。输入 `import std.http.{` 可选公开声明；只写 `import std.http` 时，输入 `http.` 才补全该模块的公开成员，普通名称补全不会混入全部依赖的 API。`pub import std.http.{…}`、`import macro` 和 `import decorator` 也支持补全。

普通名称报未解析时，按 `Ctrl+.` 打开 Quick Fix。编辑器优先复用现有模块别名，否则可添加模块导入并把当前用处改成限定名。同名声明分来源列出；默认别名冲突时，建议一个未占用的别名。块尾分号使返回值误变成 `Unit` 时可直接删掉它；**Fix All** 批量修复当前文件中确定多余的分号，不替用户选择导入来源。

输入 `http.listen(8080, ho` 会提示 `host:`；参数提示会列出匹配已输入实参的重载。声明前的 `///` 文档显示在补全、悬停和签名提示里。可从宏、装饰器调用或导入跳到实现。重命名默认模块别名会补 `as`，保留导入路径。诊断在后台合并连续编辑，只发布当前版本，源码错误不会变成悬停或跳转失败弹窗。

**Rename Symbol** 会在未保存缓冲区上试改，检查被改和未改的引用是否仍指向同一声明，再返回带文档版本的编辑。公开声明要求明确的 Lyra workspace，重命名会一起检查可见的工作区成员和 `tests/*.ly`。有捕获冲突、只能修改部分引用、引用由宏生成，或分析不完整时不返回编辑；包缓存和 SDK 不在修改范围内。

同一次分析可在 Problems 中显示多个独立函数的错误。重载歧义列出候选声明位置，宏生成代码出错时可查看宏定义作为关联位置。普通定义跟随源码绑定（包括类型别名）；**Go to Type Definition** 跳到检查器确认的实际用户类型。光标下的同一符号在当前文件内一起高亮，同名但被遮蔽的符号不混在一起。

语义高亮把解析出的参数、变量、字段、方法、模块等交给当前主题着色，尚未绑定的名字沿用 TextMate 高亮。**Outline** 和面包屑显示手写声明和成员，即使函数体有语法错误也可查看。编辑器可折叠块、连续导入和多行注释。**Go to Symbol in Workspace** 可搜索未打开的本地包，不搜索 SDK 和包缓存。

编辑 `lyra.json5` 时，补全按包、workspace、依赖和脚本对象区分字段，`kind` 提示 `app` / `lib`。校验支持单引号、注释和尾逗号，问题指向对应的字段值或语法位置。未保存的清单只用于编辑器提示；依赖解析和宏输入授权仍以磁盘文件为准。

省略类型的 `val`、`var` 和函数／方法返回值会显示淡色的原生 Inlay Hints，例如 `val router: Router = http.Router()` 中的 `: Router`。提示不写入源码，可用 `editor.inlayHints.enabled` 控制显示。

**Format Document** 使用和 `lyra fmt` 一样的词法规则，处理编辑器缓冲区并遵循缩进设置，不要求类型检查通过。

**Test Explorer** 从 workspace 的 `tests/*.ly` 发现 Lyra 测试，可运行单个测试或整组测试。测试列表使用本地清单，不解析依赖；运行时复用 `lyra test --format=json`，失败显示编译诊断或退出码，并附带标准输出和错误输出。

**Debug** 使用 LLVM 的 `lldb-dap` 启动原生程序。先用 `lyra build <包目录> --debug-info` 生成调试版，再选择 **Lyra: Launch Debug Build**；默认读取 `build/debug/app`，Windows 会自动补 `.exe`。若 `lldb-dap` 不在 `PATH`，设置 `lyra.debugAdapterPath` 为其绝对路径。跨挂起点的异步断点与局部变量由编译器生成的原生符号提供。

## 真实编辑器验收

执行 `npm ci` 后，从仓库根目录运行：

```sh
python editors/vscode/tests/smoke.py <VS-Code-桌面程序路径> <Lyra-SDK-程序路径>
```

第一个路径在 Windows 填 `Code.exe`，Linux 填桌面程序 `code`，不是同发行包里的 `bin/code` 启动脚本。测试使用独立配置和临时项目，覆盖跨包定义与类型定义、大纲、未打开包的符号搜索、导入 Quick Fix、磁盘依赖更新、未保存的 Unicode 宏报错和清单补全，以及 HTTP/SQLite 示例中的输入补全、签名提示、诊断和行内类型提示。

打包后可加 `--vsix <扩展包路径>`，改为加载从 VSIX 解包的扩展和依赖；验收该包时不读取源码目录的 `node_modules`。第二个参数可以直接指向解包后 SDK 的 `bin/lyra` 或 `bin/lyra.exe`。
