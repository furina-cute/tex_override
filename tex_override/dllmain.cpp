/*
 * tex_override — INI-driven TextureOverride
 * ReShade 6.8.0 add-on (API version 20), Direct3D 11
 *
 * ============================================================================
 * 1. 多维度复合匹配（解决纹理哈希跨会话漂移）
 * ============================================================================
 *
 * 背景：动态图集类引擎每次进程启动都会重新分配图集尺寸/打包顺序，
 * 于是“纹理描述符哈希”在同一逻辑资源上跨会话并不稳定。
 *
 * 本插件因此引入两类**跨会话稳定**的锚点，并允许与纹理哈希自由组合：
 *
 *   (a) 着色器字节码哈希 —— 主锚点
 *       match_ps_hash = <CRC32C(DXBC bytecode of pixel shader)>
 *       match_vs_hash = <CRC32C(DXBC bytecode of vertex shader)>
 *
 *   (b) 绘制几何上下文 —— 次锚点（已有能力，继续保留）
 *       match_first_index / match_index_count
 *
 *   (c) 纹理哈希 —— 精确定位（可选，可能漂移）
 *       hash = <descriptor 或 pixel hash>   或   hash = *（任意纹理）
 *
 * 稳定标识的获取路径（ReShade 6.8.0 add-on API）：
 *
 *   addon_event::init_pipeline
 *       D3D11 上由 ID3D11Device::CreateVertexShader / CreatePixelShader 触发。
 *       回调参数 subobjects 中，类型 vertex_shader / pixel_shader 的
 *       sub-object 数据是 shader_desc，其 code / code_size 就是引擎传入的
 *       DXBC 字节码。**不能直接 CRC 整段字节**：DXBC 头部含 16 字节 MD5
 *       校验和（部分引擎还会写时间戳），重新打包后会变。这里解析容器，
 *       只对 chunk（fourCC + size + payload）做 CRC32C，并记录：
 *
 *           pipeline( = ID3D11PixelShader*)  ->  { ps_hash, vs_hash }
 *
 *   addon_event::bind_pipeline
 *       D3D11 上由 PSSetShader / VSSetShader 触发，是每帧数万次的高频回调。
 *       因此这里**只写两个句柄**（纯 store，无锁、无查表、无哈希）：
 *
 *           cmd_list_data::current_pipeline[stage]
 *
 *   addon_event::draw_indexed
 *       句柄或 epoch 变化时才解析一次哈希（线程局部缓存，未命中才短暂加锁，
 *       每个 pipeline 每个线程最多一次）；之后只是两次整数比较。
 *
 * 为什么跨会话不变：
 *   DXBC chunk 内容由编译器确定性产出，由引擎资源包/可执行文件携带；
 *   与之无关的校验和、容器偏移表、总大小都被排除在哈希之外。
 *   与进程地址、GPU、驱动、窗口尺寸、图集打包顺序也都无关。
 *   同一游戏版本 + 同一份 shader 资源 => 任意次重启得到完全相同的 CRC32C。
 *
 * ============================================================================
 * 2. 规则语法（与旧版完全向后兼容）
 * ============================================================================
 *
 *   [TextureOverrideUID]
 *   hash = 83fd403f              ; 纹理哈希；写 * 表示任意纹理（可省略）
 *   match_ps_hash = 0x1a2b3c4d   ; 可选，像素着色器字节码哈希
 *   match_vs_hash = 0x5e6f7a8b   ; 可选，顶点着色器字节码哈希
 *   match_first_index = 0        ; 可选，DrawIndexed.StartIndexLocation
 *   match_index_count = 420      ; 可选，DrawIndexed.IndexCount
 *   match_priority = 1           ; 可选，越大越优先
 *   handling = skip              ; skip | replace
 *   active = 1
 *   hotkey = 0x1C300000          ; 可选，运行时热键（VK<<24 | alt<<16 | ctrl<<8 | shift）
 *                                ; 0/省略 = 未绑定；面板里点一下输入框即可捕获
 *
 * 布尔逻辑：
 *   - 同一规则内，所有“写出来”的条件之间是 AND（全部满足才命中）；
 *     没写的条件视为通配。
 *   - 不同规则之间是 OR：按 match_priority 取最大者，相同优先级取文件中
 *     靠前的规则。
 *   - 只写稳定锚点即可命中（hash = * + match_ps_hash + match_index_count），
 *     也可叠加纹理哈希做精确定位。
 *   - 安全约束：规则至少要写 hash / match_ps_hash / match_vs_hash 之一；
 *     纯 `hash = *` 且无着色器条件的规则会被拒绝加载，避免误伤整个场景。
 *
 * ============================================================================
 * 3. 匹配流程（draw_indexed 内，零锁，两条路径）
 * ============================================================================
 *
 *   publish_rules 把规则拆成两个集合：
 *     - rule_table::wildcard_rules        ：hash = *（按 priority 降序预排序）
 *     - rule_table::index_by_texture_hash ：需要纹理哈希的规则
 *
 *   特快通道（hash = *）：
 *     哈希解析完成后立刻做一次「预过滤」：把当前 pipeline 下 shader 条件可能
 *     命中的通配符规则缓存到 cmd_list_data::wildcard（最多 8 条，按优先级）。
 *     绝大多数 3D 模型的 shader 与该集合不相交，count == 0，draw 里一次字节
 *     比较就跳过；命中的也只需要再比对几何参数，**完全不读取 SRV 列表**。
 *     注意：通配符规则一旦命中就**立即返回**，优先级绝对高于任何带纹理哈希的
 *     规则（即使后者的 match_priority 更高）。并且通配符规则无法确定替换目标，
 *     `replace` 会在 publish 时降级为 `skip` 并写一条警告日志。
 *
 *   慢速通道（纹理哈希规则）：
 *     只有当通配符规则全部未命中、且规则集里确实存在纹理规则时才执行：
 *       1) 遍历命令列表的 active SRV 槽位（VS/HS/DS/GS/PS/CS 六个阶段）；
 *       2) 哈希按需计算（线程局部缓存，未命中才 GetResource/GetDesc）；
 *       3) 用 index_by_texture_hash 取候选，再校验 stable anchors / 几何参数。
 *
 *   push_descriptors 侧：
 *     - 规则集里没有纹理规则、且不在狩猎采集时，回调开头直接 return，
 *       完全不记录 stage_bindings；
 *     - 狩猎采集期间（g_hunting_collecting）即使只有 hash = * 规则也照常记录，
 *       否则候选列表永远是空的；采集结束后自动恢复零开销；
 *     - 有纹理规则时只写 resource_view 句柄，绝不计算任何哈希；
 *     - 阶段匹配用 shader_stage 位标志遍历，完整覆盖 VS/HS/DS/GS/PS/CS。
 *
 *   bind_pipeline 只写入 current_pipeline[]（VS/HS/DS/GS/PS/CS 六个阶段，纯 store，
 *   不加锁、不查表）。draw 发现句柄或 epoch 变化时才解析一次哈希，
 *   解析走线程局部缓存（含 {0,0} 负缓存），未命中才短暂持有 shader_mutex
 *   （每个 pipeline 每个线程最多一次），之后全是几次整数比较。
 *
 *   draw_indexed 里没有任何 atomic RMW：
 *     命中/跳过/绘制数都记在 cmd_list_data 的普通 uint32 上（local_draws /
 *     local_skipped / local_applied / local_srv_resolved），on_present 每帧
 *     汇总到 device_data 的原子计数里，供面板读取。
 *
 *   注意：面板的 `Draws` 统计的是「进入过匹配流程的绘制」——即规则集非空
 *   或狩猎采集开启时的 draw。规则为空且未采集时 draw 会在最前面直接返回，
 *   不会计数，因此它不是引擎的全量 Draw Call 数。
 *
 *   线程安全 / 生命周期：
 *     - get_cmd_data() / get_device_data() 每次都先比对全局 epoch，再使用
 *       线程本地缓存指针，避免对象在其它线程销毁后地址复用造成 UAF；
 *       **所有回调（含 on_destroy_pipeline / on_destroy_command_list）一律走这两个入口**，
 *       不允许直接调用 frame->get_private_data；
 *     - epoch 先推进、后释放私有数据（destroy_device / destroy_command_list），
 *       杜绝“读到旧 epoch 又拿到将释放指针”的窗口；
 *     - 旧规则表保留最近 16 张，回收在 publish_rules 内部完成（不再跨线程依赖 present）；
 *     - 狩猎采集纪元 g_hunting_collect_epoch：挂机期间 push_descriptors 早退，
 *       stage_bindings 可能残留已销毁的 view 句柄。每次开启 Hunting 推进 epoch，
 *       **清空动作只在 push_descriptors 内发生**（新纪元 + 紧随其后的真实绑定），
 *       draw 侧只比对、不清空：不一致就跳过本次采集/匹配，绝不使用未刷新的句柄。
 *
 *   descriptor 哈希缓存的精确失效（避免 GetDesc 风暴）：
 *     - 引擎每帧会销毁几十个临时 CB/RTV/DSV。若每次资源销毁都推进全局 epoch，
 *       所有线程的纹理哈希缓存每帧被清空几十次，下一次绘制全部 miss，
 *       每帧上千次 GetDesc —— 这正是掉帧来源；
 *     - 现在只有 **2D 纹理** 销毁才登记墓碑句柄（无锁环形缓冲，容量 1024），
 *       各线程按序摘掉对应条目，成本正比于真正销毁的纹理数；
 *     - 句柄复用：init_resource 时若句柄已在 live_textures 中（说明上一张同句柄
 *       纹理的销毁事件丢失），保守地整体失效一次并告警；
 *     - 视图（SRV/RTV）的创建/销毁完全不影响该缓存。
 *
 *   view 句柄复用的根治（限制 12）：
 *     - bound_entry 的 descriptor/pixel 哈希以 view 句柄为键。D3D11 会把销毁后的
 *       句柄值分配给新 view，若引擎用同一数值重绑同一槽位，set() 的 old == view
 *       快路径会把旧哈希当成新 view 的哈希（张冠李戴）；
 *     - 这里不做“每次绑定查一次戳表”（那要锁 + 哈希查找，落在每帧数万次的
 *       push_descriptors 上），而是复用同一套无锁环登记**已销毁的 view**，
 *       各命令列表用 views_validated_seq 水位线：push/draw 前一次 atomic load
 *       比较，只有环推进时才把这段时间内死掉的 view 从绑定表**整条摘除**
 *       （连 view 一起清空，绝不只清哈希——句柄可能已悬垂，再解析就是 UAF）；
 *     - 只在有纹理规则或狩猎采集时追踪，默认配置零开销。
 *
 * ============================================================================
 * 3.1 着色器哈希为什么能跨会话稳定
 * ============================================================================
 *
 *   不能直接 CRC 整个 DXBC：容器头里有 16 字节 MD5 校验和（以及个别引擎写入的
 *   时间戳/构建信息），重新打包后会变，但着色器本身没变。
 *
 *   因此 hash_shader_bytecode() 做两件事：
 *     - magic == 'DXBC'：解析容器，跳过 header（含 4..20 的 MD5 校验和）与
 *       偏移表，只对每个 chunk 的 fourCC + size + 数据做 CRC32C；
 *     - 容器结构异常时退化为跳过前 20 字节（magic + 校验和）。
 *
 *   chunk 内容（SHDR/SHEX/DXIL/ISGN/OSGN/RDEF...）由编译器确定性产出，
 *   与进程地址、GPU、驱动、图集打包顺序无关，因此同一份 shader 资源在任意次
 *   重启后得到同一个哈希。
 *
 * 性能：最热路径（hash = *）每次 draw 只做：一次 acquire load、两次指针比较、
 * 一次 shader 解析检查、若干整数比较；没有容器分配、没有锁、没有 SRV 遍历。
 * 纹理哈希只在确实存在纹理规则的配置下才在 draw 内按需计算。
 *
 * ============================================================================
 * 4. Hunting Mode
 * ============================================================================
 *
 *   - 只有 Hunting Mode 开启时 Numpad 1/2/3 生效。
 *   - 候选条目 = (纹理哈希, PS 哈希, VS 哈希) 三元组，面板同时展示几何参数。
 *   - 点选候选（或用 Numpad 1/2 步进）会**立刻隐藏**该候选覆盖的绘制，
 *     用"元素消失"确认选中项；预览与规则表完全解耦（不合成规则、不写 INI），
 *     可随时用面板上的 Preview 开关关掉。实现见 candidate_preview（seqlock）。
 *   - Numpad 3 创建规则时自动写入当前捕获的 ps/vs 哈希与 first_index/index_count；
 *     “Include texture hash” 关闭时写 hash = *，得到纯稳定锚点规则。
 *
 * 5. 每规则热键（运行中开关单条规则）
 * ============================================================================
 *
 *   - 规则的 hotkey 字段打包 (vk << 24) | (alt << 16) | (ctrl << 8) | shift，
 *     与 ShaderToggler 的 ToggleKey 同格式；0 表示未绑定。
 *   - 判定 = 主键按下沿 + 修饰键“严格相等”（绑 A 时按 Alt 不触发）。
 *   - 每帧只采样一次键盘边沿（poll_key_edges），Hunting 键与规则热键共用同一份
 *     快照；否则同一个键被两处查询时会互相吞掉边沿。
 *   - 捕获用原始按键状态（不走快照），避免与 present 线程抢同一个边沿；
 *     捕获期间与 ImGui 抢键盘期间（250ms 截止时间戳）都不触发。
 *   - 触发后翻转 active 并 publish_rules（持 publish_mutex），置 unsaved 标记；
 *     不自动写盘。
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d11.h>

#include <imgui.h>
#include <reshade.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace reshade::api;

// -----------------------------------------------------------------------------
// CRC-32C (Castagnoli)
// -----------------------------------------------------------------------------
static constexpr std::array<uint32_t, 256> build_crc32c_table()
{
	std::array<uint32_t, 256> table{};
	for (uint32_t i = 0; i < 256; ++i)
	{
		uint32_t crc = i;
		for (uint32_t bit = 0; bit < 8; ++bit)
			crc = (crc >> 1) ^ (0x82F63B78u & (0u - (crc & 1u)));
		table[i] = crc;
	}
	return table;
}

static constexpr std::array<uint32_t, 256> kCrc32cTable = build_crc32c_table();

static uint32_t crc32c_update(uint32_t crc, const void *data, size_t size)
{
	const auto *bytes = static_cast<const uint8_t *>(data);
	for (size_t i = 0; i < size; ++i)
		crc = (crc >> 8) ^ kCrc32cTable[(crc ^ bytes[i]) & 0xFFu];
	return crc;
}

static uint32_t crc32c(const void *data, size_t size)
{
	return crc32c_update(0xFFFFFFFFu, data, size) ^ 0xFFFFFFFFu;
}

// -----------------------------------------------------------------------------
// 着色器字节码哈希：跳过 DXBC 头部易变字段（MD5 校验和 / 容器头）
//
// DXBC 容器布局：
//   0x00 'DXBC'
//   0x04 uint8 checksum[16]     <- 随重新打包变化，必须跳过
//   0x14 uint32 unknown
//   0x18 uint32 total_size
//   0x1C uint32 chunk_count
//   0x20 uint32 chunk_offsets[chunk_count]
//   每个 chunk: fourCC + uint32 size + payload
//
// 这里只对 chunk 的 fourCC + size + payload 做 CRC32C，容器头完全不参与，
// 因此校验和/偏移表变化不会影响结果。容器结构异常时退化为跳过前 20 字节。
// -----------------------------------------------------------------------------
static uint32_t hash_shader_bytecode(const void *code, size_t size)
{
	const auto *bytes = static_cast<const uint8_t *>(code);
	if (bytes == nullptr || size == 0)
		return 0;

	if (size >= 0x20 && std::memcmp(bytes, "DXBC", 4) == 0)
	{
		uint32_t chunk_count = 0;
		std::memcpy(&chunk_count, bytes + 0x1C, sizeof(chunk_count));

		if (chunk_count != 0 && chunk_count <= 128 &&
			size >= 0x20 + static_cast<size_t>(chunk_count) * 4)
		{
			uint32_t crc = 0xFFFFFFFFu;
			bool valid = true;

			for (uint32_t i = 0; i < chunk_count && valid; ++i)
			{
				uint32_t offset = 0;
				std::memcpy(&offset, bytes + 0x20 + static_cast<size_t>(i) * 4, sizeof(offset));

				if (offset > size || size - offset < 8)
				{
					valid = false;
					break;
				}

				uint32_t chunk_size = 0;
				std::memcpy(&chunk_size, bytes + offset + 4, sizeof(chunk_size));

				if (chunk_size > size - offset - 8)
				{
					valid = false;
					break;
				}

				// fourCC + size + payload；不含容器头与校验和
				crc = crc32c_update(crc, bytes + offset, 8 + static_cast<size_t>(chunk_size));
			}

			if (valid)
				return crc ^ 0xFFFFFFFFu;
		}

		// 结构异常：至少跳过 magic + 校验和
		return crc32c(bytes + 20, size - 20);
	}

	// 非 DXBC 容器（例如裸 DXIL）：没有已知的可变头，整体哈希
	return crc32c(bytes, size);
}

// -----------------------------------------------------------------------------
// 整数集合：支持 0 / 0,512 / 100-500 / 100- / -500 / *
// -----------------------------------------------------------------------------
struct value_set
{
	bool any = true;
	std::vector<std::pair<uint32_t, uint32_t>> ranges;

	bool matches(uint32_t value) const
	{
		if (any)
			return true;
		for (const auto &range : ranges)
		{
			if (value >= range.first && value <= range.second)
				return true;
		}
		return false;
	}

	void add(uint32_t value)
	{
		any = false;
		ranges.emplace_back(value, value);
	}

	void normalize()
	{
		if (any || ranges.empty())
			return;

		std::sort(ranges.begin(), ranges.end());
		std::vector<std::pair<uint32_t, uint32_t>> merged;
		merged.push_back(ranges.front());

		for (size_t i = 1; i < ranges.size(); ++i)
		{
			auto &last = merged.back();
			const auto &current = ranges[i];

			if (current.first <= last.second ||
				(last.second != UINT32_MAX && current.first == last.second + 1))
			{
				last.second = std::max(last.second, current.second);
			}
			else
			{
				merged.push_back(current);
			}
		}

		ranges.swap(merged);
	}

	bool operator==(const value_set &other) const
	{
		return any == other.any && ranges == other.ranges;
	}

	bool operator!=(const value_set &other) const
	{
		return !(*this == other);
	}
};

static bool parse_u32(const std::string &text, uint32_t &out)
{
	if (text.empty())
		return false;

	char *end = nullptr;
	const size_t start = text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X') ? 2 : 0;
	const unsigned long long value = std::strtoull(text.c_str() + start, &end, start == 2 ? 16 : 10);
	if (end == text.c_str() + start || *end != '\0' || value > 0xFFFFFFFFull)
		return false;

	out = static_cast<uint32_t>(value);
	return true;
}

static bool parse_value_set(const std::string &text, value_set &out)
{
	out.any = false;
	out.ranges.clear();

	std::string input = text;
	// trim
	const size_t begin = input.find_first_not_of(" \t\r\n");
	if (begin == std::string::npos)
	{
		out.any = true;
		return true;
	}
	const size_t end = input.find_last_not_of(" \t\r\n");
	input = input.substr(begin, end - begin + 1);

	if (input == "*")
	{
		out.any = true;
		return true;
	}

	size_t position = 0;
	while (position <= input.size())
	{
		const size_t comma = input.find(',', position);
		std::string token = input.substr(position, comma == std::string::npos ? std::string::npos : comma - position);

		const size_t token_begin = token.find_first_not_of(" \t");
		const size_t token_end = token.find_last_not_of(" \t");
		token = token_begin == std::string::npos ? std::string() : token.substr(token_begin, token_end - token_begin + 1);

		if (token == "*")
		{
			out.any = true;
			return true;
		}

		if (!token.empty())
		{
			const size_t dash = token.find('-');
			if (dash == std::string::npos)
			{
				uint32_t value = 0;
				if (!parse_u32(token, value))
					return false;
				out.ranges.emplace_back(value, value);
			}
			else
			{
				std::string lhs = token.substr(0, dash);
				std::string rhs = token.substr(dash + 1);

				const size_t lb = lhs.find_first_not_of(" \t");
				const size_t le = lhs.find_last_not_of(" \t");
				lhs = lb == std::string::npos ? std::string() : lhs.substr(lb, le - lb + 1);

				const size_t rb = rhs.find_first_not_of(" \t");
				const size_t re = rhs.find_last_not_of(" \t");
				rhs = rb == std::string::npos ? std::string() : rhs.substr(rb, re - rb + 1);

				uint32_t low = 0;
				uint32_t high = UINT32_MAX;

				if (!lhs.empty() && !parse_u32(lhs, low))
					return false;
				if (!rhs.empty() && !parse_u32(rhs, high))
					return false;

				if (low > high)
					std::swap(low, high);

				out.ranges.emplace_back(low, high);
			}
		}

		if (comma == std::string::npos)
			break;
		position = comma + 1;
	}

	if (out.ranges.empty())
		out.any = true;
	else
		out.normalize();

	return true;
}

static std::string value_set_to_string(const value_set &set)
{
	if (set.any)
		return {};

	std::string text;
	for (size_t i = 0; i < set.ranges.size(); ++i)
	{
		const auto &range = set.ranges[i];
		if (i != 0)
			text += ", ";

		if (range.first == range.second)
		{
			text += std::to_string(range.first);
		}
		else if (range.second == UINT32_MAX)
		{
			text += std::to_string(range.first) + "-";
		}
		else
		{
			text += std::to_string(range.first) + "-" + std::to_string(range.second);
		}
	}
	return text;
}

// -----------------------------------------------------------------------------
// 规则
// -----------------------------------------------------------------------------
enum class handling_mode
{
	skip,
	replace
};

struct texture_override_rule
{
	std::string name;
	uint32_t hash = 0;                 // 纹理哈希（hash_any 时忽略）
	bool hash_any = false;             // hash = *
	uint32_t ps_hash = 0;              // 稳定锚点：像素着色器字节码 CRC32C
	uint32_t vs_hash = 0;              // 稳定锚点：顶点着色器字节码 CRC32C
	value_set first_index;
	value_set index_count;
	int priority = 0;
	handling_mode handling = handling_mode::skip;
	bool active = true;
	size_t order = 0;

	// 运行时热键：打包成 (vk << 24) | (alt << 16) | (ctrl << 8) | shift。
	// 0 = 未绑定。按下时翻转该规则的 active（不写盘，需手动 Save to INI）。
	uint32_t hotkey = 0;
};

// 不可变规则表：UI 修改后整体替换，draw 只做一次 atomic load
struct rule_table
{
	// 单调递增的 id：旧表可能被回收、新表可能落在同一地址，
	// 因此任何缓存都必须比较 id 而不是指针。
	uint64_t id = 0;

	// 全量规则（面板快照用；wildcard_rules 是它的副本）
	std::vector<texture_override_rule> rules;

	// 需要纹理哈希才能匹配的规则：纹理哈希 -> rules 下标
	std::unordered_multimap<uint32_t, size_t> index_by_texture_hash;

	// hash = * 的规则副本，已按 (priority desc, order asc) 预排序：
	// draw 特快通道只遍历这个连续数组，完全不碰 SRV
	std::vector<texture_override_rule> wildcard_rules;

	bool has_texture_rules = false;
};

// draw 时一次性打包的匹配上下文
struct draw_context
{
	uint32_t first_index = 0;
	uint32_t index_count = 0;
	uint32_t ps_hash = 0;
	uint32_t vs_hash = 0;
};

static bool parse_hex_hash(const std::string &text, uint32_t &out)
{
	std::string input = text;

	const size_t begin = input.find_first_not_of(" \t\r\n");
	if (begin == std::string::npos)
		return false;
	const size_t end = input.find_last_not_of(" \t\r\n");
	input = input.substr(begin, end - begin + 1);

	if (input.size() > 2 && input[0] == '0' && (input[1] == 'x' || input[1] == 'X'))
		input = input.substr(2);

	if (input.empty() || input.size() > 8)
		return false;

	// strtoul 会接受 '+'/'-'：hash = -1 会被静默解析成 0xFFFFFFFF。
	// 哈希必须是纯十六进制数字，先严格校验首字符。
	if (!std::isxdigit(static_cast<unsigned char>(input[0])))
		return false;

	char *end_ptr = nullptr;
	const unsigned long value = std::strtoul(input.c_str(), &end_ptr, 16);
	if (end_ptr == input.c_str() || *end_ptr != '\0')
		return false;

	out = static_cast<uint32_t>(value);
	return true;
}

// -----------------------------------------------------------------------------
// 每设备数据
// -----------------------------------------------------------------------------
struct candidate
{
	uint32_t hash = 0;     // 纹理哈希（descriptor 或 pixel）
	uint32_t ps_hash = 0;  // 稳定锚点：像素着色器字节码哈希
	uint32_t vs_hash = 0;  // 稳定锚点：顶点着色器字节码哈希
	uint32_t first_index = 0;
	uint32_t index_count = 0;
};

// 候选唯一键：同一纹理在不同着色器下可能对应不同 UI 元素
struct candidate_key
{
	uint32_t hash = 0;
	uint32_t ps_hash = 0;
	uint32_t vs_hash = 0;

	bool operator==(const candidate_key &other) const
	{
		return hash == other.hash && ps_hash == other.ps_hash && vs_hash == other.vs_hash;
	}
};

struct candidate_key_hasher
{
	size_t operator()(const candidate_key &key) const
	{
		uint64_t value = static_cast<uint64_t>(key.hash) * 0x9E3779B185EBCA87ull;
		value ^= static_cast<uint64_t>(key.ps_hash) * 0xC2B2AE3D27D4EB4Full;
		value ^= static_cast<uint64_t>(key.vs_hash) * 0x165667B19E3779F9ull;
		value ^= value >> 29;
		return static_cast<size_t>(value);
	}
};

enum class hash_mode
{
	descriptor = 0,
	pixel = 1
};

struct resource_hasher
{
	size_t operator()(resource value) const { return static_cast<size_t>(value.handle); }
};

// 每个命令列表独立的 SRV 绑定缓存（4 阶段 x 128 槽位 + 紧凑活动列表）
struct bound_entry
{
	resource_view view = {};
	uint32_t descriptor_hash = 0;
	uint32_t pixel_hash = 0; // 由 draw_indexed 首次查询时填充
};

struct stage_bindings
{
	std::array<bound_entry, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> slots = {};
	std::array<std::pair<uint16_t, bound_entry>, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> active = {};
	uint16_t active_count = 0;

	void rebuild_active()
	{
		active_count = 0;
		for (UINT slot = 0; slot < slots.size(); ++slot)
		{
			if (slots[slot].view.handle != 0)
				active[active_count++] = { static_cast<uint16_t>(slot), slots[slot] };
		}
	}

	void set(UINT slot, resource_view view)
	{
		if (slot >= slots.size())
			return;

		const resource_view old = slots[slot].view;

		if (old.handle == 0 && view.handle == 0)
			return;

		if (old.handle == 0)
		{
			// 不变量：old.handle == 0 ⇒ 该槽为空 ⇒ 非空槽位最多 127 个 ⇒
			// active_count 必然 < active.size()，这里不可能写越界。
			// 重建只作为不变量被破坏时的自愈手段，正常情况下不会执行。
			if (active_count >= active.size())
				rebuild_active();

			// 用断言表达不变量，而不是在 Release 里留一个永远不可达的分支
			assert(active_count < active.size());

			slots[slot] = { view, 0, 0 };
			active[active_count++] = { static_cast<uint16_t>(slot), slots[slot] };
			return;
		}

		if (view.handle == 0)
		{
			slots[slot] = {};
			for (uint16_t i = 0; i < active_count; ++i)
			{
				if (active[i].first == slot)
				{
					active[i] = active[--active_count];
					break;
				}
			}
			return;
		}

		if (old == view)
			return; // 同一视图重复绑定：哈希缓存仍然有效

		slots[slot] = { view, 0, 0 };

		for (uint16_t i = 0; i < active_count; ++i)
		{
			if (active[i].first == slot)
			{
				active[i].second = slots[slot];
				break;
			}
		}
	}
};

// shader 字节码哈希（跨会话稳定锚点）：pipeline 句柄 -> 签名
struct shader_signature
{
	uint32_t vertex = 0;
	uint32_t pixel = 0;
};

// 六个图形/计算阶段的统一索引，与 shader_stage 的位标志一一对应
enum stage_slot : int
{
	stage_vertex = 0,
	stage_hull,
	stage_domain,
	stage_geometry,
	stage_pixel,
	stage_compute,
	stage_count
};

// 六个阶段的固定顺序：既用于 descriptor 位标志遍历，也用于 null 绑定/遍历
static constexpr shader_stage kStageList[stage_count] = {
	shader_stage::vertex,
	shader_stage::hull,
	shader_stage::domain,
	shader_stage::geometry,
	shader_stage::pixel,
	shader_stage::compute
};

// pipeline_stage 的位标志与 shader_stage 不是同一套数值，单独维护
static constexpr pipeline_stage kPipelineStageBits[stage_count] = {
	pipeline_stage::vertex_shader,
	pipeline_stage::hull_shader,
	pipeline_stage::domain_shader,
	pipeline_stage::geometry_shader,
	pipeline_stage::pixel_shader,
	pipeline_stage::compute_shader
};

// 通配符规则预过滤：命中当前 shader 的那几条规则（按 priority 顺序）
struct wildcard_prefilter
{
	static constexpr uint8_t capacity = 8;

	const texture_override_rule *rules[capacity] = {};
	uint8_t count = 0;
	bool overflow = false;
};

struct __declspec(uuid("d4e6f8a0-1b3c-4d5e-9f70-2a4c6e8b0d13")) cmd_list_data
{
	stage_bindings stages[stage_count];

	// bind_pipeline 只做无锁 store（覆盖 VS/HS/DS/GS/PS/CS），
	// draw 发现句柄/epoch 变化时才解析一次哈希（匹配只用到 PS/VS）。
	uint64_t current_pipeline[stage_count] = {};
	uint64_t resolved_pipeline[stage_count] = {};
	uint32_t stage_hash[stage_count] = {};
	uint64_t shader_epoch = 0;

	// 预过滤：绑定 pipeline 后先算出“这个 shader 可能命中哪些 hash = * 规则”，
	// draw 时只检查几何参数。绝大多数 3D shader 的 count 为 0，直接跳过。
	// 用规则表 id（而不是指针）判断缓存有效性：旧表可能被回收、新表可能复用同一地址。
	wildcard_prefilter wildcard;
	uint64_t wildcard_table_id = 0;
	uint32_t wildcard_ps = 0;
	uint32_t wildcard_vs = 0;

	// 线程本地计数（普通变量）：draw 里只做 ++，present 时汇总到全局
	uint32_t local_draws = 0;
	uint32_t local_skipped = 0;
	uint32_t local_applied = 0;
	uint32_t local_srv_resolved = 0;
	uint32_t local_push_updates = 0;
	// 因"候选预览"被跳过的绘制数（单独计数，避免污染 Skipped 的语义）
	uint32_t local_preview_skipped = 0;
	uint32_t last_ps_hash = 0;
	uint32_t last_vs_hash = 0;

	// 本命令列表已经历过的狩猎采集纪元；与全局纪元不一致说明绑定数据陈旧
	uint64_t current_hunting_epoch = 0;

	// 绑定表已校验到的 view 墓碑序号：push/draw 前若发现 g_dead_views 推进了，
	// 就把这段时间内死掉的 view 从绑定表里摘掉（见 revalidate_view_bindings）。
	uint64_t views_validated_seq = 0;
};

struct __declspec(uuid("6b0f4d2a-3c1e-4a7b-9f2d-1e5c8a9b0d31")) device_data
{
	// 是否运行在 D3D11 上。非 D3D11 时禁止把 ReShade 句柄强转成原生 D3D 指针。
	bool is_d3d11 = false;
	// 安全模式：唯一会**写入管线状态**的路径（replace 的 SetShaderResources）
	// 与唯一会**读 CPU 纹理数据**的路径（pixel 哈希）全部关闭。
	// 用途：怀疑插件把驱动喂成 TDR / 蓝屏时，用 [Settings] safe_mode = 1 做二分。
	std::atomic<bool> safe_mode{ false };
	// 非 D3D11 的 replace 警告只写一次。
	// 注意：这条路径可能被多个命令列表线程同时进入，必须是原子变量。
	std::atomic<bool> replace_warned{ false };

	// 像素哈希 / 帧内候选（渲染线程写，UI 读）
	std::mutex mutex;
	std::unordered_map<resource, uint32_t, resource_hasher> pixel_hashes;

	// 存活中的 2D 纹理句柄（受 mutex 保护）。
	// 用途：把 descriptor 哈希缓存的失效精确限定在“纹理真的被销毁”上。
	// 一帧内几十个临时 CB/RTV 的销毁不再触发全量清缓存，
	// 同时还能识别句柄复用（init 时句柄已在集合里 = 上一次销毁事件丢失）。
	std::unordered_set<uint64_t> live_textures;
	std::unordered_map<candidate_key, candidate, candidate_key_hasher> frame_candidates;
	std::unordered_map<candidate_key, candidate, candidate_key_hasher> collected_candidates;

	// 着色器字节码哈希：只在 init/destroy_pipeline 时写入
	std::mutex shader_mutex;
	std::unordered_map<uint64_t, shader_signature> shader_hashes;

	// 不可变规则表：整体替换，draw 无锁读取
	std::atomic<const rule_table *> rules{ nullptr };
	// 发布互斥：publish_rules 会修改 retired_rule_tables / 回收旧表。
	// 单窗口单 present 线程时它天然串行；但同一 device 挂多个 swapchain 时，
	// ReShade 可能在不同线程分别回调 present/overlay，届时并发 push_back/erase
	// 会直接破坏堆。发布是低频操作，加一把锁的代价可以忽略。
	std::mutex publish_mutex;
	std::vector<std::unique_ptr<rule_table>> retired_rule_tables;

	std::atomic<uint64_t> shader_hashes_resolved{ 0 };
	std::atomic<int> hash_mode_value{ static_cast<int>(hash_mode::descriptor) };

	std::vector<candidate> candidates;
	int candidate_index = -1;

	std::atomic<bool> hunting{ false };

	// 新规则是否省略纹理哈希（只写稳定锚点）
	std::atomic<bool> stable_only_rules{ true };

	// 最近一次 draw 的稳定标识（诊断用）
	std::atomic<uint32_t> last_ps_hash{ 0 };
	std::atomic<uint32_t> last_vs_hash{ 0 };

	// 候选快照采集
	std::atomic<int> frames_to_collect{ 10 };
	std::atomic<int> frames_collected{ 0 };
	std::atomic<bool> collecting{ false };

	// 候选预览（只影响渲染，不影响规则表）：勾掉后立刻取消隐藏
	std::atomic<bool> preview_enabled{ true };
	std::atomic<uint32_t> preview_skipped{ 0 };

	// Group settings 窗口状态（仅 overlay 线程访问）
	bool settings_open = false;
	int settings_index = -1;
	char edit_hash[16] = {};
	char edit_ps[16] = {};
	char edit_vs[16] = {};
	char edit_first[512] = {};
	char edit_count[512] = {};
	int edit_priority = 0;
	int edit_handling = 0;
	bool edit_active = true;
	std::string settings_error;

	// 跨线程请求关闭编辑器。
	// process_hotkeys（present 线程）不能直接改 overlay 线程的 settings_open /
	// settings_index / settings_error（后者是 std::string）——多 swapchain 时会
	// 变成跨线程改堆。这里只置请求，由 draw_overlay 开头落实。
	std::atomic<bool> request_close_settings{ false };

	// ---- 规则热键 ----
	// 正在捕获新热键：期间不触发任何规则热键（否则按下的键会顺手翻转规则）
	std::atomic<bool> capturing_hotkey{ false };
	// ImGui 正在吃键盘（用户在输入框里打字）时的抑制截止时间（GetTickCount64，毫秒）。
	// 用"截止时间"而不是布尔值：ReShade 在叠加层关闭时不再回调 draw_overlay，
	// 布尔值会永久卡在 true，导致规则热键彻底失效；时间戳会自己过期。
	std::atomic<uint64_t> keyboard_suppressed_until{ 0 };
	// 编辑中的热键（仅 overlay 线程；点 Done 才写回规则）
	uint32_t edit_hotkey = 0;
	bool edit_hotkey_capturing = false;

	// HUD 悬浮窗（仅 overlay 线程访问）
	float hud_x = 24.0f;
	float hud_y = 24.0f;
	float hud_alpha = 0.75f;
	bool hud_pos_initialized = false;
	bool hud_pos_dirty = false;

	// Group settings 焦点
	bool focus_hash_next = false;

	// 是否有未写盘的规则修改
	std::atomic<bool> has_unsaved_changes{ false };

	resource null_texture = {};
	resource_view null_srv = {};

	std::atomic<uint32_t> draws_seen{ 0 };
	std::atomic<uint32_t> draws_skipped{ 0 };
	std::atomic<uint32_t> srv_resolved{ 0 };
	std::atomic<uint32_t> rules_applied{ 0 };
};

// descriptor hash 的线程局部缓存（key 为 resource.handle）：绘制线程无锁命中
//
// 失效策略 = 精确失效（tombstone ring），而不是全局清空：
//
//   引擎每帧会创建/销毁几十个临时常量缓冲、RTV、DSV。若每次销毁都推进一个
//   全局 epoch，所有线程的纹理哈希缓存每帧被清空几十次，下一次绘制全部 miss，
//   于是每帧上千次 GetDesc —— 这就是掉帧的来源。
//
//   现在只有 **2D 纹理** 销毁才登记一个“墓碑”句柄（环形缓冲，无锁），各线程
//   在下次解析前按序把这几条缓存摘掉：工作量正比于真正销毁的纹理数，
//   与帧率、draw 数无关。视图（SRV/RTV）的创建/销毁完全不影响本缓存。
//
//   g_resource_hash_epoch 保留两个兜底用途：
//     (1) 某线程落后超过环容量（环内条目已被覆盖）→ 该线程整体清空；
//     (2) 设备拆除，device_data 已失效、无法判断资源类型 → 保守失效。
static std::atomic<uint64_t> g_resource_hash_epoch{ 1 };
// 当前各线程缓存条目总数（近似值：插入 +1，清理时减去本线程清掉的条目数）
static std::atomic<int> g_current_cache_size{ 0 };
static std::atomic<uint64_t> g_cache_hits{ 0 };
static std::atomic<uint64_t> g_cache_misses{ 0 };

static thread_local uint64_t t_resource_hash_epoch = 0;
static thread_local std::unordered_map<uint64_t, uint32_t> t_resource_hashes;

// 已销毁 2D 纹理的句柄环（多生产者 / 多消费者，纯原子操作，不持锁）
//
// 每个槽带一个 stamp：生产者先写 handle、再写 stamp（release），消费者只有在
// stamp == 自己期望的序号时才认为该槽可用。这样既避免了“claim 了序号但还没写完
// handle”被误读成 0 的问题，也能识别出“落后一整圈、槽已被覆盖”的极端情况。
//
// 序号从 1 起（槽的 stamp 初值为 0）：若从 0 起，第一个生产者领到的序号 0 恰好
// 等于槽的初始 stamp，消费者会把“已领号、还没写完 handle”的 0 号槽误判为完整，
// 直接跳过并永不回头 —— 第一枚墓碑就被永久丢掉了（漏一次失效）。
// 因此 seq 与 t_dead_textures_seen 的起点必须同为 1，初始 stamp 0 表示“尚未写入”。
//
// 极端情形（生产者被抢占到超过一整圈）：老生产者最后写回，会让某个槽的 stamp
// 落后于消费者的位置。消费者此时只会“本次不推进”（stamp < 位置），绝不会误删
// 别的句柄；下一次有新墓碑写入时即自动恢复（或走整体清空兜底）。因此该设计
// 的最坏结果是短暂的多算一次哈希，而不是脏匹配。
static constexpr uint64_t kDeadTextureRingSize = 1024; // 必须是 2 的幂

struct dead_texture_ring
{
	struct slot
	{
		std::atomic<uint64_t> stamp{ 0 };  // 0 = 该槽从未被写入
		std::atomic<uint64_t> handle{ 0 };
	};

	slot slots[kDeadTextureRingSize] = {};
	std::atomic<uint64_t> seq{ 1 }; // 下一个要分配的序号（从 1 起，单调递增）
};

static dead_texture_ring g_dead_textures;
static thread_local uint64_t t_dead_textures_seen = 1; // 与 seq 同起点

// 同一套无锁环，第二份实例专门登记**已销毁的 view 句柄**。
//
// 为什么需要它：bound_entry 里缓存的 descriptor/pixel 哈希是按 view 句柄记的。
// D3D11 在 view 销毁后会把同一句柄值分配给新 view，如果引擎用这个数值重新绑定
// 同一个槽位，set() 的 `old == view` 快路径会认为“还是同一个视图”，于是把旧
// 哈希当成新 view 的哈希 —— 张冠李戴（跳过或替换错纹理）。
//
// 这里不做“每次绑定都查一次戳表”（那会给 push_descriptors 加一把锁 + 一次
// 哈希查找，每帧数万次）。改为：销毁时登记，各命令列表用 views_validated_seq
// 水位线，在 push/draw 之前一次性把这段时间内死掉的 view 从绑定表**摘除**。
// 热路径成本：一次 atomic load + 比较；只有水位线推进时才做一次批量摘除。
static dead_texture_ring g_dead_views;

// 一次批量摘除最多回溯多少枚 view 墓碑；超过则整体丢弃绑定表（保守但正确）
static constexpr uint64_t kDeadViewBatchSize = 256;

// 批量摘除用的临时集合：thread_local 复用，稳态下无堆分配
static thread_local std::unordered_set<uint64_t> t_destroyed_views;

static void ring_push(dead_texture_ring &ring, uint64_t handle)
{
	if (handle == 0)
		return;

	const uint64_t seq = ring.seq.fetch_add(1, std::memory_order_relaxed);
	dead_texture_ring::slot &slot = ring.slots[seq & (kDeadTextureRingSize - 1)];

	slot.handle.store(handle, std::memory_order_relaxed);
	// stamp 最后写：消费者看到 stamp == seq 才认为槽内容完整
	slot.stamp.store(seq, std::memory_order_release);
}

// 登记一个已销毁的纹理句柄（写侧：on_destroy_resource，可能有多个线程）
static void note_destroyed_texture(uint64_t handle)
{
	ring_push(g_dead_textures, handle);
}

// 登记一个已销毁的 view 句柄（写侧：on_destroy_resource_view）
static void note_destroyed_view(uint64_t handle)
{
	ring_push(g_dead_views, handle);
}

// 整体清空并追上进度（兜底路径）
static void reset_descriptor_hash_cache(uint64_t caught_up_seq)
{
	if (!t_resource_hashes.empty())
	{
		g_current_cache_size.fetch_sub(static_cast<int>(t_resource_hashes.size()), std::memory_order_relaxed);
		t_resource_hashes.clear();
	}

	t_dead_textures_seen = caught_up_seq;
}

// 消费墓碑（读侧：resolve_descriptor_hash 内，常规开销只有一次原子读）
static void drain_destroyed_textures()
{
	const uint64_t written = g_dead_textures.seq.load(std::memory_order_acquire);
	if (written == t_dead_textures_seen)
		return;

	// 落后超过环容量：中间条目已被覆盖，只能整体清空（长时间不绘制的线程）
	if (written - t_dead_textures_seen > kDeadTextureRingSize)
	{
		reset_descriptor_hash_cache(written);
		return;
	}

	while (t_dead_textures_seen != written)
	{
		const uint64_t seq = t_dead_textures_seen;
		const dead_texture_ring::slot &slot = g_dead_textures.slots[seq & (kDeadTextureRingSize - 1)];
		const uint64_t stamp = slot.stamp.load(std::memory_order_acquire);

		if (stamp != seq)
		{
			// stamp > seq：槽已被下一圈覆盖（本线程落后）-> 整体清空兜底
			// stamp < seq：生产者已登记序号但还没写完 handle -> 下次再处理
			if (stamp > seq)
				reset_descriptor_hash_cache(written);
			return;
		}

		const uint64_t handle = slot.handle.load(std::memory_order_acquire);
		if (handle != 0 && t_resource_hashes.erase(handle) != 0)
			g_current_cache_size.fetch_sub(1, std::memory_order_relaxed);

		++t_dead_textures_seen;
	}
}

// 精确判断：handle 是否在 [since_seq, 当前) 这段墓碑区间内被销毁。
//
// 用途是“插入后二次校验”：线程 A 在计算哈希的同时，线程 B 可能刚好销毁了
// 同一张纹理。若那次销毁发生在 A 读取墓碑环位置之前，其墓碑已经被排空；
// 若发生在之后，就必须在这里被精确识别——不能依赖再排空一次，因为排空
// 可能走“落后整圈 → 整体清空”的兜底路径，从而跳过这条墓碑。
static bool texture_destroyed_since(uint64_t handle, uint64_t since_seq)
{
	const uint64_t written = g_dead_textures.seq.load(std::memory_order_acquire);
	if (written <= since_seq)
		return false;

	// 落后整圈：中间条目已被覆盖，无法确定 -> 按“已销毁”处理（调用方整体清空）
	if (written - since_seq > kDeadTextureRingSize)
		return true;

	for (uint64_t seq = since_seq; seq != written; ++seq)
	{
		const dead_texture_ring::slot &slot = g_dead_textures.slots[seq & (kDeadTextureRingSize - 1)];
		const uint64_t stamp = slot.stamp.load(std::memory_order_acquire);

		// 生产者已登记序号但还没写完 handle：稍后的 drain 会处理，这里跳过
		if (stamp < seq)
			continue;
		// 已被下一圈覆盖：信息丢失，保守按已销毁处理
		if (stamp != seq)
			return true;

		if (slot.handle.load(std::memory_order_acquire) == handle)
			return true;
	}

	return false;
}

// -----------------------------------------------------------------------------
// 绑定表的 view 失效校验（限制 12 的根治）
//
// 触发点：on_push_descriptors / on_draw_indexed 里各一次，常规开销是
// 「一次 atomic load + 与水位线比较」。只有当 g_dead_views 推进过，才做一次
// 批量摘除：
//
//   1) 把 [views_validated_seq, now) 区间里被销毁的 view 句柄收进线程本地集合；
//   2) 遍历六个 stage 的 active 列表，把 view 句柄命中的条目**整条摘掉**
//      （连 view 一起清空，绝不只清哈希：句柄可能已经悬垂，任何再解析都是 UAF）；
//   3) 水位线只推进到“确实处理完”的位置（生产者还没写完的槽留到下次）。
//
// 为什么不用 per-entry 的 stamp：那样每次 push 都要拿一次戳表（锁或探测），
// 落在每帧数万次 × 每描述符一次的路径上。水位线把同样的精确性压缩成
// 「每命令列表一次比较 + 每帧一次批量扫描」。
// -----------------------------------------------------------------------------
static void revalidate_view_bindings(cmd_list_data *cmd_data)
{
	const uint64_t now = g_dead_views.seq.load(std::memory_order_acquire);
	if (cmd_data->views_validated_seq == now)
		return;

	// 首次使用：历史墓碑与本命令列表无关，只记录起点
	if (cmd_data->views_validated_seq == 0)
	{
		cmd_data->views_validated_seq = now;
		return;
	}

	const uint64_t since = cmd_data->views_validated_seq;
	bool drop_all = false;

	t_destroyed_views.clear();

	if (now - since > kDeadViewBatchSize)
	{
		// 落后太多（长时间没 push/draw）：无法精确比对，整体丢弃绑定表。
		// 绑定只会在下一次 push_descriptors 时重建，代价有限。
		drop_all = true;
	}
	else
	{
		uint64_t seq = since;
		for (; seq != now; ++seq)
		{
			const dead_texture_ring::slot &slot = g_dead_views.slots[seq & (kDeadTextureRingSize - 1)];
			const uint64_t stamp = slot.stamp.load(std::memory_order_acquire);

			if (stamp < seq)
				break; // 生产者已领号但还没写完：本次先处理到这里，下次再来

			if (stamp != seq)
			{
				// 槽已被下一圈覆盖：信息丢失，只能保守处理
				drop_all = true;
				seq = now;
				break;
			}

			const uint64_t handle = slot.handle.load(std::memory_order_acquire);
			if (handle != 0)
				t_destroyed_views.insert(handle);
		}

		// 水位线只推进到已处理的位置
		cmd_data->views_validated_seq = seq;
	}

	if (drop_all)
	{
		cmd_data->views_validated_seq = now;

		for (int stage = 0; stage < stage_count; ++stage)
			cmd_data->stages[stage] = {};
		return;
	}

	if (t_destroyed_views.empty())
		return;

	for (int stage = 0; stage < stage_count; ++stage)
	{
		stage_bindings &binding = cmd_data->stages[stage];

		uint16_t write = 0;
		for (uint16_t i = 0; i < binding.active_count; ++i)
		{
			const uint16_t slot = binding.active[i].first;
			const uint64_t handle = binding.active[i].second.view.handle;

			if (handle != 0 && t_destroyed_views.find(handle) != t_destroyed_views.end())
			{
				// 整条摘除：slots[] 也清空，避免后续任何路径再解析这个句柄
				binding.slots[slot] = {};
				continue;
			}

			binding.active[write++] = binding.active[i];
		}

		binding.active_count = write;
	}
}

// 像素哈希查询缓存：push_descriptors 不加锁，draw 侧只在首次遇到视图时加锁一次
static std::atomic<uint64_t> g_pixel_insert_epoch{ 1 }; // 有新像素哈希写入时推进
static std::atomic<uint64_t> g_pixel_reset_epoch{ 1 };  // 有资源销毁时推进

static thread_local uint64_t t_pixel_reset_seen = 0;
static thread_local std::unordered_map<uint64_t, uint32_t> t_pixel_hits;
static thread_local std::unordered_map<uint64_t, uint64_t> t_pixel_misses;

// shader 字节码哈希缓存：bind_pipeline 高频路径，第一次查询后线程本地命中
static std::atomic<uint64_t> g_pipeline_epoch{ 1 }; // destroy_pipeline 时推进
static thread_local uint64_t t_pipeline_epoch_seen = 0;
static thread_local std::unordered_map<uint64_t, shader_signature> t_pipeline_hashes;

// 命令列表私有数据缓存：ReShade 的 get_private_data 内部要拿 shared_mutex，
// 高频回调里每次调用都是一次原子操作。这里按 native 指针线程本地缓存。
static std::atomic<uint64_t> g_cmd_list_epoch{ 1 };
static thread_local uint64_t t_cmd_list_epoch_seen = 0;
static thread_local std::unordered_map<const command_list *, cmd_list_data *> t_cmd_data;
static thread_local const command_list *t_last_cmd_list = nullptr;
static thread_local cmd_list_data *t_last_cmd_data = nullptr;
// 本线程触碰过的命令列表：present 时只汇总这些（避免跨线程读写普通计数器）
static thread_local std::vector<cmd_list_data *> t_owned_cmd_data;

static cmd_list_data *get_cmd_data(command_list *cmd_list)
{
	if (cmd_list == nullptr)
		return nullptr;

	// epoch 检查必须放在最前面：对象可能在别的线程销毁且地址被复用，
	// 先比对 epoch 才能安全使用任何缓存指针。
	const uint64_t epoch = g_cmd_list_epoch.load(std::memory_order_relaxed);
	if (t_cmd_list_epoch_seen != epoch)
	{
		t_cmd_list_epoch_seen = epoch;
		t_cmd_data.clear();
		t_owned_cmd_data.clear();
		t_last_cmd_list = nullptr;
		t_last_cmd_data = nullptr;
	}

	// 第一层：同一线程连续访问同一命令列表（绝大多数情况）
	if (cmd_list == t_last_cmd_list && t_last_cmd_data != nullptr)
		return t_last_cmd_data;

	const auto cached = t_cmd_data.find(cmd_list);
	if (cached != t_cmd_data.end())
	{
		t_last_cmd_list = cmd_list;
		t_last_cmd_data = cached->second;
		return cached->second;
	}

	cmd_list_data *data = cmd_list->get_private_data<cmd_list_data>();
	if (data == nullptr)
		data = cmd_list->create_private_data<cmd_list_data>();

	if (data != nullptr)
	{
		if (t_cmd_data.size() > 64)
		{
			t_cmd_data.clear();
			t_owned_cmd_data.clear();
		}
		t_cmd_data.emplace(cmd_list, data);
		t_owned_cmd_data.push_back(data);
		t_last_cmd_list = cmd_list;
		t_last_cmd_data = data;
	}

	return data;
}

// device_data 缓存：ReShade 的 device::get_private_data 同样带锁
static std::atomic<uint64_t> g_device_epoch{ 1 };
static thread_local uint64_t t_device_epoch_seen = 0;
static thread_local device *t_last_device = nullptr;
static thread_local device_data *t_last_device_data = nullptr;

static device_data *get_device_data(device *dev)
{
	if (dev == nullptr)
		return nullptr;

	// 同样先比对 epoch，再使用快速路径上的缓存指针
	const uint64_t epoch = g_device_epoch.load(std::memory_order_relaxed);
	if (t_device_epoch_seen != epoch)
	{
		t_device_epoch_seen = epoch;
		t_last_device = nullptr;
		t_last_device_data = nullptr;
	}

	if (dev == t_last_device && t_last_device_data != nullptr)
		return t_last_device_data;

	device_data *data = dev->get_private_data<device_data>();
	t_last_device = dev;
	t_last_device_data = data;
	return data;
}

// 规则集里是否存在需要纹理哈希的规则：push_descriptors 的最快出口
static std::atomic<bool> g_texture_rules_active{ false };

// 狩猎采集进行中：即使没有纹理规则，也必须记录 stage_bindings，
// 否则候选列表永远是空的（纯 hash = * 配置下的常见场景）。
static std::atomic<bool> g_hunting_collecting{ false };

// 狩猎采集纪元：每次开启 Hunting 都推进。
// 挂机期间 push_descriptors 早退，stage_bindings 里可能残留早已销毁的 view 句柄；
// 新纪元强制各命令列表丢弃旧绑定，避免 record_draw_candidates 解引用悬垂句柄。
static std::atomic<uint64_t> g_hunting_collect_epoch{ 1 };

// -----------------------------------------------------------------------------
// 候选预览：把"当前选中的候选"对应的绘制临时跳过
//
// 目的：Numpad 1/2 只是换一个哈希数字，用户看不出画面上是哪个元素。
// ShaderToggler / REST 的做法是步进时把当前 shader 隐藏，用"元素消失"来确认
// 选中了什么；这里做成 draw 内的一次精确匹配（PS/VS 相等做廉价预筛，
// 再比对绑定 SRV 的纹理哈希），语义上等于"这条候选覆盖的所有绘制"。
//
// 三字段 + seq 的 seqlock：overlay / present 线程写，多个 draw 线程读。
// 读到"写了一半"就放弃本次预览——宁可少隐藏一帧，也不做错匹配。
// -----------------------------------------------------------------------------
struct candidate_preview
{
	std::atomic<uint32_t> hash{ 0 };
	std::atomic<uint32_t> ps_hash{ 0 };
	std::atomic<uint32_t> vs_hash{ 0 };
	std::atomic<bool> enabled{ false };
	std::atomic<uint32_t> seq{ 0 };
};

static candidate_preview g_preview;

static void set_candidate_preview(bool enabled, uint32_t hash, uint32_t ps_hash, uint32_t vs_hash)
{
	g_preview.seq.fetch_add(1, std::memory_order_relaxed);
	g_preview.hash.store(hash, std::memory_order_relaxed);
	g_preview.ps_hash.store(ps_hash, std::memory_order_relaxed);
	g_preview.vs_hash.store(vs_hash, std::memory_order_relaxed);
	g_preview.enabled.store(enabled, std::memory_order_relaxed);
	// release：保证前面几个 store 在序号变偶数之后对读侧可见
	g_preview.seq.fetch_add(1, std::memory_order_release);
}

// 读侧：序号为奇数或前后不一致（写者正在更新）时返回 false
static bool read_candidate_preview(uint32_t &hash, uint32_t &ps_hash, uint32_t &vs_hash)
{
	if (!g_preview.enabled.load(std::memory_order_relaxed))
		return false;

	const uint32_t s1 = g_preview.seq.load(std::memory_order_acquire);
	if ((s1 & 1u) != 0)
		return false;

	hash = g_preview.hash.load(std::memory_order_relaxed);
	ps_hash = g_preview.ps_hash.load(std::memory_order_relaxed);
	vs_hash = g_preview.vs_hash.load(std::memory_order_relaxed);

	return g_preview.seq.load(std::memory_order_acquire) == s1;
}

// push_descriptors 计数（诊断，不依赖 device_data）
static std::atomic<uint64_t> g_push_updates{ 0 };

// 像素哈希统计（诊断）
static std::atomic<uint64_t> g_pixel_hash_bytes{ 0 };
static std::atomic<bool> g_logged_shader_hash{ false };
static std::atomic<bool> g_logged_untracked_resource{ false };

// -----------------------------------------------------------------------------
// 日志
// -----------------------------------------------------------------------------
static void log_info(const char *text) { reshade::log::message(reshade::log::level::info, text); }
static void log_error(const char *text) { reshade::log::message(reshade::log::level::error, text); }
static void log_warn(const char *text) { reshade::log::message(reshade::log::level::warning, text); }

// 非 D3D11 API 无法安全执行 replace：只警告一次，避免每帧刷屏
static void note_replace_unavailable(device_data *data)
{
	if (data == nullptr)
		return;

	// exchange 保证多线程下只有第一个调用者写日志
	if (data->replace_warned.exchange(true, std::memory_order_relaxed))
		return;

	log_warn("[tex_override] replace unavailable on non-D3D11 API, falling back to skip");
}

// -----------------------------------------------------------------------------
// 配置路径
// -----------------------------------------------------------------------------
static constexpr const wchar_t *kConfigFileName = L"TextureOverride.ini";

static std::filesystem::path get_config_path()
{
	std::wstring buffer(MAX_PATH, L'\0');
	DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));

	while (length != 0 && length == buffer.size())
	{
		buffer.resize(buffer.size() * 2);
		length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
	}

	if (length == 0)
		return std::filesystem::path(kConfigFileName);

	buffer.resize(length);
	return std::filesystem::path(buffer).parent_path() / kConfigFileName;
}

static std::string trim_copy(const std::string &text)
{
	const size_t begin = text.find_first_not_of(" \t\r\n");
	if (begin == std::string::npos)
		return {};
	const size_t end = text.find_last_not_of(" \t\r\n");
	return text.substr(begin, end - begin + 1);
}

static std::string lower_copy(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return text;
}

// -----------------------------------------------------------------------------
// 规则表：写时整体替换，读侧无锁
//
// 规则被拆成两个互斥集合，draw 可以走不同的路径：
//   - wildcard_rules        ：hash = *，只需 shader + 几何条件，完全不碰 SRV
//   - index_by_texture_hash ：需要纹理哈希，命中后还要按纹理匹配
// -----------------------------------------------------------------------------
// 保留最近多少张旧规则表。draw 只在单次调用内持有裸指针，
// 16 张的宽限窗口足以覆盖任何并发；present 中回收最老的，避免无限增长。
static constexpr size_t kMaxRetiredRuleTables = 16;

// 规则表 id 发生器（单调递增，永不回绕到已回收的表）
static std::atomic<uint64_t> g_rule_table_id{ 1 };

static void publish_rules(device_data *data, std::vector<texture_override_rule> rules)
{
	// 整个发布过程（建表 + 回收旧表 + 发布指针）必须串行
	std::lock_guard<std::mutex> publish_lock(data->publish_mutex);

	for (size_t i = 0; i < rules.size(); ++i)
		rules[i].order = i;

	// 通配符规则无法确定要替换哪一张纹理：replace 会误伤整次绘制，
	// 这里直接降级为 skip，并记录一次警告。
	for (texture_override_rule &rule : rules)
	{
		if ((rule.hash_any || rule.hash == 0) && rule.handling == handling_mode::replace)
		{
			rule.handling = handling_mode::skip;

			char message[256];
			std::snprintf(message, sizeof(message),
				"[Rules] '%s': wildcard (hash = *) has no target texture; replace coerced to skip",
				rule.name.empty() ? "<unnamed>" : rule.name.c_str());
			log_warn(message);
		}
	}

	// 安全模式：完全禁止写入管线状态（replace 的唯一实现方式是回调内
	// SetShaderResources）。用于二分“是否是插件把驱动喂成 TDR/蓝屏”。
	if (data->safe_mode.load(std::memory_order_relaxed))
	{
		for (texture_override_rule &rule : rules)
		{
			if (rule.handling != handling_mode::replace)
				continue;

			rule.handling = handling_mode::skip;

			char message[256];
			std::snprintf(message, sizeof(message),
				"[Rules] '%s': safe_mode enabled; replace coerced to skip",
				rule.name.empty() ? "<unnamed>" : rule.name.c_str());
			log_warn(message);
		}
	}

	auto table = std::make_unique<rule_table>();
	table->id = g_rule_table_id.fetch_add(1, std::memory_order_relaxed);
	table->rules = std::move(rules);
	table->wildcard_rules.reserve(table->rules.size());

	for (size_t i = 0; i < table->rules.size(); ++i)
	{
		const texture_override_rule &rule = table->rules[i];
		if (!rule.active)
			continue;

		if (rule.hash_any || rule.hash == 0)
			table->wildcard_rules.push_back(rule);
		else
			table->index_by_texture_hash.emplace(rule.hash, i);
	}

	// 通配符规则预排序：priority 降序，同优先级按文件顺序。
	// draw 只要找到第一条匹配的规则即可，无需遍历。
	std::stable_sort(table->wildcard_rules.begin(), table->wildcard_rules.end(),
		[](const texture_override_rule &lhs, const texture_override_rule &rhs)
		{
			if (lhs.priority != rhs.priority)
				return lhs.priority > rhs.priority;
			return lhs.order < rhs.order;
		});

	table->has_texture_rules = !table->index_by_texture_hash.empty();

	const rule_table *raw = table.get();

	// 旧表不立即释放：draw 线程可能仍在用裸指针。保留最近 kMaxRetiredRuleTables 张，
	// 回收也在这里完成——retired_rule_tables 的全部操作都留在 publish_rules 的
	// 同一调用栈内，只要调用方互斥（UI / 热键 / reload 不并发）就绝对安全。
	data->retired_rule_tables.push_back(std::move(table));
	while (data->retired_rule_tables.size() > kMaxRetiredRuleTables)
		data->retired_rule_tables.erase(data->retired_rule_tables.begin());

	data->rules.store(raw, std::memory_order_release);

	// push_descriptors 的唯一判据：没有纹理规则时完全不需要 SRV 绑定信息
	g_texture_rules_active.store(raw->has_texture_rules, std::memory_order_release);
}

static const rule_table *acquire_rules(const device_data *data)
{
	return data->rules.load(std::memory_order_acquire);
}

static std::vector<texture_override_rule> copy_rules(const rule_table *table)
{
	return table != nullptr ? table->rules : std::vector<texture_override_rule>{};
}

// 单条规则的稳定锚点 + 几何条件校验（AND）
static inline bool rule_matches_draw(const texture_override_rule &rule, const draw_context &ctx)
{
	if (!rule.active)
		return false;

	// 写了就必须严格相等；运行时取不到（0）视为不匹配，避免过宽命中
	if (rule.ps_hash != 0 && rule.ps_hash != ctx.ps_hash)
		return false;
	if (rule.vs_hash != 0 && rule.vs_hash != ctx.vs_hash)
		return false;

	if (!rule.first_index.matches(ctx.first_index))
		return false;
	if (!rule.index_count.matches(ctx.index_count))
		return false;

	return true;
}

// 只要 shader 匹配的几何条件校验（预过滤之后 draw 内只剩这些）
static inline bool rule_matches_geometry(const texture_override_rule &rule, uint32_t first_index, uint32_t index_count)
{
	if (!rule.first_index.matches(first_index))
		return false;
	if (!rule.index_count.matches(index_count))
		return false;
	return true;
}

// 预过滤：把「当前 shader 可能命中的 hash = * 规则」缓存进 cmd_list_data。
// 只在 pipeline 变化 / 规则表替换时重算，draw 里只有 3 次整数比较 + 少量指针遍历。
static void refresh_wildcard_prefilter(const rule_table *rules, cmd_list_data *cmd)
{
	// 用 id 而不是指针：旧表被回收后新表可能分配到同一地址
	const uint64_t table_id = rules != nullptr ? rules->id : 0;

	const uint32_t ps_hash = cmd->stage_hash[stage_pixel];
	const uint32_t vs_hash = cmd->stage_hash[stage_vertex];

	if (cmd->wildcard_table_id == table_id &&
		cmd->wildcard_ps == ps_hash &&
		cmd->wildcard_vs == vs_hash)
		return;

	cmd->wildcard_table_id = table_id;
	cmd->wildcard_ps = ps_hash;
	cmd->wildcard_vs = vs_hash;
	cmd->wildcard.count = 0;
	cmd->wildcard.overflow = false;

	if (rules == nullptr)
		return;

	// wildcard_rules 已按 priority 降序预排序，这里按同样顺序收集
	for (const texture_override_rule &rule : rules->wildcard_rules)
	{
		if (rule.ps_hash != 0 && rule.ps_hash != ps_hash)
			continue;
		if (rule.vs_hash != 0 && rule.vs_hash != vs_hash)
			continue;

		if (cmd->wildcard.count < wildcard_prefilter::capacity)
		{
			cmd->wildcard.rules[cmd->wildcard.count++] = &rule;
		}
		else
		{
			// 超过容量（极罕见）：回退为遍历全部通配符规则
			cmd->wildcard.overflow = true;
			break;
		}
	}
}

// -----------------------------------------------------------------------------
// INI 加载
// -----------------------------------------------------------------------------
static void load_config(device_data *data)
{
	const std::filesystem::path path = get_config_path();

	std::ifstream file(path);
	if (!file)
	{
		log_info("[Config] TextureOverride.ini not found; 0 rules");
		// 没有配置文件时必须回到默认模式，否则“删掉 INI 再 Reload”会沿用旧模式
		data->hash_mode_value.store(static_cast<int>(hash_mode::descriptor), std::memory_order_relaxed);
		publish_rules(data, {});
		data->has_unsaved_changes.store(false, std::memory_order_relaxed);
		return;
	}

	std::vector<texture_override_rule> parsed;
	std::string line;

	bool in_section = false;
	bool in_settings = false;
	bool section_is_legacy_list = false;
	size_t section_counter = 0;
	int warnings = 0;
	size_t line_number = 0;
	int parsed_hash_mode = -1;
	int parsed_safe_mode = -1;

	texture_override_rule current;
	bool current_valid = true;
	bool current_has_hash = false;
	size_t section_line = 0;
	size_t error_line = 0;
	std::string section_display;

	auto report_skip = [&](const char *reason, size_t at_line)
	{
		char message[256];
		std::snprintf(message, sizeof(message),
			"[Config] skipped invalid section '%s' at line %u: %s",
			section_display.c_str(),
			static_cast<unsigned>(at_line),
			reason);
		log_warn(message);
	};

	auto mark_invalid = [&](const char *reason)
	{
		current_valid = false;
		if (error_line == 0)
		{
			error_line = line_number;
			report_skip(reason, line_number);
		}
	};

	auto flush = [&]()
	{
		if (!in_section)
			return;

		if (current_valid && !current_has_hash &&
			current.hash == 0 && current.ps_hash == 0 && current.vs_hash == 0)
		{
			current_valid = false;
			if (error_line == 0)
			{
				error_line = section_line;
				report_skip("missing hash / match_ps_hash / match_vs_hash", section_line);
			}
		}

		// 省略 hash（或写 hash = *）都表示“任意纹理”
		if (current_valid && current.hash == 0)
			current.hash_any = true;

		// 保护：纯通配符规则会把整个场景都命中，必须至少有一个稳定锚点
		if (current_valid && current.hash_any &&
			current.ps_hash == 0 && current.vs_hash == 0)
		{
			current_valid = false;
			if (error_line == 0)
			{
				error_line = section_line;
				report_skip("hash = * requires match_ps_hash or match_vs_hash", section_line);
			}
		}

		// 注意：旧式 [TextureOverride] 列表节里含 0xHASH=1 时 current_valid 已被置 false，
		// 因此这里不会重复生成规则；而 [TextureOverride] 里直接写 hash= 仍然有效。
		if (current_valid &&
			(current_has_hash || current.ps_hash != 0 || current.vs_hash != 0))
		{
			current.order = parsed.size();
			if (current.name.empty())
				current.name = "TextureOverride_" + std::to_string(section_counter);
			parsed.push_back(current);
		}
		else if (!section_is_legacy_list)
		{
			++warnings;

			if (!current_valid && error_line == 0)
				report_skip("invalid value", section_line);
		}

		current = {};
		current_valid = true;
		current_has_hash = false;
		section_line = 0;
		error_line = 0;
		section_display.clear();
	};

	while (std::getline(file, line))
	{
		++line_number;

		// 第一行去掉 UTF-8 BOM（记事本/部分编辑器保存时会有），
		// 否则首个 section 头无法识别，整段 key 被静默忽略。
		if (line_number == 1 && line.size() >= 3 &&
			static_cast<unsigned char>(line[0]) == 0xEF &&
			static_cast<unsigned char>(line[1]) == 0xBB &&
			static_cast<unsigned char>(line[2]) == 0xBF)
		{
			line.erase(0, 3);
		}

		// 行尾注释：';' 或 '#' 出现在空白之后即视为注释起点
		// （值只可能是数字/区间/*，不会包含这两个字符）
		for (size_t i = 1; i < line.size(); ++i)
		{
			if ((line[i] == ';' || line[i] == '#') &&
				(line[i - 1] == ' ' || line[i - 1] == '\t'))
			{
				line.erase(i);
				break;
			}
		}

		const std::string trimmed = trim_copy(line);
		if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#')
			continue;

		if (trimmed.front() == '[' && trimmed.back() == ']')
		{
			flush();

			++section_counter;
			const std::string section = lower_copy(trimmed.substr(1, trimmed.size() - 2));
			in_section = section.compare(0, 15, "textureoverride") == 0;
			in_settings = (section == "settings");
			section_is_legacy_list = in_section && section == "textureoverride";
			current = {};
			current_valid = true;
			current_has_hash = false;
			current.name = trimmed.substr(1, trimmed.size() - 2);
			section_line = line_number;
			error_line = 0;
			section_display = current.name;
			continue;
		}

		if (!in_section && !in_settings)
			continue;

		const size_t equals = trimmed.find('=');
		if (equals == std::string::npos)
			continue;

		const std::string key = lower_copy(trim_copy(trimmed.substr(0, equals)));
		const std::string value = trim_copy(trimmed.substr(equals + 1));

		if (in_settings)
		{
			if (key == "hash_mode")
			{
				const std::string mode = lower_copy(value);
				if (mode == "descriptor")
					parsed_hash_mode = static_cast<int>(hash_mode::descriptor);
				else if (mode == "pixel")
					parsed_hash_mode = static_cast<int>(hash_mode::pixel);
			}
			else if (key == "safe_mode")
			{
				uint32_t flag = 0;
				if (parse_u32(value, flag))
					parsed_safe_mode = (flag != 0);
			}
			continue;
		}

		// 兼容旧的 0xHASH=1 列表：右侧值为 0 表示该条目被禁用（3DMigoto 习惯）
		if (section_is_legacy_list && key.size() > 2 && key[0] == '0' && (key[1] == 'x' || key[1] == 'X'))
		{
			uint32_t flag = 1;
			if (!value.empty() && !parse_u32(value, flag))
				flag = 1; // 无法解析的右侧值按“启用”处理，保持向后兼容

			uint32_t hash = 0;
			if (flag != 0 && parse_hex_hash(key, hash) && hash != 0)
			{
				texture_override_rule rule;
				rule.name = "TextureOverride_" + std::to_string(parsed.size());
				rule.hash = hash;
				rule.handling = handling_mode::skip;
				rule.order = parsed.size();
				parsed.push_back(rule);
			}

			// 让该 section 结束时 flush 不产生“缺少 hash”的警告
			current_valid = false;
			continue;
		}

		if (key == "hash" || key == "match_hash" || key == "match_texture_hash")
		{
			if (value == "*")
			{
				current.hash = 0;
				current.hash_any = true;
				current_has_hash = true;
			}
			else
			{
				uint32_t hash = 0;
				if (!parse_hex_hash(value, hash))
				{
					mark_invalid("hash must be 1-8 hex digits or *");
				}
				else if (hash == 0)
				{
					// 与 Group settings 保持一致：0 不是有效纹理哈希，
					// 写 0 会退化成“任意纹理”的通配符规则，必须报错而不是静默接受。
					mark_invalid("hash must be non-zero (use * to match any texture)");
				}
				else
				{
					current.hash = hash;
					current.hash_any = false;
					current_has_hash = true;
				}
			}
		}
		else if (key == "match_ps_hash" || key == "match_pixel_shader_hash" || key == "ps_hash")
		{
			uint32_t hash = 0;
			if (parse_hex_hash(value, hash))
				current.ps_hash = hash;
			else
				mark_invalid("match_ps_hash must be 1-8 hex digits");
		}
		else if (key == "match_vs_hash" || key == "match_vertex_shader_hash" || key == "vs_hash")
		{
			uint32_t hash = 0;
			if (parse_hex_hash(value, hash))
				current.vs_hash = hash;
			else
				mark_invalid("match_vs_hash must be 1-8 hex digits");
		}
		else if (key == "match_first_index")
		{
			if (!parse_value_set(value, current.first_index))
				mark_invalid("invalid match_first_index");
		}
		else if (key == "match_index_count")
		{
			if (!parse_value_set(value, current.index_count))
				mark_invalid("invalid match_index_count");
		}
		else if (key == "match_priority")
		{
			char *end = nullptr;
			const long priority = std::strtol(value.c_str(), &end, 10);
			if (end != value.c_str() && *end == '\0')
				current.priority = static_cast<int>(priority);
			else
				mark_invalid("invalid match_priority");
		}
		else if (key == "handling")
		{
			const std::string handling = lower_copy(value);
			if (handling == "skip")
				current.handling = handling_mode::skip;
			else if (handling == "replace" || handling == "null")
				current.handling = handling_mode::replace;
			else
				mark_invalid("handling must be skip/replace/null");
		}
		else if (key == "active")
		{
			if (value == "0")
				current.active = false;
			else if (value == "1")
				current.active = true;
			else
				mark_invalid("active must be 0 or 1");
		}
		else if (key == "hotkey")
		{
			// 打包值：(vk << 24) | (alt << 16) | (ctrl << 8) | shift
			uint32_t packed = 0;
			if (!parse_u32(value, packed))
			{
				mark_invalid("hotkey must be a packed 32-bit value (use the in-game capture box)");
			}
			else if (packed != 0 && ((packed >> 24) & 0xFFu) == 0)
			{
				mark_invalid("hotkey has no key code in the high byte");
			}
			else
			{
				current.hotkey = packed;
			}
		}
		// 其他键忽略（例如 3DMigoto 的 ps-t0 / vb0 / ib，本插件不处理）
	}

	flush();

	if (parsed_hash_mode >= 0)
		data->hash_mode_value.store(parsed_hash_mode, std::memory_order_relaxed);

	// 必须先于 publish_rules 生效：安全模式会在发布时把 replace 降级为 skip
	if (parsed_safe_mode >= 0)
	{
		data->safe_mode.store(parsed_safe_mode != 0, std::memory_order_relaxed);
		if (parsed_safe_mode != 0)
		{
			data->hash_mode_value.store(static_cast<int>(hash_mode::descriptor), std::memory_order_relaxed);
			log_warn("[tex_override] SAFE MODE: replace coerced to skip, pixel hashing disabled");
		}
	}

	const size_t loaded_count = parsed.size();

	publish_rules(data, std::move(parsed));

	char message[160];
	std::snprintf(message, sizeof(message), "[Config] loaded %u rules",
		static_cast<unsigned>(loaded_count));
	log_info(message);

	if (warnings != 0)
	{
		std::snprintf(message, sizeof(message), "[Config] %d section(s) skipped (missing/invalid hash or value)",
			warnings);
		log_warn(message);
	}

	data->has_unsaved_changes.store(false, std::memory_order_relaxed);
}

// -----------------------------------------------------------------------------
// INI 保存
// -----------------------------------------------------------------------------
static bool save_config(std::vector<texture_override_rule> rules, int hash_mode_value, bool safe_mode)
{
	const std::filesystem::path path = get_config_path();
	std::filesystem::path temp = path;
	temp += L".tmp";

	{
		std::ofstream file(temp, std::ios::trunc);
		if (!file)
		{
			log_error("[Config] failed to open TextureOverride.ini.tmp");
			return false;
		}

		file << "; generated by tex_override\n";
		file << "; hash        = texture hash (descriptor or pixel, see hash_mode); * = any texture\n";
		file << "; match_ps_hash = CRC32C of the pixel shader DXBC (stable across restarts)\n";
		file << "; match_vs_hash = CRC32C of the vertex shader DXBC (stable across restarts)\n";
		file << "; conditions inside one section are AND; sections are OR (higher match_priority wins)\n\n";

		file << "[Settings]\n";
		file << "hash_mode = "
			<< (hash_mode_value == static_cast<int>(hash_mode::pixel) ? "pixel" : "descriptor")
			<< "\n";
		file << "safe_mode = " << (safe_mode ? 1 : 0) << "\n\n";

		for (size_t i = 0; i < rules.size(); ++i)
		{
			const texture_override_rule &rule = rules[i];

			file << "[TextureOverride_" << i << "]\n";

			if (rule.hash_any)
			{
				file << "hash = *\n";
			}
			else
			{
				char hash_text[16];
				std::snprintf(hash_text, sizeof(hash_text), "0x%08X", rule.hash);
				file << "hash = " << hash_text << "\n";
			}

			if (rule.ps_hash != 0)
			{
				char ps_text[16];
				std::snprintf(ps_text, sizeof(ps_text), "0x%08X", rule.ps_hash);
				file << "match_ps_hash = " << ps_text << "\n";
			}

			if (rule.vs_hash != 0)
			{
				char vs_text[16];
				std::snprintf(vs_text, sizeof(vs_text), "0x%08X", rule.vs_hash);
				file << "match_vs_hash = " << vs_text << "\n";
			}

			const std::string first_index = value_set_to_string(rule.first_index);
			if (!first_index.empty())
				file << "match_first_index = " << first_index << "\n";

			const std::string index_count = value_set_to_string(rule.index_count);
			if (!index_count.empty())
				file << "match_index_count = " << index_count << "\n";

			if (rule.priority != 0)
				file << "match_priority = " << rule.priority << "\n";

			if (rule.hotkey != 0)
			{
				char hotkey_text[16];
				std::snprintf(hotkey_text, sizeof(hotkey_text), "0x%08X", rule.hotkey);
				file << "hotkey = " << hotkey_text << "\n";
			}

			file << "handling = " << (rule.handling == handling_mode::skip ? "skip" : "replace") << "\n";
			file << "active = " << (rule.active ? 1 : 0) << "\n\n";
		}

		file.flush();
		if (!file)
		{
			log_error("[Config] failed to write TextureOverride.ini.tmp");
			return false;
		}
	}

	if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
	{
		log_error("[Config] failed to replace TextureOverride.ini");
		return false;
	}

	char message[128];
	std::snprintf(message, sizeof(message), "[Config] saved %u rules", static_cast<unsigned>(rules.size()));
	log_info(message);
	return true;
}

// -----------------------------------------------------------------------------
// Descriptor hash
// -----------------------------------------------------------------------------
static uint32_t descriptor_hash_from_texture2d(const D3D11_TEXTURE2D_DESC &desc)
{
	char text[128];
	std::snprintf(text, sizeof(text), "%u_%u_%u_%u_%u",
		desc.Width,
		desc.Height,
		static_cast<unsigned>(desc.Format),
		desc.MipLevels,
		desc.ArraySize);

	return crc32c(text, std::strlen(text));
}

// 缓存以 **资源** 句柄为 key：视图随时被创建/销毁，但同一纹理的描述符哈希
// 不会变。失效只发生在该纹理真的被销毁时（见 drain_destroyed_textures）。
static uint32_t resolve_descriptor_hash(device *dev, resource_view view)
{
	// 非 D3D11 下不能把 ReShade 句柄强转成 ID3D11Resource*：
	// 直接返回 0（纹理哈希匹配失效），但 shader 哈希 / 通配符规则仍然可用。
	device_data *data = get_device_data(dev);
	if (data == nullptr || !data->is_d3d11)
		return 0;

	const uint64_t epoch = g_resource_hash_epoch.load(std::memory_order_relaxed);
	if (t_resource_hash_epoch != epoch)
	{
		// 全局兜底失效（设备拆除 / 句柄复用告警 / 线程落后超过墓碑环容量）
		t_resource_hash_epoch = epoch;
		reset_descriptor_hash_cache(g_dead_textures.seq.load(std::memory_order_acquire));
	}
	else
	{
		// 精确失效：只摘掉此期间真正销毁的那几张纹理
		drain_destroyed_textures();
	}

	const resource res = dev != nullptr ? dev->get_resource_from_view(view) : resource{};
	if (res.handle == 0)
		return 0;

	const auto cached = t_resource_hashes.find(res.handle);
	if (cached != t_resource_hashes.end())
	{
		g_cache_hits.fetch_add(1, std::memory_order_relaxed);
		return cached->second;
	}

	if (t_resource_hashes.size() > 4096)
	{
		g_current_cache_size.fetch_sub(static_cast<int>(t_resource_hashes.size()), std::memory_order_relaxed);
		t_resource_hashes.clear();
	}

	g_cache_misses.fetch_add(1, std::memory_order_relaxed);

	// 安全网（只走未命中路径，成本可忽略）：只有我们在 init_resource 中登记过的
	// 2D 纹理才允许解析。挡的是“view 在 revalidate 之后、这里解析之前被销毁”
	// 的窄窗——那时 get_resource_from_view 可能返回已释放的 ID3D11Resource*，
	// 紧接着的 GetType/GetDesc 会直接把非法指针喂进驱动。
	{
		std::lock_guard<std::mutex> lock(data->mutex);
		if (data->live_textures.find(res.handle) == data->live_textures.end())
		{
			if (!g_logged_untracked_resource.exchange(true, std::memory_order_relaxed))
			{
				log_warn("[tex_override] descriptor hash skipped: resource not tracked "
					"(created before injection?); texture-hash rules may miss it");
			}
			return 0;
		}
	}

	// 在读取任何内容之前先取一次墓碑环位置：
	// 之后插入的条目若恰好在此期间被销毁，必须能通过“二次校验”摘掉。
	const uint64_t ring_position = g_dead_textures.seq.load(std::memory_order_acquire);

	uint32_t hash = 0;

	// 缓存未命中才做一次 GetDesc；资源句柄在本函数返回时仍然有效（draw 期间不会销毁）
	auto *resource = reinterpret_cast<ID3D11Resource *>(static_cast<uintptr_t>(res.handle));
	if (resource != nullptr)
	{
		D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
		resource->GetType(&dimension);

		if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
		{
			D3D11_TEXTURE2D_DESC desc = {};
			static_cast<ID3D11Texture2D *>(resource)->GetDesc(&desc);
			hash = descriptor_hash_from_texture2d(desc);
		}
	}

	if (hash == 0)
		return 0;

	t_resource_hashes.emplace(res.handle, hash);
	g_current_cache_size.fetch_add(1, std::memory_order_relaxed);

	// 二次校验（TOCTOU 收尾）：
	// 若在 ring_position 之后又有纹理销毁，可能正好是本句柄——销毁事件与上面的
	// GetDesc 并发。这里直接在墓碑区间里精确查找本句柄；命中说明刚插入的缓存
	// 已经过期，整表作废（极端罕见：同类事件每线程每次销毁最多一次）。
	if (texture_destroyed_since(res.handle, ring_position))
		reset_descriptor_hash_cache(g_dead_textures.seq.load(std::memory_order_acquire));

	return hash;
}

// -----------------------------------------------------------------------------
// 像素内容哈希（hash_mode::pixel 使用）
//
// 关键点：哈希在资源创建/CPU 上传时就计算，**与当前 hash_mode 无关**。
// 否则用户运行中切到 pixel 模式时，此前创建的所有纹理都没有哈希，
// 候选列表会永远是空的。为控制开销，每张纹理最多扫描 kPixelHashByteBudget
// 字节（超出时按固定步长抽行），并且绝不回读 GPU。
// -----------------------------------------------------------------------------
static constexpr uint64_t kPixelHashByteBudget = 512ull * 1024ull; // 512 KiB / texture

static bool compute_pixel_hash(
	const resource_desc &desc,
	const void *data,
	uint32_t row_pitch,
	uint32_t slice_pitch,
	uint32_t &out_hash,
	uint64_t *out_bytes = nullptr)
{
	if (data == nullptr || slice_pitch == 0 || desc.type != resource_type::texture_2d)
		return false;
	if (desc.texture.samples > 1)
		return false;

	const uint32_t row_bytes = format_row_pitch(desc.texture.format, desc.texture.width);
	const uint32_t rows = format_is_block_compressed(desc.texture.format)
		? (desc.texture.height + 3) / 4
		: desc.texture.height;

	if (row_bytes == 0 || rows == 0 || row_pitch < row_bytes)
		return false;

	const uint64_t available = slice_pitch;
	const uint64_t pitch = row_pitch;

	uint32_t count = rows;
	if (available < pitch * rows)
	{
		if (available < pitch)
			return false;
		count = static_cast<uint32_t>(available / pitch);
	}

	// 大纹理抽样：固定步长，保证同一进程内结果确定
	uint32_t step = 1;
	if (static_cast<uint64_t>(count) * pitch > kPixelHashByteBudget)
		step = static_cast<uint32_t>((static_cast<uint64_t>(count) * pitch + kPixelHashByteBudget - 1) / kPixelHashByteBudget);

	// 把尺寸/格式混入初值，避免同内容不同尺寸的纹理撞哈希
	uint32_t crc = 0xFFFFFFFFu;
	crc ^= desc.texture.width * 0x9E3779B1u;
	crc ^= static_cast<uint32_t>((desc.texture.height & 0xFFFFu) << 16 | (static_cast<uint32_t>(desc.texture.format) & 0xFFFFu));
	crc = (crc << 1) | (crc >> 31);

	const auto *base = static_cast<const uint8_t *>(data);
	uint64_t hashed_bytes = 0;

	for (uint32_t y = 0; y < count; y += step)
	{
		const auto *row = base + static_cast<size_t>(y) * pitch;
		for (uint32_t x = 0; x < row_bytes; ++x)
			crc = (crc >> 8) ^ kCrc32cTable[(crc ^ row[x]) & 0xFFu];
		hashed_bytes += row_bytes;
	}

	out_hash = crc ^ 0xFFFFFFFFu;
	if (out_bytes != nullptr)
		*out_bytes = hashed_bytes;
	return true;
}

static void on_init_resource(
	device *dev,
	const resource_desc &desc,
	const subresource_data *initial_data,
	resource_usage /*initial_state*/,
	resource res)
{
	auto *data = get_device_data(dev);
	if (data == nullptr)
		return;

	// 只登记 2D 纹理：descriptor 哈希缓存只对纹理生效，CB/RTV/DSV 等
	// 每帧几十个的临时资源既不进集合、也不会在销毁时触发失效。
	uint32_t hash = 0;
	uint64_t bytes = 0;
	const bool has_pixel_hash =
		!data->safe_mode.load(std::memory_order_relaxed) &&
		initial_data != nullptr && initial_data->data != nullptr &&
		compute_pixel_hash(desc, initial_data->data, initial_data->row_pitch, initial_data->slice_pitch, hash, &bytes);

	bool handle_reused = false;
	bool hash_inserted = false;

	{
		std::lock_guard<std::mutex> lock(data->mutex);

		if (desc.type == resource_type::texture_2d)
		{
			// 句柄已在集合里 => 上一张同句柄纹理的销毁事件丢失了
			// （例如注入前创建的资源）。此时必须保守失效一次，避免命中脏哈希。
			handle_reused = !data->live_textures.insert(res.handle).second;
		}

		if (has_pixel_hash)
			hash_inserted = data->pixel_hashes.emplace(res, hash).second;
	}

	if (handle_reused)
	{
		g_resource_hash_epoch.fetch_add(1, std::memory_order_relaxed);
		log_warn("[tex_override] texture handle reused without destroy event; descriptor hash cache invalidated");
	}

	if (!hash_inserted)
		return;

	// 新哈希可用，让 draw 侧的负缓存失效
	g_pixel_insert_epoch.fetch_add(1, std::memory_order_relaxed);
	g_pixel_hash_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

static void on_destroy_resource(device *dev, resource res)
{
	device_data *data = get_device_data(dev);

	// 设备拆除中（device_data 已失效）：无法判断资源类型，只能保守地让
	// 所有线程的 descriptor 缓存整体作废一次。
	if (data == nullptr)
	{
		g_resource_hash_epoch.fetch_add(1, std::memory_order_relaxed);
		return;
	}

	bool was_texture = false;
	bool pixel_removed = false;

	{
		std::lock_guard<std::mutex> lock(data->mutex);
		was_texture = data->live_textures.erase(res.handle) != 0;
		pixel_removed = data->pixel_hashes.erase(res) != 0;
	}

	// 只有被登记的 2D 纹理销毁才失效缓存。一帧内几十个 CB/RTV 的销毁
	// 不再引起全量清空 + GetDesc 风暴（这是原先的掉帧来源）。
	if (was_texture)
		note_destroyed_texture(res.handle);

	if (pixel_removed)
		g_pixel_reset_epoch.fetch_add(1, std::memory_order_relaxed);
}

// view 销毁：登记墓碑，让各命令列表在下次 push/draw 前把相关绑定摘掉。
//
// 无条件登记（不再看“是否正在使用绑定表”）：那个判断会引入竞态——
//   场景 A：规则曾经存在 → 绑定表里留着规则时代的 view 句柄 → 规则被删光
//          （g_texture_rules_active 变 false）→ 这期间销毁的 view 不入环 →
//          重新启用规则后，这些悬垂句柄直接进入解析路径（UAF）。
//   场景 B：规则刚发布，销毁事件恰好落在判断之前。
// 代价只有一次 fetch_add + 一个槽写入（每帧几十次），完全可以无条件承担。
static void on_destroy_resource_view(device *dev, resource_view view)
{
	if (view.handle == 0)
		return;

	note_destroyed_view(view.handle);

	// 像素哈希的线程局部缓存以 view 句柄为键，无法跨线程精确摘除：
	// 只在 pixel 模式（调试/狩猎精度模式）下整体失效一次，主路径不受影响。
	device_data *data = get_device_data(dev);
	if (data != nullptr &&
		data->hash_mode_value.load(std::memory_order_relaxed) == static_cast<int>(hash_mode::pixel))
	{
		g_pixel_reset_epoch.fetch_add(1, std::memory_order_relaxed);
	}
}

static bool on_update_texture_region(
	device *dev,
	const subresource_data &src,
	resource dst,
	uint32_t dst_subresource,
	const subresource_box *dst_box)
{
	if (dst_subresource != 0 || dst_box != nullptr)
		return false;
	// 没有 CPU 数据就无从计算，直接放弃（防止空指针解引用）
	if (src.data == nullptr)
		return false;

	auto *data = get_device_data(dev);
	if (data == nullptr)
		return false;

	// 安全模式：不做任何 CPU 纹理数据扫描
	if (data->safe_mode.load(std::memory_order_relaxed))
		return false;

	// 先查表再计算：动态纹理可能每帧 UpdateSubresource，不能每帧重算
	{
		std::lock_guard<std::mutex> lock(data->mutex);
		if (data->pixel_hashes.find(dst) != data->pixel_hashes.end())
			return false;
	}

	const resource_desc desc = dev->get_resource_desc(dst);

	uint32_t hash = 0;
	uint64_t bytes = 0;
	if (!compute_pixel_hash(desc, src.data, src.row_pitch, src.slice_pitch, hash, &bytes))
		return false;

	{
		std::lock_guard<std::mutex> lock(data->mutex);
		if (!data->pixel_hashes.emplace(dst, hash).second)
			return false; // 已有哈希（首帧内容为准，避免动态纹理每帧重算）
	}

	g_pixel_insert_epoch.fetch_add(1, std::memory_order_relaxed);
	g_pixel_hash_bytes.fetch_add(bytes, std::memory_order_relaxed);

	return false;
}

// draw 侧查像素哈希：线程局部缓存 + 负结果按插入 epoch 节流，只在必要时加锁一次
static uint32_t lookup_pixel_hash(device_data *data, device *dev, resource_view view)
{
	const uint64_t reset_epoch = g_pixel_reset_epoch.load(std::memory_order_relaxed);
	if (t_pixel_reset_seen != reset_epoch)
	{
		t_pixel_reset_seen = reset_epoch;
		t_pixel_hits.clear();
		t_pixel_misses.clear();
	}

	const uint64_t key = view.handle;

	const auto hit = t_pixel_hits.find(key);
	if (hit != t_pixel_hits.end())
		return hit->second;

	const uint64_t insert_epoch = g_pixel_insert_epoch.load(std::memory_order_relaxed);
	const auto miss = t_pixel_misses.find(key);
	if (miss != t_pixel_misses.end() && miss->second == insert_epoch)
		return 0; // 本 epoch 内已确认没有哈希，不重复加锁

	const resource res = dev->get_resource_from_view(view);
	if (res.handle == 0)
		return 0;

	uint32_t value = 0;
	{
		std::lock_guard<std::mutex> lock(data->mutex);
		const auto it = data->pixel_hashes.find(res);
		if (it != data->pixel_hashes.end())
			value = it->second;
	}

	if (value != 0)
	{
		if (t_pixel_hits.size() > 4096)
			t_pixel_hits.clear();
		t_pixel_hits.emplace(key, value);
		t_pixel_misses.erase(key);
	}
	else
	{
		if (t_pixel_misses.size() > 4096)
			t_pixel_misses.clear();
		t_pixel_misses[key] = insert_epoch;
	}

	return value;
}

// -----------------------------------------------------------------------------
// 稳定标识：着色器字节码哈希（跨会话不变）
//
// init_pipeline: D3D11 的 CreateVertexShader / CreatePixelShader 触发（低频），
//                对归一化后的 DXBC 计算哈希并登记到 map。
// bind_pipeline: 高频回调，**只做两次无锁 store**，不查表、不加锁。
// draw_indexed : 发现 pipeline 句柄或 epoch 变化时才解析一次哈希，
//                解析走线程局部缓存；每个 pipeline 每个线程最多加锁一次。
// destroy_pipeline: 低频，移除条目并推进 epoch（句柄可能被复用）。
// -----------------------------------------------------------------------------
static shader_signature lookup_shader_signature(device *dev, uint64_t handle)
{
	shader_signature signature;
	if (handle == 0)
		return signature;

	const uint64_t epoch = g_pipeline_epoch.load(std::memory_order_relaxed);
	if (t_pipeline_epoch_seen != epoch)
	{
		t_pipeline_epoch_seen = epoch;
		t_pipeline_hashes.clear();
	}

	const auto cached = t_pipeline_hashes.find(handle);
	if (cached != t_pipeline_hashes.end())
		return cached->second;

	auto *data = get_device_data(dev);
	if (data == nullptr)
	{
		// 边缘场景（device_data 尚未建立/已失效）：同样写负缓存，
		// 避免每次 draw 都重复走这条慢路径。
		if (t_pipeline_hashes.size() > 4096)
			t_pipeline_hashes.clear();
		t_pipeline_hashes[handle] = signature;
		return signature;
	}

	{
		std::lock_guard<std::mutex> lock(data->shader_mutex);
		const auto found = data->shader_hashes.find(handle);
		if (found != data->shader_hashes.end())
			signature = found->second;
	}

	// 负缓存：即使没查到签名也要写入线程局部缓存。
	// 否则没有经过 init_pipeline 的 pipeline 会在每次 draw 抢 shader_mutex。
	// 失效依赖 g_pipeline_epoch（destroy_pipeline 时推进）。
	if (t_pipeline_hashes.size() > 4096)
		t_pipeline_hashes.clear();
	t_pipeline_hashes[handle] = signature;

	if (signature.vertex != 0 || signature.pixel != 0)
		data->shader_hashes_resolved.fetch_add(1, std::memory_order_relaxed);

	return signature;
}

// draw 侧解析：句柄与 epoch 都没变时只有几次整数比较
static void resolve_shader_hashes(device *dev, cmd_list_data *cmd)
{
	const uint64_t epoch = g_pipeline_epoch.load(std::memory_order_relaxed);

	if (cmd->resolved_pipeline[stage_pixel] == cmd->current_pipeline[stage_pixel] &&
		cmd->resolved_pipeline[stage_vertex] == cmd->current_pipeline[stage_vertex] &&
		cmd->shader_epoch == epoch)
		return;

	cmd->stage_hash[stage_pixel] =
		lookup_shader_signature(dev, cmd->current_pipeline[stage_pixel]).pixel;
	cmd->stage_hash[stage_vertex] =
		lookup_shader_signature(dev, cmd->current_pipeline[stage_vertex]).vertex;

	cmd->resolved_pipeline[stage_pixel] = cmd->current_pipeline[stage_pixel];
	cmd->resolved_pipeline[stage_vertex] = cmd->current_pipeline[stage_vertex];
	cmd->shader_epoch = epoch;
}

static void on_init_pipeline(
	device *dev,
	pipeline_layout /*layout*/,
	uint32_t subobject_count,
	const pipeline_subobject *subobjects,
	pipeline pipe)
{
	if (pipe.handle == 0 || subobjects == nullptr || subobject_count == 0)
		return;

	auto *data = get_device_data(dev);
	if (data == nullptr)
		return;

	shader_signature signature;

	for (uint32_t i = 0; i < subobject_count; ++i)
	{
		const pipeline_subobject &object = subobjects[i];
		if (object.data == nullptr || object.count == 0)
			continue;

		uint32_t *target = nullptr;
		switch (object.type)
		{
		case pipeline_subobject_type::vertex_shader:
			target = &signature.vertex;
			break;
		case pipeline_subobject_type::pixel_shader:
			target = &signature.pixel;
			break;
		default:
			continue;
		}

		// 遍历该 sub-object 的所有条目，取第一个带字节码的（兼容多条目后端）
		const auto *shaders = static_cast<const shader_desc *>(object.data);
		for (uint32_t j = 0; j < object.count; ++j)
		{
			if (shaders[j].code == nullptr || shaders[j].code_size == 0)
				continue;

			*target = hash_shader_bytecode(shaders[j].code, shaders[j].code_size);
			break;
		}
	}

	if (signature.vertex == 0 && signature.pixel == 0)
		return;

	if (!g_logged_shader_hash.exchange(true, std::memory_order_relaxed))
	{
		char message[160];
		std::snprintf(message, sizeof(message),
			"[Shader] DXBC hash captured: ps=0x%08X vs=0x%08X (header/checksum excluded)",
			signature.pixel, signature.vertex);
		log_info(message);
	}

	// 顺手写入本线程缓存（覆盖可能存在的负缓存），紧随其后的解析就不必加锁
	if (t_pipeline_hashes.size() > 4096)
		t_pipeline_hashes.clear();
	t_pipeline_hashes[pipe.handle] = signature;

	std::lock_guard<std::mutex> lock(data->shader_mutex);
	data->shader_hashes[pipe.handle] = signature;
}

static void on_bind_pipeline(command_list *cmd_list, pipeline_stage stages, pipeline pipe)
{
	// 高频路径：六个 shader 阶段都只做一次无锁 store，不查表、不加锁、不算哈希
	cmd_list_data *cmd_data = get_cmd_data(cmd_list);
	if (cmd_data == nullptr)
		return;

	const uint32_t bits = static_cast<uint32_t>(stages);
	for (int stage = 0; stage < stage_count; ++stage)
	{
		if ((bits & static_cast<uint32_t>(kPipelineStageBits[stage])) != 0)
			cmd_data->current_pipeline[stage] = pipe.handle;
	}
}

static void on_destroy_pipeline(device *dev, pipeline pipe)
{
	// 必须走 epoch 保护的访问路径：device 可能已在别的线程拆除/复用地址，
	// 直接用 dev->get_private_data 会绕过校验，构成 UAF。
	if (device_data *data = get_device_data(dev); data != nullptr)
	{
		std::lock_guard<std::mutex> lock(data->shader_mutex);
		data->shader_hashes.erase(pipe.handle);
	}

	// 句柄可能被复用：推进 epoch 丢弃所有线程的着色器缓存
	g_pipeline_epoch.fetch_add(1, std::memory_order_relaxed);
}

// -----------------------------------------------------------------------------
// 狩猎采集纪元
//
// 挂机期间（无纹理规则、未采集）push_descriptors 一直早退，stage_bindings 可能
// 保存着早已销毁的 view 句柄。开启 Hunting 时推进全局纪元，各命令列表在
// **下一次 push_descriptors** 中清空整张表，并由本帧的真实绑定重新填充。
//
// 职责分工（关键）：
//   sync_hunting_epoch     —— 只允许在 on_push_descriptors 内调用：清空 + 记录新纪元。
//   hunting_epoch_matches  —— draw 侧只比对、绝不清空。不一致说明本帧的绑定还没进来，
//                             现有句柄可能悬垂，本次直接跳过，而不是拿它去解引用。
// -----------------------------------------------------------------------------
static void sync_hunting_epoch(cmd_list_data *cmd_data)
{
	const uint64_t epoch = g_hunting_collect_epoch.load(std::memory_order_relaxed);
	if (cmd_data->current_hunting_epoch == epoch)
		return;

	for (int stage = 0; stage < stage_count; ++stage)
		cmd_data->stages[stage] = {};

	cmd_data->current_hunting_epoch = epoch;
}

static bool hunting_epoch_matches(const cmd_list_data *cmd_data)
{
	return cmd_data->current_hunting_epoch == g_hunting_collect_epoch.load(std::memory_order_relaxed);
}

// -----------------------------------------------------------------------------
// push_descriptors：只记录 SRV 句柄，绝不做哈希计算
//
// 最快的出口：没有纹理规则且不在狩猎采集时直接返回，连阶段/槽位都不记录。
// -----------------------------------------------------------------------------
static void on_push_descriptors(
	command_list *cmd_list,
	shader_stage stages,
	pipeline_layout /*layout*/,
	uint32_t /*param_index*/,
	const descriptor_table_update &update)
{
	const bool hunting = g_hunting_collecting.load(std::memory_order_relaxed);
	const bool texture_rules = g_texture_rules_active.load(std::memory_order_relaxed);

	// 没有纹理规则、也不在狩猎采集 -> 完全不需要 SRV 绑定信息
	if (!hunting && !texture_rules)
		return;

	cmd_list_data *cmd_data = get_cmd_data(cmd_list);
	if (cmd_data == nullptr)
		return;

	// 只有真正的 SRV 更新才会刷新绑定表；CB/UAV 等更新不做处理，
	// 因此纪元同步要放在这些早退之后——避免“清空了旧绑定、却没有任何新绑定
	// 写入就被标记为目标已刷新”的窗口。
	if (update.type != descriptor_type::shader_resource_view &&
		update.type != descriptor_type::sampler_with_resource_view)
		return;
	if (update.count == 0 || update.descriptors == nullptr)
		return;

	// 新纪元：先清空可能残留的陈旧 view 句柄，再写入本次的真实绑定。
	// 清空只发生在这里（binding 只能由 push 刷新），draw 侧只比对不清空。
	sync_hunting_epoch(cmd_data);

	// 关键顺序：先摘掉已销毁的 view，再执行 set()。
	// 否则 set() 的 `old == view` 快路径会把旧哈希记成“新 view 的哈希”。
	revalidate_view_bindings(cmd_data);

	// 普通变量自增，present 时汇总（避免高频 atomic RMW）
	++cmd_data->local_push_updates;

	for (int stage = 0; stage < stage_count; ++stage)
	{
		// 用真实的 shader_stage 位标志匹配，覆盖 VS/HS/DS/GS/PS/CS
		if ((static_cast<uint32_t>(stages) & static_cast<uint32_t>(kStageList[stage])) == 0)
			continue;

		stage_bindings &binding = cmd_data->stages[stage];

		// 默认状态清理会一次性绑定 0..127 的空视图，直接重置更高效
		if (update.count >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT &&
			update.binding == 0)
		{
			binding = {};
			continue;
		}

		for (uint32_t i = 0; i < update.count; ++i)
		{
			const UINT slot = update.binding + i;
			if (slot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT)
				break;

			resource_view view = {};
			if (update.type == descriptor_type::shader_resource_view)
				view = static_cast<const resource_view *>(update.descriptors)[i];
			else
				view = static_cast<const sampler_with_resource_view *>(update.descriptors)[i].view;

			// 只写句柄；哈希推迟到 draw_indexed 真正需要匹配时才计算（线程本地缓存）
			binding.set(slot, view);
		}
	}
}

static void on_init_command_list(command_list *cmd_list)
{
	if (cmd_list->get_private_data<cmd_list_data>() == nullptr)
		cmd_list->create_private_data<cmd_list_data>();
}

static void on_destroy_command_list(command_list *cmd_list)
{
	// 走 epoch 保护的访问路径（与 push/draw 一致）：命令列表可能已在别的线程
	// 销毁且地址被复用，直接 cmd_list->get_private_data 会绕过校验。
	cmd_list_data *cmd_data = get_cmd_data(cmd_list);

	// 先从本线程的汇总列表 / 快慢路径缓存里摘掉，避免 present 读到已释放的计数器
	if (cmd_data != nullptr)
	{
		for (auto it = t_owned_cmd_data.begin(); it != t_owned_cmd_data.end(); ++it)
		{
			if (*it == cmd_data)
			{
				t_owned_cmd_data.erase(it);
				break;
			}
		}

		if (t_last_cmd_data == cmd_data)
		{
			t_last_cmd_list = nullptr;
			t_last_cmd_data = nullptr;
		}

		for (auto it = t_cmd_data.begin(); it != t_cmd_data.end(); ++it)
		{
			if (it->second == cmd_data)
			{
				t_cmd_data.erase(it);
				break;
			}
		}
	}

	// 先推进 epoch，再释放私有数据：否则存在“别的线程刚读到旧 epoch、
	// 随后拿到即将被释放的指针”的窗口。
	g_cmd_list_epoch.fetch_add(1, std::memory_order_relaxed);

	cmd_list->destroy_private_data<cmd_list_data>();
}

// -----------------------------------------------------------------------------
// init / destroy
// -----------------------------------------------------------------------------
static void on_init_device(device *dev)
{
	auto *data = dev->create_private_data<device_data>();

	// 跨 API 兼容性检测：非 D3D11 时所有原生强转都必须禁用
	data->is_d3d11 = (dev->get_api() == device_api::d3d11);
	if (!data->is_d3d11)
	{
		log_warn("[tex_override] Compatibility issue: non-D3D11 API detected (D3D12/Vulkan). "
			"Some features (like replace) will be disabled.");
	}

	const uint32_t transparent = 0x00000000u;

	subresource_data initial = {};
	initial.data = const_cast<uint32_t *>(&transparent);
	initial.row_pitch = sizeof(transparent);
	initial.slice_pitch = sizeof(transparent);

	const resource_desc desc(
		1, 1, 1, 1,
		format::r8g8b8a8_unorm,
		1,
		memory_heap::default_,
		resource_usage::shader_resource);

	if (!dev->create_resource(desc, &initial, resource_usage::shader_resource, &data->null_texture))
	{
		log_error("[tex_override] failed to create 1x1 null texture");
		return;
	}

	if (!dev->create_resource_view(
			data->null_texture,
			resource_usage::shader_resource,
			resource_view_desc(format::r8g8b8a8_unorm),
			&data->null_srv))
	{
		log_error("[tex_override] failed to create 1x1 null SRV");
		dev->destroy_resource(data->null_texture);
		data->null_texture = {};
		return;
	}

	log_info("[tex_override] INI rule mode; 1x1 null SRV created");
	load_config(data);
}

static void on_destroy_device(device *dev)
{
	auto *data = dev->get_private_data<device_data>();
	if (data == nullptr)
		return;

	if (data->null_srv.handle != 0)
		dev->destroy_resource_view(data->null_srv);
	if (data->null_texture.handle != 0)
		dev->destroy_resource(data->null_texture);

	// 先清残留状态、再推进 epoch，最后才释放私有数据：
	// 保证任何线程要么看到旧 device_data（仍存活），要么看到新 epoch 并清缓存。
	g_texture_rules_active.store(false, std::memory_order_release);
	g_hunting_collecting.store(false, std::memory_order_relaxed);
	// 设备级状态全部失效：候选预览也要清掉，否则新设备会继续隐藏旧哈希
	set_candidate_preview(false, 0, 0, 0);
	// 设备级资源全部失效：descriptor / 像素哈希缓存必须整体作废
	g_resource_hash_epoch.fetch_add(1, std::memory_order_relaxed);
	g_pixel_reset_epoch.fetch_add(1, std::memory_order_relaxed);
	g_device_epoch.fetch_add(1, std::memory_order_relaxed);

	dev->destroy_private_data<device_data>();
}

// -----------------------------------------------------------------------------
// replace 处理
// -----------------------------------------------------------------------------
static void bind_null_to_stage(ID3D11DeviceContext *ctx, shader_stage stage, UINT slot, ID3D11ShaderResourceView *null_view)
{
	switch (stage)
	{
	case shader_stage::pixel:
		ctx->PSSetShaderResources(slot, 1, &null_view);
		break;
	case shader_stage::vertex:
		ctx->VSSetShaderResources(slot, 1, &null_view);
		break;
	case shader_stage::geometry:
		ctx->GSSetShaderResources(slot, 1, &null_view);
		break;
	case shader_stage::compute:
		ctx->CSSetShaderResources(slot, 1, &null_view);
		break;
	case shader_stage::hull:
		ctx->HSSetShaderResources(slot, 1, &null_view);
		break;
	case shader_stage::domain:
		ctx->DSSetShaderResources(slot, 1, &null_view);
		break;
	default:
		break;
	}
}

// -----------------------------------------------------------------------------
// draw_indexed
//
// 两条路径：
//   特快通道 hash = * ：只比较 shader 哈希与几何参数，完全不触碰 SRV 列表
//   慢速通道 纹理哈希 ：遍历 active SRV，哈希按需计算并回写绑定缓存
// -----------------------------------------------------------------------------
static void null_active_slots(command_list *cmd_list, cmd_list_data *cmd_data, device_data *data)
{
	if (!data->is_d3d11)
	{
		note_replace_unavailable(data);
		return;
	}

	auto *ctx = reinterpret_cast<ID3D11DeviceContext *>(static_cast<uintptr_t>(cmd_list->get_native()));
	auto *null_view = reinterpret_cast<ID3D11ShaderResourceView *>(static_cast<uintptr_t>(data->null_srv.handle));
	if (ctx == nullptr || null_view == nullptr)
		return;

	for (int stage = 0; stage < stage_count; ++stage)
	{
		stage_bindings &binding = cmd_data->stages[stage];
		for (uint16_t i = 0; i < binding.active_count; ++i)
			bind_null_to_stage(ctx, kStageList[stage], binding.active[i].first, null_view);
	}
}

static void null_texture_slots(
	command_list *cmd_list,
	cmd_list_data *cmd_data,
	device_data *data,
	bool pixel_mode,
	uint32_t target_hash)
{
	if (!data->is_d3d11)
	{
		note_replace_unavailable(data);
		return;
	}

	auto *ctx = reinterpret_cast<ID3D11DeviceContext *>(static_cast<uintptr_t>(cmd_list->get_native()));
	auto *null_view = reinterpret_cast<ID3D11ShaderResourceView *>(static_cast<uintptr_t>(data->null_srv.handle));
	if (ctx == nullptr || null_view == nullptr)
		return;


	for (int stage = 0; stage < stage_count; ++stage)
	{
		stage_bindings &binding = cmd_data->stages[stage];
		for (uint16_t i = 0; i < binding.active_count; ++i)
		{
			const bound_entry &entry = binding.active[i].second;
			const uint32_t hash = pixel_mode ? entry.pixel_hash : entry.descriptor_hash;
			if (hash == target_hash)
				bind_null_to_stage(ctx, kStageList[stage], binding.active[i].first, null_view);
		}
	}
}

// 狩猎模式专用：把所有绑定纹理记录成候选（不在正常渲染路径上）
// 先在栈上收集，再一次性加锁批量插入，避免一个 draw 内多次抢锁。
static void record_draw_candidates(
	device *dev,
	device_data *data,
	cmd_list_data *cmd_data,
	bool pixel_mode,
	const draw_context &ctx,
	uint32_t first_index,
	uint32_t index_count)
{
	struct pending_candidate
	{
		candidate_key key;
		candidate value;
	};

	// SRV 槽位最多 6*128，实际通常个位数；固定上限避免堆分配
	static thread_local std::array<pending_candidate, 32> pending;
	uint32_t pending_count = 0;
	bool limit_reached = false;

	for (int stage = 0; stage < stage_count && !limit_reached; ++stage)
	{
		stage_bindings &binding = cmd_data->stages[stage];

		for (uint16_t i = 0; i < binding.active_count; ++i)
		{
			bound_entry &entry = binding.active[i].second;
			const UINT slot = binding.active[i].first;
			const resource_view view = entry.view;
			if (view.handle == 0)
				continue;

			uint32_t hash = 0;
			if (pixel_mode)
			{
				hash = entry.pixel_hash;
				if (hash == 0)
				{
					hash = lookup_pixel_hash(data, dev, view);
					if (hash != 0)
					{
						entry.pixel_hash = hash;
						binding.slots[slot].pixel_hash = hash;
					}
				}
			}
			else
			{
				hash = entry.descriptor_hash;
				if (hash == 0)
				{
					hash = resolve_descriptor_hash(dev, view);
					entry.descriptor_hash = hash;
					// 同步写回槽位表：rebuild_active() 会用 slots[] 重建 active[]
					binding.slots[slot].descriptor_hash = hash;
				}
			}

			if (hash == 0)
				continue;

			if (pending_count >= pending.size())
			{
				// 极端情况：单次 draw 的候选异常多。停止收集（含外层 stage 循环），
				// 已收集的部分仍然批量写入，避免浪费。
				limit_reached = true;
				break;
			}

			pending[pending_count].key = candidate_key{ hash, ctx.ps_hash, ctx.vs_hash };
			pending[pending_count].value.hash = hash;
			pending[pending_count].value.ps_hash = ctx.ps_hash;
			pending[pending_count].value.vs_hash = ctx.vs_hash;
			pending[pending_count].value.first_index = first_index;
			pending[pending_count].value.index_count = index_count;
			++pending_count;
		}
	}

	if (pending_count == 0)
		return;

	std::lock_guard<std::mutex> lock(data->mutex);
	for (uint32_t i = 0; i < pending_count; ++i)
		data->frame_candidates[pending[i].key] = pending[i].value;
}

// -----------------------------------------------------------------------------
// 候选预览匹配
//
// 只有 hunting 预览开启、且本次 draw 的 PS/VS 与候选一致时才走到这里
// （两字段比较，绝大多数 draw 一次就退出）。命中判定：任意绑定 SRV 的纹理
// 哈希等于候选哈希——与慢速通道共用同一份 bound_entry / 线程局部缓存，
// 不额外产生 GetDesc。
//
// 语义上等于"这条候选覆盖的所有绘制"，因此同一纹理+同一 shader 的多个 draw
// 会一起消失，这正是预览想要的：让用户看到候选覆盖了哪块画面。
// -----------------------------------------------------------------------------
static bool preview_covers_draw(
	device *dev,
	device_data *data,
	cmd_list_data *cmd_data,
	bool pixel_mode,
	const draw_context &ctx)
{
	uint32_t target_hash = 0;
	uint32_t target_ps = 0;
	uint32_t target_vs = 0;

	if (!read_candidate_preview(target_hash, target_ps, target_vs))
		return false;

	// 候选记录时的稳定锚点必须一致（0 表示当时没抓到，按严格相等处理）
	if (target_ps != ctx.ps_hash || target_vs != ctx.vs_hash)
		return false;

	for (int stage = 0; stage < stage_count; ++stage)
	{
		stage_bindings &binding = cmd_data->stages[stage];

		for (uint16_t i = 0; i < binding.active_count; ++i)
		{
			bound_entry &entry = binding.active[i].second;
			const UINT slot = binding.active[i].first;
			const resource_view view = entry.view;
			if (view.handle == 0)
				continue;

			uint32_t hash = 0;
			if (pixel_mode)
			{
				hash = entry.pixel_hash;
				if (hash == 0)
				{
					hash = lookup_pixel_hash(data, dev, view);
					if (hash != 0)
					{
						entry.pixel_hash = hash;
						binding.slots[slot].pixel_hash = hash;
					}
				}
			}
			else
			{
				hash = entry.descriptor_hash;
				if (hash == 0)
				{
					hash = resolve_descriptor_hash(dev, view);
					entry.descriptor_hash = hash;
				}
			}

			if (hash != 0 && hash == target_hash)
				return true;
		}
	}

	return false;
}

static bool on_draw_indexed(
	command_list *cmd_list,
	uint32_t index_count,
	uint32_t /*instance_count*/,
	uint32_t first_index,
	int32_t /*vertex_offset*/,
	uint32_t /*first_instance*/)
{
	device *const dev = cmd_list->get_device();
	device_data *data = get_device_data(dev);
	if (data == nullptr)
		return false;

	const bool hunting = data->hunting.load(std::memory_order_relaxed);
	const bool collecting = hunting && data->collecting.load(std::memory_order_relaxed);
	const rule_table *rules = acquire_rules(data);
	const bool has_rules = rules != nullptr && !rules->rules.empty();
	// 预览开启时即使没有规则、也不在采集，仍需进入匹配（否则点了候选没反应）
	const bool preview_on = g_preview.enabled.load(std::memory_order_relaxed);

	// 1) 没规则、不在采集、也没开预览：立即返回（此时连 hash mode 都不读）
	if (!has_rules && !collecting && !preview_on)
		return false;

	const bool pixel_mode =
		data->hash_mode_value.load(std::memory_order_relaxed) == static_cast<int>(hash_mode::pixel);

	cmd_list_data *cmd_data = get_cmd_data(cmd_list);
	if (cmd_data == nullptr)
		return false;

	// 稳定锚点：bind_pipeline 只记录了句柄，这里按需解析（句柄/epoch 未变则零成本）
	resolve_shader_hashes(dev, cmd_data);

	// 普通变量自增，present 时汇总（draw 内没有任何 atomic RMW）
	++cmd_data->local_draws;

	draw_context ctx;
	ctx.first_index = first_index;
	ctx.index_count = index_count;
	ctx.ps_hash = cmd_data->stage_hash[stage_pixel];
	ctx.vs_hash = cmd_data->stage_hash[stage_vertex];

	if (ctx.ps_hash != 0 || ctx.vs_hash != 0)
	{
		cmd_data->last_ps_hash = ctx.ps_hash;
		cmd_data->last_vs_hash = ctx.vs_hash;
	}

	// 狩猎采集只在调试模式下发生，正常渲染完全跳过
	if (collecting)
	{
		// 只比对、不清空：新纪元后的首次 push 还没发生的话，stage_bindings 里
		// 可能仍是挂机期间留下的悬垂句柄。此时跳过本次采集（不清理，清理只由
		// push_descriptors 在新的真实绑定到来时完成），下一次 draw 自然能抓到。
		if (hunting_epoch_matches(cmd_data))
		{
			// 先摘掉已销毁的 view：候选列表绝不能把悬垂句柄解析成哈希
			revalidate_view_bindings(cmd_data);
			record_draw_candidates(dev, data, cmd_data, pixel_mode, ctx, first_index, index_count);
		}
	}

	// 2) 候选预览：把当前选中的候选隐藏掉，让用户"看见"它对应哪块画面。
	//    必须先于规则判定，且只在绑定表处于当前纪元时可用（否则句柄可能悬垂）。
	//    预览只是观察手段：退出 Hunting 时会被清除，不会污染规则表。
	//    注意不能放在 has_rules 判断之后——纯狩猎（还没有任何规则）时也要能预览。
	if (preview_on && hunting && hunting_epoch_matches(cmd_data))
	{
		revalidate_view_bindings(cmd_data);

		if (preview_covers_draw(dev, data, cmd_data, pixel_mode, ctx))
		{
			++cmd_data->local_preview_skipped;
			return true;
		}
	}

	if (!has_rules)
		return false;

	// 3) 特快通道：hash = * 规则。
	//    预过滤已在 pipeline 解析后完成：绝大多数 3D shader 的 count 为 0，
	//    这里只有一次字节比较就跳过，完全不看纹理、也不比对 shader 哈希。
	refresh_wildcard_prefilter(rules, cmd_data);

	if (cmd_data->wildcard.overflow)
	{
		for (const texture_override_rule &rule : rules->wildcard_rules)
		{
			if (!rule_matches_draw(rule, ctx))
				continue;

			++cmd_data->local_applied;
			if (rule.handling == handling_mode::skip || !data->is_d3d11)
			{
				if (rule.handling == handling_mode::replace)
					note_replace_unavailable(data);
				++cmd_data->local_skipped;
				return true;
			}

			null_active_slots(cmd_list, cmd_data, data);
			return false;
		}
	}
	else if (cmd_data->wildcard.count != 0)
	{
		for (uint8_t i = 0; i < cmd_data->wildcard.count; ++i)
		{
			const texture_override_rule &rule = *cmd_data->wildcard.rules[i];
			if (!rule_matches_geometry(rule, first_index, index_count))
				continue;

			++cmd_data->local_applied;
			if (rule.handling == handling_mode::skip || !data->is_d3d11)
			{
				if (rule.handling == handling_mode::replace)
					note_replace_unavailable(data);
				++cmd_data->local_skipped;
				return true;
			}

			// 防御：通配符 replace 在 publish 时已被降级为 skip，正常不会走到这里。
			// 真走到时也必须要求绑定表已在本纪元刷新，否则可能是悬垂句柄。
			if (hunting_epoch_matches(cmd_data))
				null_active_slots(cmd_list, cmd_data, data);
			return false;
		}
	}

	// 3) 慢速通道：只有存在纹理哈希规则时才遍历 SRV
	if (!rules->has_texture_rules)
		return false;

	// 绑定表尚未在当前纪元下刷新（开启 Hunting 后还没有新的 push）：其中的
	// view 句柄可能已经悬垂，此时绝不解析或替换，直接按未命中处理。
	if (!hunting_epoch_matches(cmd_data))
		return false;

	// 先把已销毁的 view 从绑定表里摘掉，再解析哈希/替换：
	// 句柄可能被复用成同一数值，旧哈希继续留存就会张冠李戴。
	revalidate_view_bindings(cmd_data);

	const texture_override_rule *best = nullptr;

	for (int stage = 0; stage < stage_count; ++stage)
	{
		stage_bindings &binding = cmd_data->stages[stage];

		for (uint16_t i = 0; i < binding.active_count; ++i)
		{
			bound_entry &entry = binding.active[i].second;
			const UINT slot = binding.active[i].first;
			const resource_view view = entry.view;
			if (view.handle == 0)
				continue;

			uint32_t hash = 0;
			if (pixel_mode)
			{
				// 绝不回读 GPU：哈希只来自 init_resource / update_texture_region
				hash = entry.pixel_hash;
				if (hash == 0)
				{
					hash = lookup_pixel_hash(data, dev, view);
					if (hash != 0)
					{
						entry.pixel_hash = hash;
						binding.slots[slot].pixel_hash = hash;
					}
				}
			}
			else
			{
				hash = entry.descriptor_hash;
				if (hash == 0)
				{
					hash = resolve_descriptor_hash(dev, view);
					entry.descriptor_hash = hash;
				}
			}

			if (hash == 0)
				continue;

			++cmd_data->local_srv_resolved;

			const auto range = rules->index_by_texture_hash.equal_range(hash);
			for (auto it = range.first; it != range.second; ++it)
			{
				const texture_override_rule &rule = rules->rules[it->second];
				if (!rule_matches_draw(rule, ctx))
					continue;

				if (best == nullptr ||
					rule.priority > best->priority ||
					(rule.priority == best->priority && rule.order < best->order))
					best = &rule;
			}
		}
	}

	if (best == nullptr)
		return false;

	++cmd_data->local_applied;

	if (best->handling == handling_mode::skip || !data->is_d3d11)
	{
		if (best->handling == handling_mode::replace)
			note_replace_unavailable(data);
		++cmd_data->local_skipped;
		return true;
	}

	null_texture_slots(cmd_list, cmd_data, data, pixel_mode, best->hash);
	return false;
}

// -----------------------------------------------------------------------------
// 热键（仅 Hunting Mode 生效）
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// 键盘边沿：每帧采样一次，供"规则热键"与"Hunting 键"共用
//
// 早期是每个键各自调用 key_pressed_edge()：同一个键若被两处查询，第一次消费掉
// 边沿后第二次必然返回 false（互相吞键）。现在改成一次采样整张 256 键表：
// 规则热键、Hunting 键、以及"捕获热键"都读同一份快照。
// 代价是每帧 256 次 GetAsyncKeyState（用户态、无系统调用），可忽略。
// -----------------------------------------------------------------------------
static thread_local std::array<bool, 256> t_key_down{};
static thread_local std::array<bool, 256> t_key_pressed{};

static void poll_key_edges()
{
	for (unsigned vk = 0; vk < 256; ++vk)
	{
		const bool down = (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
		t_key_pressed[vk] = down && !t_key_down[vk];
		t_key_down[vk] = down;
	}
}

static bool key_pressed(int vk)
{
	return t_key_pressed[static_cast<unsigned>(vk) & 0xFFu];
}

static bool key_down(int vk)
{
	return t_key_down[static_cast<unsigned>(vk) & 0xFFu];
}

static bool is_modifier_vk(int vk)
{
	switch (vk)
	{
	case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
	case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
	case VK_MENU: case VK_LMENU: case VK_RMENU:
		return true;
	default:
		return false;
	}
}

// -----------------------------------------------------------------------------
// 规则热键
//
// 打包格式（与 ShaderToggler 的 ToggleKey 一致，便于用户直接抄配置）：
//     (vk << 24) | (alt << 16) | (ctrl << 8) | shift
// 0 表示未绑定。语义：主键按下沿 + 修饰键"严格相等"——绑 A 时按住 Alt 不会触发。
// -----------------------------------------------------------------------------
static uint32_t make_hotkey(int vk, bool alt, bool ctrl, bool shift)
{
	return ((static_cast<uint32_t>(vk) & 0xFFu) << 24) |
		(alt ? 0x010000u : 0u) |
		(ctrl ? 0x000100u : 0u) |
		(shift ? 0x000001u : 0u);
}

static int hotkey_vk(uint32_t packed)
{
	return static_cast<int>((packed >> 24) & 0xFFu);
}

static bool modifier_down(int generic_vk, int left_vk, int right_vk)
{
	return key_down(generic_vk) || key_down(left_vk) || key_down(right_vk);
}

static bool hotkey_pressed(uint32_t packed)
{
	if (packed == 0)
		return false;

	const int vk = hotkey_vk(packed);
	if (vk == 0 || !key_pressed(vk))
		return false;

	const bool need_alt = ((packed >> 16) & 0xFFu) != 0;
	const bool need_ctrl = ((packed >> 8) & 0xFFu) != 0;
	const bool need_shift = (packed & 0xFFu) != 0;

	const bool alt = modifier_down(VK_MENU, VK_LMENU, VK_RMENU);
	const bool ctrl = modifier_down(VK_CONTROL, VK_LCONTROL, VK_RCONTROL);
	const bool shift = modifier_down(VK_SHIFT, VK_LSHIFT, VK_RSHIFT);

	return need_alt == alt && need_ctrl == ctrl && need_shift == shift;
}

// 键名（够用即可；未覆盖的键退回 "VK 0xNN"）
static const char *vk_name(int vk)
{
	switch (vk)
	{
	case VK_LBUTTON: return "Left Mouse";
	case VK_RBUTTON: return "Right Mouse";
	case VK_MBUTTON: return "Middle Mouse";
	case VK_BACK: return "Backspace";
	case VK_TAB: return "Tab";
	case VK_RETURN: return "Enter";
	case VK_PAUSE: return "Pause";
	case VK_CAPITAL: return "Caps Lock";
	case VK_ESCAPE: return "Escape";
	case VK_SPACE: return "Space";
	case VK_PRIOR: return "Page Up";
	case VK_NEXT: return "Page Down";
	case VK_END: return "End";
	case VK_HOME: return "Home";
	case VK_LEFT: return "Left Arrow";
	case VK_UP: return "Up Arrow";
	case VK_RIGHT: return "Right Arrow";
	case VK_DOWN: return "Down Arrow";
	case VK_INSERT: return "Insert";
	case VK_DELETE: return "Delete";
	case VK_DECIMAL: return "Numpad Decimal";
	case VK_DIVIDE: return "Numpad /";
	case VK_MULTIPLY: return "Numpad *";
	case VK_SUBTRACT: return "Numpad -";
	case VK_ADD: return "Numpad +";
	case VK_NUMLOCK: return "Num Lock";
	case VK_SCROLL: return "Scroll Lock";
	case VK_OEM_1: return "OEM ;";
	case VK_OEM_PLUS: return "OEM +";
	case VK_OEM_COMMA: return "OEM ,";
	case VK_OEM_MINUS: return "OEM -";
	case VK_OEM_PERIOD: return "OEM .";
	case VK_OEM_2: return "OEM /";
	case VK_OEM_3: return "OEM ~";
	case VK_OEM_4: return "OEM [";
	case VK_OEM_5: return "OEM \\";
	case VK_OEM_6: return "OEM ]";
	case VK_OEM_7: return "OEM '";
	default: break;
	}

	if (vk >= 'A' && vk <= 'Z')
	{
		static thread_local char letters[32] = {};
		letters[0] = static_cast<char>(vk);
		letters[1] = '\0';
		return letters;
	}
	if (vk >= '0' && vk <= '9')
	{
		static thread_local char digits[32] = {};
		digits[0] = static_cast<char>(vk);
		digits[1] = '\0';
		return digits;
	}
	if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
	{
		static thread_local char numpad[32] = {};
		std::snprintf(numpad, sizeof(numpad), "Numpad %d", vk - VK_NUMPAD0);
		return numpad;
	}
	if (vk >= VK_F1 && vk <= VK_F24)
	{
		static thread_local char fkeys[32] = {};
		std::snprintf(fkeys, sizeof(fkeys), "F%d", vk - VK_F1 + 1);
		return fkeys;
	}

	static thread_local char fallback[32] = {};
	std::snprintf(fallback, sizeof(fallback), "VK 0x%02X", vk);
	return fallback;
}

static std::string hotkey_to_string(uint32_t packed)
{
	if (packed == 0)
		return "None";

	std::string text;
	if (((packed >> 16) & 0xFFu) != 0) text += "Alt + ";
	if (((packed >> 8) & 0xFFu) != 0) text += "Ctrl + ";
	if ((packed & 0xFFu) != 0) text += "Shift + ";
	text += vk_name(hotkey_vk(packed));
	return text;
}

// 按下某条规则的绑定键 → 翻转它的 active 并重新发布规则表。
// 只翻转、不写盘：是否持久化留给用户点 Save to INI。
static void process_rule_hotkeys(device_data *data)
{
	// 正在捕获热键、或用户正在输入框里打字（250ms 抑制窗）时不要触发
	if (data->capturing_hotkey.load(std::memory_order_relaxed) ||
		GetTickCount64() < data->keyboard_suppressed_until.load(std::memory_order_relaxed))
	{
		return;
	}

	const rule_table *rules = acquire_rules(data);
	if (rules == nullptr || rules->rules.empty())
		return;

	const bool hunting = data->hunting.load(std::memory_order_relaxed);

	// Hunting 期间 Numpad 1/2/3 属于 Hunting 键，不允许被规则热键抢走
	static constexpr uint32_t kHumanSpeedLimit = 8;
	size_t toggles[kHumanSpeedLimit] = {};
	size_t count = 0;

	for (size_t i = 0; i < rules->rules.size(); ++i)
	{
		const uint32_t hotkey = rules->rules[i].hotkey;
		if (hotkey == 0 || !hotkey_pressed(hotkey))
			continue;

		if (hunting)
		{
			const int vk = hotkey_vk(hotkey);
			if (vk == VK_NUMPAD1 || vk == VK_NUMPAD2 || vk == VK_NUMPAD3)
				continue;
		}

		if (count < kHumanSpeedLimit)
			toggles[count++] = i;
	}

	if (count == 0)
		return;

	std::vector<texture_override_rule> updated = copy_rules(rules);

	for (size_t i = 0; i < count; ++i)
	{
		texture_override_rule &rule = updated[toggles[i]];
		rule.active = !rule.active;

		char message[256];
		std::snprintf(message, sizeof(message), "[Hotkey] '%s' -> %s (%s)",
			rule.name.empty() ? "<unnamed>" : rule.name.c_str(),
			rule.active ? "active" : "disabled",
			hotkey_to_string(rule.hotkey).c_str());
		log_info(message);
	}

	publish_rules(data, std::move(updated));
	data->has_unsaved_changes.store(true, std::memory_order_relaxed);
}

static void toggle_rule_for_candidate(device_data *data)
{
	// 先在 data 锁内取出候选，随后基于不可变规则表做写时复制
	candidate selected;

	{
		std::lock_guard<std::mutex> data_lock(data->mutex);

		if (data->candidate_index < 0 ||
			data->candidate_index >= static_cast<int>(data->candidates.size()))
			return;

		selected = data->candidates[data->candidate_index];
	}

	std::vector<texture_override_rule> rules = copy_rules(acquire_rules(data));

	// 生成规则：默认只写稳定锚点（hash = *），避免图集重排后失效
	const bool stable_only = data->stable_only_rules.load(std::memory_order_relaxed);
	const bool has_stable = selected.ps_hash != 0 || selected.vs_hash != 0;

	texture_override_rule plain;
	plain.name = "Hunting_" + std::to_string(rules.size());
	plain.ps_hash = selected.ps_hash;
	plain.vs_hash = selected.vs_hash;
	plain.first_index.add(selected.first_index);
	plain.index_count.add(selected.index_count);
	plain.handling = handling_mode::skip;

	// 没有捕获到稳定标识时，回退为纹理哈希规则
	if (stable_only && has_stable)
	{
		plain.hash = 0;
		plain.hash_any = true;
	}
	else
	{
		plain.hash = selected.hash;
		plain.hash_any = false;
	}

	// 再次按 Numpad 3 / 点击按钮 = 取消：删除所有覆盖该候选的规则
	size_t removed = 0;
	for (auto it = rules.begin(); it != rules.end();)
	{
		const bool same_anchor =
			it->ps_hash == selected.ps_hash && it->vs_hash == selected.vs_hash;
		const bool same_texture = it->hash_any || it->hash == selected.hash;
		const bool covers_draw =
			it->first_index.matches(selected.first_index) &&
			it->index_count.matches(selected.index_count);

		if (same_anchor && same_texture && covers_draw)
		{
			it = rules.erase(it);
			++removed;
		}
		else
		{
			++it;
		}
	}

	char message[192];

	if (removed != 0)
	{
		std::snprintf(message, sizeof(message),
			"[Hunting] removed %u rule(s) for texture 0x%08X ps=0x%08X vs=0x%08X",
			static_cast<unsigned>(removed), selected.hash, selected.ps_hash, selected.vs_hash);
		log_info(message);
	}
	else
	{
		rules.push_back(plain);

		char texture_text[16] = "*";
		if (!plain.hash_any)
			std::snprintf(texture_text, sizeof(texture_text), "0x%08X", plain.hash);

		std::snprintf(message, sizeof(message),
			"[Hunting] added rule texture=%s ps=0x%08X vs=0x%08X fi=%u ic=%u",
			texture_text, selected.ps_hash, selected.vs_hash,
			selected.first_index, selected.index_count);
		log_info(message);
	}

	data->has_unsaved_changes.store(true, std::memory_order_relaxed);
	publish_rules(data, std::move(rules));

	// 规则数量变了，Group settings 里记下的下标已失效，必须关掉编辑器，
	// 否则“Done”会把编辑内容套到另一条规则上（或静默丢弃）。
	// 本函数可能由 present 线程的热键调用，而 settings_* 属于 overlay 线程
	// （settings_error 还是 std::string）——只发请求，由 draw_overlay 落实。
	data->request_close_settings.store(true, std::memory_order_relaxed);
}

static void process_hotkeys(device_data *data)
{
	const bool prev_key = key_pressed(VK_NUMPAD1);
	const bool next_key = key_pressed(VK_NUMPAD2);
	const bool mark_key = key_pressed(VK_NUMPAD3);

	// 只有 Hunting Mode 开启时数字键才可用；捕获热键或在输入框打字时都不要抢键
	if (!data->hunting.load(std::memory_order_relaxed))
		return;
	if (data->capturing_hotkey.load(std::memory_order_relaxed) ||
		GetTickCount64() < data->keyboard_suppressed_until.load(std::memory_order_relaxed))
		return;
	if (!prev_key && !next_key && !mark_key)
		return;

	if (mark_key)
	{
		toggle_rule_for_candidate(data);
		return;
	}

	std::lock_guard<std::mutex> lock(data->mutex);
	const int count = static_cast<int>(data->candidates.size());
	if (count == 0)
		return;

	if (prev_key)
		data->candidate_index = (data->candidate_index - 1 + count) % count;
	if (next_key)
		data->candidate_index = (data->candidate_index + 1) % count;

	// 步进的同时更新预览：用"元素消失"确认选中了什么（与点列表等价）
	if (data->preview_enabled.load(std::memory_order_relaxed) &&
		data->candidate_index >= 0 && data->candidate_index < count)
	{
		const candidate &selected = data->candidates[data->candidate_index];
		set_candidate_preview(true, selected.hash, selected.ps_hash, selected.vs_hash);
	}
}

static void restart_candidate_collection(device_data *data)
{
	std::lock_guard<std::mutex> lock(data->mutex);

	data->frame_candidates.clear();
	data->collected_candidates.clear();
	data->candidates.clear();
	data->candidate_index = -1;
	data->frames_collected.store(0, std::memory_order_relaxed);
	data->collecting.store(true, std::memory_order_relaxed);

	// 候选全部重建，旧的预览目标已不存在
	set_candidate_preview(false, 0, 0, 0);

	// 新纪元：先推进 epoch，再打开采集。各命令列表会在下一次 push/draw 时
	// 丢弃挂机期间残留的陈旧 view 句柄，避免解引用已销毁的 SRV。
	g_hunting_collect_epoch.fetch_add(1, std::memory_order_relaxed);

	// 让 push_descriptors 开始记录 SRV 绑定（没有纹理规则时也需要）
	g_hunting_collecting.store(true, std::memory_order_relaxed);

	log_info("[Hunting] candidate collection restarted");
}

// -----------------------------------------------------------------------------
// present
// -----------------------------------------------------------------------------
// 每帧一次：把本线程各命令列表的普通计数器汇总到全局原子变量。
// draw_indexed 里因此不再有 fetch_add。
static void flush_command_counters(device_data *data)
{
	auto drop_all = [](uint64_t new_epoch)
	{
		t_cmd_list_epoch_seen = new_epoch;
		t_cmd_data.clear();
		t_owned_cmd_data.clear();
		t_last_cmd_list = nullptr;
		t_last_cmd_data = nullptr;
	};

	// 先做 epoch 检查：跨线程销毁的命令列表必须在这里被整体丢弃
	const uint64_t entry_epoch = g_cmd_list_epoch.load(std::memory_order_relaxed);
	if (t_cmd_list_epoch_seen != entry_epoch)
		drop_all(entry_epoch);

	// 注意：epoch 是快照而不是锁。命令列表有可能在本线程遍历期间被另一个线程
	// 销毁，因此在每次解引用前都重新校验一次，尽量缩短窗口（彻底消除需要引用计数）。
	for (size_t i = 0; i < t_owned_cmd_data.size(); ++i)
	{
		const uint64_t epoch = g_cmd_list_epoch.load(std::memory_order_relaxed);
		if (epoch != t_cmd_list_epoch_seen)
		{
			drop_all(epoch);
			return;
		}

		cmd_list_data *cmd = t_owned_cmd_data[i];
		if (cmd == nullptr)
			continue;

		if (cmd->local_draws != 0)
		{
			data->draws_seen.fetch_add(cmd->local_draws, std::memory_order_relaxed);
			cmd->local_draws = 0;
		}
		if (cmd->local_skipped != 0)
		{
			data->draws_skipped.fetch_add(cmd->local_skipped, std::memory_order_relaxed);
			cmd->local_skipped = 0;
		}
		if (cmd->local_applied != 0)
		{
			data->rules_applied.fetch_add(cmd->local_applied, std::memory_order_relaxed);
			cmd->local_applied = 0;
		}
		if (cmd->local_srv_resolved != 0)
		{
			data->srv_resolved.fetch_add(cmd->local_srv_resolved, std::memory_order_relaxed);
			cmd->local_srv_resolved = 0;
		}
		if (cmd->local_push_updates != 0)
		{
			g_push_updates.fetch_add(cmd->local_push_updates, std::memory_order_relaxed);
			cmd->local_push_updates = 0;
		}
		if (cmd->local_preview_skipped != 0)
		{
			data->preview_skipped.fetch_add(cmd->local_preview_skipped, std::memory_order_relaxed);
			cmd->local_preview_skipped = 0;
		}
		if (cmd->last_ps_hash != 0 || cmd->last_vs_hash != 0)
		{
			data->last_ps_hash.store(cmd->last_ps_hash, std::memory_order_relaxed);
			data->last_vs_hash.store(cmd->last_vs_hash, std::memory_order_relaxed);
		}
	}
}

// -----------------------------------------------------------------------------
// 长跑看门狗：每分钟把各结构的规模写一行日志。
//
// 系统重启（内核 bugcheck）不会留下插件侧的痕迹，但在蓝屏前写入的 ReShade.log
// 能回答一个关键问题：某个结构是否在数小时内无界增长（live_textures /
// pixel_hashes / 候选数 / 两个墓碑环的序号）。这决定问题属于“泄漏”还是“喂了
// 驱动非法状态”。
// -----------------------------------------------------------------------------
static std::atomic<uint64_t> g_watchdog_last_tick{ 0 };
static std::atomic<uint64_t> g_watchdog_start_tick{ 0 };

static void log_watchdog(device_data *data)
{
	const uint64_t now = GetTickCount64();

	if (g_watchdog_start_tick.load(std::memory_order_relaxed) == 0)
		g_watchdog_start_tick.store(now, std::memory_order_relaxed);

	uint64_t last = g_watchdog_last_tick.load(std::memory_order_relaxed);
	if (last != 0 && now - last < 60000)
		return;

	// 多 present 线程时只让一个写日志
	if (!g_watchdog_last_tick.compare_exchange_strong(last, now, std::memory_order_relaxed))
		return;

	size_t live_textures = 0;
	size_t pixel_hashes = 0;
	size_t candidates = 0;

	{
		std::lock_guard<std::mutex> lock(data->mutex);
		live_textures = data->live_textures.size();
		pixel_hashes = data->pixel_hashes.size();
		candidates = data->collected_candidates.size();
	}

	const rule_table *rules = acquire_rules(data);
	const size_t rule_count = rules != nullptr ? rules->rules.size() : 0;
	const uint64_t uptime = (now - g_watchdog_start_tick.load(std::memory_order_relaxed)) / 1000;

	char message[352];
	std::snprintf(message, sizeof(message),
		"[Watch] up=%llus draws=%u skipped=%u rules=%u live_tex=%u pix_hash=%u cand=%u "
		"srv_cache=%d dead_tex=%llu dead_view=%llu safe=%d",
		static_cast<unsigned long long>(uptime),
		data->draws_seen.load(std::memory_order_relaxed),
		data->draws_skipped.load(std::memory_order_relaxed),
		static_cast<unsigned>(rule_count),
		static_cast<unsigned>(live_textures),
		static_cast<unsigned>(pixel_hashes),
		static_cast<unsigned>(candidates),
		g_current_cache_size.load(std::memory_order_relaxed),
		static_cast<unsigned long long>(g_dead_textures.seq.load(std::memory_order_relaxed)),
		static_cast<unsigned long long>(g_dead_views.seq.load(std::memory_order_relaxed)),
		data->safe_mode.load(std::memory_order_relaxed) ? 1 : 0);

	log_info(message);
}

static void on_present(
	command_queue * /*queue*/,
	swapchain *swapchain,
	const rect * /*source_rect*/,
	const rect * /*dest_rect*/,
	uint32_t /*dirty_rect_count*/,
	const rect * /*dirty_rects*/)
{
	device *const dev = swapchain->get_device();
	device_data *data = get_device_data(dev);
	if (data == nullptr)
		return;

	flush_command_counters(data);

	// 每帧采样一次键盘边沿，供規則热键与 Hunting 键共用
	poll_key_edges();

	process_hotkeys(data);
	process_rule_hotkeys(data);

	log_watchdog(data);

	const bool hunting = data->hunting.load(std::memory_order_relaxed);

	// Hunting 关闭时清掉预览，避免"退出狩猎后画面还少一块"
	if (!hunting)
		set_candidate_preview(false, 0, 0, 0);

	{
		std::lock_guard<std::mutex> lock(data->mutex);

		if (!hunting)
		{
			data->frame_candidates.clear();
			data->collected_candidates.clear();
			data->candidates.clear();
			data->candidate_index = -1;
			data->frames_collected.store(0, std::memory_order_relaxed);
			data->collecting.store(false, std::memory_order_relaxed);
			g_hunting_collecting.store(false, std::memory_order_relaxed);
			return;
		}

		if (!data->collecting.load(std::memory_order_relaxed))
		{
			// 已暂停：候选列表保持冻结，不再更新
			data->frame_candidates.clear();
			return;
		}

		// 累积 N 帧的并集，避免动态加载导致列表闪烁
		for (const auto &entry : data->frame_candidates)
			data->collected_candidates[entry.first] = entry.second;
		data->frame_candidates.clear();

		std::vector<candidate> found;
		found.reserve(data->collected_candidates.size());
		for (const auto &entry : data->collected_candidates)
			found.push_back(entry.second);

		std::sort(found.begin(), found.end(),
			[](const candidate &lhs, const candidate &rhs)
			{
				if (lhs.ps_hash != rhs.ps_hash)
					return lhs.ps_hash < rhs.ps_hash;
				if (lhs.vs_hash != rhs.vs_hash)
					return lhs.vs_hash < rhs.vs_hash;
				if (lhs.hash != rhs.hash)
					return lhs.hash < rhs.hash;
				if (lhs.first_index != rhs.first_index)
					return lhs.first_index < rhs.first_index;
				return lhs.index_count < rhs.index_count;
			});

		data->candidates = std::move(found);

		if (data->candidates.empty())
			data->candidate_index = -1;
		else if (data->candidate_index < 0 ||
			data->candidate_index >= static_cast<int>(data->candidates.size()))
			data->candidate_index = 0;

		const int collected_frames = data->frames_collected.fetch_add(1, std::memory_order_relaxed) + 1;
		const int target_frames = std::max(1, data->frames_to_collect.load(std::memory_order_relaxed));

		if (collected_frames >= target_frames)
		{
			data->collecting.store(false, std::memory_order_relaxed);
			data->frame_candidates.clear();
			// 采集结束：push_descriptors 可以恢复极速早退
			g_hunting_collecting.store(false, std::memory_order_relaxed);

			char message[128];
			std::snprintf(message, sizeof(message),
				"[Hunting] paused after %d frame(s), %u candidate(s)",
				collected_frames, static_cast<unsigned>(data->candidates.size()));
			log_info(message);
		}
	}
}

// -----------------------------------------------------------------------------
// ImGui
// -----------------------------------------------------------------------------
static void draw_overlay(effect_runtime *runtime)
{
	device *const dev = runtime->get_device();
	device_data *data = get_device_data(dev);
	if (data == nullptr)
		return;

	// 0) 落实跨线程请求（present/热键线程 → overlay 线程）。
	// settings_open / settings_index / settings_error(std::string) 只允许 overlay
	// 线程改动，否则多 swapchain 场景下就是跨线程改堆。
	if (data->request_close_settings.exchange(false, std::memory_order_relaxed))
	{
		data->settings_open = false;
		data->settings_index = -1;
		data->settings_error.clear();
		// 关掉编辑器的同时退出热键捕获态，否则规则热键会被永久抑制
		data->edit_hotkey_capturing = false;
		data->capturing_hotkey.store(false, std::memory_order_relaxed);
	}

	// 1) 锁内只做快照
	struct overlay_snapshot
	{
		bool hunting = false;
		bool collecting = false;
		int frames_to_collect = 10;
		int frames_collected = 0;
		int hash_mode_value = 0;
		int cached_views = 0;
		uint64_t cache_hits = 0;
		uint64_t cache_misses = 0;
		uint64_t push_updates = 0;
		uint64_t shader_resolved = 0;
		size_t pixel_hash_count = 0;
		uint64_t pixel_hash_bytes = 0;
		uint32_t last_ps_hash = 0;
		uint32_t last_vs_hash = 0;
		bool stable_only = true;
		bool safe_mode = false;
		bool preview_enabled = true;
		uint32_t preview_skipped = 0;
		uint32_t resolved = 0;
		uint32_t applied = 0;
		uint32_t draws = 0;
		uint32_t skipped = 0;
		std::vector<candidate> candidates;
		int candidate_index = -1;
		std::vector<texture_override_rule> rules;
	} snapshot;

	{
		std::lock_guard<std::mutex> lock(data->mutex);

		snapshot.hunting = data->hunting.load(std::memory_order_relaxed);
		snapshot.collecting = data->collecting.load(std::memory_order_relaxed);
		snapshot.frames_to_collect = data->frames_to_collect.load(std::memory_order_relaxed);
		snapshot.frames_collected = data->frames_collected.load(std::memory_order_relaxed);
		snapshot.hash_mode_value = data->hash_mode_value.load(std::memory_order_relaxed);
		snapshot.cached_views = g_current_cache_size.load(std::memory_order_relaxed);
		snapshot.cache_hits = g_cache_hits.load(std::memory_order_relaxed);
		snapshot.cache_misses = g_cache_misses.load(std::memory_order_relaxed);
		snapshot.push_updates = g_push_updates.load(std::memory_order_relaxed);
		snapshot.shader_resolved = data->shader_hashes_resolved.load(std::memory_order_relaxed);
		snapshot.pixel_hash_count = data->pixel_hashes.size();
		snapshot.pixel_hash_bytes = g_pixel_hash_bytes.load(std::memory_order_relaxed);
		snapshot.last_ps_hash = data->last_ps_hash.load(std::memory_order_relaxed);
		snapshot.last_vs_hash = data->last_vs_hash.load(std::memory_order_relaxed);
		snapshot.stable_only = data->stable_only_rules.load(std::memory_order_relaxed);
		snapshot.safe_mode = data->safe_mode.load(std::memory_order_relaxed);
		snapshot.preview_enabled = data->preview_enabled.load(std::memory_order_relaxed);
		snapshot.preview_skipped = data->preview_skipped.load(std::memory_order_relaxed);
		snapshot.resolved = data->srv_resolved.load(std::memory_order_relaxed);
		snapshot.applied = data->rules_applied.load(std::memory_order_relaxed);
		snapshot.draws = data->draws_seen.load(std::memory_order_relaxed);
		snapshot.skipped = data->draws_skipped.load(std::memory_order_relaxed);
		snapshot.candidates = data->candidates;
		snapshot.candidate_index = data->candidate_index;
	}

	// 规则表是独立原子指针 + 不可变快照，拷贝不需要 data->mutex。
	// publish_rules（唯一回收旧表的地方）与 overlay 同在 present 线程，
	// 因此拷贝期间指针不会被回收。
	snapshot.rules = copy_rules(acquire_rules(data));

	// 2) 锁外绘制，只记录意图
	bool hunting_changed = false;
	bool hunting_value = snapshot.hunting;
	bool restart_requested = false;
	bool hash_mode_changed = false;
	int new_hash_mode = snapshot.hash_mode_value;

	bool select_requested = false;
	uint32_t select_hash = 0;
	uint32_t select_ps = 0;
	uint32_t select_vs = 0;

	bool toggle_requested = false;
	bool save_requested = false;
	bool reload_requested = false;

	int remove_index = -1;
	int active_index = -1;
	bool active_value = true;
	int open_settings_index = -1;
	bool apply_settings = false;
	int apply_settings_index = -1;
	texture_override_rule apply_settings_rule;

	int view_index = snapshot.candidate_index;
	const int candidate_count = static_cast<int>(snapshot.candidates.size());

	auto select_local = [&](int index)
	{
		if (index >= 0 && index < candidate_count)
		{
			view_index = index;
			select_requested = true;
			select_hash = snapshot.candidates[index].hash;
			select_ps = snapshot.candidates[index].ps_hash;
			select_vs = snapshot.candidates[index].vs_hash;

			// 选中即预览：把这条候选对应的绘制立刻隐藏，用"元素消失"确认它是什么。
			// 原子写 + seqlock，draw 线程随时可读，不需要走"意图 + 帧末落实"的路径。
			if (snapshot.preview_enabled && snapshot.hunting)
				set_candidate_preview(true, select_hash, select_ps, select_vs);
		}
	};

	const uint64_t cache_total = snapshot.cache_hits + snapshot.cache_misses;
	const double cache_rate = cache_total != 0
		? (100.0 * static_cast<double>(snapshot.cache_hits) / static_cast<double>(cache_total))
		: 0.0;

	// ---------------- 主面板 ----------------
	// 主面板尺寸随分辨率收缩，避免小屏/高缩放下溢出
	const ImVec2 display_size = ImGui::GetIO().DisplaySize;
	const float ui_scale = (display_size.y < 600.0f || display_size.x < 800.0f) ? 0.8f : 1.0f;
	const ImVec2 main_window_size(
		std::min(780.0f * ui_scale, std::max(360.0f, display_size.x - 32.0f)),
		std::min(600.0f * ui_scale, std::max(260.0f, display_size.y - 48.0f)));

	ImGui::SetNextWindowSize(main_window_size, ImGuiCond_FirstUseEver);
	if (ImGui::Begin("Texture Override", nullptr))
	{
		ImGui::Text("Draws: %u   Skipped: %u   Rules applied: %u",
			snapshot.draws, snapshot.skipped, snapshot.applied);
		ImGui::Text("Cache hit: %llu / %llu (%.1f%%)   Active SRV Cache: %d",
			static_cast<unsigned long long>(snapshot.cache_hits),
			static_cast<unsigned long long>(cache_total),
			cache_rate,
			snapshot.cached_views);
		ImGui::Text("push_descriptors: %llu   shader hashes resolved: %llu",
			static_cast<unsigned long long>(snapshot.push_updates),
			static_cast<unsigned long long>(snapshot.shader_resolved));
		ImGui::Text("Last draw: ps=0x%08X  vs=0x%08X", snapshot.last_ps_hash, snapshot.last_vs_hash);
		if (snapshot.safe_mode)
		{
			ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
				"SAFE MODE: replace disabled, pixel hashing off ([Settings] safe_mode = 1)");
		}
		ImGui::TextDisabled("stable anchors: CRC32C(DXBC chunks of PS/VS, checksum excluded)");
		ImGui::TextDisabled("descriptor hash = CRC32C(Width_Height_Format_MipLevels_ArraySize)");

		if (snapshot.draws > 0 && snapshot.shader_resolved == 0)
		{
			ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
				"No shader bytecode captured yet; match_ps_hash / match_vs_hash cannot match.");
		}

		ImGui::Separator();

		// Hunting 控制区
		if (ImGui::Checkbox("Hunting Mode", &hunting_value))
			hunting_changed = true;

		ImGui::SameLine();
		if (!snapshot.hunting)
			ImGui::TextDisabled("off");
		else if (snapshot.collecting)
			ImGui::Text("collecting %d / %d", snapshot.frames_collected, snapshot.frames_to_collect);
		else
			ImGui::Text("paused (%d / %d)", snapshot.frames_collected, snapshot.frames_to_collect);

		int frames = snapshot.frames_to_collect;
		if (ImGui::SliderInt("Frames to collect", &frames, 1, 120))
			data->frames_to_collect.store(std::max(1, frames), std::memory_order_relaxed);

		ImGui::SameLine();
		if (ImGui::Button("Refresh"))
			restart_requested = true;

		int mode_value = snapshot.hash_mode_value;
		const char *hash_mode_items[] = {
			"Descriptor Hash (Fast)",
			"Pixel Content Hash (Accurate, may miss dynamic)"
		};

		if (ImGui::Combo("Hash Mode", &mode_value, hash_mode_items, 2))
		{
			new_hash_mode = mode_value;
			hash_mode_changed = true;
		}

		bool stable_only = snapshot.stable_only;
		if (ImGui::Checkbox("Stable-only new rules (hash = *)", &stable_only))
			data->stable_only_rules.store(stable_only, std::memory_order_relaxed);
		ImGui::SameLine();
		ImGui::TextDisabled("Numpad 3 writes ps/vs hash + draw range");

		ImGui::Text("Pixel hashes: %u texture(s), %.1f MB hashed",
			static_cast<unsigned>(snapshot.pixel_hash_count),
			static_cast<double>(snapshot.pixel_hash_bytes) / (1024.0 * 1024.0));

		if (mode_value == static_cast<int>(hash_mode::pixel) && snapshot.pixel_hash_count == 0)
		{
			ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
				"No pixel hash captured yet. Textures are hashed on CPU upload "
				"(init_resource / UpdateSubresource); GPU-written textures stay unknown.");
		}
		else if (mode_value == static_cast<int>(hash_mode::pixel))
		{
			ImGui::TextDisabled("Pixel hashes are computed on CPU upload only; GPU-written textures are skipped.");
		}

		if (ImGui::CollapsingHeader("HUD overlay"))
		{
			if (ImGui::SliderFloat("HUD X", &data->hud_x, 0.0f, 3000.0f, "%.0f"))
				data->hud_pos_dirty = true;
			if (ImGui::SliderFloat("HUD Y", &data->hud_y, 0.0f, 2000.0f, "%.0f"))
				data->hud_pos_dirty = true;
			ImGui::SliderFloat("HUD Alpha", &data->hud_alpha, 0.1f, 1.0f, "%.2f");
			ImGui::TextDisabled("You can also drag the HUD window directly.");
		}

		ImGui::Separator();

		// 候选列表（冻结后不再变化）
		const bool has_candidate = view_index >= 0 && view_index < candidate_count;

		if (ImGui::Button("Prev (Numpad 1)") && snapshot.hunting && candidate_count > 0)
			select_local((view_index - 1 + candidate_count) % candidate_count);
		ImGui::SameLine();
		if (ImGui::Button("Next (Numpad 2)") && snapshot.hunting && candidate_count > 0)
			select_local((view_index + 1) % candidate_count);
		ImGui::SameLine();
		if (ImGui::Button("Add/Remove rule (Numpad 3)") && snapshot.hunting && has_candidate)
			toggle_requested = true;

		// 预览开关：开时"点选/步进候选"会立刻把对应绘制隐藏，靠元素消失来确认选中项。
		bool preview_enabled = snapshot.preview_enabled;
		if (ImGui::Checkbox("Preview selected (hide it live)", &preview_enabled))
		{
			data->preview_enabled.store(preview_enabled, std::memory_order_relaxed);

			if (!preview_enabled)
			{
				set_candidate_preview(false, 0, 0, 0);
			}
			else if (snapshot.hunting && has_candidate)
			{
				const candidate &selected = snapshot.candidates[view_index];
				set_candidate_preview(true, selected.hash, selected.ps_hash, selected.vs_hash);
			}
		}
		if (snapshot.hunting && snapshot.preview_skipped != 0)
		{
			ImGui::SameLine();
			ImGui::TextDisabled("preview hidden: %u draw(s)", snapshot.preview_skipped);
		}

		if (has_candidate)
		{
			const candidate &selected = snapshot.candidates[view_index];
			ImGui::Text("Current: tex=0x%08X  ps=0x%08X  vs=0x%08X",
				selected.hash, selected.ps_hash, selected.vs_hash);
			ImGui::Text("         fi=%u  ic=%u", selected.first_index, selected.index_count);
		}
		else
		{
			ImGui::TextDisabled("No candidate. Enable Hunting Mode and render the target UI.");
		}

		ImGui::Text("Candidates: %d", candidate_count);
		if (ImGui::BeginChild("##candidates", ImVec2(0.0f, 130.0f), true))
		{
			for (int i = 0; i < candidate_count; ++i)
			{
				const candidate &entry = snapshot.candidates[i];

				ImGui::PushID(i);
				char label[160];
				std::snprintf(label, sizeof(label),
					"%s0x%08X  ps=%08X vs=%08X  fi=%u ic=%u",
					(i == view_index) ? "> " : "  ",
					entry.hash, entry.ps_hash, entry.vs_hash,
					entry.first_index, entry.index_count);

				if (ImGui::Selectable(label, i == view_index))
					select_local(i);
				ImGui::PopID();
			}

			if (candidate_count == 0)
			{
				if (snapshot.hunting && !snapshot.collecting)
					ImGui::TextWrapped("Candidate list is empty. Try clicking Refresh or move your camera.");
				else if (snapshot.hunting)
					ImGui::TextDisabled("Collecting candidates...");
				else
					ImGui::TextDisabled("Enable Hunting Mode to collect candidates.");
			}
		}
		ImGui::EndChild();

		ImGui::Separator();

		// Rules 表格
		if (ImGui::BeginTable("##rules", 10,
			ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("Tex");
			ImGui::TableSetupColumn("PS");
			ImGui::TableSetupColumn("VS");
			ImGui::TableSetupColumn("FI");
			ImGui::TableSetupColumn("IC");
			ImGui::TableSetupColumn("Key");
			ImGui::TableSetupColumn("Mode");
			ImGui::TableSetupColumn("Act");
			ImGui::TableSetupColumn("Set");
			ImGui::TableSetupColumn("Del");
			ImGui::TableHeadersRow();

			for (int i = 0; i < static_cast<int>(snapshot.rules.size()); ++i)
			{
				const texture_override_rule &rule = snapshot.rules[i];
				const std::string first = value_set_to_string(rule.first_index);
				const std::string count_text = value_set_to_string(rule.index_count);

				ImGui::PushID(i);
				ImGui::TableNextRow();

				ImGui::TableSetColumnIndex(0);
				if (rule.hash_any)
					ImGui::TextUnformatted("*");
				else
					ImGui::Text("0x%08X", rule.hash);

				ImGui::TableSetColumnIndex(1);
				if (rule.ps_hash != 0)
					ImGui::Text("%08X", rule.ps_hash);
				else
					ImGui::TextDisabled("-");

				ImGui::TableSetColumnIndex(2);
				if (rule.vs_hash != 0)
					ImGui::Text("%08X", rule.vs_hash);
				else
					ImGui::TextDisabled("-");

				ImGui::TableSetColumnIndex(3);
				ImGui::TextUnformatted(first.empty() ? "*" : first.c_str());

				ImGui::TableSetColumnIndex(4);
				ImGui::TextUnformatted(count_text.empty() ? "*" : count_text.c_str());

				ImGui::TableSetColumnIndex(5);
				if (rule.hotkey != 0)
					ImGui::TextUnformatted(hotkey_to_string(rule.hotkey).c_str());
				else
					ImGui::TextDisabled("-");

				ImGui::TableSetColumnIndex(6);
				ImGui::TextUnformatted(rule.handling == handling_mode::skip ? "skip" : "replace");

				ImGui::TableSetColumnIndex(7);
				bool active = rule.active;
				if (ImGui::Checkbox("##active", &active))
				{
					active_index = i;
					active_value = active;
				}

				ImGui::TableSetColumnIndex(8);
				if (ImGui::Button("Settings"))
					open_settings_index = i;

				ImGui::TableSetColumnIndex(9);
				if (ImGui::Button("X"))
					remove_index = i;

				ImGui::PopID();
			}

			ImGui::EndTable();
		}

		if (snapshot.rules.empty())
			ImGui::TextDisabled("No rules. Use Hunting Mode or edit TextureOverride.ini.");

		ImGui::Separator();

		if (ImGui::Button("Save to INI"))
			save_requested = true;
		ImGui::SameLine();
		if (ImGui::Button("Reload from INI"))
			reload_requested = true;

		if (data->has_unsaved_changes.load(std::memory_order_relaxed))
		{
			ImGui::SameLine();
			ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.2f, 1.0f), "(Unsaved)");
		}
	}
	ImGui::End();

	// ---------------- 打开设置窗口 ----------------
	if (open_settings_index >= 0 &&
		open_settings_index < static_cast<int>(snapshot.rules.size()))
	{
		const texture_override_rule &rule = snapshot.rules[open_settings_index];

		data->settings_open = true;
		data->settings_index = open_settings_index;
		data->settings_error.clear();
		data->edit_priority = rule.priority;
		data->edit_handling = rule.handling == handling_mode::skip ? 0 : 1;
		data->edit_active = rule.active;
		// 热键编辑：进入时把当前绑定拷进编辑缓冲，捕获状态清零
		data->edit_hotkey = rule.hotkey;
		data->edit_hotkey_capturing = false;
		data->capturing_hotkey.store(false, std::memory_order_relaxed);

		const std::string first = value_set_to_string(rule.first_index);
		const std::string count_text = value_set_to_string(rule.index_count);

		char hash_buffer[16] = "*";
		if (!rule.hash_any)
			std::snprintf(hash_buffer, sizeof(hash_buffer), "%08X", rule.hash);

		char ps_buffer[16] = {};
		if (rule.ps_hash != 0)
			std::snprintf(ps_buffer, sizeof(ps_buffer), "%08X", rule.ps_hash);

		char vs_buffer[16] = {};
		if (rule.vs_hash != 0)
			std::snprintf(vs_buffer, sizeof(vs_buffer), "%08X", rule.vs_hash);

		std::snprintf(data->edit_hash, sizeof(data->edit_hash), "%s", hash_buffer);
		std::snprintf(data->edit_ps, sizeof(data->edit_ps), "%s", ps_buffer);
		std::snprintf(data->edit_vs, sizeof(data->edit_vs), "%s", vs_buffer);
		std::snprintf(data->edit_first, sizeof(data->edit_first), "%s", first.c_str());
		std::snprintf(data->edit_count, sizeof(data->edit_count), "%s", count_text.c_str());
	}

	// ---------------- Group settings ----------------
	if (data->settings_open)
	{
		bool window_open = true;
		bool settings_done = false;

		ImGui::SetNextWindowSize(ImVec2(480.0f, 460.0f), ImGuiCond_FirstUseEver);
		ImGui::Begin("Group settings", &window_open);

		if (data->settings_index < 0)
		{
			ImGui::TextDisabled("No rule selected.");
		}
		else
		{
			if (data->focus_hash_next)
			{
				ImGui::SetKeyboardFocusHere();
				data->focus_hash_next = false;
			}

			ImGui::TextDisabled("Conditions are AND; empty / * means \"any\".");
			ImGui::InputText("hash (or *)", data->edit_hash, sizeof(data->edit_hash));
			ImGui::InputText("match_ps_hash", data->edit_ps, sizeof(data->edit_ps));
			ImGui::InputText("match_vs_hash", data->edit_vs, sizeof(data->edit_vs));
			ImGui::InputText("match_first_index", data->edit_first, sizeof(data->edit_first));
			ImGui::InputText("match_index_count", data->edit_count, sizeof(data->edit_count));
			ImGui::InputInt("match_priority", &data->edit_priority);

			const char *handling_items[] = { "skip", "replace" };
			const std::string live_hash = trim_copy(data->edit_hash);
			const bool live_hash_any = live_hash.empty() || live_hash == "*";

			if (live_hash_any)
			{
				// 通配符规则没有可替换的目标纹理：只允许 skip
				data->edit_handling = 0;
				ImGui::BeginDisabled();
				ImGui::Combo("handling", &data->edit_handling, handling_items, 2);
				ImGui::EndDisabled();
				ImGui::TextDisabled("Wildcard rules only support skip");
			}
			else
			{
				ImGui::Combo("handling", &data->edit_handling, handling_items, 2);
			}

			ImGui::Checkbox("Active", &data->edit_active);

			// ---- 运行时热键（捕获式）----
			// 点输入框进入捕获；捕获期间 present 线程不触发任何规则热键。
			// Backspace 清除，Escape 取消。
			{
				char hotkey_buffer[64] = {};
				std::snprintf(hotkey_buffer, sizeof(hotkey_buffer), "%s",
					data->edit_hotkey_capturing
						? "Press a key... (Backspace clears, Escape cancels)"
						: hotkey_to_string(data->edit_hotkey).c_str());

				ImGui::InputText("hotkey", hotkey_buffer, sizeof(hotkey_buffer),
					ImGuiInputTextFlags_ReadOnly);

				if (ImGui::IsItemClicked() || ImGui::IsItemActivated())
				{
					if (!data->edit_hotkey_capturing)
					{
						data->edit_hotkey_capturing = true;
						data->capturing_hotkey.store(true, std::memory_order_relaxed);
					}
				}

				if (data->edit_hotkey_capturing)
				{
					// 用原始按键状态捕获，不走边沿快照——避免与 present 的轮询互相吞键
					if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0)
					{
						data->edit_hotkey_capturing = false;
						data->capturing_hotkey.store(false, std::memory_order_relaxed);
					}
					else if ((GetAsyncKeyState(VK_BACK) & 0x8000) != 0)
					{
						data->edit_hotkey = 0;
						data->edit_hotkey_capturing = false;
						data->capturing_hotkey.store(false, std::memory_order_relaxed);
					}
					else
					{
						for (int vk = 8; vk < 256; ++vk)
						{
							if (is_modifier_vk(vk))
								continue;
							if ((GetAsyncKeyState(vk) & 0x8000) == 0)
								continue;

							const bool alt = (GetAsyncKeyState(VK_LMENU) & 0x8000) != 0 ||
								(GetAsyncKeyState(VK_RMENU) & 0x8000) != 0;
							const bool ctrl = (GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0 ||
								(GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0;
							const bool shift = (GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0 ||
								(GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0;

							data->edit_hotkey = make_hotkey(vk, alt, ctrl, shift);
							data->edit_hotkey_capturing = false;
							data->capturing_hotkey.store(false, std::memory_order_relaxed);
							break;
						}
					}
				}

				// 与其它规则撞键时给出提示（两边都会触发；不阻止）
				if (data->edit_hotkey != 0)
				{
					int conflicts = 0;
					for (int i = 0; i < static_cast<int>(snapshot.rules.size()); ++i)
					{
						if (i == data->settings_index)
							continue;
						if (snapshot.rules[i].hotkey == data->edit_hotkey)
							++conflicts;
					}

					if (conflicts != 0)
					{
						ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
							"Same shortcut is used by %d other rule(s); pressing it toggles all of them.",
							conflicts);
					}
					else
					{
						ImGui::TextDisabled("Toggling at runtime does not write the INI; press Save to INI to keep it.");
					}
				}
				else
				{
					ImGui::TextDisabled("No shortcut. Click the field and press a key (Backspace clears).");
				}
			}

			if (!data->settings_error.empty())
				ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", data->settings_error.c_str());

			if (ImGui::Button("Done", ImVec2(120.0f, 0.0f)))
			{
				uint32_t hash = 0;
				uint32_t ps_hash = 0;
				uint32_t vs_hash = 0;
				value_set first;
				value_set count_set;

				const std::string hash_text = trim_copy(data->edit_hash);
				const bool hash_any = hash_text.empty() || hash_text == "*";

				// 输入框被填满说明内容在载入时已被截断：此时接受修改会把
				// 截断后的（可能错误的）区间写回规则，必须拒绝。
				const bool first_truncated =
					std::strlen(data->edit_first) + 1 >= sizeof(data->edit_first);
				const bool count_truncated =
					std::strlen(data->edit_count) + 1 >= sizeof(data->edit_count);

				if (first_truncated || count_truncated)
				{
					data->settings_error = "Value list is too long for the editor; shorten it in the INI file.";
				}
				else if (!hash_any && (!parse_hex_hash(hash_text, hash) || hash == 0))
				{
					data->settings_error = "hash must be 1-8 hex digits or *.";
					data->focus_hash_next = true;
				}
				else if (!trim_copy(data->edit_ps).empty() && !parse_hex_hash(data->edit_ps, ps_hash))
				{
					data->settings_error = "Invalid match_ps_hash.";
				}
				else if (!trim_copy(data->edit_vs).empty() && !parse_hex_hash(data->edit_vs, vs_hash))
				{
					data->settings_error = "Invalid match_vs_hash.";
				}
				else if (hash_any && ps_hash == 0 && vs_hash == 0)
				{
					data->settings_error = "hash = * needs match_ps_hash or match_vs_hash.";
				}
				else if (!parse_value_set(data->edit_first, first))
				{
					data->settings_error = "Invalid match_first_index.";
				}
				else if (!parse_value_set(data->edit_count, count_set))
				{
					data->settings_error = "Invalid match_index_count.";
				}
				else
				{
					apply_settings_rule = {};
					apply_settings_rule.hash = hash_any ? 0 : hash;
					apply_settings_rule.hash_any = hash_any;
					apply_settings_rule.ps_hash = ps_hash;
					apply_settings_rule.vs_hash = vs_hash;
					apply_settings_rule.first_index = std::move(first);
					apply_settings_rule.index_count = std::move(count_set);
					apply_settings_rule.priority = data->edit_priority;
					apply_settings_rule.handling = data->edit_handling == 0
						? handling_mode::skip
						: handling_mode::replace;
					apply_settings_rule.active = data->edit_active;
					apply_settings_rule.hotkey = data->edit_hotkey;

					apply_settings = true;
					apply_settings_index = data->settings_index;

					data->settings_index = -1;
					data->settings_error.clear();
					settings_done = true;
				}
			}
		}

		ImGui::End();
		data->settings_open = window_open && !settings_done;

		if (!data->settings_open)
		{
			data->settings_index = -1;
			// 窗口关闭/取消：退出捕获态，否则规则热键会被永久抑制
			data->edit_hotkey_capturing = false;
			data->capturing_hotkey.store(false, std::memory_order_relaxed);
		}
	}

	// ---------------- HUD ----------------
	if (snapshot.hunting)
	{
		// 只有滑块/首次显示时强制位置；其余时间让 ImGui 自己维护拖拽后的位置
		const bool apply_position = data->hud_pos_dirty || !data->hud_pos_initialized;

		if (apply_position)
			ImGui::SetNextWindowPos(ImVec2(data->hud_x, data->hud_y), ImGuiCond_Always);

		ImGui::SetNextWindowBgAlpha(data->hud_alpha);

		const ImGuiWindowFlags flags =
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
			ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

		const bool hud_open = ImGui::Begin("##texovr_hud", nullptr, flags);
		const ImVec2 hud_pos = ImGui::GetWindowPos();

		if (hud_open)
		{
			ImGui::TextUnformatted("Hunting Mode: ON");

			const bool has_candidate = view_index >= 0 && view_index < candidate_count;
			if (has_candidate)
			{
				const candidate &selected = snapshot.candidates[view_index];
				// 让用户知道"画面少一块"是预览，而不是规则/插件出错
				if (snapshot.preview_enabled)
					ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "PREVIEW: candidate hidden");

				ImGui::Text("tex: 0x%08X", selected.hash);
				ImGui::Text("ps:  0x%08X", selected.ps_hash);
				ImGui::Text("vs:  0x%08X", selected.vs_hash);
				ImGui::Text("fi=%u ic=%u", selected.first_index, selected.index_count);
			}
			else
			{
				ImGui::TextUnformatted("Current: (none)");
			}

			ImGui::TextUnformatted("Numpad 1 / 2: Prev / Next");
			ImGui::TextUnformatted("Numpad 3: Toggle Rule");
			ImGui::TextDisabled("Drag to move");
		}

		ImGui::End();

		// 拖拽后的位置同步回主面板滑块
		data->hud_x = hud_pos.x;
		data->hud_y = hud_pos.y;
		data->hud_pos_initialized = true;

		if (apply_position)
			data->hud_pos_dirty = false;
	}

	// ---------------- 离开 UI 后落实修改 ----------------
	if (hunting_changed)
	{
		data->hunting.store(hunting_value, std::memory_order_relaxed);

		if (hunting_value)
		{
			restart_candidate_collection(data);
		}
		else
		{
			std::lock_guard<std::mutex> lock(data->mutex);
			data->frame_candidates.clear();
			data->collected_candidates.clear();
			data->candidates.clear();
			data->candidate_index = -1;
			data->frames_collected.store(0, std::memory_order_relaxed);
			data->collecting.store(false, std::memory_order_relaxed);
			g_hunting_collecting.store(false, std::memory_order_relaxed);
		}

		// 退出 Hunting：立刻取消预览，否则画面上会一直少一块
		if (!hunting_value)
			set_candidate_preview(false, 0, 0, 0);
	}

	if (restart_requested && data->hunting.load(std::memory_order_relaxed))
		restart_candidate_collection(data);

	if (hash_mode_changed)
	{
		data->hash_mode_value.store(new_hash_mode, std::memory_order_relaxed);
		data->has_unsaved_changes.store(true, std::memory_order_relaxed);

		{
			std::lock_guard<std::mutex> lock(data->mutex);
			data->frame_candidates.clear();
			data->collected_candidates.clear();
			data->candidates.clear();
			data->candidate_index = -1;
			data->frames_collected.store(0, std::memory_order_relaxed);
			data->collecting.store(false, std::memory_order_relaxed);
			g_hunting_collecting.store(false, std::memory_order_relaxed);
		}

		if (data->hunting.load(std::memory_order_relaxed))
			restart_candidate_collection(data);
		else
			set_candidate_preview(false, 0, 0, 0); // 候选已清空，预览目标不再有效

		log_info(new_hash_mode == static_cast<int>(hash_mode::pixel)
			? "[Hash] switched to pixel content hash (only textures uploaded through the CPU have a hash)"
			: "[Hash] switched to descriptor hash (fast)");
	}

	if (select_requested)
	{
		std::lock_guard<std::mutex> lock(data->mutex);

		int found = -1;
		for (int i = 0; i < static_cast<int>(data->candidates.size()); ++i)
		{
			const candidate &entry = data->candidates[i];
			if (entry.hash == select_hash &&
				entry.ps_hash == select_ps &&
				entry.vs_hash == select_vs)
			{
				found = i;
				break;
			}
		}

		if (found >= 0)
			data->candidate_index = found;
		else if (data->candidate_index >= static_cast<int>(data->candidates.size()))
			data->candidate_index = data->candidates.empty() ? -1 : 0;
	}

	if (remove_index >= 0)
	{
		std::vector<texture_override_rule> rules = copy_rules(acquire_rules(data));

		if (remove_index < static_cast<int>(rules.size()))
		{
			rules.erase(rules.begin() + remove_index);
			publish_rules(data, std::move(rules));

			if (data->settings_index == remove_index)
			{
				data->settings_open = false;
				data->settings_index = -1;
			}
			else if (data->settings_index > remove_index)
			{
				--data->settings_index;
			}

			char message[96];
			std::snprintf(message, sizeof(message), "[Rules] removed rule #%d", remove_index);
			log_info(message);

			data->has_unsaved_changes.store(true, std::memory_order_relaxed);
		}
	}

	if (active_index >= 0)
	{
		std::vector<texture_override_rule> rules = copy_rules(acquire_rules(data));

		if (active_index < static_cast<int>(rules.size()))
		{
			rules[active_index].active = active_value;
			publish_rules(data, std::move(rules));
			data->has_unsaved_changes.store(true, std::memory_order_relaxed);
		}
	}

	if (apply_settings && apply_settings_index >= 0)
	{
		std::vector<texture_override_rule> rules = copy_rules(acquire_rules(data));

		if (apply_settings_index < static_cast<int>(rules.size()))
		{
			apply_settings_rule.name = rules[apply_settings_index].name;
			rules[apply_settings_index] = std::move(apply_settings_rule);

			publish_rules(data, std::move(rules));
			data->has_unsaved_changes.store(true, std::memory_order_relaxed);
			log_info("[Rules] settings applied");
		}
	}

	if (toggle_requested)
		toggle_rule_for_candidate(data);

	if (save_requested)
	{
		std::vector<texture_override_rule> rules_snapshot = copy_rules(acquire_rules(data));

		if (save_config(std::move(rules_snapshot),
			data->hash_mode_value.load(std::memory_order_relaxed),
			data->safe_mode.load(std::memory_order_relaxed)))
			data->has_unsaved_changes.store(false, std::memory_order_relaxed);
	}

	if (reload_requested)
	{
		load_config(data);

		// 重新加载后，旧的 settings 索引可能已失效
		data->settings_open = false;
		data->settings_index = -1;
		data->settings_error.clear();
		data->edit_hotkey_capturing = false;
		data->capturing_hotkey.store(false, std::memory_order_relaxed);
	}

	// 让 present 线程知道"现在键盘被 ImGui 占着"（用户在输入框里打字）。
	// 规则热键据此跳过——否则在 hash 输入框里敲 "A" 会翻转绑定 A 的规则。
	// 用时间戳而不是布尔：叠加层关闭后本回调不再运行，布尔会永久为真。
	if (ImGui::GetIO().WantCaptureKeyboard)
	{
		data->keyboard_suppressed_until.store(GetTickCount64() + 250, std::memory_order_relaxed);
	}
}

