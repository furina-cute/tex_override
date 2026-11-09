# Texture Override (INI Rules)

---
**[Chinese](https://github.com/furina-cute/tex_override/blob/main/README_zh.md)**

**[English](https://github.com/furina-cute/tex_override/blob/main/README.md)**

---
A [ReShade](https://reshade.me) injectable driven by INI rules, designed to match draw calls by texture hash, shader bytecode hash, and draw geometry context combination, and perform **skip** (skip the draw call) or **replace** (replace texture with 1x1 transparent).

Targeting D3D11 dynamic atlas scenes: texture descriptor hash drifts across sessions, but shader bytecode hash remains stable.

## Features

- **Multi-dimensional composite matching** — Conditions within a rule are AND; rules themselves are OR (prioritized by `match_priority`)
  - Texture hash (descriptor or pixel content hash, can write `*` as wildcard)
  - **Pixel / vertex shader bytecode hash** (stable cross-session anchor)
  - `first_index` / `index_count` geometric range
- **Cross-session stable shader hash** — CRC32C on DXBC chunk `fourCC + size + payload`, skipping MD5 checksum, offset table and total size; same shader resource re-packed gives identical result across restarts
- **Two matching paths**
  - Fast path: `hash = *` rules only need shader + geometric conditions, completely bypass SRV list reading
  - Slow path: only traverses active SRV when texture hash rules exist; hashes computed on-demand and thread-cached
- **Hunting Mode** — In-run collection of candidates (texture hash + PS/VS hash + geometric parameters), NumPad one-click generate/delete rules
- **Zero hot-path overhead** — `bind_pipeline` / `push_descriptors` / `draw_indexed` are lock-free, allocation-free, no atomic RMW (counters summarized at present)
- **ImGui panel** — Real-time stats, candidate list, rule table, rule settings editor, HUD overlay

## Installation

1. Install **ReShade 6.8.0** (Add-on API v20)
2. Place the compiled `tex_override.addon64` (or `.addon32`) into the game's ReShade `addons` folder
3. Launch the game, enable **Texture Override** in the ReShade overlay
4. Configuration file `TextureOverride.ini` reads from the **host executable's directory**

> ⚠️ **Direct3D 11 only** supports `replace`; D3D12/Vulkan will downgrade to `skip` and output a single warning; shader hash and `skip` still work.

## Configuration Syntax

```ini
[Settings]
hash_mode = descriptor      ; descriptor (fast) | pixel (exact, only textures uploaded to CPU have hash)

[TextureOverride_Example]
hash = 0x83fd403f           ; texture hash; write * for any texture (optional)
match_ps_hash = 0x1A2B3C4D  ; optional, pixel shader bytecode CRC32C
match_vs_hash = 0x5E6F7A8B  ; optional, vertex shader bytecode CRC32C
match_first_index = 0       ; optional, DrawIndexed.StartIndexLocation, supports 0 / 0,512 / 100-500 / 100- / -500 / *
match_index_count = 420     ; optional, DrawIndexed.IndexCount
match_priority = 1          ; optional, higher is more prioritized (default 0)
handling = skip             ; skip | replace
active = 1                  ; 0 disable this rule
```

### Boolean Logic

- **Within a single rule**: all written conditions are **AND**; unspecified conditions act as wildcard.
- **Between different rules**: **OR**, pick the maximum by `match_priority`; same priority picks the one appearing first in the file.
- **Safety constraint**: at least one of `hash` / `match_ps_hash` / `match_vs_hash` must be written.
- Pure `hash = *` with no shader conditions will be **rejected on load**, to prevent accidental scene-wide matching.
- `hash = *` with `replace` degrades to `skip` at load time (cannot determine replacement target).

### Key Aliases

| Standard Key | Compatible Alias |
| --- | --- |
| `hash` | `match_hash`, `match_texture_hash` |
| `match_ps_hash` | `match_pixel_shader_hash`, `ps_hash` |
| `match_vs_hash` | `match_vertex_shader_hash`, `vs_hash` |
| `handling = replace` | `handling = null` |

## Examples

**Anchor-only, never invalid across sessions (recommended):**

```ini
[TextureOverride_UI_Icon]
hash = *
match_ps_hash = 0x9C2F1E4D
match_first_index = 0
match_index_count = 6
handling = skip
```

**Exact match for a specific texture:**

```ini
[TextureOverride_Bloom_Texture]
hash = 0x83FD403F
handling = replace
```

## Hunting Mode

With **Hunting Mode** checked in the panel:

| Key | Action |
| --- | --- |
| `Numpad 1` | Previous candidate |
| `Numpad 2` | Next candidate |
| `Numpad 3` | Add / remove rule for current candidate |

- Candidate = `(texture hash, PS hash, VS hash)` triad, also shows `first_index` / `index_count`
- Default **Stable-only new rules** checked: generates rules with `hash = *`, keeping only stable anchors
- Disabled: writes texture hash for precise localization
- After collecting N frames (default 10,可调 1–120) the list pauses and freezes

## Hash Notes

### Shader Bytecode Hash (cross-session stable)

Do not CRC the entire DXBC: the container header contains a 16-byte MD5 checksum (some engines also write timestamps), which changes on re-packaging. This plugin parses the container, **only CRC32C on each chunk's `fourCC + size + payload`**.

```
DXBC Container Layout:
0x00 'DXBC'
0x04 checksum[16]          ← skip
0x14 unknown
0x18 total_size
0x1C chunk_count
0x20 chunk_offsets[...]    ← skip
     Each chunk: fourCC + size + payload   ← participates in hash
```

If container structure is abnormal, degrades to "skip first 20 bytes"; non-DXBC (e.g. raw DXIL) whole-hash.

### Texture Hash

- **descriptor**: `CRC32C(Width_Height_Format_MipLevels_ArraySize)`, fast but may drift after atlas reordering
- **pixel**: CRC32C on texture content, computed at CPU upload stage in `init_resource` / `update_texture_region`, each texture scans at most 512 KiB (beyond fixed-stride row skipping), **never reads from GPU**

## Building

Requires a ReShade add-on development environment (`reshade.hpp`, ImGui, and ReShade Add-on SDK v20).

```
C++17 / MSVC (Win32 or x64, consistent with ReShade architecture)
Dependencies: ReShade Add-on SDK 6.8.0
```

Registered via `reshade::register_addon` in `DllMain`, and hook `init_device` / `draw_indexed` / `push_descriptors` / `present` events.

## Known Limitations

- `replace` only supports **D3D11** (other APIs downgrade to `skip`)
- `hash = *` cannot determine replacement target, can only `skip`
- Pixel hash only covers textures uploaded via CPU; GPU-direct written textures have no hash
- Panel's `Draws` statistic counts **draw calls entering the matching flow** (non-empty rule set or hunting mode active), **not** the engine's total draw call count

## License

[MIT](https://github.com/furina-cute/tex_override/blob/main/LICENSE)
