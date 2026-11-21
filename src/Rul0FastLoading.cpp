#include "Rul0FastLoading.h"
#include "Rul0Parser.h"
#include "RZIniFile.h"
#include "NetworkStubs.h"
#include "Patching.h"
#include "Logger.h"
#include "wil/result_macros.h"
#include <chrono>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <memory>
#include <new>
#include <string_view>

// The game loads the RUL0 file (the puzzle pieces) with cRZIniFile, which reads the file in 40 byte steps and builds
// several strings per line, once to find the sections and once more for the entries of every section. The code below
// hands the game's loader an object that serves the same sections and entries from a copy of the file in memory.
// Everything that interprets the entries and fills the intersection rules is still the game's own code.
namespace
{
	using nSC4Networks::cIntRule;
	typedef SC4HashMap<uint32_t, cIntRule> RuleMap;

	// cRZString
	struct GameString
	{
		void* vtable;
		char* begin;
		char* end;
		char* capacity;
		uint32_t refCount;
	};
	typedef void (__cdecl* IniCallback)(const GameString& first, const GameString& second, void* context);

	typedef void (__cdecl* pfn_LoadIntersectionRules)(void* iniFile, RuleMap* rules);
	typedef void (__cdecl* pfn_RotateGrid)(Rul0Parser::ByteVector* grid, int32_t* width, int32_t* point, uint8_t rotation, char swapDirections);
	typedef void (__cdecl* pfn_TransposeGrid)(Rul0Parser::ByteVector* grid, uint32_t* width, int32_t* point, char swapDirections);
	typedef void (__thiscall* pfn_RuleMap_Clear)(RuleMap* pThis);
	typedef void* (__cdecl* pfn_Allocate)(uint32_t size);
	typedef void (__cdecl* pfn_Free)(void* memory);
	const pfn_LoadIntersectionRules LoadIntersectionRules = reinterpret_cast<pfn_LoadIntersectionRules>(0x641b60);
	const pfn_RotateGrid RotateGrid = reinterpret_cast<pfn_RotateGrid>(0x609040);
	const pfn_TransposeGrid TransposeGrid = reinterpret_cast<pfn_TransposeGrid>(0x608f20);
	const pfn_RuleMap_Clear RuleMap_Clear = reinterpret_cast<pfn_RuleMap_Clear>(0x62a3c0);
	const pfn_Allocate Allocate = reinterpret_cast<pfn_Allocate>(0x90cf54);
	const pfn_Free Free = reinterpret_cast<pfn_Free>(0x90cf63);
	void* const cRZString_vtable = reinterpret_cast<void*>(0xa80810);
	constexpr uint32_t LoadIntersectionRules_CallSite = 0x625556;
	constexpr uint32_t RotateGrid_CallSites[] = {0x60972c, 0x609759};
	constexpr uint32_t TransposeGrid_CallSites[] = {0x609445, 0x609471};

	bool sVerifyFastLoading = false;
	bool sRotateGridsQuickly = true;

	double millisecondsSince(std::chrono::steady_clock::time_point start)
	{
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	}

	// Takes the place of the cRZIniFile. The loader (0x641b60) and its callbacks (0x641a50, 0x6416b0, 0x640320) call
	// two virtual functions of it and use nothing else.
	struct FastIniFile
	{
		void* const* vtable;
		const Rul0Parser::IniFile* file;
	};

	GameString toGameString(std::string_view text)  // a NUL has to follow the text
	{
		char* const begin = const_cast<char*>(text.data());
		return {cRZString_vtable, begin, begin + text.size(), begin + text.size() + 1, 0};
	}

	// cRZIniFile::EnumEntries (0x91d91c). __fastcall with an unused second parameter stands for __thiscall.
	bool __fastcall FastIniFile_EnumEntries(FastIniFile* pThis, void*, const GameString& sectionName, IniCallback callback, void* context)
	{
		const Rul0Parser::Section* const section = pThis->file->Find(std::string_view(sectionName.begin, sectionName.end - sectionName.begin));
		if (section == nullptr) {
			return false;
		}
		const Rul0Parser::Entry* const entries = pThis->file->Entries(*section);
		for (size_t i = 0; i < section->entryCount; i++) {
			const GameString key = toGameString(entries[i].key), value = toGameString(entries[i].value);
			callback(key, value, context);
		}
		return true;
	}

	// cRZIniFile's enumeration of its sections (0x91cfa5)
	bool __fastcall FastIniFile_EnumSections(FastIniFile* pThis, void*, IniCallback callback, void* context)
	{
		const GameString none = toGameString(std::string_view("", 0));
		for (const Rul0Parser::Section& section : pThis->file->Sections()) {
			const GameString name = toGameString(section.name);
			callback(name, none, context);
		}
		return true;
	}

