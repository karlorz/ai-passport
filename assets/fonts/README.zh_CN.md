<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 字库资源（Fonts）

本目录存放项目可复用的字库资源。每个字库子目录或单个字库文件，应附说明。

## 如何使用

- 命名要能反映字族、字重、字级与格式。
- 记录来源、许可、字符范围、转换命令与目标放置路径。
- 添加字库前评估 Flash 与内部 RAM 影响；ESP32-C3 无 PSRAM。
- 不提交许可不允许分发的字库。

## Pocket Pal 16 px 子集

| 文件 | 用途 |
| --- | --- |
| [`pocket_pal_chars.txt`](pocket_pal_chars.txt) | 字符清单：133 个码位，每行一个 `U+XXXX` 加注释列。它既是生成输入，也是覆盖检查的基准。 |
| [`pocket_pal_font_16.c`](pocket_pal_font_16.c) | 生成的 LVGL 9.5 字库源码，符号名为 `pocket_pal_font_16`。 |

```c
LV_FONT_DECLARE(pocket_pal_font_16);
```

仅声明符号不会编译或链接该文件；必须把 `.c` 加入使用方组件的 `SRCS`。文字控件还需要在绘制文字的那个零件上显式指定该字库，并确认已启用 `CONFIG_LV_TXT_ENC_UTF8`。

### 来源与许可

| 字段 | 值 |
| --- | --- |
| 字族 | Noto Sans SC |
| 实例 | 可变字体，按 `wght=400` 实例化 |
| 许可 | SIL Open Font License 1.1 |
| 下载地址 | <https://github.com/google/fonts/raw/main/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf> |
| 生成时使用的本地文件名 | `NotoSansSC.ttf` |
| 是否提交 | 否。仓库只跟踪生成的 C 源码与字符清单。 |

Noto Sans SC 是可变字体：生成器通过 Pillow 的 `set_variation_by_axes([400])` 实例化 400 字重，而不是直接光栅化默认实例。许可允许再分发，但 17 MB 的源文件不提交：用上面的地址即可复现生成的子集，仓库只需要渲染结果。

### 覆盖范围

清单包含可打印 ASCII `U+0020`-`U+007E`、作为分隔符的间隔号 `U+00B7`，以及 37 个汉字：产品需求中
[字符串清单](../../docs/pocket-pal-design.zh_CN.md#字符串清单固定集合驱动字体子集)的 35 个不重复字符，加上只出现在整句里的 `U+4E00`、`U+4E0B`（来自“稍等一下”）。权威列表是
[`pocket_pal_chars.txt`](pocket_pal_chars.txt)；上文的英文说明因仓库检查不允许中文正文而不重复整句。

本集合之外的内容——任意中文 Wi-Fi SSID、用户输入的名字、全角标点——在该字库中没有字形，不在覆盖范围内。显示此类文字的应用需要明确的“不支持字符”策略，或使用包含这些字形的后备字库。

### 为什么在仓库内生成

LVGL 官方转换工具 `lv_font_conv` 1.5.3（2021 年）在当前 Node.js 版本上会写出损坏的字形位图。输出看起来是合法字库，构建也能通过；缺陷只在屏幕上表现为所有文字变成噪点。因此
[`tools/gen_pocket_pal_font.py`](../../tools/gen_pocket_pal_font.py)
改用 Pillow/FreeType 光栅化清单中的字符，直接生成 LVGL 的 `fmt_txt` 结构，然后重新读取自己刚写出的文件，按 LVGL 的 PLAIN 4 bpp 读取方式逐字解码。任何不一致都会让命令失败。相关故障背景见
[LVGL 字库格式笔记](../../docs/reference/shinku-chen/lvgl-font-format-and-glyph-coverage.zh_CN.md)。

### 重新生成

在仓库根目录执行。生成需要 Pillow，覆盖检查不需要。

```bash
# 覆盖检查：仅使用标准库。
python3 tools/gen_pocket_pal_font.py --check

# 重新生成字库。任何装有 Pillow 的解释器都可以。
/tmp/pocket-pal-font/venv/bin/python tools/gen_pocket_pal_font.py \
  --font /tmp/pocket-pal-font/NotoSansSC.ttf --python-requires-pillow
```

`--check` 只扫描 `main/pocket_pal_*.c` 与 `main/pocket_pal_*.h` 中 C 字符串字面量里的非 ASCII 字符——不包括注释和字符常量——一旦新的界面文案需要清单里没有的字形，就会带缺失的 `U+XXXX` 列表失败。当 Pocket Pal 界面尚未渲染任何文字时，它改为检查 PRD 字符串清单，因此该检查不会是空转。把码位加入 `pocket_pal_chars.txt` 后重新生成；切勿手工修改生成的 `.c`。

### 格式与度量

| 字段 | 值 |
| --- | --- |
| 字号 / 位深 | 16 px，4 bpp |
| 位图格式 | PLAIN（`bitmap_format = 0`），未压缩，高半字节在前，每个字形按字节对齐 |
| cmap | 2 个：`FORMAT0_TINY` 覆盖 `U+0020`-`U+007E`（95 个字），`SPARSE_TINY` 覆盖其余 38 个码位 |
| 字距调整 | 无，`.kern_dsc = NULL` |
| `line_height` | 19 px |
| `base_line` | 4 px |

`line_height` 是排版参数，不是字库的自然行距。Noto Sans SC 在 16 px 下自然行高约 24 px；若照搬该值，会在 240 x 320 的屏幕上白白浪费四分之一的高度，并静默裁掉按 19 px 行高排版的内容。

`base_line = 4` 由本次光栅化实测得出，而非照抄字库的 `hhea`/`OS2` 字段。本清单的字形墨迹最高到基线上方 15 px、最低到下方 5 px，合计 20 px，比 19 px 的行盒多出 1 px。`base_line = 4` 保留完整上伸部分，任何汉字和字母顶部都不会被裁，只牺牲 `U+007C` 竖线的最后一行——Pocket Pal 从不显示该字符。若取 `base_line = 5`，被裁掉的会变成 `U+0060` 的顶部。

### Flash 占用

生成时实测：

| 项目 | 字节 |
| --- | --- |
| 字形位图（133 个字） | 8 519 |
| 字形描述表、cmap、`unicode_list`、`lv_font_fmt_txt_dsc_t`、`lv_font_t` | 1 256 |
| **常量数据合计** | **9 775（9.5 KiB）** |

该字库是 `const` 数据，LVGL 直接从 Flash 读取，因此不占用静态 RAM，运行时也不占堆。上表为各数组之和加上 32 位目标上 LVGL 9.5 描述结构的大小；编译出的目标文件实测常量数据为 9 836 字节，与估算一致（差异来自链接器对齐填充）。扩大清单后位图段大致按新增字数增长，该字号下每个汉字约 60-90 字节。

