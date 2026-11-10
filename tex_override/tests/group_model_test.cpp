// group_model_test.cpp  (generated from dllmain.cpp -- do not edit by hand)
//
// Unit test for the Toggle Group bookkeeping: group indices, implicit-group
// pruning and "delete rule / delete group" index fix-up.
//
// The tested block (struct toggle_group .. prune_implicit_groups) is extracted
// verbatim from dllmain.cpp, so it cannot drift from the shipped
// implementation. The rendering side relies on rules referencing groups by
// index, which makes off-by-one errors here silent and nasty; this test pins
// them down.
//
// Build/run:
//   cl /nologo /std:c++17 /O2 /EHsc /utf-8 /Fe:group_model_test.exe group_model_test.cpp
//   group_model_test.exe

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// dllmain.cpp: struct value_set (only what the rule struct needs)
// ---------------------------------------------------------------------------
struct value_set
{
	bool any = true;
	std::vector<std::pair<uint32_t, uint32_t>> ranges;
};

// ---------------------------------------------------------------------------
// dllmain.cpp: struct texture_override_rule
// ---------------------------------------------------------------------------
struct texture_override_rule
{
	std::string name;
	uint32_t hash = 0;
	bool hash_any = false;
	uint32_t ps_hash = 0;
	uint32_t vs_hash = 0;
	value_set first_index;
	value_set index_count;
	int priority = 0;
	bool active = true;
	size_t order = 0;
	size_t group = 0;
	uint32_t hotkey = 0;
};

// ---------------------------------------------------------------------------
// dllmain.cpp: struct toggle_group .. prune_implicit_groups
// ---------------------------------------------------------------------------
struct toggle_group
{
	std::string name;
	uint32_t hotkey = 0;
	bool active = true;
	bool persistent = false;
};

struct rule_set
{
	std::vector<texture_override_rule> rules;
	std::vector<toggle_group> groups;
};

static int find_group_index(const std::vector<toggle_group> &groups, const std::string &name)
{
	for (size_t i = 0; i < groups.size(); ++i)
	{
		if (groups[i].name == name)
			return static_cast<int>(i);
	}
	return -1;
}

static std::string make_unique_group_name(const std::vector<toggle_group> &groups, const std::string &base)
{
	if (find_group_index(groups, base) < 0)
		return base;

	for (int suffix = 2; suffix < 10000; ++suffix)
	{
		const std::string candidate = base + " (" + std::to_string(suffix) + ")";
		if (find_group_index(groups, candidate) < 0)
			return candidate;
	}

	return base + " (copy)";
}

static void prune_implicit_groups(rule_set &set)
{
	std::vector<bool> used(set.groups.size(), false);
	for (const texture_override_rule &rule : set.rules)
	{
		if (rule.group < used.size())
			used[rule.group] = true;
	}

	std::vector<size_t> remap(set.groups.size(), SIZE_MAX);
	std::vector<toggle_group> kept;
	kept.reserve(set.groups.size());

	for (size_t i = 0; i < set.groups.size(); ++i)
	{
		if (!used[i] && !set.groups[i].persistent)
			continue;

		remap[i] = kept.size();
		kept.push_back(std::move(set.groups[i]));
	}

	for (texture_override_rule &rule : set.rules)
	{
		if (rule.group < remap.size() && remap[rule.group] != SIZE_MAX)
			rule.group = remap[rule.group];
		else
			rule.group = 0;
	}

	set.groups = std::move(kept);
}

static void remove_group(rule_set &set, size_t index)
{
	if (index >= set.groups.size())
		return;

	set.groups.erase(set.groups.begin() + index);

	for (auto it = set.rules.begin(); it != set.rules.end();)
	{
		if (it->group == index)
		{
			it = set.rules.erase(it);
		}
		else
		{
			if (it->group > index)
				--it->group;
			++it;
		}
	}
}

static void remove_rule(rule_set &set, size_t index)
{
	if (index >= set.rules.size())
		return;

	const size_t group = set.rules[index].group;
	set.rules.erase(set.rules.begin() + index);

	if (group >= set.groups.size() || set.groups[group].persistent)
		return;

	for (const texture_override_rule &rule : set.rules)
	{
		if (rule.group == group)
			return;
	}

	remove_group(set, group);
}

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------
static int g_failures = 0;

static void check(bool condition, const char *what)
{
	if (condition)
	{
		std::printf("  ok   %s\n", what);
		return;
	}

	std::printf("  FAIL %s\n", what);
	++g_failures;
}

static toggle_group make_group(const char *name, bool persistent)
{
	toggle_group group;
	group.name = name;
	group.persistent = persistent;
	return group;
}

static texture_override_rule make_rule(const char *name, size_t group)
{
	texture_override_rule rule;
	rule.name = name;
	rule.group = group;
	return rule;
}

// 三条规则：G0 隐式组里 1 条，G1 显式组里 2 条
static rule_set make_scene()
{
	rule_set set;
	set.groups.push_back(make_group("Implicit", false)); // 0
	set.groups.push_back(make_group("Explicit", true));  // 1

	set.rules.push_back(make_rule("a", 0));
	set.rules.push_back(make_rule("b", 1));
	set.rules.push_back(make_rule("c", 1));
	return set;
}