	void* const FastIniFile_vtable[] = {
		nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
		reinterpret_cast<void*>(FastIniFile_EnumEntries),   // 0x34
		reinterpret_cast<void*>(FastIniFile_EnumSections),  // 0x38
	};

	// Rotate and Transpose entries of rules without cells make the game count to an arbitrary number, see Rul0Parser.h.
	void RotateGridHook(Rul0Parser::ByteVector* grid, int32_t* width, int32_t* point, uint8_t rotation, char swapDirections)
	{
		if (sRotateGridsQuickly) {
			Rul0Parser::RotateGridQuickly(RotateGrid, grid, width, point, rotation, swapDirections);
		} else {
			RotateGrid(grid, width, point, rotation, swapDirections);
		}
	}

	void TransposeGridHook(Rul0Parser::ByteVector* grid, uint32_t* width, int32_t* point, char swapDirections)
	{
		if (sRotateGridsQuickly) {
			Rul0Parser::TransposeGridQuickly(TransposeGrid, grid, width, point, swapDirections);
		} else {
			TransposeGrid(grid, width, point, swapDirections);
		}
	}

	// Reads the file of the cRZIniFile and lets the game's loader fill `rules` from it.
	// Returns false, without having changed anything, if the game has to read this file itself.
	bool loadIntersectionRulesFast(void* iniFile, RuleMap* rules, size_t& sectionCount, size_t& entryCount)
	{
		uint32_t size = 0;
		const std::unique_ptr<char[]> data = RZIniFile::ReadAll(iniFile, size);
		Rul0Parser::IniFile file;
		try {
			if (data == nullptr || !file.Parse(data.get(), size)) {
				return false;
			}
		} catch (const std::bad_alloc&) {
			return false;
		}
		sectionCount = file.Sections().size();
		entryCount = file.EntryCount();
		FastIniFile fastIniFile = {FastIniFile_vtable, &file};
		LoadIntersectionRules(&fastIniFile, rules);
		return true;
	}

	template <typename T, typename Same>
	bool sameVectors(const SC4Vector<T>& a, const SC4Vector<T>& b, Same&& same)
	{
		return std::equal(a.begin(), a.end(), b.begin(), b.end(), same);
	}

	template <typename Key, typename Value, typename Same>
	bool sameHashMaps(const SC4HashMap<Key, Value>& a, const SC4HashMap<Key, Value>& b, Same&& same)
	{
		if (a.mpEnd - a.mpStart != b.mpEnd - b.mpStart || a.mSize != b.mSize) {
			return false;
		}
		for (ptrdiff_t i = 0; i < a.mpEnd - a.mpStart; i++) {
			auto nodeA = a.mpStart[i];
			auto nodeB = b.mpStart[i];
			for (; nodeA != nullptr && nodeB != nullptr; nodeA = nodeA->next, nodeB = nodeB->next) {
				if (nodeA->item.first != nodeB->item.first || !same(nodeA->item.second, nodeB->item.second)) {
					return false;
				}
			}
			if (nodeA != nodeB) {
				return false;
			}
		}
		return true;
	}

	bool samePoints(const SC4Point<int32_t>& a, const SC4Point<int32_t>& b)
	{
		return a.x == b.x && a.y == b.y;
	}

