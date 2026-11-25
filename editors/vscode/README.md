# Lyra Language for VS Code

为 `.ly` 提供高亮、注释与括号配对。诊断、补全、悬停、定义、引用、重命名、行内类型提示和格式化由原生 SDK 的 `lyra lsp --stdio` 提供。扩展不下载 SDK 或包依赖，不编译或运行项目，也不启动准备脚本。

## 本地打包和安装

在此目录中使用 Node.js 22 或更新版本执行：

```sh
npm ci
npm run check
npm exec --yes --package=@vscode/vsce@4.1.1 -- vsce package --allow-missing-repository --skip-license -o lyra-language.vsix
code --install-extension lyra-language.vsix
```

也可用 VS Code 的 **Extensions: Install from VSIX...** 选择生成的文件。需要 VS Code 1.100 或更新版本和支持 `lsp --stdio` 的 Lyra SDK。复制 SDK 时保留 `bin`、`lib` 和 `share` 的相对位置。

## 配置 SDK

默认从 VS Code 扩展宿主的 `PATH` 查找 `lyra`。也可以在用户设置中配置 SDK 的绝对路径，例如 Windows：

```json
{
  "lyra.serverPath": "E:/SDKs/Lyra/bin/lyra.exe"
}
```

在 Linux 上可改为 `/home/me/lyra-sdk/bin/lyra`。配置值只填路径，不添加额外引号或命令行参数。使用 WSL 或 SSH 时，在远程扩展宿主安装扩展，并配置远程的路径或 `PATH`。修改设置后执行 **Developer: Reload Window**。在受信任的工作区打开 `.ly` 文件即可启动语言服务；启动报错可在 **Output → Lyra Language Server** 看日志。

打开包含 `lyra.json5` 的包目录或其 workspace；对已有锁的外部依赖，先在终端执行 `lyra fetch <包目录> --locked`，让语言服务使用已下载的源码。多根工作区使用同一个语言进程，由服务端按当前文件所属的包分析。

语言服务按入口包和可达依赖缓存语义分析。修改无关包不会重检当前包；修改相关包时会重跑模块链接和类型检查，并复用未变且未展开宏的文件的解析结果。VS Code 会监听这些包的源码、相关清单和锁文件，以及显式授权的宏输入；即使依赖文件未打开，磁盘修改也会刷新诊断。

补全从当前缓冲区读取关键字、声明、导入及作用域内的局部名称。输入 `v` 可补全 `val`、`var`；语法还没写完整时，也能补全已识别的名称。自动弹出遵循 VS Code 的 `editor.quickSuggestions`，也可按 `Ctrl+Space` 手动触发。源码错误显示在编辑器和 Problems 中，不作为悬停或跳转请求错误弹窗。

省略类型的 `val`、`var` 和函数／方法返回值会显示淡色的原生 Inlay Hints，例如 `val router: Router = Router()` 中的 `: Router`。提示不写入源码，可用 `editor.inlayHints.enabled` 控制显示。

**Format Document** 使用和 `lyra fmt` 一样的词法规则，处理编辑器缓冲区并遵循缩进设置，不要求类型检查通过。

## 真实编辑器验收

执行 `npm ci` 后，从仓库根目录运行：

```sh
python editors/vscode/tests/smoke.py <VS-Code-桌面程序路径> <Lyra-SDK-程序路径>
```

第一个路径在 Windows 填 `Code.exe`，Linux 填桌面程序 `code`，不是同发行包里的 `bin/code` 启动脚本。测试使用独立配置和临时项目，检查跨包定义跳转、磁盘依赖更新、未保存的 Unicode 宏报错，以及在 HTTP/SQLite 示例中输入一半时的补全、诊断和行内类型提示。

打包后可加 `--vsix <扩展包路径>`，改为加载从 VSIX 解包的扩展和依赖；验收该包时不读取源码目录的 `node_modules`。第二个参数可以直接指向解包后 SDK 的 `bin/lyra` 或 `bin/lyra.exe`。
