# Lyra 文档站

站点使用 Astro 7 + Starlight 构建，包含手写教程和由 Lyra 编译器生成的语言与 API 参考。英文 locale 目前只有翻译说明页，其余页面回退到简体中文。

## 本地开发

需要 Node.js 22.23.4 或更新版本，以及当前工作区构建的 Lyra 编译器（参见[仓库构建说明](../README.md)）。

```bash
cd docs
npm ci
npm run dev
```

开发服务器默认在 `http://localhost:4321/lyra/`。构建静态站点：

```bash
npm run build
npm run preview
```

`npm run dev`、`npm run build` 和 `npm run check` 都会先运行 `lyra doc --stdlib`。默认从仓库的 Debug / Release preset 构建目录找编译器，其次查 `PATH`；也可设置 `LYRA_DOC_COMPILER` 覆盖，填绝对路径或相对于运行 npm 命令的 `docs/` 目录的路径，例如 `../build/windows-dev/bin/lyra.exe`。若开发服务器已启动，重建编译器后可单独运行 `npm run generate:reference` 刷新参考。

产物在 `docs/dist/`。生成的 Markdown（`src/content/docs/reference/`）、参考数据（`public/reference.json`）和构建缓存都不提交。API 说明应写到对应 Lyra 声明的 `///` 中；数据格式见[编译器参考数据说明](../compiler/reference.md)。

## 内容结构

```text
src/content/docs/
├── index.mdx                 # 首页
├── 404.mdx                   # 自定义 404
├── guide/                    # 教程
│   ├── index.mdx             # 教程总览
│   └── *.mdx                 # 章节
├── reference/                # 自动生成的语言与 API 参考
└── en/                       # 英文 locale（当前只有说明页）
```

侧边栏、站点标题和语言配置在 `docs/astro.config.mjs`。新增手写教程章节时同时更新侧边栏；参考索引和侧边栏条目会自动更新。中文是 root locale，英文翻译放在 `src/content/docs/en/` 下对应路径。

## 代码块约定

教程里的 Lyra 代码块由 `tools/check_docs.py` 实际编译；生成的参考页不参与这项校验，因为 API 声明和注释里的代码片段不需要独立成文件：

- ` ```lyra `：完整源文件。脚本用 `lyra check` 编译；如果块里有 `fn main` 且后面紧跟 ` ```output ` 块，还会 `lyra run` 并比较标准输出。
- ` ```lyra check `：只编译，不运行。适合服务、需要外部文件的程序或会阻塞的入口。
- ` ```lyra no-check `：跳过。只用于多文件示例、宏实现或无法独立成文件的片段。
- ` ```output `：紧跟在可运行 Lyra 块后的预期标准输出。
- ` ```bash ` / ` ```text ` / ` ```json5 ` / ` ```toml `：命令、终端输出和配置文件，不参与 Lyra 编译。

从仓库根目录运行校验（需要先构建编译器）：

```bash
python tools/check_docs.py --lyra build/windows-release/bin/lyra.exe
```

Linux 上把 `--lyra` 换成 `build/dev/bin/lyra` 或 SDK 里的 `bin/lyra`。脚本会为每个块创建临时包，运行结束后删除。

## 部署

`.github/workflows/docs.yml` 在文档、编译器或标准库等相关源码变更时，从同一提交构建 Lyra、验证教程示例，再运行 `npm run check` 和 `npm run build`。PR 只验证；官方仓库 `main` 的匹配变更会自动部署，也可在 Actions 中选择 `main` 手动运行 `Docs`。这与 tag 触发的 SDK 发布相互独立。

仓库的 Pages 来源为 **GitHub Actions**，`github-pages` 环境只允许 `main`。站点地址是 [isyuah.github.io/lyra](https://isyuah.github.io/lyra/)。如果以后换自定义域名，同步修改 `astro.config.mjs` 的 `site` 和 `site.mjs` 的 `siteBase`。

## 写作边界

教程只讲当前仓库能验证的行为。新增或修改行为说明前，先确认编译器、`tests/` 或 `examples/` 有对应证据；RFC 里“已接受但未实现”的内容不要写成可用 API。版本边界统一写 `0.1.0-alpha.1`，不写具体发布日期。
