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

第一条输出可按 SHA-256 校验的确定性归档，包含清单、`src/` 下的普通文件（包括 `.macros/`、`.typedMacros/`、`.decorators/`、`.expressions/` 中的实现）、`inputs/` 下由 `macroInputs` 明确声明的[只读宏输入](../std/meta/README.md)，以及 `native/` 下明确声明的 C 源码、头文件和静态库。宏目录是源码包的一部分，但其中的实现不当作应用的普通运行时代码加载。打包不跟随符号链接，不包含 `build/`、workspace 锁或包根的私有配置。第二条将同样的字节和索引写入本地静态中心。同一包名和版本不允许二次发布。库依赖的 `path` 或指向作者磁盘的 `registry` 也不允许发布。发布包依赖同一个中心的另一包时可写 `registry: 'self'`；跨中心依赖写明确的 HTTP(S) 基地址。

在上例把 `cfg` 的 `path` 换成 `registry: '../../center'`，运行 `lyra lock demo` 即可从本地中心消费发布包，源代码中仍使用 `cfg`。把中心的同一目录树交给静态 HTTP(S) 服务，把 `registry` 改成服务基地址，也能使用同一份包；客户端只发 GET，不接收服务器指定的下载地址或脚本。

| 命令 | 行为 |
|---|---|
| `lyra lock DIR` / `lyra fetch DIR` | 解析整张图，核对摘要，获取和展开缺失的发布包；已有且可用的锁优先。未完成不会替换锁。 |
| `lyra lock DIR --update` | 主动重选满足约束的版本，跳过普通撤回的版本。 |
| `lyra fetch DIR --locked` | 缺锁或锁与当前清单不兼容时报错；允许下载指定制品。 |
| `lyra fetch DIR --locked --offline` | 不尝试网络。只读本地路径、静态中心和已校验的缓存；缺制品时报告包名、确切版本与摘要。 |

默认包缓存为 `<workspace>/.lyra/cache/`；可设置 `LYRA_CACHE` 指到专用目录。HTTP(S) 下载由 Lyra SDK 自带的静态注册中心传输程序完成，不依赖宿主 `curl`，不经 shell，且不跟随重定向；`--offline` 不执行下载。`LYRA_REGISTRY_CA_FILE` 可显式指定 HTTPS 注册中心的 PEM 信任链。锁要提交到源码管理，缓存和 `dist/` 不需要提交。

`build`、`run` 和 `test` 在包解析后校验源码、宏输入、SDK 与原生目标文件、静态库的内容指纹。现有可执行文件及其 PDB 也匹配时，直接复用构建结果和警告，跳过解析、类型检查、LLVM lowering 和链接。每次直接校验项目输入与产物内容；未变化的 SDK 文件用文件身份、大小和系统变更时间复用其摘要。构建记录使用 `LYRA_CACHE/compiler/builds/`，未设置时写入输出目录的 `.lyra-cache/builds/`。删除或修改产物、输入变化、损坏或缺失记录会触发构建；`--rebuild` 可跳过构建结果和 SDK 摘要的复用。

含 C 源码的依赖、GNU linker script 和 thin archive 可引用额外文件。在能完整记录这些依赖前，涉及这些输入的构建不复用整个产物。

原生目标文件缓存仍使用同一缓存根下的 `objects/`，键包含精确 LLVM IR、目标三元组、CPU、LLVM 版本及代码生成选项。前端重跑后若 LLVM IR 未变，仍可命中目标文件缓存。构建记录只用于同一输出路径，不把其他目录里的可执行文件或 PDB 恢复到这里。

## 随包分发 C 库

```json5
{
  name: 'acme/image', version: '0.1.0', kind: 'lib',
  native: {
    sources: ['native/resize.c'],
    headers: ['native/include/resize.h'],
    includeDirs: ['native/include'],
    archives: {
      'linux-x64': ['native/linux/libcodec.a'],
      'windows-x64': ['native/windows/codec.lib'],
    },
  },
}
```

四项都可省略；只用 C 源码时无需 `archives`。一旦声明目标静态库，构建只选择当前目标对应的列表，不会拿另一平台的库凑用。Linux 归档须包含 x86-64 ELF 目标文件，Windows 须包含 x64 COFF 目标文件；不接受依赖归档外文件的 thin archive、导入库和 LLVM bitcode。

`build`、`run`、`test` 使用 SDK 内置 Clang、C 头文件和归档器编译声明的 `.c`，自动链接传递依赖的原生库。编译器默认搜索包自己的 `native/` 和显式声明的头目录。C 代码及静态库是受信任的原生依赖；它们不享有 Lyra 安全代码的内存检查。构建不运行包提供的脚本，不读取宿主 Clang 配置。`--link` 仍可用来接入包外已经构建好的库。

## 静态中心和脚本边界

- `index/<完整包名>.json` 中 `versions` 各项含 `version`、`sha256`、`yanked`，制品在 `artifacts/<sha256>.lypkg`。中心维护者可将普通撤回版本的 `yanked` 置为 `true`：新解析跳过，已有锁的摘要一致时沿用并告警；摘要变更即失败。离线所见的撤回状态以最后获取的元数据为准。服务端不需要运行 Lyra。
- 可选脚本声明示例：`scripts: { prepare: { command: 'generate-inputs', outputs: ['src/generated.ly'] } }`。解析结果列出包名、版本、来源、命令和缺失产物；普通命令不执行。当前不提供脚本授权或沙箱执行命令，也不在构建时偷偷补跑。
- `.lypkg` 可以携带按目标选择的静态库，但不承诺跨编译器的二进制 ABI。目前未提供发布签名、凭据管理、恶意包封禁或静态网站之外的服务端发布接口。清单、锁和归档格式是这个编译器版本的格式，语言约束见[模块与包 RFC](../rfc/modules-packages.md)。
