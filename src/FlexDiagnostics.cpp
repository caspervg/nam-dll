#include "FlexDiagnostics.h"
#include "NetworkStubs.h"
#include "Patching.h"
#include "Logger.h"
#include "wil/result.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

// Call sites (Windows SimCity 4.exe 1.1.641), all plain `call rel32`:
//   0x66108d  cSC4ViewInputControlNetworkIntxTool::OnMouseDownL -> cSC4NetworkTool::InsertIsolatedHighwayIntersection
//             (thiscall: x, z, ruleId, commit; RET 0x10). The hover preview uses a different call site (0x66166f).
//   0x62d28e  InsertIsolatedHighwayIntersection -> cSC4NetworkTool::DoAdditionalDraws (ecx = tool, no stack args)
//             (merges the queued puzzle piece edges into the cells: Merge 0x620f00)
//   0x62d2c2  InsertIsolatedHighwayIntersection -> cSC4NetworkTool::SolveNetwork (thiscall, 2 args, RET 8)
//             (SolveMultiTypeIntersections, RUL1/INRUL lookup; fills solvedCells)
namespace
{
	constexpr uint32_t Click_CallSite = 0x66108d;
	constexpr uint32_t Click_Target = 0x62cd50;
	constexpr uint32_t AdditionalDraws_CallSite = 0x62d28e;
	constexpr uint32_t AdditionalDraws_Target = 0x6384e0;
	constexpr uint32_t Solve_CallSite = 0x62d2c2;
	constexpr uint32_t Solve_Target = 0x635cb0;

	typedef bool (__thiscall* pfn_InsertIsolatedHighwayIntersection)(cSC4NetworkTool* pThis, int32_t x, int32_t z, uint32_t ruleId, uint32_t commit);
	typedef void (__thiscall* pfn_DoAdditionalDraws)(cSC4NetworkTool* pThis);
	typedef int32_t (__thiscall* pfn_SolveNetwork)(cSC4NetworkTool* pThis, int32_t arg1, int32_t arg2);

	const auto InsertIsolatedHighwayIntersection = reinterpret_cast<pfn_InsertIsolatedHighwayIntersection>(Click_Target);
	const auto DoAdditionalDraws = reinterpret_cast<pfn_DoAdditionalDraws>(AdditionalDraws_Target);
	const auto SolveNetwork = reinterpret_cast<pfn_SolveNetwork>(Solve_Target);

	constexpr const char* kNetworkNames[13] = {
		"Road", "Rail", "Highway", "Street", "Pipe", "Power", "Avenue", "Subway", "LightRail", "Monorail",
		"OneWayRoad", "DirtRoad", "GroundHwy"};

	struct LoggedCell
	{
		char label;          // the CellLayout letter, or '*' for the handle (origin) cell
		int32_t dx, dz;      // relative to the handle
		uint32_t x, z;       // city cell
		bool hasCheckType;
		nSC4Networks::CheckType checkType;
	};

	std::ofstream sLog;
	std::vector<std::pair<uint32_t, uint32_t>> sHidRanges;  // low 16 bits, inclusive
	bool sClickActive = false;   // inside the click's InsertIsolatedHighwayIntersection
	bool sLogging = false;       // ... and the piece's HID is one we log
	uint32_t sRuleId = 0;
	int32_t sX = 0, sZ = 0;
	std::vector<LoggedCell> sCells;

	constexpr uint32_t mkCellXZ(uint32_t x, uint32_t z) { return z * 0x10000 + x; }

	bool hidSelected(uint32_t hid)
	{
		if (sHidRanges.empty()) {
			return true;
		}
		uint32_t low = hid & 0xffff;
		for (auto& [a, b] : sHidRanges) {
			if (low >= a && low <= b) {
				return true;
			}
		}
		return false;
	}

	std::string edgesStr(uint32_t e)
	{
		char buf[64];  // 0xSSEENNWW plus (w,n,e,s)
		std::snprintf(buf, sizeof(buf), "0x%08X(w%u,n%u,e%u,s%u)", e, e & 0xff, (e >> 8) & 0xff, (e >> 16) & 0xff, e >> 24);
		return buf;
	}