	// Returns the name of the first part of two rules that differs, or null. Padding is left out, and so is what the
	// game does not initialise: the bounds and the grid width of a rule without cells, and a replacement that is not set.
	const char* findDifference(const cIntRule& a, const cIntRule& b)
	{
		using namespace nSC4Networks;
		const uint8_t* const bytesA = reinterpret_cast<const uint8_t*>(&a);
		const uint8_t* const bytesB = reinterpret_cast<const uint8_t*>(&b);
		const auto sameBytes = [bytesA, bytesB](size_t from, size_t to) { return std::memcmp(bytesA + from, bytesB + from, to - from) == 0; };
		typedef SC4Vector<SC4Point<int32_t>> Points;
		const Points& otherCellsA = *reinterpret_cast<const Points*>(bytesA + 0x34);  // not filled from RUL0
		const Points& otherCellsB = *reinterpret_cast<const Points*>(bytesB + 0x34);
		const GameString& effectA = *reinterpret_cast<const GameString*>(bytesA + 0x80);
		const GameString& effectB = *reinterpret_cast<const GameString*>(bytesB + 0x80);

		if (a.hid != b.hid) {
			return "the ID";
		} else if (!sameVectors(a.pieces, b.pieces, [](const cIntPiece& p, const cIntPiece& q) {
				return std::memcmp(&p, &q, offsetof(cIntPiece, flip) + 1) == 0 && std::memcmp(&p.offset, &q.offset, sizeof(p.offset)) == 0; })) {
			return "Piece";
		} else if (!samePoints(a.stepOffsets, b.stepOffsets)) {
			return "StepOffsets";
		} else if (a.autoTileIndices.size() != b.autoTileIndices.size()
			|| !sameVectors(a.autoTileIndices, b.autoTileIndices, [](uint8_t p, uint8_t q) { return p == q; })
			|| !sameVectors(a.staticCells, b.staticCells, samePoints)
			|| !sameVectors(a.unnamedStaticCells, b.unnamedStaticCells, samePoints)
			|| !sameVectors(otherCellsA, otherCellsB, samePoints)
			|| !sameVectors(a.checkCells, b.checkCells, [](const cIntCheckCell& p, const cIntCheckCell& q) {
				return samePoints(p.cell, q.cell) && p.letter == q.letter; })
			|| !sameHashMaps(a.tileDefs, b.tileDefs, [](const cTileDef& p, const cTileDef& q) {
				return std::memcmp(&p, &q, offsetof(cTileDef, isUnnamed) + 1) == 0; })) {
			return "the cells of CellLayout";
		} else if (!a.autoTileIndices.empty() && (!samePoints(a.min, b.min) || !samePoints(a.max, b.max) || a.autoTileGridWidth != b.autoTileGridWidth)) {
			return "the bounds of CellLayout";
		} else if (a.hasReplacementIntersection != b.hasReplacementIntersection
			|| (a.hasReplacementIntersection && !samePoints(a.replacementIntersection, b.replacementIntersection))) {
			return "ReplacementIntersection";
		} else if (!sameHashMaps(a.checkTypes, b.checkTypes, [](const CheckType& p, const CheckType& q) {
				return p.networks[0] == q.networks[0] && p.networks[1] == q.networks[1]
					&& (p.networks[0] >= 0x80 || (p.edges1 == q.edges1 && p.mask1 == q.mask1))  // only set for a network
					&& (p.networks[1] >= 0x80 || (p.edges2 == q.edges2 && p.mask2 == q.mask2))
					&& std::memcmp(&p.isMultiOk, &q.isMultiOk, 3) == 0; })) {  // with isStatic and isOptional
			return "CheckType";
		} else if (!sameBytes(0x78, 0x7c)) {
			return "ConvertQueryID";
		} else if (effectA.end - effectA.begin != effectB.end - effectB.begin || std::memcmp(effectA.begin, effectB.begin, effectA.end - effectA.begin) != 0) {
			return "PreviewEffect";
		} else if (!sameBytes(0x94, 0x9c)) {
			return "HandleOffset";
		} else if (!sameVectors(a.constraints, b.constraints, [](uint8_t p, uint8_t q) { return p == q; })
			|| a.constraintsGridWidth != b.constraintsGridWidth || !samePoints(a.constraintsOrigin, b.constraintsOrigin)) {
			return "ConsLayout";
		} else if (!sameBytes(0xb4, 0xb8)) {
			return "Costs";
		} else if (a.autoTileBase != b.autoTileBase || a.autoPathBase != b.autoPathBase) {
			return "AutoTileBase or AutoPathBase";
		} else if (a.networkFlags != b.networkFlags || a.oneWayDir != b.oneWayDir || a.autoPlace != b.autoPlace) {
			return "the network flags, OneWayDir or AutoPlace";
		} else if (a.hidRotPrev != b.hidRotPrev || a.hidRotNext != b.hidRotNext || a.hidTabPrev != b.hidTabPrev || a.hidTabNext != b.hidTabNext) {
			return "the rings of Ordering";
		}
		return nullptr;
	}

