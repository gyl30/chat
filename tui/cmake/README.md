# FTXUI 字素编辑与渲染补丁

FTXUI 保持 v7.0.3 子模块（`f921fad208912747c17d129a8ef75ec7624b6eec`）。
其原 Input 以 codepoint/combining mark 近似字素，左右移动、删除和覆盖会拆开
ZWJ、区域指示符、肤色修饰符、Indic 等扩展字素。此补丁使用维护中的 utf8proc
进行 UAX #29 分段，不在 Chat 中手工维护字素分段规则表。

启用 TUI 时，CMake 下载官方 utf8proc 2.12.0 / Unicode 18.0.0 固定提交：

- 源：`https://codeload.github.com/JuliaStrings/utf8proc/tar.gz/79cdd5ab40c79afa559f689ffc13b76812dee1ac`。
- archive SHA256：`e8fadfcd531d65525cbef157b866c5408e2e3f0d7c0f3aefb0bce01a7a08e35d`。
- 默认静态链接；TUI OFF 不下载此依赖。首次配置需要网络，后续使用该构建目录的下载缓存。
- 离线配置可使用 CMake 标准的 `FETCHCONTENT_SOURCE_DIR_UTF8PROC` 指向该提交的解压源码。
  配置也核验该源码的 header、实现和 Unicode 数据 SHA256，避免离线 override 绕过固定版本；
  永久 Unicode 测试核验运行时 API 2.12.0 与 Unicode 18.0.0。

肤色修饰符的显示关系另外使用官方 Unicode 18.0.0 属性数据：

- 源：`https://www.unicode.org/Public/18.0.0/ucd/emoji/emoji-data.txt`。
- 原文件：`unicode/emoji-data-18.0.0.txt`；SHA256：
  `80d00f8e616a0ef27fd6b8de3b758c06383b5d917e2977709578e68baf733bf1`。
- 生成表：`unicode/emoji_properties_18.inc`；SHA256：
  `b200a53eeae768e7e67a2d465936b0cb0e859618cc877b99dac79d454e2e6937`。
- 表只提取 `Emoji_Modifier_Base`（51 ranges / 136 codepoints）与 `Emoji_Modifier`（5 codepoints），
  不是手写 emoji 白名单。数据版本和 utf8proc Unicode 版本必须一起审查，不能独立换表。
- 完全离线核验：`python3 tui/cmake/generate_emoji_properties.py --check`。
  不带 `--check` 时向 stdout 输出确定性的 UTF-8 表，生成器不写源码、不联网；可比较或保存输出后审查。
  正常 CMake 配置不需要 Python，只校验 data/table pin，并将表 COPYONLY 到 patched 源目录。
  更新必须同时审查官方版本、原数据 hash、生成表 hash、utf8proc pin、版本测试和上下文 fixtures。

显示关系依据 UTS#51 Emoji 18.0 / revision 31 的 section 2.4：
`https://www.unicode.org/reports/tr51/tr51-31.html#Diversity`。
modifier 只附着于紧邻的 `Emoji_Modifier_Base`；旧 defective 数据允许中间恰好一个 VS16。
Mn、VS15、ZWJ、第二个 VS16 或前一个 modifier 均断开这条局部关系；
不能因为同一 EGC 里较早出现过 emoji base 就认定后面的 modifier 合法。
RGI modifier sequence 集合不是这一结构关系的等价白名单：Unicode18 RGI 有 675 pairs，
属性结构有 680 pairs（差集是 FAMILY × 5 个 modifier）。

`ftxui_unicode.cmake` 校验全部 21 个原始文件的 SHA256，然后从 pristine 副本在构建目录
精确应用 `ftxui-7.0.3-grapheme.patch`（零 fuzz），不修改子模块。
原 `screen`、`dom` 和 `component` target 替换对应翻译单元，公有与内部声明均来自修补目录；
不靠额外对象覆盖静态 archive 的符号。重配置先恢复 pristine 源，生成文件内容不变时不重编。
升级 FTXUI 时必须重新审查原始源码、补丁与 Unicode 回归，校验失败不静默使用旧实现。