	std::string netName(uint8_t n)
	{
		return n < 13 ? kNetworkNames[n] : (n == 0xff ? "-" : "?");
	}

	std::string checkTypeStr(const nSC4Networks::CheckType& ct)
	{
		std::ostringstream s;
		for (int k = 0; k < 2; k++) {
			if (ct.networks[k] == 0xff) continue;
			char buf[48];
			std::snprintf(buf, sizeof(buf), " mask 0x%08X", k == 0 ? ct.mask1 : ct.mask2);
			s << " " << netName(ct.networks[k]) << ":" << edgesStr(k == 0 ? ct.edges1 : ct.edges2) << buf;
		}
		s << (ct.isStatic ? " [built]" : "") << (ct.isOptional ? " [optional]" : "") << (ct.isMultiOk ? " [multiok]" : "");
		if (!ct.isStatic && !ct.isOptional) s << " [check]";
		return s.str();
	}

	// `create`: compute the cell info if it is not cached yet (only for the piece's own cells, which the
	// game loads for this placement anyway); otherwise only report what is cached
	std::string cellStr(cSC4NetworkTool* tool, uint32_t x, uint32_t z, bool create)
	{
		if (x >= tool->numCellsX || z >= tool->numCellsZ) {
			return "outside the city";
		}
		uint32_t xz = mkCellXZ(x, z);
		cSC4NetworkCellInfo* cell = create ? tool->GetCellInfo(xz) : tool->networkWorldCache.GetCell(xz);
		if (cell == nullptr) {
			return "not cached";
		}
		std::ostringstream s;
		bool any = false;
		for (uint32_t n = 0; n < 13; n++) {
			if (cell->networkTypeFlags & (1u << n)) {
				s << (any ? " " : "") << kNetworkNames[n] << ":" << edgesStr(cell->edgesPerNetwork[n]);
				any = true;
			}
		}
		if (!any) {
			s << "empty";
		}
		if (cell->networkOccupant != nullptr) {
			char buf[64];
			std::snprintf(buf, sizeof(buf), " | occupant tile 0x%08X rf 0x%02X",
				cell->networkOccupant->PieceId(), cell->networkOccupant->GetRotationAndFlip());
			s << buf;
		}
		if (cell->isNetworkLot) s << " [network lot]";
		if (cell->isImmovable) s << " [immovable]";
		return s.str();
	}

	void logCells(cSC4NetworkTool* tool, const char* phase, bool create)
	{
		sLog << "  " << phase << ":\n";
		for (auto& c : sCells) {
			char head[48];
			std::snprintf(head, sizeof(head), "    %c (%+d,%+d) @%u,%u: ", c.label, c.dx, c.dz, c.x, c.z);
			sLog << head << cellStr(tool, c.x, c.z, create) << "\n";
		}
	}

	void logSolved(cSC4NetworkTool* tool, int32_t result)
	{
		sLog << "  SolveNetwork returned " << result << "; tiles chosen for the piece's cells:\n";
		for (auto& c : sCells) {
			uint32_t xz = mkCellXZ(c.x, c.z);
			bool found = false;
			for (auto& sc : tool->solvedCells) {
				if (sc.xz == xz) {
					char buf[96];
					std::snprintf(buf, sizeof(buf), "    %c @%u,%u: tile 0x%08X rf 0x%02X\n", c.label, c.x, c.z, sc.id, (uint32_t)sc.rf);
					sLog << buf;
					found = true;
				}
			}
			if (!found) {
				char buf[64];
				std::snprintf(buf, sizeof(buf), "    %c @%u,%u: (not solved)\n", c.label, c.x, c.z);
				sLog << buf;
			}
		}
	}

	bool __fastcall Hook_Click(cSC4NetworkTool* pThis, void*, int32_t x, int32_t z, uint32_t ruleId, uint32_t commit)
	{
		sClickActive = true;
		sLogging = false;
		sRuleId = ruleId;
		sX = x;
		sZ = z;
		sCells.clear();
		bool result = InsertIsolatedHighwayIntersection(pThis, x, z, ruleId, commit);
		if (sLogging) {
			logCells(pThis, "after placement (cached cells and occupants)", false);
			sLog << "  result: " << (result ? "placed" : "not placed") << "\n\n";
			sLog.flush();
		}
		sClickActive = false;
		sLogging = false;
		return result;
	}