int main()
{
	std::printf("group model test\n");

	// 1) 删掉隐式组的最后一条规则 -> 组消失，后续下标前移
	{
		rule_set set = make_scene();
		set.groups.push_back(make_group("Other", false)); // 2
		set.rules.push_back(make_rule("d", 2));

		remove_rule(set, 0); // 删 "a"

		check(set.rules.size() == 3, "remove_rule: rule count");
		check(set.groups.size() == 2, "remove_rule: empty implicit group pruned");
		check(set.groups[0].name == "Explicit", "remove_rule: groups renumbered");
		check(set.rules[0].group == 0 && set.rules[1].group == 0, "remove_rule: explicit group index fixed");
		check(set.rules[2].group == 1 && set.groups[1].name == "Other", "remove_rule: trailing group index fixed");
	}

	// 2) 删掉显式组的最后一条规则 -> 空组保留（用户先建组后补规则）
	{
		rule_set set;
		set.groups.push_back(make_group("Explicit", true));
		set.rules.push_back(make_rule("only", 0));

		remove_rule(set, 0);

		check(set.rules.empty(), "remove_rule: rule erased");
		check(set.groups.size() == 1 && set.groups[0].persistent, "remove_rule: persistent group kept");
	}

	// 3) 删组 -> 组内规则一起删，其它组的规则下标前移
	{
		rule_set set = make_scene();
		remove_group(set, 0);

		check(set.groups.size() == 1 && set.groups[0].name == "Explicit", "remove_group: group erased");
		check(set.rules.size() == 2, "remove_group: its rules erased");
		check(set.rules[0].group == 0 && set.rules[1].group == 0, "remove_group: remaining rules renumbered");
	}

	// 4) 删中间组 -> 后面的规则跟着前移，且仍指向原来的组
	{
		rule_set set = make_scene();
		set.groups.push_back(make_group("Third", true)); // 2
		set.rules.push_back(make_rule("d", 2));

		remove_group(set, 1); // 删 "Explicit"

		check(set.groups.size() == 2, "remove_group(middle): group count");
		check(set.groups[1].name == "Third", "remove_group(middle): order preserved");
		check(set.rules.size() == 2, "remove_group(middle): rules of that group gone");
		check(set.rules[0].name == "a" && set.rules[0].group == 0, "remove_group(middle): earlier rule untouched");
		check(set.rules[1].name == "d" && set.rules[1].group == 1, "remove_group(middle): later rule follows its group");
	}

	// 5) 剪枝不会误删仍被引用的隐式组（从显式组里删一条规则）
	{
		rule_set set = make_scene();
		set.rules.erase(set.rules.begin() + 1); // 删 "b"，组 1 里还有 "c"
		prune_implicit_groups(set);

		check(set.groups.size() == 2, "prune: explicit group kept");
		check(set.groups[0].name == "Implicit", "prune: referenced implicit group kept");
		check(set.rules.size() == 2, "prune: rule count");
		check(set.rules[0].group == 0 && set.rules[1].group == 1, "prune: indices unchanged when nothing removed");
	}

	// 5b) 隐式组的规则被搬进别的组 -> 隐式组消失，显式组保留
	{
		rule_set set = make_scene();
		set.rules[0].group = 1; // "a" 搬到显式组
		prune_implicit_groups(set);

		check(set.groups.size() == 1 && set.groups[0].name == "Explicit", "prune: implicit group dropped");
		check(set.rules.size() == 3 && set.rules[0].group == 0, "prune: moved rule remapped");
	}

	// 6) 一个完全没被引用的隐式组被剪掉，引用它的规则下标被重映射
	{
		rule_set set;
		set.groups.push_back(make_group("Dead", false));   // 0
		set.groups.push_back(make_group("Alive", false));  // 1
		set.rules.push_back(make_rule("a", 1));

		prune_implicit_groups(set);

		check(set.groups.size() == 1 && set.groups[0].name == "Alive", "prune: dead implicit group removed");
		check(set.rules.size() == 1 && set.rules[0].group == 0, "prune: rule remapped to new index");
	}

	// 7) 组名唯一化
	{
		std::vector<toggle_group> groups;
		groups.push_back(make_group("Hunting_3", false));

		const std::string first = make_unique_group_name(groups, "Hunting_3");
		check(first == "Hunting_3 (2)", "unique name: suffix (2)");

		groups.push_back(make_group(first.c_str(), false));
		const std::string second = make_unique_group_name(groups, "Hunting_3");
		check(second == "Hunting_3 (3)", "unique name: next suffix");

		check(make_unique_group_name(groups, "Fresh") == "Fresh", "unique name: unused base unchanged");
	}

	// 8) 越界删除必须是 no-op（UI 快照可能比表旧一帧）
	{
		rule_set set = make_scene();
		remove_rule(set, 99);
		remove_group(set, 99);

		check(set.rules.size() == 3 && set.groups.size() == 2, "out-of-range delete is a no-op");
	}

	std::printf(g_failures == 0 ? "\nALL PASSED\n" : "\n%d FAILURE(S)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
