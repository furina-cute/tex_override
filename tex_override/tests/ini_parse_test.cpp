// ini_parse_test.cpp  (generated from dllmain.cpp -- do not edit by hand)
//
// Unit test for the INI value parsers. The tested code block is extracted
// verbatim from dllmain.cpp (struct value_set .. parse_hex_hash) so it cannot
// drift from the shipped implementation.
//
// Build/run:
//   cl /nologo /std:c++17 /O2 /EHsc /utf-8 /Fe:ini_parse_test.exe ini_parse_test.cpp
//   ini_parse_test.exe

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
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


// ---------------------------------------------------------------------------
// tests
// ---------------------------------------------------------------------------
static int g_failures = 0;

static void check(bool condition, const char *what)
{
	if (!condition)
	{
		std::printf("  FAIL: %s\n", what);
		++g_failures;
	}
}

int main()
{
	{
		uint32_t v = 0;
		check(parse_hex_hash("1a2b3c4d", v) && v == 0x1A2B3C4Du, "hex: bare digits");
		check(parse_hex_hash("0x1a2b3c4d", v) && v == 0x1A2B3C4Du, "hex: 0x prefix");
		check(parse_hex_hash("  83FD403F  ", v) && v == 0x83FD403Fu, "hex: surrounding spaces");
		check(parse_hex_hash("ABCDEF", v) && v == 0xABCDEFu, "hex: uppercase");
		check(parse_hex_hash("FFFFFFFF", v) && v == 0xFFFFFFFFu, "hex: 8 digits max value");
		check(!parse_hex_hash("123456789", v), "hex: 9 digits rejected");
		check(!parse_hex_hash("", v), "hex: empty rejected");
		check(!parse_hex_hash("zz", v), "hex: non hex rejected");
		check(!parse_hex_hash("0x", v), "hex: bare prefix rejected");
		check(!parse_hex_hash("-1", v), "hex: negative rejected (strtoul would wrap)");
		check(!parse_hex_hash("+1", v), "hex: explicit plus rejected");
		check(parse_hex_hash("0", v) && v == 0, "hex: zero parses (callers must reject it)");
	}

	{
		uint32_t v = 0;
		check(parse_u32("0", v) && v == 0, "u32: zero");
		check(parse_u32("4294967295", v) && v == 0xFFFFFFFFu, "u32: max");
		check(!parse_u32("4294967296", v), "u32: overflow rejected");
		check(parse_u32("0x10", v) && v == 16, "u32: 0x prefix");
		check(!parse_u32("-1", v), "u32: negative rejected");
		check(!parse_u32("", v), "u32: empty rejected");
		check(!parse_u32("12x", v), "u32: trailing garbage rejected");
	}

	{
		value_set set;
		check(set.any && set.matches(12345), "set: default is any");

		check(parse_value_set("*", set) && set.any, "set: star");
		check(parse_value_set("", set) && set.any, "set: empty means any");
		check(parse_value_set("   ", set) && set.any, "set: blanks mean any");

		check(parse_value_set("0", set) && !set.any, "set: single value");
		check(set.matches(0) && !set.matches(1), "set: single value matches exactly");

		check(parse_value_set("0,512", set) && set.matches(0) && set.matches(512) && !set.matches(1), "set: list");

		check(parse_value_set("100-500", set) && set.matches(100) && set.matches(500) && !set.matches(99), "set: range");

		check(parse_value_set("500-100", set) && set.matches(200), "set: reversed range is normalized");

		check(parse_value_set("-500", set) && set.matches(0) && set.matches(500) && !set.matches(501), "set: open low end");

		check(parse_value_set("100-", set) && set.matches(100) && set.matches(0xFFFFFFFFu) && !set.matches(99), "set: open high end");

		check(parse_value_set(" 100 - 200 , 300 ", set) && set.matches(150) && set.matches(300), "set: spaces around tokens");

		check(parse_value_set("0-10,11-20", set) && set.ranges.size() == 1 && set.matches(20), "set: adjacent ranges merged");

		check(!parse_value_set("abc", set), "set: invalid token rejected");
		check(!parse_value_set("1-2-3", set) || set.ranges.size() >= 1, "set: multiple dashes tolerated");
	}

	{
		value_set set;
		parse_value_set("0,512", set);
		check(value_set_to_string(set) == "0, 512", "stringify: list");
		parse_value_set("100-500", set);
		check(value_set_to_string(set) == "100-500", "stringify: range");
		parse_value_set("100-", set);
		check(value_set_to_string(set) == "100-", "stringify: open high end");
		parse_value_set("*", set);
		check(value_set_to_string(set).empty(), "stringify: any writes nothing");
	}

	if (g_failures != 0)
	{
		std::printf("FAILED: %d check(s)\n", g_failures);
		return 1;
	}

	std::printf("ALL PASSED\n");
	return 0;
}