	void __fastcall Hook_DoAdditionalDraws(cSC4NetworkTool* pThis)
	{
		if (sLogging) {
			logCells(pThis, "before merge (current tiles)", true);
		}
		DoAdditionalDraws(pThis);
		if (sLogging) {
			logCells(pThis, "after merge (DoAdditionalDraws)", false);
		}
	}

	int32_t __fastcall Hook_SolveNetwork(cSC4NetworkTool* pThis, void*, int32_t arg1, int32_t arg2)
	{
		int32_t result = SolveNetwork(pThis, arg1, arg2);
		if (sLogging) {
			logCells(pThis, "after solve (edges used for RUL1/INRUL)", false);
			logSolved(pThis, result);
		}
		return result;
	}

	void installCallHook(uint32_t callSite, uint32_t expectedTarget, void* hook)
	{
		auto p = reinterpret_cast<uint8_t*>(callSite);
		uint32_t target = callSite + 5 + *reinterpret_cast<int32_t*>(p + 1);
		THROW_HR_IF(E_FAIL, p[0] != 0xe8 || target != expectedTarget);  // already patched by something else: leave it alone
		Patching::InstallCallHook(callSite, reinterpret_cast<void (*)(void)>(hook));
	}

	void parseRanges(const std::string& text)
	{
		std::stringstream ss(text);
		std::string item;
		while (std::getline(ss, item, ',')) {
			auto dash = item.find('-');
			try {
				uint32_t a = std::stoul(item.substr(0, dash), nullptr, 0);
				uint32_t b = dash == std::string::npos ? a : std::stoul(item.substr(dash + 1), nullptr, 0);
				sHidRanges.emplace_back(a & 0xffff, b & 0xffff);
			} catch (const std::exception&) {
				// ignore malformed entries
			}
		}
	}
}

void FlexDiagnostics::OnRule(uint32_t x, uint32_t z, const nSC4Networks::cIntRule& rule)
{
	if (!sClickActive || !sLog.is_open() || !hidSelected(rule.hid)) {
		return;
	}
	sLogging = true;
	sCells.clear();
	sCells.push_back({'*', 0, 0, x, z, false, {}});
	for (auto& cc : rule.checkCells) {
		LoggedCell c{cc.letter, cc.cell.x, cc.cell.y, x + cc.cell.x, z + cc.cell.y, false, {}};
		if (auto it = rule.checkTypes.find(static_cast<uint8_t>(cc.letter)); it != rule.checkTypes.end()) {
			c.hasCheckType = true;
			c.checkType = it->second;
		}
		sCells.push_back(c);
	}
	char buf[160];
	std::snprintf(buf, sizeof(buf), "Placing HID 0x%08X (rule 0x%08X) at handle %u,%u; AutoTileBase 0x%08X, %u check cells\n",
		rule.hid, sRuleId, x, z, rule.autoTileBase, (uint32_t)rule.checkCells.size());
	sLog << buf;
	for (auto& c : sCells) {
		if (c.hasCheckType) {
			char head[32];
			std::snprintf(head, sizeof(head), "  CheckType %c (%+d,%+d):", c.label, c.dx, c.dz);
			sLog << head << checkTypeStr(c.checkType) << "\n";
		}
	}
}

void FlexDiagnostics::Install(const std::filesystem::path& logFilePath, const std::string& hidRanges)
{
	parseRanges(hidRanges);
	sLog.open(logFilePath, std::ios::out | std::ios::app);
	THROW_HR_IF(E_FAIL, !sLog.is_open());
	sLog << "=== NAM.dll FLEX placement diagnostics (HID ranges: " << (hidRanges.empty() ? "all" : hidRanges) << ") ===\n";
	sLog.flush();
	installCallHook(Click_CallSite, Click_Target, reinterpret_cast<void*>(&Hook_Click));
	installCallHook(AdditionalDraws_CallSite, AdditionalDraws_Target, reinterpret_cast<void*>(&Hook_DoAdditionalDraws));
	installCallHook(Solve_CallSite, Solve_Target, reinterpret_cast<void*>(&Hook_SolveNetwork));
}
