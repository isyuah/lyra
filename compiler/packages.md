# Lyra 包工具

## 两个本地包

```sh
lyra new demo/app --name acme/app
lyra new demo/config --name acme/config --lib
```

`new` 创建 `kind: 'app'` 的可执行包；`--lib` 创建 `kind: 'lib'` 的库，不需要入口 `main`。手写清单未指定 `kind` 时默认为应用。在 `demo/lyra.json5` 声明开发成员：

```json5
{ workspace: { members: ['app', 'config'] } }
```

在 `demo/app/lyra.json5` 中增加依赖；简称 `cfg` 只在调用方生效，不是包名：

```json5
{
  name: 'acme/app', version: '0.1.0',
  dependencies: {
    cfg: { name: 'acme/config', version: '^0.1.0', path: '../config' },
  },
}
```

`lyra lock demo/app` 会为整个 workspace 生成 `demo/lyra.lock`。`lyra fetch demo --locked --offline` 校验同一张图；workspace 全部是本地依赖时不需要注册中心、缓存或 `curl`。本地成员的锁摘要针对清单；编辑本地 `.ly` 文件不需要重新锁，和已发布包的整包摘要不同。

`lyra.json5` 当前接受 JSON 以及注释、尾逗号、单引号字符串、未加引号的 ASCII 标识符键。包名各段由 `/` 分开，段内允许 ASCII 字母、数字、`_`、`-`；依赖简称须是标识符。版本和范围都写成字符串。版本约束支持精确版本、`^1.2.3`、`^0.2.3`、`^0.0.3`；预发布版要显式选择。解析器优先沿用满足条件的锁，对不兼容依赖会回溯尝试其他可行版本；没有解时给出依赖链。

## 分发源码包

```sh
lyra pack demo/config -o config.lypkg
lyra pack demo/config --registry ./center
```

第一条输出可按 SHA-256 校验的确定性归档，包含清单、`src/` 下的普通文件（包括 `src/.macros/`、`src/.decorators/`、`src/.expressions/` 中的实现），以及 `inputs/` 下由 `macroInputs` 明确声明的[只读宏输入](../std/meta/README.md)。宏目录是源码包的一部分，但其中的实现不当作应用的普通运行时代码加载。打包不跟随符号链接，不包含 `build/`、workspace 锁或包根的私有配置。第二条将同样的字节和索引写入本地静态中心。同一包名和版本不允许二次发布。库依赖的 `path` 或指向作者磁盘的 `registry` 也不允许发布。发布包依赖同一个中心的另一包时可写 `registry: 'self'`；跨中心依赖写明确的 HTTP(S) 基地址。

在上例把 `cfg` 的 `path` 换成 `registry: '../../center'`，运行 `lyra lock demo` 即可从本地中心消费发布包，源代码中仍使用 `cfg`。把中心的同一目录树交给静态 HTTP(S) 服务，把 `registry` 改成服务基地址，也能使用同一份包；客户端只发 GET，不接收服务器指定的下载地址或脚本。

| 命令 | 行为 |
|---|---|
| `lyra lock DIR` / `lyra fetch DIR` | 解析整张图，核对摘要，获取和展开缺失的发布包；已有且可用的锁优先。未完成不会替换锁。 |
| `lyra lock DIR --update` | 主动重选满足约束的版本，跳过普通撤回的版本。 |
| `lyra fetch DIR --locked` | 缺锁或锁与当前清单不兼容时报错；允许下载指定制品。 |
| `lyra fetch DIR --locked --offline` | 不尝试网络。只读本地路径、静态中心和已校验的缓存；缺制品时报告包名、确切版本与摘要。 |

默认包缓存为 `<workspace>/.lyra/cache/`；可设置 `LYRA_CACHE` 指到专用目录。原生目标文件缓存使用 `LYRA_CACHE/compiler/objects/`；未设置时写入可执行文件输出目录下的 `.lyra-cache/objects/`，键包含精确 LLVM IR、目标、Clang 路径与版本及编译选项。命中且内容校验通过时只跳过 IR 到目标文件的编译，前端检查与链接仍每次运行；缓存损坏或无法读写时照常构建。HTTP(S) 下载在**构建机**调用 `curl`，不经 shell，且不跟随重定向；`--offline` 从不调用它。锁要提交到源码管理，缓存和 `dist/` 不需要提交。

## 静态中心和脚本边界

- `index/<完整包名>.json` 中 `versions` 各项含 `version`、`sha256`、`yanked`，制品在 `artifacts/<sha256>.lypkg`。中心维护者可将普通撤回版本的 `yanked` 置为 `true`：新解析跳过，已有锁的摘要一致时沿用并告警；摘要变更即失败。离线所见的撤回状态以最后获取的元数据为准。服务端不需要运行 Lyra。
- 可选脚本声明示例：`scripts: { prepare: { command: 'generate-inputs', outputs: ['src/generated.ly'] } }`。解析结果列出包名、版本、来源、命令和缺失产物；普通命令不执行。当前不提供脚本授权或沙箱执行命令，也不在构建时偷偷补跑。
- 目前分发的是源码，不承诺跨编译器的二进制 ABI、发布签名、凭据管理、被封禁的恶意包或静态网站之外的服务端发布接口。清单、锁和归档格式是这个编译器版本的格式，语言约束见[模块与包 RFC](../rfc/modules-packages.md)。
