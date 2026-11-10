# 贴图覆盖 (INI规则)

**[Chinese](https://github.com/furina-cute/tex_override/blob/main/README_zh.md)**

**[English](https://github.com/furina-cute/tex_override/blob/main/README.md)**

---
一个由 INI 驱动的 [ReShade](https://reshade.me) 附加组件（ReShade 6.8.0 Add-on / DirectX 11）：按纹理哈希 + 绘制上下文匹配，命中后 `skip` 或 `replace`。

## 匹配模型

```ini
[Settings]
hash_mode = descriptor   ; descriptor（快） | pixel（精确，仅 CPU 上传的纹理）
safe_mode = 0            ; 1 = 安全模式：禁用 replace 与像素哈希（排查系统重启用）

; 只写稳定锚点：跨进程重启依然有效（动态图集不会影响结果）
[TextureOverrideNameCard]
match_ps_hash = 0x1a2b3c4d
match_vs_hash = 0x5e6f7a8b
match_first_index = 0
match_index_count = 420
handling = skip

; 稳定锚点 + 纹理哈希：更精确，但纹理哈希可能随图集重排漂移
[TextureOverrideUID]
hash = 83fd403f
match_ps_hash = 0x1a2b3c4d
match_index_count = 420
handling = skip
hotkey = 0x1C300000     ; 运行时热键：Caps Lock（面板里点一下就能捕获，无需手写）

; 兼容旧写法（只有纹理哈希）
[TextureOverrideLegacy]
hash = a1b2c3d4
handling = skip
```

### 条件与布尔逻辑

| 键 | 含义 | 稳定性 |
|---|---|---|
| `match_ps_hash` | 像素着色器 DXBC 字节码的 CRC32C | **跨会话稳定** |
| `match_vs_hash` | 顶点着色器 DXBC 字节码的 CRC32C | **跨会话稳定** |
| `match_first_index` | `DrawIndexed` 的 `StartIndexLocation` | 稳定（几何数据） |
| `match_index_count` | `DrawIndexed` 的 `IndexCount` | 稳定（几何数据） |
| `hash` | 纹理哈希（descriptor 或 pixel）；`*` / 省略表示任意 | descriptor 可能漂移 |

- **同一规则内为 AND**：写出来的条件必须全部满足；省略的条件视为通配。
- **规则之间为 OR**：按 `match_priority` 取最大者，相同优先级取文件中靠前的规则。
- **通配符优先**：`hash = *` 规则走特快通道，命中即决策，优先于任何纹理哈希规则；`wildcard_rules` 内部仍按 `match_priority` 降序预排序。
- **安全约束**：规则至少要有 `hash`（非 `*`）、`match_ps_hash`、`match_vs_hash` 之一；`hash = *` 且没有着色器条件的规则会被拒绝加载，避免误伤整个场景。
- 运行时取不到某个锚点（例如事件没有触发，值为 0）时，写了该条件的规则**不会**命中，宁可漏抓也不误伤。

`hash_mode` 支持两种纹理哈希模式（主面板 `Hash Mode` 下拉框同步）：

- `descriptor`（默认，快）：`CRC32C("<Width>_<Height>_<Format>_<MipLevels>_<ArraySize>")`，只读 `D3D11_TEXTURE2D_DESC`。尺寸与格式相同的纹理会共享同一哈希。
- `pixel`（准，但覆盖有限）：对纹理像素内容做 CRC32C，每张纹理最多扫描 512 KiB（超出按固定步长抽行）。**与当前模式无关地**在 `init_resource` / `update_texture_region` 中计算，因此在运行中切换模式也能立刻看到此前加载的纹理；GPU 直接写入、从不经过 CPU 上传的纹理仍然没有像素哈希。`draw_indexed` 绝不回读 GPU。

- `match_first_index` / `match_index_count` 支持：单值 `420`、列表 `0, 512`、闭区间 `100-500`、开区间 `100-` / `-500`、通配 `*`。
- `match_priority`：数值越大越优先。
- `handling`：
  - `skip`：命中后 `draw_indexed` 返回 `true`，取消整次绘制。
  - `replace` / `null`：把命中槽位绑定为 1x1 全透明 SRV，绘制继续。
- `active`：`1` / `0`，默认 `1`。
- **`hotkey`**：运行时热键，打包成 `(vk << 24) | (alt << 16) | (ctrl << 8) | shift`（与 ShaderToggler 的 `ToggleKey` 同格式），`0` / 省略表示未绑定。按下时**翻转该规则的 `active`**，立即生效、立即重发规则表；不自动写盘，需要点 `Save to INI` 才持久化。**不用手写**：`Group settings` 里点 `Capture shortcut`，按任意键即捕获（`Esc` 取消、`Backspace` 清除），支持 `Alt`/`Ctrl`/`Shift` 组合，撞键时面板会提示。
- **`name`**：组名（面板 `List of Toggle Groups` 与 INI 都显示）。节名仍然是 `[TextureOverride_<i>]`——加载器只认 `textureoverride` 前缀，改名会丢掉整段规则，所以名字单独存成 `name =`。

所有 `[TextureOverride*]` 段落都会被读取；同时兼容旧的 `[TextureOverride]` + `0xHASH=1` 列表（等价于无上下文的 skip 规则）。旧式列表里 `0xHASH=0` 表示**该条目被禁用**（沿用 3DMigoto 习惯），不会生成规则。

`hash` 也接受别名 `match_hash` / `match_texture_hash`，`match_ps_hash` 接受 `ps_hash`，`match_vs_hash` 接受 `vs_hash`。

解析细节（写 INI 时会踩到的坑，均已处理）：

- **行尾注释**：`;` 或 `#` 前有空白即视为注释起点，例如 `hash = 83fd403f ; UI 血条`；整行 `;` / `#` 注释照旧。
- **UTF-8 BOM**：文件开头的 `EF BB BF` 会被跳过（记事本另存常带），否则首个 section 会被整段忽略。
- **`hash = 0` 会报错**：`0` 不是有效纹理哈希，写它等于退化成"任意纹理"（通配符），因此明确拒绝并给出带行号的警告；要匹配任意纹理请写 `hash = *`。
- **哈希必须是纯十六进制**：`-1`、`+1` 之类会被拒绝（`strtoul` 本身接受符号，这里做了前置校验）。

> **为什么 shader 字节码哈希是稳定的**：DXBC 容器头里的 16 字节 MD5 校验和（以及个别引擎写入的时间戳/构建信息）会随重新打包变化，所以插件**不对整段字节做 CRC**：`hash_shader_bytecode()` 解析容器，只对每个 chunk 的 `fourCC + size + payload` 做 CRC32C，容器头与校验和完全不参与。chunk 内容由编译器确定性产出，与进程地址、GPU、驱动、窗口尺寸、图集打包顺序无关。同一游戏版本 + 同一 shader 资源 ⇒ 任意次重启后哈希完全一致。

> **已知取舍**：同一像素着色器绘制的多个 UI 元素会共享 `match_ps_hash`，需要配合 `match_index_count` / `match_first_index` 区分；`hash` 仍可用于精确锁定。


---

## 1. 目录与依赖

```
tex_override/                          # 仓库根
├── README.md                          # 英文说明（简版）
├── README_zh.md                       # 本文件（中文详版）
├── LICENSE                            # MIT
├── .gitignore / .gitattributes
└── tex_override/                      # 工程目录
    ├── dllmain.cpp                    # 插件全部实现（单文件）
    ├── tex_override.vcxproj           # MSVC 工程
    ├── tex_override.sln               # 解决方案（Debug/Release x64）
    ├── examples/
    │   └── TextureOverride.example.ini  # 规则示例（改名为 TextureOverride.ini 使用）
    ├── tests/
    │   ├── cache_invalidation_test.cpp  # 墓碑环失效逻辑的多线程压力测试
    │   ├── view_binding_test.cpp        # view 句柄复用 / 绑定水位线测试
    │   └── ini_parse_test.cpp           # INI 解析器单元测试（代码块从 dllmain.cpp 抽取）
    └── reshade-sdk/                   # 第三方依赖，不随仓库提交
        ├── include/                   # ReShade v6.8.0 头文件
        └── deps/imgui/                # ImGui docking 1.92.5（IMGUI_VERSION_NUM == 19250）
```

`reshade-sdk/` 被 `.gitignore` 排除。首次构建前在**仓库根目录**执行：

```bat
git clone --recursive --branch v6.8.0 https://github.com/crosire/reshade.git reshade-sdk
:: 或使用已有的 ReShade 源码树
set RESHADE_SDK=D:\path\to\reshade
```

工程按以下顺序查找 SDK：环境变量 `RESHADE_SDK` → 工程目录下的 `reshade-sdk/` → 仓库根目录下的 `reshade-sdk/`（`$(ProjectDir)..\reshade-sdk`）。

ImGui 必须是 ReShade v6.8.0 的 `deps/imgui` 子模块版本，否则 `reshade_overlay.hpp` 会 `#error`。

### 项目设置（vcxproj 已配好）

| 项 | 值 |
|---|---|
| 配置类型 | Dynamic Library |
| C++ 标准 | `/std:c++17` |
| 预处理器 | `WIN32_LEAN_AND_MEAN;NOMINMAX;ImTextureID=ImU64;_CRT_SECURE_NO_WARNINGS`（Release 额外定义 `NDEBUG`） |
| 附加包含目录 | `$(ReShadeSdk)\include;$(ReShadeSdk)\deps\imgui` |
| 运行库 | 静态 CRT：`/MT`（Debug `/MTd`） |
| 附加选项 | `/utf-8` |
| 输出扩展名 | x64 → `.addon64` |

不要编译 `imgui.cpp`；ImGui 函数通过 ReShade 运行时注入的函数表转发。

---

## 2. 命令行构建

```bat
set RESHADE_SDK=D:\path\to\reshade
cd /d D:\VS_code_File\tex_override
MSBuild.exe tex_override\tex_override.sln /t:Rebuild /p:Configuration=Release /p:Platform=x64 /m
```

产物：

```
tex_override\bin\x64\Release\tex_override.addon64
```

已验证：MSVC 14.44 / v143，Release 与 Debug 两个配置均 0 error / 0 warning（`/W4 /permissive-` 亦为 0 warning），x64 PE，导出 `NAME` / `DESCRIPTION`，依赖仅 `KERNEL32.dll`、`USER32.dll`。

> Release 必须定义 `NDEBUG`（工程文件已配）。否则 `assert` 会把 `_wassert` 与断言字符串带进 addon（约 +19 KB），且不变量一旦被破坏会直接 `abort()` 掉游戏进程。

安装：放到游戏实际注入的 exe 同目录，ReShade 安装时勾选 Add-on 支持。

---

## 3. 使用流程

### 3.1 直接写 INI

在游戏 exe 同目录创建 `TextureOverride.ini`，写入规则后启动游戏（或进游戏后点 `Reload from INI`）。日志：

```
[Config] loaded N rules
[tex_override] INI rule mode; 1x1 null SRV created
```

### 3.3 面板与 HUD

主面板 `Texture Override`：

- 顶部：`Draws / Skipped / Rules applied / Cache hit / push_descriptors / shader resolved` 诊断计数，以及 `Last draw: ps=... vs=...`。
  - **统计口径**：`Draws` 只统计「进入过匹配流程」的绘制，也就是规则集非空或狩猎采集开启时的 `draw_indexed`；规则为空且未采集的 draw 会在最前面直接返回、不计数。因此它不是引擎的全量 Draw Call 数。
- Hunting 控制区：`Hunting Mode` 勾选、`Frames to collect` 滑块（1–120）、`Refresh`。
- `Stable-only new rules (hash = *)`：勾选时 Numpad 3 生成不依赖纹理哈希的规则（默认开启）。
- `Pixel hashes: N texture(s), X MB hashed`：像素哈希采集情况；为 0 时会提示原因。
- 候选列表：进入 Hunting 后累积 `Frames to collect` 帧的候选并集，随后**冻结**；每条候选显示 `纹理哈希 / PS 哈希 / VS 哈希 / fi / ic`。
  - **点选即预览**：点某一行（或用 Numpad 1/2 步进）会立刻把该候选对应的绘制**隐藏**，用"元素消失"确认它是哪一块画面，再按 Numpad 3 落规则。开关是候选列表上方的 `Preview selected (hide it live)`（默认开）；旁边的 `preview hidden: N draw(s)` 是预览跳过的绘制数（不计入 `Skipped`）。
- `List of Toggle Groups`（对齐 ShaderToggler 的行式列表，默认展开）：每行是 `X | [序号] | Active 勾选 | Edit | Find | 组名 (热键, is active)`。
  - `Edit` 打开 `Group settings`；`Find` 会开启 Hunting 并重新采集，随后用 Numpad 1/2 找元素、Numpad 3 增删该组的匹配项。
  - 组名来自 INI 的 `name =`（`Save to INI` 会写回），没名字时显示 `<unnamed>`。
- `Rule details`：原始哈希表 `Tex / PS / VS / FI / IC / Key / Mode / Act / Set / Del`（排查用；日常操作走上面的列表）。
- 底部：`Save to INI` / `Reload from INI`；存在未保存修改时显示黄色 `(Unsaved)`。
- 窗口默认尺寸会随分辨率收缩（屏幕高 < 600 或宽 < 800 时按 0.8 缩放）。

`Group settings` 独立窗口（点击行内 `Settings` / 列表里的 `Edit` 打开）：字段区可滚动，含 `name`、`hash`（支持 `*`）、`match_ps_hash`、`match_vs_hash`、`match_first_index`、`match_index_count`、`match_priority`、`handling`、`Active`、`hotkey`（`Capture shortcut` / `Clear` / `Cancel capture`），点 `Done` 应用并关闭。

> **通配符与 replace**：`hash` 为 `*`（或留空）时，`handling` 下拉框会被禁用并提示 `Wildcard rules only support skip`——通配符规则没有可替换的目标纹理。即使 INI 里写了 `replace`，加载时也会被强制降级为 `skip` 并在日志中告警。
>
> **优先级**：通配符规则（`hash = *`）命中即**立即返回**，优先级绝对高于任何带纹理哈希的规则——即使后者的 `match_priority` 更高。若需要两者共存，请把纹理规则也改成通配符形式（用 `match_ps_hash` + 几何条件区分）。

HUD：Hunting 开启时**用 ImGui 前景绘制列表直接画在屏幕坐标上**（不是 ImGui 窗口），并且由 `addon_event::reshade_overlay` 驱动 —— **关掉 ReShade 面板（Home）后依然常驻**（与 ShaderToggler 的 `ShaderTogglerInfo` 同一机制）。内容为 `Hunting Mode: ON` / `PREVIEW: candidate hidden`（预览开启且已选中候选时）/ 当前候选的 tex、ps、vs、fi、ic / 两行 Numpad 提示。按住左键直接拖拽即可移动，位置与透明度也可在面板 `HUD overlay` 里用 `HUD X / HUD Y / HUD Alpha` 调整；位置会被钳在屏幕内。

### 3.4 热键

**固定的 Hunting 键**（不可改）：

| 按键 | 条件 | 功能 |
|---|---|---|
| `Numpad 1` | Hunting Mode 开启 | 上一个候选（同时刷新预览） |
| `Numpad 2` | Hunting Mode 开启 | 下一个候选（同时刷新预览） |
| `Numpad 3` | Hunting Mode 开启 | 添加/删除当前候选的规则 |

**每规则热键**（可选，INI 的 `hotkey` 或面板捕获）：任意键 + `Alt`/`Ctrl`/`Shift` 组合，按下即翻转该规则的 `active`。语义与实现上的几个要点：

- 修饰键**严格相等**：绑 `A` 时按住 `Alt` 不会触发；绑 `Ctrl+A` 时只按 `A` 也不会触发。
- Hunting 期间 `Numpad 1/2/3` 归 Hunting 键所有，规则热键不会抢。
- 正在捕获新热键、或 ImGui 正在吃键盘（在输入框里打字，250 ms 抑制窗）时不触发。
- 多条规则绑同一个键时**全部一起翻转**（面板会提示"Same shortcut is used by N other rule(s)"）。
- 翻转只改运行时规则表，不写盘；`Save to INI` 才会把当前状态持久化。

Hunting Mode 关闭时数字键完全无效。Hunting 关闭且规则为空、也没有开预览时，`draw_indexed` 直接返回，不做任何绑定扫描。

---

## 4. 实现要点

- **稳定标识获取路径**：`addon_event::init_pipeline`（D3D11 的 `CreateVertexShader` / `CreatePixelShader` 触发，低频）从 sub-object 的 `shader_desc.code` 取 DXBC，解析容器后只对 chunk（fourCC + size + payload）算 `CRC32C`，**排除 16 字节 MD5 校验和与容器头**，记录 `pipeline -> {ps_hash, vs_hash}`；`destroy_pipeline` 移除条目并推进 epoch。
- **bind_pipeline 零锁**：该回调每帧数万次，覆盖 VS/HS/DS/GS/PS/CS 六个阶段，只做纯 store（`cmd_list_data::current_pipeline[]`），不查表、不加锁、不算哈希。哈希在 `draw_indexed` 里按需解析：句柄或 epoch 未变时只有几次整数比较，变化时才走线程局部缓存（未命中才短暂持有 `shader_mutex`）。
- **负缓存**：`lookup_shader_signature` 把 `{0,0}` 也写入线程局部缓存。没有经过 `init_pipeline` 的 pipeline 不会在每次 draw 抢 `shader_mutex`，只靠 `g_pipeline_epoch` 失效。
- **规则表一分为二**：`publish_rules` 把规则拆成 `wildcard_rules`（`hash = *`，按 priority 降序预排序）和 `index_by_texture_hash`（需要纹理哈希）。`has_texture_rules` 决定 `push_descriptors` 是否需要工作。
- **通配符 replace 降级**：`hash = *` 无法确定要替换哪张纹理，`publish_rules` 会把这类规则的 `replace` 强制改成 `skip` 并写一条 `[Rules] ... coerced to skip` 警告，避免把整个模型清空。
- **draw 预过滤**：哈希解析完成后，把「当前 pipeline 的 shader 条件可能命中的通配符规则」缓存进 `cmd_list_data::wildcard`（最多 8 条，按优先级）。缓存有效性用单调递增的 `rule_table::id` 判定（旧表回收后新表可能复用地址，绝不能比指针）。绝大多数 3D 模型的 shader 命中集为空，draw 里一次 `count == 0` 判断就跳过；命中时只再比对几何参数，**完全不遍历 SRV**。
- **draw 无原子操作**：`draws_seen` / `draws_skipped` / `rules_applied` / `srv_resolved` 在 draw 中只是 `cmd_list_data` 的普通 `uint32` 自增；`on_present` 每帧汇总到 `device_data` 的原子计数（面板读取）。
- **draw 慢速通道**：仅当通配符未命中且规则集里存在纹理规则时，才遍历六个阶段的 active SRV；`descriptor_hash` / `pixel_hash` 都按需计算并回写绑定缓存（线程局部缓存，未命中才 `GetDesc`）。
- **push_descriptors 极简**：回调开头先看 `g_texture_rules_active` 与 `g_hunting_collecting`；纯 `hash = *` 且不在采集时直接 `return`，连 `stage_bindings` 都不记录。**狩猎采集期间**（`g_hunting_collecting == true`）即使没有纹理规则也会记录 SRV，否则候选列表永远是空的；采集结束后自动恢复零开销。有纹理规则时**只写 `resource_view` 句柄**，绝不计算哈希；阶段用 `shader_stage` 位标志遍历，覆盖 VS/HS/DS/GS/PS/CS。`push_descriptors` 计数也改为 `cmd_list_data::local_push_updates` 普通变量，present 时汇总。
- **狩猎纪元（防悬垂句柄）**：挂机期间没有纹理规则，`push_descriptors` 早退会让 `stage_bindings` 一直保留早已销毁的 `view` 句柄。每次开启 Hunting 都推进 `g_hunting_collect_epoch`；**清空动作只在 `push_descriptors` 内发生**（新纪元到达后紧接着就是本帧的真实绑定），`draw_indexed` 侧只比对不清空：纪元不一致说明绑定还没刷新，本次直接跳过采集与纹理匹配，因此绝不会解引用陈旧 SRV。代价是新纪元后的第一批 draw（在引擎重新 push 之前）不参与采集，属于安全换取的可见性损失。
- **私有数据缓存与 UAF 防护**：`command_list::get_private_data` / `device::get_private_data` 内部都带 shared_mutex，热路径全部走线程本地缓存；**每次取用前先比对全局 epoch**，再使用缓存的裸指针，避免对象在其它线程销毁后地址复用造成 UAF。**所有回调（含 `destroy_pipeline` / `destroy_command_list`）统一走 `get_device_data()` / `get_cmd_data()`**，不再直接调用 `frame->get_private_data`；`destroy_command_list` / `destroy_device` 都是「先推进 epoch、后释放私有数据」，杜绝「读到旧 epoch 又拿到将释放指针」的窗口。
- **规则表回收**：旧 `rule_table` 保留最近 16 张（`kMaxRetiredRuleTables`），push 与回收都发生在 `publish_rules` 的同一次调用内（不再依赖 present 线程），避免未来引入异步 reload 时出现数据竞争。
- **descriptor hash 缓存精确失效**：缓存 key 为 `resource.handle`（`device::get_resource_from_view` 解析），但**只有 2D 纹理销毁才让它失效**。`destroy_resource` 里通过 `device_data::live_textures`（`init_resource` 时登记，仅 `resource_type::texture_2d`）判断类型，命中则把一个「墓碑句柄」写入无锁环形缓冲（512 项）；各线程在下次解析前按序摘掉这几条缓存——成本正比于**真正销毁的纹理数**，与帧率、draw 数无关。视图（SRV/RTV）的创建/销毁完全不影响该缓存。原先「每次资源销毁都无条件推进 epoch」会让所有线程的纹理缓存每帧被清空几十次（引擎每帧销毁几十个临时 CB/RTV），引发 `GetDesc` 风暴和掉帧。
- **句柄复用兜底**：`init_resource` 发现新纹理句柄已在 `live_textures` 中（说明上一张同句柄纹理的销毁事件丢失，例如注入前创建的资源）时，保守地整体失效一次并写警告；`g_resource_hash_epoch` 只保留两个兜底用途——线程落后超过墓碑环容量、以及设备拆除时无法判断类型。`destroy_device` 还会整体失效 descriptor / pixel 缓存。
- **view 句柄复用（根治）**：`bound_entry` 缓存的 descriptor / pixel 哈希以 view 句柄为键，而 D3D11 会把销毁后的句柄值分配给新 view。`on_destroy_resource_view` **无条件**把句柄登记进第二份无锁墓碑环（`g_dead_views`，容量 1024），各 `cmd_list_data` 用 `views_validated_seq` 水位线：`push_descriptors` 与 `draw_indexed`（采集与慢速通道）前各做一次 `atomic load + 比较`，仅在环推进时批量遍历 `active` 把相关条目**整条摘除**（连 view 一起清空——句柄可能已悬垂，任何再解析都是 UAF）。水位线只推进到"确实处理完"的位置；落后超过 256 条则整体丢弃绑定表（保守但正确，绑定会在下一次 push 重建）。
  - 追踪**不看**"当前是否有纹理规则"：否则会出现"规则被删光期间销毁的 view 不入环 → 重新启用规则后悬垂句柄进入解析路径"的竞态。代价只有一次 `fetch_add`（每帧几十次）。
  - `tests/view_binding_test.cpp` 用 4 个绑定/绘制线程 + 30 万次销毁重建（句柄池仅 64 个）验证。
- **跨 API 兼容**：`on_init_device` 用 `device::get_api()` 记录 `device_data::is_d3d11`。非 D3D11 时：`resolve_descriptor_hash` 直接返回 0（不做任何原生强转），`replace` 在决策点降级为 `skip` 并只警告一次；shader 哈希 / 通配符匹配不受影响。
- **零锁热路径**：descriptor hash / pixel hash / shader hash 全部线程局部缓存 + 全局 epoch；`push_descriptors` / `draw_indexed` 中没有任何加锁。规则表发布为不可变快照（`std::atomic<const rule_table*>`），写时复制整体替换。
- **候选预览**：hunting 时"点选/步进候选 → 立刻隐藏对应绘制"，用元素消失确认选中项（等价于 ShaderToggler/REST 的"步进即隐藏"）。实现是一个 32 位 seqlock（`candidate_preview`：hash / ps / vs / enabled + 序号），overlay 与 present 线程写、draw 线程读，读到写一半就放弃本帧预览，绝不做错匹配。匹配时先用 PS/VS 两个字段做廉价预筛（绝大多数 draw 一次比较就退出），命中同 shader 才遍历绑定 SRV，并复用慢速通道的 `bound_entry` / 线程局部哈希缓存，不额外产生 `GetDesc`。语义上等于"这条候选覆盖的所有绘制"，因此同一纹理+同一 shader 的多个 draw 会一起隐藏；预览与规则表完全解耦（没有合成规则、不写 INI），退出 Hunting 或清空候选时自动解除。它要求绑定表处于当前狩猎纪元（否则句柄可能悬垂），并且**优先于规则判定**——预览期间被隐藏的绘制不会再走规则匹配。
- **键盘边沿与规则热键**：`poll_key_edges()` 每帧只采样一次整张 256 键表（`t_key_down` / `t_key_pressed`），Hunting 键与规则热键共用同一份快照——早期"每个键各自 `GetAsyncKeyState` 边沿"会让同一个键被两处查询时互相吞键。热键判定是"主键按下沿 + 修饰键严格相等"，修饰键同时检查 generic 与 L/R 两个 VK 码。
- **热键捕获**：捕获与热键判定都读 **`effect_runtime` 的按键状态**（`is_key_down` / 由它派生的边沿），在 `addon_event::reshade_present` 里处理——与 ShaderToggler 的 `KeyData` 同一套数据源。**不能用 `GetAsyncKeyState`**：ReShade 用 inline hook 包了它，当面板打开且 `imgui_io.WantCaptureKeyboard` 为真时（点 `Capture shortcut` 之后正好是这个状态）`HookGetAsyncKeyState` 对所有键盘键返回 0，第三方 addon 调到的也是这个被 hook 的函数——这正是"点捕获后按任何键都没反应"的根因。流程：overlay 只置 `capturing_hotkey`，present 线程命中后写 `captured_hotkey` + `hotkey_capture_seq`，overlay 下一帧取值显示；跳过鼠标 1–6 与修饰键本身，`Esc` 取消、`Backspace` 清除，成功/取消都写日志，捕获期间每 3 秒还会打印一行 `[Hotkey] waiting for a key... (runtime sees N key(s) down)` 便于排查。`Group settings` 的字段区放在可滚动子窗口里，窗口被缩小或行数变多时热键行不会掉到看不见的位置。
- **HUD 常驻（为什么必须用 `reshade_overlay`）**：`register_overlay` 注册的窗口只在面板打开时被调用（ReShade 的 `runtime_gui.cpp` 里 `if (widget.title == "OSD" ? show_splash_window : !_show_overlay) continue;`），所以 HUD 不能挂在那里；`addon_event::reshade_overlay` 是无条件派发的，而且那一帧的早退判断里有 `&& !has_addon_event<addon_event::reshade_overlay>()` —— **只要注册了这个事件，即使面板被 Home 关掉，ReShade 也会继续跑 ImGui 帧并渲染 draw data**。这正是 ShaderToggler 的 `ShaderTogglerInfo` 能常驻的原因。绘制本身用 `GetForegroundDrawList()` 按屏幕坐标画（不是窗口），所以不存在停靠/被面板遮挡；拖拽用手动命中测试，有窗口在鼠标下时让给窗口。
- **像素哈希**：在 `init_resource`（有 `initial_data`）与 `update_texture_region`（CPU 上传）中计算，**与当前 hash_mode 无关**，因此运行中切换模式仍能看到已加载纹理；每张纹理最多扫描 512 KiB，超出按固定步长抽行；draw 中没有任何 `CopyResource` / `Map` 回读。
- **候选冻结**：进入 Hunting 或点 `Refresh` 后，累积 `Frames to collect` 帧的候选并集，达到目标后停止采集并冻结列表。候选键是 `(纹理哈希, PS 哈希, VS 哈希)` 三元组。
- **候选批量插入**：`record_draw_candidates` 先在栈上收集（固定 32 项，无堆分配），一次 `lock_guard` 批量写入 `frame_candidates`，一个 draw 只抢一次锁。
- **缓存面板**：`Active SRV Cache` 显示当前各线程 descriptor 缓存条目总数，`Cache hit` 显示命中率。
- **skip**：任意命中 skip 规则 → `return true`。
- **replace**：仅纹理规则支持；通配符规则会被降级为 `skip`（见上文）。纹理规则只替换哈希匹配的槽位。
- **INI**：手写解析器，支持 section / 整行与行尾注释 / UTF-8 BOM / 列表 / 区间 / 大小写不敏感键名；解析失败会带行号与节名写入日志；保存采用 `.tmp` + `MoveFileExW` 原子替换。
- **并发安全（规则发布）**：`publish_rules` 全程持有 `device_data::publish_mutex`。同一 device 挂多个 swapchain 时 ReShade 可能在不同线程回调 present/overlay，没有这把锁时并发 `push_back`/`erase` `retired_rule_tables` 会直接破坏堆。
- **编辑器一致性**：`match_first_index` / `match_index_count` 的编辑缓冲区为 512 字节；若载入时内容已被截断，点 `Done` 会被拒绝并提示（避免把截断后的错误区间写回规则）。Numpad 3 / 主面板加删规则会改变下标，因此发布后会请求关闭 Group settings，防止 `Done` 套用到另一条规则上。
- **UI 状态线程归属**：`settings_*` / `edit_*` / `hud_*` 只允许 overlay 线程读写。热键在 present 线程触发，因此 `toggle_rule_for_candidate` 不再直接改这些字段，而是置 `device_data::request_close_settings`（原子），由 `draw_overlay` 开头落实——`settings_error` 是 `std::string`，跨线程 `clear()` 会破坏堆。
- **Release 定义 `NDEBUG`**：`stage_bindings::set` 里的 `assert` 是"`old.handle == 0` ⇒ 非空槽位 ≤ 127 ⇒ active 未满"这条不变量的文档化表达。若 Release 未定义 `NDEBUG`，`assert` 会把 `_wassert` 与断言字符串拖进 addon（实测 +19 KB），且不变量一旦被破坏会在游戏里 `abort()`。工程文件已显式加上（Debug 保持断言生效）。
- **UI 锁**：`draw_overlay` 只在开头用锁拷贝候选/计数器快照，规则表拷贝移到锁外（独立原子指针），随后执行 ImGui 绘制；按钮只记录意图，绘制结束后再落实修改。
- **热键**：`GetAsyncKeyState` 边沿检测（ReShade 6.8.0 无 `on_frame`）。
- **并发正确性自测**：`tests/cache_invalidation_test.cpp` 把墓碑环失效逻辑原样抽出来，用 6 个 worker 线程做 `resolve`/插入、同时单线程持续「销毁 + 重建」20 万次（含句柄复用、以及故意延迟排空以触发「落后整圈 → 整体清空」的兜底路径），结束后在每个 worker 自己的线程里排空并断言：**缓存里不存在属于旧世代（已销毁）句柄的条目**。构建与运行：

  ```bat
  cd tex_override\tests
  cl /nologo /std:c++17 /O2 /EHsc /utf-8 /Fe:cache_invalidation_test.exe cache_invalidation_test.cpp
  cache_invalidation_test.exe
  ```

  末行输出 `ALL PASSED` 视为通过（已实测 20 万次销毁 × 6 线程、连续 10 轮无 stale 条目）。其中包含一条回归用例：**进程产生的第一枚墓碑必须生效**（若环序号从 0 起，会与槽的初始 `stamp` 撞值而被永久跳过）。
- **解析器自测**：`tests/ini_parse_test.cpp` 的受测代码块由 `dllmain.cpp` **原样抽取**（`struct value_set` … `parse_hex_hash`），覆盖：十六进制（含 `0x` 前缀、8 位上限、拒绝 `-1`/`+1`/`0x`）、`u32` 溢出、`*`/空值/单值/列表/区间/开区间/反序区间/相邻区间合并、以及 `value_set_to_string` 往返。

  ```bat
  cl /nologo /std:c++17 /O2 /EHsc /utf-8 /Fe:ini_parse_test.exe ini_parse_test.cpp
  ini_parse_test.exe
  ```
- **view 绑定自测**：`tests/view_binding_test.cpp` 含两个确定性用例（进程第一枚 view 墓碑必须生效；句柄被复用后重绑必须先失效）与一个并发压测（4 个绑定/绘制线程 + 30 万次销毁重建、句柄池仅 64 个以放大复用、其中一个线程故意延迟校验以触发"落后超过批量上限 → 整体丢弃"兜底）。静默期断言：**存活绑定的哈希必须等于其句柄的当前世代**。

  ```bat
  cl /nologo /std:c++17 /O2 /EHsc /utf-8 /Fe:view_binding_test.exe view_binding_test.cpp
  view_binding_test.exe
  ```

---

## 5. 常见构建问题

**MSB6001，提示 `HTTP_PROXY` / `http_proxy` 重复键**

```bat
set http_proxy=
set https_proxy=
set HTTP_PROXY=
set HTTPS_PROXY=
```

**`#error Unexpected ImGui version`**

`deps/imgui/imgui.h` 不是 19250 的 docking 版本。

**`C4819`**

工程已加 `/utf-8`；自建项目时补上。

---

## 6. 已知限制

1. **描述符碰撞**：`Width_Height_Format_MipLevels_ArraySize` 相同的纹理共享哈希。
2. `hash` 在 descriptor 模式下是元数据哈希；pixel 模式是插件自己的内容 CRC32C，不与 3DMigoto 的像素哈希保证一致。请使用候选列表里的值。
3. pixel 模式只覆盖经 CPU 上传的纹理（`init_resource` 带 `initial_data` 或 `UpdateSubresource`），且每张纹理最多采样 512 KiB；GPU 直接写入的纹理没有像素哈希，匹配时会被跳过。
4. **稳定锚点的边界**：`match_ps_hash` / `match_vs_hash` 绑定的是同一份 DXBC，因此同一 shader 的不同绘制需要靠 `match_index_count` / `match_first_index` / `hash` 区分；若游戏在运行时用同一份源码重新编译出不同字节（少见），哈希会变化。
5. **通配符不支持 replace**：`hash = *` 无法确定要替换哪张纹理，配置成 `replace` 会被强制降级为 `skip`（日志有警告），避免整块模型消失。
6. 检查 VS/HS/DS/GS/PS/CS 六个阶段的 SRV；不处理 UAV/RTV/DSV/采样器。
7. 只拦截 `draw_indexed`；纯 `Draw` 与间接绘制未覆盖。
8. `replace` 绑定的是 1x1 透明 SRV，不支持加载替换图片。
9. **跨 API 兼容**：插件只对 D3D11 做了完整验证。检测到 D3D12 / Vulkan / OpenGL 时不会崩溃：`replace` 自动降级为 `skip`（日志警告一次），基于纹理哈希的匹配失效（`resolve_descriptor_hash` 直接返回 0），但 `match_ps_hash` / `match_vs_hash` 与 `hash = *` 规则仍然工作。
10. **狩猎纪元后的首批 draw**：开启 Hunting 会推进纪元，强制丢弃旧的 SRV 绑定。若引擎采用「绑定不变就不重新 `SetShaderResources`」的策略，新纪元后的前几次 draw 可能采集不到候选（等待一次真实绑定）。这是为了不触碰挂机期间可能已销毁的句柄而做的安全取舍。
11. **注入前创建的纹理**：`live_textures` 只登记插件收到 `init_resource` 之后的 2D 纹理。极少数在注入前就已创建、之后才被销毁并复用句柄的纹理，其 descriptor 哈希可能残留一轮；`init_resource` 的句柄复用检测会在此后首次复用该句柄时整体失效一次。
12. **view 句柄复用（已根治，见实现要点）**：早期版本中 `stage_bindings` 的哈希以 view 句柄为键，句柄被驱动复用成同一数值时会保留旧哈希。现在由「已销毁 view 的墓碑 + 命令列表水位线」精确摘除相关绑定。残余：`t_pixel_hits`（线程局部、以 view 句柄为键）无法跨线程精确摘除，只在 **pixel 模式** 下随 view 销毁整体失效；descriptor 模式不受影响。
13. **`replace` 不会自动恢复原绑定**：ReShade 的 `draw_indexed` 在绘制**执行前**触发，回调里无法"画完再还原"。因此被替换的槽位会一直保持 1x1 透明，直到引擎自己重新绑定。若同一帧内有多个元素共用该槽位且不重绑，后续元素也会一起变透明。
14. **epoch 是快照，不是锁**：私有数据访问靠"先比对全局 epoch"避免地址复用 UAF。若引擎在另一个线程销毁命令列表、而本线程恰在 `present` 中汇总它的计数器，仍存在极窄窗口（`flush_command_counters` 已改为每次迭代重新校验以缩短它）。彻底消除需要引用计数，未实现。
15. **规则热键的边界**：键码是 Windows `VK_*`，**不区分左右修饰键**（绑 `RAlt` 会按 `Alt` 记录）；`Backspace` / `Esc` 被保留给"清除/取消"，不能作为绑定键；鼠标左/右/中/X1/X2 也不参与捕获（避免点按钮时被当成绑定）；多条规则同键会同时翻转（面板有提示但不阻止）；热键只在规则表非空、且不在捕获/输入状态时生效。

---

## 7. 疑难排查：长时间游玩后系统重启（蓝屏）

**先厘清责任边界**：这是一个用户态 DLL，**不可能**直接让系统重启。系统重启只有三种可能：

1. 内核 bugcheck（蓝屏）后 Windows 自动重启 —— 最常见，通常是显卡驱动触发；
2. WHEA 硬件错误（CPU/内存/PCIe）—— 与插件无关；
3. 电源/温度保护 —— 与插件无关。

插件能做的只有一件事：**把非法状态喂给显卡驱动**，由驱动引发内核错误。唯一符合这个描述的是 `replace`（在 `draw_indexed` 回调里调用 `SetShaderResources` 改写管线状态）。

### 第一步：拿到 bugcheck 代码（决定后续方向）

以管理员身份打开 PowerShell：

```powershell
# 最近的蓝屏记录（BugCheck / 事件 ID 1001、41）
Get-WinEvent -FilterHashtable @{LogName='System'; Id=1001,41} -MaxEvents 20 |
  Select-Object TimeCreated, Id, ProviderName, Message | Format-List

# 是否同时出现 WHEA（硬件错误）或显卡驱动报错
Get-WinEvent -LogName System -MaxEvents 300 |
  Where-Object { $_.ProviderName -match 'WHEA|nvlddmkm|amdkmdag|igfx' } |
  Select-Object TimeCreated, ProviderName, Id, Message | Format-List
```

对照结果：

| 代码 / 现象 | 含义 | 下一步 |
|---|---|---|
| `0x116` `VIDEO_TDR_FAILURE` | 显卡驱动超时/崩溃 | 与插件高度相关 → 做第二步 |
| `0x117` `VIDEO_TDR_TIMEOUT_DETECTED` | 同上 | 同上 |
| `0x124` `WHEA_UNCORRECTABLE_ERROR` | 硬件（CPU/内存/供电） | 与插件无关，查散热/内存/电源 |
| `0x1A` / `0x3B` / `0xC2` | 内存管理 / 堆损坏 | 可能与其他软件冲突，先只留 ReShade |
| 没有蓝屏记录但机器重启 | 电源/温度保护 | 记录 GPU 温度与功耗 |

也可以看 `C:\Windows\MEMORY.DMP` 与 `C:\Windows\Minidump\*.dmp`，用 WinDbg 的 `!analyze -v` 看 `MODULE_NAME` 是否指向 `nvlddmkm.sys` / `amdkmdag.sys`。

### 第二步：用安全模式做二分

在 `TextureOverride.ini` 的 `[Settings]` 里加一行：

```ini
[Settings]
safe_mode = 1
```

安全模式会**完全关闭**插件里两个"有副作用"的动作：

- `replace` 一律降级为 `skip`（不再于回调内调用 `SetShaderResources`）；
- 关闭像素哈希（不再扫描 CPU 纹理数据），`hash_mode` 强制回到 `descriptor`。

日志会打印 `[tex_override] SAFE MODE: replace coerced to skip, pixel hashing disabled`，面板顶部也会显示红色 `SAFE MODE` 提示。

- **safe_mode = 1 跑 2 小时不复现** → 问题出在 `replace` 路径（或像素哈希）。此时只用 `skip` 类规则即可长期安全运行；若要继续用 `replace`，请把复现时间点前后的规则发我，我按具体槽位/纹理缩小范围。
- **safe_mode = 1 仍然重启** → 与插件的"写入"路径无关。接着做第三步。

### 第三步：排除插件本身

把 `tex_override.addon64` 改名/移出 ReShade 目录，只保留 ReShade 本体，同样跑 2 小时：

- 仍然重启 → 与插件无关（ReShade、其他插件、驱动、硬件）；
- 不再重启 → 回到第二步，把该次的 `ReShade.log` 全文发我。

### 插件自带的长跑探针

无论哪种情况，`ReShade.log` 里每分钟会有一行：

```
[Watch] up=3600s draws=1283902 skipped=37 rules=4 live_tex=812 pix_hash=0 cand=0 srv_cache=210 dead_tex=9123 dead_view=20481 safe=0
```

`live_tex` / `pix_hash` / `cand` 若随时间单调增长到数十万，说明是**无界增长**（内存压力路径），而不是驱动非法状态；`dead_tex` / `dead_view` 是墓碑序号，只增不减属正常。这行日志在蓝屏后仍留在文件里（ReShade 每帧刷新），是定位的关键证据。

### 已知的可疑点（按可能性排序）

1. `replace` 在 `draw_indexed` 回调内改写引擎的 SRV 绑定 —— 唯一会改变驱动状态的行为（限制 13 也源于它）。
2. 长时间运行后 `pixel_hashes` / `live_textures` 随游戏资源增长（属于内存压力，不是非法状态）。
3. `resolve_descriptor_hash` 里"view 在 revalidate 与解析之间被销毁"的窄窗 —— 已加安全网：不在 `live_textures` 里的资源直接返回 0，绝不对其调用 `GetType`/`GetDesc`。
