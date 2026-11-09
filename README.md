# Texture Override (INI Rules)

一个由 INI 驱动的 [ReShade](https://reshade.me) 附加组件，用来按纹理哈希、着色器字节码哈希与绘制几何上下文组合匹配绘制调用，并执行 **skip**（跳过绘制）或 **replace**（替换纹理为 1×1 透明）。

面向 D3D11 引擎的动态图集场景：纹理描述符哈希跨会话会漂移，但着色器字节码哈希不会。

---

## 特性

- **多维度复合匹配** — 条件之间是 AND，规则之间是 OR（按 `match_priority` 取优先）
  - 纹理哈希（descriptor 或 pixel 内容哈希，可写 `*` 通配）
  - **像素 / 顶点着色器字节码哈希**（跨会话稳定锚点）
  - `first_index` / `index_count` 几何范围
- **跨会话稳定的着色器哈希** — 对 DXBC 容器的 chunk 做 CRC32C，跳过 MD5 校验和、偏移表与总大小；同一份 shader 资源任意次重启结果相同
- **两条匹配路径**
  - 快速通道：`hash = *` 规则只需着色器 + 几何条件，完全不读取 SRV 列表
  - 慢速通道：仅在存在纹理哈希规则时才遍历活跃 SRV，哈希按需计算并按线程缓存
- **Hunting Mode** — 运行中采集候选（纹理哈希 + PS/V 哈希 + 几何参数），小键盘一键生成/删除规则
- **零热路径开销** — `bind_pipeline` / `push_descriptors` / `draw_indexed` 均为无锁、无分配、无 atomic RMW（计数器在 present 时汇总）
- **ImGui 面板** — 实时统计、候选列表、规则表格、规则设置编辑、HUD 悬浮窗

---

## 安装

1. 安装 **ReShade 6.8.0**（附加组件 API v20）
2. 将编译好的 `tex_override.addon64`（或 `.addon32`）放入游戏目录下 ReShade 的 `addons` 文件夹
3. 启动游戏，在 ReShade 叠加层中启用 **Texture Override**
4. 配置文件 `TextureOverride.ini` 会读取自 **宿主可执行文件所在目录**

> ⚠️ 仅在 **Direct3D 11** 上支持 `replace`；D3D12/Vulkan 下 `replace` 会降级为 `skip` 并输出一次警告，着色器哈希与 `skip` 仍可用。

---

## 配置语法

```ini
[Settings]
hash_mode = descriptor      ; descriptor (快) | pixel (精确，仅 CPU 上传的纹理有哈希)

[TextureOverride_Example]
hash = 0x83fd403f           ; 纹理哈希；写 * 表示任意纹理（可省略）
match_ps_hash = 0x1A2B3C4D  ; 可选，像素着色器字节码 CRC32C
match_vs_hash = 0x5E6F7A8B  ; 可选，顶点着色器字节码 CRC32C
match_first_index = 0       ; 可选，DrawIndexed.StartIndexLocation，支持 0 / 0,512 / 100-500 / 100- / -500 / *
match_index_count = 420     ; 可选，DrawIndexed.IndexCount
match_priority = 1          ; 可选，越大越优先（默认 0）
handling = skip             ; skip | replace
active = 1                  ; 0 关闭该规则
```

### 布尔逻辑

- **同一规则内**：所有写出的条件为 **AND**；未写出的条件视为通配。
- **不同规则间**：**OR**，按 `match_priority` 取最大者；同优先级取文件中靠前者。
- **安全约束**：规则至少要写 `hash` / `match_ps_hash` / `match_vs_hash` 之一。
- 纯 `hash = *` 且无着色器条件会被拒绝加载，避免误伤整个场景。
- `hash = *` 的 `replace` 会在加载时降级为 `skip`（无法确定替换目标）。

### 键名别名

| 标准键名 | 兼容别名 |
| --- | --- |
| `hash` | `match_hash`, `match_texture_hash` |
| `match_ps_hash` | `match_pixel_shader_hash`, `ps_hash` |
| `match_vs_hash` | `match_vertex_shader_hash`, `vs_hash` |
| `handling = replace` | `handling = null` |

---

## 示例

**只靠稳定锚点，跨会话永不失效（推荐）：**

```ini
[TextureOverride_UI_Icon]
hash = *
match_ps_hash = 0x9C2F1E4D
match_first_index = 0
match_index_count = 6
handling = skip
```

**精确匹配特定纹理：**

```ini
[TextureOverride_Bloom_Texture]
hash = 0x83FD403F
handling = replace
```

---

## Hunting Mode

面板勾选 **Hunting Mode** 后：

| 热键 | 动作 |
| --- | --- |
| `Numpad 1` | 上一个候选 |
| `Numpad 2` | 下一个候选 |
| `Numpad 3` | 为当前候选添加 / 删除规则 |

- 候选 = `(纹理哈希, PS 哈希, VS 哈希)` 三元组，同时展示 `first_index` / `index_count`
- 默认勾选 **Stable-only new rules**：生成规则时写 `hash = *`，只保留稳定锚点
- 关闭该选项则写入纹理哈希，做精确定位
- 采集 N 帧（默认 10，可调 1–120）后暂停并冻结候选列表

---

## 哈希说明

### 着色器字节码哈希（跨会话稳定）

不直接 CRC 整个 DXBC：容器头含 16 字节 MD5 校验和（部分引擎还写时间戳），重新打包后会变。本插件解析容器，**只对每个 chunk 的 `fourCC + size + payload` 做 CRC32C**。

```
DXBC 容器布局：
0x00 'DXBC'
0x04 checksum[16]          ← 跳过
0x14 unknown
0x18 total_size
0x1C chunk_count
0x20 chunk_offsets[...]    ← 跳过
     每个 chunk: fourCC + size + payload   ← 参与哈希
```

容器结构异常时退化为「跳过前 20 字节」；非 DXBC（例如裸 DXIL）整体哈希。

### 纹理哈希

- **descriptor**：`CRC32C(Width_Height_Format_MipLevels_ArraySize)`，快速但图集重排后可能漂移
- **pixel**：对纹理内容做 CRC32C，在 `init_resource` / `update_texture_region` 的 CPU 上传阶段计算，每张纹理最多扫描 512 KiB（超出按固定步长抽行），**绝不回读 GPU**

---

## 构建

需要一个 ReShade 附加组件开发环境（`reshade.hpp`、ImGui 与 ReShade 附加组件 API v20）。

```
C++17 / MSVC（Win32 或 x64，与 ReShade 架构一致）
依赖：ReShade Add-on SDK 6.8.0
```

在 `DllMain` 中通过 `reshade::register_addon` 注册，并挂接 `init_device` / `draw_indexed` / `push_descriptors` / `present` 等事件。

---

## 已知限制

- `replace` 仅支持 **D3D11**（其它 API 降级为 `skip`）
- `hash = *` 无法确定替换目标，只能 `skip`
- pixel 哈希只覆盖通过 CPU 上传的纹理；GPU 直写纹理没有哈希
- 面板中的 `Draws` 统计的是**进入匹配流程的绘制**（规则集非空或采集开启时），不是引擎的全量 Draw Call 数

---

## 许可

MIT
