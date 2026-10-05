# FTXUI 字素编辑补丁

FTXUI 保持 v7.0.3 子模块（`f921fad208912747c17d129a8ef75ec7624b6eec`）。
其原 Input 以 codepoint/combining mark 近似字素，左右移动、删除和覆盖会拆开
ZWJ、区域指示符、肤色修饰符、Indic 等扩展字素。此补丁使用维护中的 utf8proc
进行 UAX #29 分段，不在 Chat 中维护 Unicode 规则表。

启用 TUI 时，CMake 下载官方 utf8proc 2.12.0 / Unicode 18.0.0 固定提交：

- 源：`https://codeload.github.com/JuliaStrings/utf8proc/tar.gz/79cdd5ab40c79afa559f689ffc13b76812dee1ac`。
- archive SHA256：`e8fadfcd531d65525cbef157b866c5408e2e3f0d7c0f3aefb0bce01a7a08e35d`。
- 默认静态链接；TUI OFF 不下载此依赖。首次配置需要网络，后续使用该构建目录的下载缓存。
- 离线配置可使用 CMake 标准的 `FETCHCONTENT_SOURCE_DIR_UTF8PROC` 指向该提交的解压源码。

`ftxui_unicode.cmake` 校验三个原始文件的 SHA256，然后从 pristine 副本在构建目录
精确应用 `ftxui-7.0.3-grapheme.patch`（零 fuzz），不修改子模块。
原 `screen` 和 `component` target 各替换一个翻译单元，内部声明也来自修补目录；
不靠额外对象覆盖静态 archive 的符号。重配置先恢复 pristine 源，生成文件内容不变时不重编。
升级 FTXUI 时必须重新审查原始源码、补丁与 Unicode 回归，校验失败不静默使用旧实现。

每次 Input 事件或渲染共用操作内的字素边界，插删后重新分段当前内容。
不持久缓存内容/边界，不改变发送、粘贴或网络语义。
CRLF 保持原始 bytes 和 byte offset；非法 UTF-8 保持原 bytes，每个非法 byte 是独立编辑边界。

永久回归使用官方 `GraphemeBreakTest-18.0.0.txt` 的 853 条断言作为独立边界 oracle。
文件 SHA256：`b0cf047ee94485bbdc846de2b902f5f8a815f6b674f9d04223cddadd91c9df31`。
测试不联网获取 fixture，不从修复算法反推 expected boundaries。
Git 属性只允许补丁语法的空 context 行和官方 fixture 注释的原始尾空格，
不规范化这两种原始文件，也不豁免 C++ 源码的 whitespace 检查。

本补丁只完成编辑边界和宽字组合符附着位置修复；不是完整终端显示列策略。
leading combining、emoji span、Screen continuation、裁剪、原生选择与终端 shaping
仍需独立验收，不能仅凭字素回归通过关闭 T04。

FTXUI 补丁保留原 MIT 版权声明。utf8proc 的完整版权及许可证见
`utf8proc-LICENSE.md`，分发静态链接二进制时也必须携带该声明。
Unicode 测试数据保留官方头部，完整许可见 `../tests/data/UNICODE-LICENSE.txt`。