	// Debugging aid: lets the game load the rules as usual, then loads them once more with the fast loader into a
	// second map and reports rules that differ.
	void loadIntersectionRulesVerified(void* iniFile, RuleMap* rules)
	{
		Logger& logger = Logger::GetInstance();
		const uint32_t bucketCount = static_cast<uint32_t>(rules->mpEnd - rules->mpStart);
		sRotateGridsQuickly = false;
		auto start = std::chrono::steady_clock::now();
		LoadIntersectionRules(iniFile, rules);
		const double originalTime = millisecondsSince(start);
		sRotateGridsQuickly = true;

		// an empty map like the one the game started with, so that both maps grow in the same way
		RuleMap fastRules = *rules;
		fastRules.mSize = 0;
		fastRules.mpStart = bucketCount == 0 ? nullptr : static_cast<RuleMap::HashMapNode**>(Allocate(bucketCount * sizeof(RuleMap::HashMapNode*)));
		if (fastRules.mpStart == nullptr) {
			logger.WriteLine(LogLevel::Error, "RUL0 verification: could not create a second set of rules.");
			return;
		}
		std::memset(fastRules.mpStart, 0, bucketCount * sizeof(RuleMap::HashMapNode*));
		fastRules.mpEnd = fastRules.mpCapacity = fastRules.mpStart + bucketCount;

		start = std::chrono::steady_clock::now();
		size_t sectionCount = 0, entryCount = 0;
		if (!loadIntersectionRulesFast(iniFile, &fastRules, sectionCount, entryCount)) {
			logger.WriteLine(LogLevel::Info, "RUL0 verification: the fast loader would leave this file to the game.");
		} else {
			const double fastTime = millisecondsSince(start);
			// both maps were filled by the same steps, so their rules have to be in the same order as well
			const bool sameSize = rules->mSize == fastRules.mSize && rules->mpEnd - rules->mpStart == fastRules.mpEnd - fastRules.mpStart;
			size_t differences = sameSize ? 0 : 1;
			size_t placeQueryIds = 0;
			for (ptrdiff_t i = 0; sameSize && i < rules->mpEnd - rules->mpStart; i++) {
				auto expected = rules->mpStart[i];
				auto actual = fastRules.mpStart[i];
				for (; expected != nullptr && actual != nullptr; expected = expected->next, actual = actual->next) {
					const char* const difference = expected->item.first != actual->item.first ? "the order"
						: findDifference(expected->item.second, actual->item.second);
					if (difference != nullptr && differences++ < 10) {
						logger.WriteLineFormatted(LogLevel::Error, "RUL0 verification: rule 0x%08x differs in %s.", expected->item.first, difference);
					}
					// the game sets this only for rules with a PlaceQueryID, the others keep what was in memory
					placeQueryIds += std::memcmp(reinterpret_cast<const uint8_t*>(&expected->item.second) + 0x7c,
						reinterpret_cast<const uint8_t*>(&actual->item.second) + 0x7c, sizeof(uint32_t)) != 0;
				}
				differences += expected != actual;
			}
			logger.WriteLineFormatted(differences == 0 ? LogLevel::Info : LogLevel::Error,
				"RUL0 verification: game loaded %u rules in %.0f ms, fast loader %u rules from %u sections with %u entries in %.0f ms, "
				"%u differences (and %u in a PlaceQueryID, which the game does not initialise).",
				rules->mSize, originalTime, fastRules.mSize, static_cast<uint32_t>(sectionCount), static_cast<uint32_t>(entryCount), fastTime,
				static_cast<uint32_t>(differences), static_cast<uint32_t>(placeQueryIds));
		}

		RuleMap_Clear(&fastRules);
		Free(fastRules.mpStart);
	}

	// Replaces the game's call of the RUL0 loader.
	void LoadIntersectionRulesFast(void* iniFile, RuleMap* rules)
	{
		if (sVerifyFastLoading) {
			loadIntersectionRulesVerified(iniFile, rules);
			return;
		}

		Logger& logger = Logger::GetInstance();
		const auto start = std::chrono::steady_clock::now();
		size_t sectionCount = 0, entryCount = 0;
		if (loadIntersectionRulesFast(iniFile, rules, sectionCount, entryCount)) {
			logger.WriteLineFormatted(LogLevel::Info, "Loaded %u RUL0 intersection rules from %u sections with %u entries in %.0f ms.",
				rules->mSize, static_cast<uint32_t>(sectionCount), static_cast<uint32_t>(entryCount), millisecondsSince(start));
		} else {
			// nothing was changed yet, so the game can load the file from the start
			logger.WriteLine(LogLevel::Info, "Left loading of the RUL0 intersection rules to the game.");
			LoadIntersectionRules(iniFile, rules);
		}
	}

	// The hooks call the function that the game called, so each site has to be a call of it.
	void checkCallSite(uint32_t address, void* target)
	{
		const uint8_t* const callSite = reinterpret_cast<uint8_t*>(address);
		int32_t offset;
		std::memcpy(&offset, callSite + 1, sizeof(offset));
		THROW_HR_IF(E_FAIL, callSite[0] != 0xE8 || address + 5 + offset != reinterpret_cast<uint32_t>(target));
	}
}

void Rul0FastLoading::Install(bool verify)
{
	checkCallSite(LoadIntersectionRules_CallSite, LoadIntersectionRules);
	for (const uint32_t callSite : RotateGrid_CallSites) {
		checkCallSite(callSite, RotateGrid);
	}
	for (const uint32_t callSite : TransposeGrid_CallSites) {
		checkCallSite(callSite, TransposeGrid);
	}

	sVerifyFastLoading = verify;
	Patching::InstallCallHook(LoadIntersectionRules_CallSite, reinterpret_cast<void (*)(void)>(LoadIntersectionRulesFast));
	for (const uint32_t callSite : RotateGrid_CallSites) {
		Patching::InstallCallHook(callSite, reinterpret_cast<void (*)(void)>(RotateGridHook));
	}
	for (const uint32_t callSite : TransposeGrid_CallSites) {
		Patching::InstallCallHook(callSite, reinterpret_cast<void (*)(void)>(TransposeGridHook));
	}
}
