# Lyra 文档站

教程源码，使用 Astro 7 + Starlight 构建。当前只写简体中文；英文 locale 已配置，`src/content/docs/en/` 目前只有翻译说明页，其余页面回退到中文。

## 本地开发

需要 Node.js 22.23.4 或更新版本。

```bash
cd docs
npm install
npm run dev
```

开发服务器默认在 `http://localhost:4321/lyra/`。构建静态站点：

```bash
npm run build
npm run preview
```

产物在 `docs/dist/`；`docs/.astro`、`docs/dist` 和 `docs/node_modules` 不提交。

## 内容结构

```text
src/content/docs/
├── index.mdx                 # 首页
├── 404.mdx                   # 自定义 404
├── guide/                    # 教程
│   ├── index.mdx             # 教程总览
│   └── *.mdx                 # 章节
└── en/                       # 英文 locale（当前只有说明页）
```

侧边栏、站点标题和语言配置在 `docs/astro.config.mjs`。新增章节时同时更新侧边栏；中文是 root locale，英文翻译放在 `src/content/docs/en/` 下对应路径。

## 代码块约定

教程里的 Lyra 代码块不是装饰，它们由 `tools/check_docs.py` 实际编译：

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

`.github/workflows/docs.yml` 在 `main` 分支的 `docs/**` 变更后构建并部署到 GitHub Pages。仓库设置里把 Pages 的来源设为 **GitHub Actions**；站点地址是 `https://isyuah.github.io/lyra/`。如果以后换自定义域名，同步修改 `astro.config.mjs` 的 `site` 和 `base`。

## 写作边界

教程只讲当前仓库能验证的行为。新增或修改行为说明前，先确认编译器、`tests/` 或 `examples/` 有对应证据；RFC 里“已接受但未实现”的内容不要写成可用 API。版本边界统一写 `0.1.0-alpha.1`，不写具体发布日期。