// -----------------------------------------------------------------------------
// 入口
// -----------------------------------------------------------------------------
extern "C" __declspec(dllexport) const char *NAME = "Texture Override (INI Rules)";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
	"INI-driven TextureOverride: composite matching on stable shader bytecode hashes, texture hash and draw range; skip or null the draw.";

BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID)
{
	switch (fdwReason)
	{
	case DLL_PROCESS_ATTACH:
		if (!reshade::register_addon(hModule))
			return FALSE;

		reshade::register_event<reshade::addon_event::init_device>(on_init_device);
		reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);

		reshade::register_event<reshade::addon_event::init_command_list>(on_init_command_list);
		reshade::register_event<reshade::addon_event::destroy_command_list>(on_destroy_command_list);

		reshade::register_event<reshade::addon_event::init_resource>(on_init_resource);
		reshade::register_event<reshade::addon_event::destroy_resource>(on_destroy_resource);
		reshade::register_event<reshade::addon_event::destroy_resource_view>(on_destroy_resource_view);
		reshade::register_event<reshade::addon_event::update_texture_region>(on_update_texture_region);

		reshade::register_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
		reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
		reshade::register_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);

		reshade::register_event<reshade::addon_event::push_descriptors>(on_push_descriptors);
		reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
		reshade::register_event<reshade::addon_event::present>(on_present);

		reshade::register_overlay("Texture Override", draw_overlay);
		break;

	case DLL_PROCESS_DETACH:
		reshade::unregister_addon(hModule);
		break;
	}

	return TRUE;
}
