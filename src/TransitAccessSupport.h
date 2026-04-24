#pragma once

#include "cISC4App.h"
#include "cISC4BuildingOccupant.h"
#include "cISC4City.h"
#include "cISC4Lot.h"
#include "cISC4LotManager.h"
#include "cISC4Occupant.h"
#include "cISC4TrafficSimulator.h"
#include "cISCPropertyHolder.h"
#include "GZServPtrs.h"
#include "SC4List.h"
#include "SC4Rect.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace TransitAccessSupport
{
	constexpr uint32_t kRoadAccessMapLookupAddress = 0x006c0f30;
	constexpr uint32_t kTransitSwitchPointProperty = 0xe90e25a1;
	constexpr uint32_t kMotorizedVehicleNetworkMask = 0x00000449;
	constexpr uint32_t kTrafficNetworkExcludedFlag = 0x00200000;

	constexpr ptrdiff_t kPathFinderSourceMinXOffset = 0x18;
	constexpr ptrdiff_t kPathFinderSourceMinZOffset = 0x1c;
	constexpr ptrdiff_t kPathFinderSourceMaxXOffset = 0x20;
	constexpr ptrdiff_t kPathFinderSourceMaxZOffset = 0x24;

	using RoadAccessMapLookupFn = uint8_t*(__thiscall*)(void*, int32_t*);

	inline cISC4City* GetCity()
	{
		const cISC4AppPtr app;
		return app ? app->GetCity() : nullptr;
	}

	inline cISC4LotManager* GetLotManager()
	{
		cISC4City* const city = GetCity();
		return city ? city->GetLotManager() : nullptr;
	}

	inline cISC4TrafficSimulator* GetTrafficSimulator()
	{
		cISC4City* const city = GetCity();
		return city ? city->GetTrafficSimulator() : nullptr;
	}

	inline cISC4ZoneManager* GetZoneManager()
	{
		cISC4City* const city = GetCity();
		return city ? city->GetZoneManager() : nullptr;
	}

	inline void* GetTrafficNetworkMap()
	{
		cISC4City* const city = GetCity();
		return city ? reinterpret_cast<void*>(city->GetTrafficNetwork()) : nullptr;
	}

	inline bool RectsTouchBySide(const SC4Rect<int32_t>& a, const SC4Rect<int32_t>& b)
	{
		const bool zRangesOverlap = a.topLeftY <= b.bottomRightY && b.topLeftY <= a.bottomRightY;
		const bool xRangesOverlap = a.topLeftX <= b.bottomRightX && b.topLeftX <= a.bottomRightX;

		if (zRangesOverlap && (a.bottomRightX + 1 == b.topLeftX || b.bottomRightX + 1 == a.topLeftX))
		{
			return true;
		}

		return xRangesOverlap && (a.bottomRightY + 1 == b.topLeftY || b.bottomRightY + 1 == a.topLeftY);
	}

	inline bool RectsTouchOrNearlyTouchBySide(const SC4Rect<int32_t>& a, const SC4Rect<int32_t>& b, int32_t maxGapCells)
	{
		const bool zRangesOverlap = a.topLeftY <= b.bottomRightY && b.topLeftY <= a.bottomRightY;
		const bool xRangesOverlap = a.topLeftX <= b.bottomRightX && b.topLeftX <= a.bottomRightX;

		if (zRangesOverlap)
		{
			const int32_t gapAB = b.topLeftX - a.bottomRightX - 1;
			const int32_t gapBA = a.topLeftX - b.bottomRightX - 1;
			if ((gapAB >= 0 && gapAB <= maxGapCells) || (gapBA >= 0 && gapBA <= maxGapCells))
			{
				return true;
			}
		}

		if (xRangesOverlap)
		{
			const int32_t gapAB = b.topLeftY - a.bottomRightY - 1;
			const int32_t gapBA = a.topLeftY - b.bottomRightY - 1;
			if ((gapAB >= 0 && gapAB <= maxGapCells) || (gapBA >= 0 && gapBA <= maxGapCells))
			{
				return true;
			}
		}

		return false;
	}

	inline bool RectsEqual(const SC4Rect<int32_t>& a, const SC4Rect<int32_t>& b)
	{
		return a.topLeftX == b.topLeftX
			&& a.topLeftY == b.topLeftY
			&& a.bottomRightX == b.bottomRightX
			&& a.bottomRightY == b.bottomRightY;
	}

	inline bool IsValidRect(const SC4Rect<int32_t>& rect)
	{
		return rect.topLeftX <= rect.bottomRightX && rect.topLeftY <= rect.bottomRightY;
	}

	struct ScopedBoolFlag
	{
		explicit ScopedBoolFlag(bool& flag) : flag(flag)
		{
			flag = true;
		}

		ScopedBoolFlag(const ScopedBoolFlag&) = delete;
		ScopedBoolFlag& operator=(const ScopedBoolFlag&) = delete;

		~ScopedBoolFlag()
		{
			flag = false;
		}

		bool& flag;
	};

	inline bool IsResidentialZoneType(cISC4ZoneManager::ZoneType zoneType)
	{
		switch (zoneType)
		{
		case cISC4ZoneManager::ZoneType::ResidentialLowDensity:
		case cISC4ZoneManager::ZoneType::ResidentialMediumDensity:
		case cISC4ZoneManager::ZoneType::ResidentialHighDensity:
			return true;
		default:
			return false;
		}
	}

	// Some lots report an ambiguous zone type, so also inspect cells under the lot footprint.
	inline bool IsResidentialSourceLot(cISC4Lot* lot)
	{
		if (!lot)
		{
			return false;
		}

		if (IsResidentialZoneType(lot->GetZoneType()))
		{
			return true;
		}

		cISC4ZoneManager* const zoneManager = GetZoneManager();
		if (!zoneManager)
		{
			return false;
		}

		SC4Rect<int32_t> bounds;
		if (lot->GetBoundingRect(bounds))
		{
			for (int32_t z = bounds.topLeftY; z <= bounds.bottomRightY; ++z)
			{
				for (int32_t x = bounds.topLeftX; x <= bounds.bottomRightX; ++x)
				{
					if (x >= 0 && z >= 0 && IsResidentialZoneType(zoneManager->GetZoneType(x, z)))
					{
						return true;
					}
				}
			}
			return false;
		}

		int32_t x = 0;
		int32_t z = 0;
		return lot->GetLocation(x, z)
			&& x >= 0
			&& z >= 0
			&& IsResidentialZoneType(zoneManager->GetZoneType(x, z));
	}

	inline std::vector<cISC4Lot*> GetNearbySideLots(cISC4Lot* lot, int32_t maxGapCells)
	{
		std::vector<cISC4Lot*> result;
		cISC4LotManager* const lotManager = GetLotManager();
		if (!lot || !lotManager)
		{
			return result;
		}

		SC4Rect<int32_t> sourceBounds;
		if (!lot->GetBoundingRect(sourceBounds))
		{
			return result;
		}

		std::unordered_set<cISC4Lot*> candidates;
		auto addCandidate = [&](cISC4Lot* candidate)
		{
			if (candidate && candidate != lot)
			{
				candidates.insert(candidate);
			}
		};

		SC4List<cISC4Lot*> surroundingLots;
		if (lotManager->GetLotsSurroundingLot(lot, surroundingLots, 0))
		{
			for (cISC4Lot* candidate : surroundingLots)
			{
				addCandidate(candidate);
			}
		}

		SC4List<cISC4Lot*> rectLots;
		const int32_t scanRadius = maxGapCells + 1;
		if (lotManager->GetLotsInCellRect(
			rectLots,
			std::max(0, sourceBounds.topLeftX - scanRadius),
			std::max(0, sourceBounds.topLeftY - scanRadius),
			sourceBounds.bottomRightX + scanRadius,
			sourceBounds.bottomRightY + scanRadius,
			true))
		{
			for (cISC4Lot* candidate : rectLots)
			{
				addCandidate(candidate);
			}
		}

		for (int32_t x = sourceBounds.topLeftX - scanRadius; x <= sourceBounds.bottomRightX + scanRadius; ++x)
		{
			for (int32_t z = sourceBounds.topLeftY - scanRadius; z <= sourceBounds.bottomRightY + scanRadius; ++z)
			{
				if (x < 0 || z < 0)
				{
					continue;
				}
				// As the meaning of the third argument is unknown, call it with both values to avoid missing any candidates.
				addCandidate(lotManager->GetLot(x, z, true));
				addCandidate(lotManager->GetLot(x, z, false));
			}
		}

		result.reserve(candidates.size());
		for (cISC4Lot* candidate : candidates)
		{
			SC4Rect<int32_t> candidateBounds;
			if (candidate->GetBoundingRect(candidateBounds)
				&& RectsTouchOrNearlyTouchBySide(sourceBounds, candidateBounds, maxGapCells))
			{
				result.push_back(candidate);
			}
		}

		return result;
	}

	inline bool IsTransitEnabledLot(cISC4Lot* lot)
	{
		if (!lot)
		{
			return false;
		}

		cISCPropertyHolder* const lotPropertyHolder = lot->AsPropertyHolder();
		if (lotPropertyHolder && lotPropertyHolder->HasProperty(kTransitSwitchPointProperty))
		{
			return true;
		}

		cISC4BuildingOccupant* const building = lot->GetBuilding();
		if (!building)
		{
			return false;
		}

		cISC4Occupant* const buildingOccupant = building->AsOccupant();
		cISCPropertyHolder* const buildingPropertyHolder = buildingOccupant ? buildingOccupant->AsPropertyHolder() : nullptr;
		return buildingPropertyHolder && buildingPropertyHolder->HasProperty(kTransitSwitchPointProperty);
	}

	inline bool TrafficNetworkEntryHasFlag(void* entry, uint32_t flag)
	{
		if (!entry)
		{
			return false;
		}

		using HasFlagFn = bool(__thiscall*)(void*, uint32_t);
		auto** const vtable = *reinterpret_cast<void***>(entry);
		if (!vtable || !vtable[8])
		{
			return false;
		}

		const auto hasFlag = reinterpret_cast<HasFlagFn>(vtable[8]);
		return hasFlag(entry, flag);
	}

	// cISC4City exposes the traffic network object, but it has not yet been decoded in gzcom-dll
	inline bool HasRoadLikeNetworkAtCell(int32_t x, int32_t z)
	{
		void* const trafficNetworkMap = GetTrafficNetworkMap();
		if (!trafficNetworkMap)
		{
			return false;
		}

		using GetNetworkInfoFn = void*(__thiscall*)(void*, int32_t, int32_t, uint32_t, bool);
		auto** const vtable = *reinterpret_cast<void***>(trafficNetworkMap);
		if (!vtable || !vtable[8])
		{
			return false;
		}

		const auto getNetworkInfo = reinterpret_cast<GetNetworkInfoFn>(vtable[8]);
		void* const entry = getNetworkInfo(trafficNetworkMap, x, z, kMotorizedVehicleNetworkMask, true);
		return entry && !TrafficNetworkEntryHasFlag(entry, kTrafficNetworkExcludedFlag);
	}

	inline bool HasRoadLikeNetworkOnOrAroundLot(cISC4Lot* lot)
	{
		SC4Rect<int32_t> bounds;
		if (!lot || !lot->GetBoundingRect(bounds))
		{
			return false;
		}

		for (int32_t z = bounds.topLeftY - 1; z <= bounds.bottomRightY + 1; ++z)
		{
			for (int32_t x = bounds.topLeftX - 1; x <= bounds.bottomRightX + 1; ++x)
			{
				if (x >= 0 && z >= 0 && HasRoadLikeNetworkAtCell(x, z))
				{
					return true;
				}
			}
		}

		return false;
	}

	// Updates the lot's internal cached road-access result after the TE fallback succeeds.
	inline void SetRoadAccessCache(cISC4Lot* lot, const bool value)
	{
		const auto lookup = reinterpret_cast<RoadAccessMapLookupFn>(kRoadAccessMapLookupAddress);
		int32_t key = 0;
		uint8_t* const cacheValue = lookup(reinterpret_cast<uint8_t*>(lot) + 0x40, &key);
		if (cacheValue)
		{
			*cacheValue = value ? 1 : 0;
		}
	}

	inline void CollectAdjacentTransitEnabledRoadAccessLots(
		cISC4Lot* lot,
		std::vector<cISC4Lot*>& matches,
		std::unordered_set<cISC4Lot*>& seenMatches)
	{
		if (!lot || !IsResidentialSourceLot(lot))
		{
			return;
		}

		SC4Rect<int32_t> sourceBounds;
		if (!lot->GetBoundingRect(sourceBounds))
		{
			return;
		}

		const std::vector<cISC4Lot*> candidates = GetNearbySideLots(lot, 0);
		for (cISC4Lot* candidate : candidates)
		{
			SC4Rect<int32_t> candidateBounds;
			if (!candidate->GetBoundingRect(candidateBounds) || !RectsTouchBySide(sourceBounds, candidateBounds))
			{
				continue;
			}

			if (IsTransitEnabledLot(candidate) && HasRoadLikeNetworkOnOrAroundLot(candidate))
			{
				if (seenMatches.insert(candidate).second)
				{
					matches.push_back(candidate);
				}
			}
		}
	}

	inline bool HasAdjacentTransitEnabledRoadAccess(cISC4Lot* lot)
	{
		std::vector<cISC4Lot*> matches;
		std::unordered_set<cISC4Lot*> seenMatches;
		CollectAdjacentTransitEnabledRoadAccessLots(lot, matches, seenMatches);
		return !matches.empty();
	}

	inline int32_t ReadPathFinderInt32(void* pathFinder, ptrdiff_t offset)
	{
		return *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(pathFinder) + offset);
	}

	inline void WritePathFinderInt32(void* pathFinder, ptrdiff_t offset, int32_t value)
	{
		*reinterpret_cast<int32_t*>(static_cast<uint8_t*>(pathFinder) + offset) = value;
	}

	// The pathfinder type is not exposed by gzcom-dll, so source rect access uses known field offsets.
	inline SC4Rect<int32_t> GetPathFinderSourceRect(void* pathFinder)
	{
		SC4Rect<int32_t> rect{};
		rect.topLeftX = ReadPathFinderInt32(pathFinder, kPathFinderSourceMinXOffset);
		rect.topLeftY = ReadPathFinderInt32(pathFinder, kPathFinderSourceMinZOffset);
		rect.bottomRightX = ReadPathFinderInt32(pathFinder, kPathFinderSourceMaxXOffset);
		rect.bottomRightY = ReadPathFinderInt32(pathFinder, kPathFinderSourceMaxZOffset);
		return rect;
	}

	inline void SetPathFinderSourceRect(void* pathFinder, const SC4Rect<int32_t>& rect)
	{
		WritePathFinderInt32(pathFinder, kPathFinderSourceMinXOffset, rect.topLeftX);
		WritePathFinderInt32(pathFinder, kPathFinderSourceMinZOffset, rect.topLeftY);
		WritePathFinderInt32(pathFinder, kPathFinderSourceMaxXOffset, rect.bottomRightX);
		WritePathFinderInt32(pathFinder, kPathFinderSourceMaxZOffset, rect.bottomRightY);
	}
}