每次 Input 事件或渲染共用操作内的字素边界，插删后重新分段当前内容。
不持久缓存内容/边界，不改变发送、粘贴或网络语义。
CRLF 保持原始 bytes 和 byte offset；非法 UTF-8 保持原 bytes，每个非法 byte 是独立编辑边界。

永久回归使用官方 `GraphemeBreakTest-18.0.0.txt` 的 853 条断言作为独立边界 oracle。
文件 SHA256：`b0cf047ee94485bbdc846de2b902f5f8a815f6b674f9d04223cddadd91c9df31`。
测试不联网获取 fixture，不从修复算法反推 expected boundaries。
Git 属性只允许补丁语法的空 context 行和官方 fixture/emoji-data 注释的原始尾空格，
不规范化这两种原始文件，也不豁免 C++ 源码的 whitespace 检查。

渲染以完整扩展字素写入：正 span 表示 head 的列数，负 span 是 continuation 到 head 的距离。
Text、VText、Canvas 和装饰 writer 通过 `SetGlyph`/`SetCell` 提交或擦除整组，
覆盖 continuation 也会清理旧组；不完整可见的字组只留下空白，不输出半个字符。
Frame 的可见范围与选择端点分别传递；完全可见的宽字任一格被选中时复制完整原文，
被视口裁掉的字组不进入复制结果。

`DisplayWidth` 表示显示策略分配的列；`string_width` 保留旧固有宽度政策，不能用它
计算带显示载体的布局。孤立 nonspacing/enclosing mark 用 dotted circle 承载；
孤立 spacing mark 的载体在真实终端可能额外占列，使用单格替代标记，正常基字加 mark 不变。
孤立肤色修饰符也使用替代标记；正常 emoji 基字加肤色保留原字素。
非 emoji modifier-base 或被其它字符隔开的肤色仅在显示层替换为可见标记，
正文/复制/Input 的原始 EGC 和 byte offset 不变；纯孤立/连续 modifier 保留既有单载体策略。
通用 `has_base` 不排除空格，合法 space+Mn 仍原样显示。
零宽 format-only、
非法 bytes、C0/C1 和 bidi 控制用替代字符显示，不把这些载体写入正文。
Text 选择、Input 编辑和发送保持原始字素；密码仍按原字素数量遮蔽。
`Utf8ToGlyphs` 返回原始完整字素及其显示列的空续格，`CellToGlyphIndex` 使用相同列映射。
应用的换行、省略与宽度分配也使用 `DisplayWidth`，CRLF 仍是一个换行边界。

修改字符需使用 `SetGlyph`/`SetCell` 或 Canvas `DrawText`/`DrawCell`；mutable `CellAt`/`at`
只保证读取或修改样式，不保证直接改字符后修复 span。空 continuation 的 `.clear()`
没有可观察变化，不能自动解释成擦除请求。Canvas 导入尊重源 stencil，不导入半组。
自定义 separator/border 是单格装饰，只接受一个显示列的完整字素，其余值为空格。

这是编辑、显示载体与整字裁剪的实现，不是完整终端列或字体 shaping 政策。
复合 emoji 的旧列数与真实 XTerm/tmux 仍可能不同，不能仅凭永久回归通过关闭 T04。

FTXUI 补丁保留原 MIT 版权声明。utf8proc 的完整版权及许可证见
`utf8proc-LICENSE.md`，分发静态链接二进制时也必须携带该声明。
Unicode 测试数据保留官方头部，完整许可见 `../tests/data/UNICODE-LICENSE.txt`。
Emoji 属性数据及派生表的完整 Unicode License V3 见 `unicode/UNICODE-LICENSE.txt`；
派生表明确标注提取修改和来源。分发包含这些表的静态链接二进制或源码也须携带该版权/许可声明。
