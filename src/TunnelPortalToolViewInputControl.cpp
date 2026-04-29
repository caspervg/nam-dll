#include "TunnelPortalTool.h"

#include "TunnelPortalToolPlacement.h"

#include "Logger.h"
#include "cISC4App.h"
#include "cISC4City.h"
#include "cISC4View3DWin.h"
#include "cRZAutoRefCount.h"
#include "cRZBaseString.h"
#include "cSC4BaseViewInputControl.h"
#include "GZServPtrs.h"

#define NOMINMAX
#include <Windows.h>
#include <algorithm>
#include <cstdint>

namespace
{
	constexpr uint32_t kTunnelPortalViewInputControlID = 0x4A7B6E31;
	constexpr uint32_t kPrimaryCursorTextID = kTunnelPortalViewInputControlID + 1;
	constexpr uint32_t kSecondaryCursorTextID = kTunnelPortalViewInputControlID + 2;

	using TunnelPortalToolPlacement::Endpoint;
	using TunnelPortalToolPlacement::NetworkTypeName;
	using TunnelPortalToolPlacement::PlacePortalPair;
	using TunnelPortalToolPlacement::TryFindNetworkAtTile;

	cISC4City* GetCity()
	{
		cISC4AppPtr app;
		return app ? app->GetCity() : nullptr;
	}

	class TunnelPortalViewInputControl final : public cSC4BaseViewInputControl
	{
	public:
		TunnelPortalViewInputControl()
			: cSC4BaseViewInputControl(kTunnelPortalViewInputControlID)
		{
		}

		bool Init() override
		{
			const bool result = cSC4BaseViewInputControl::Init();
			if (result)
			{
				Logger::GetInstance().WriteLine(LogLevel::Debug, "TunnelPortalTool: activated, waiting for first endpoint.");
				ShowPrompt("Select first portal endpoint", "Left click a road/network tile. Esc or right click cancels.");
			}

			return result;
		}

		bool OnKeyDown(int32_t vkCode, uint32_t modifiers) override
		{
			if (vkCode == VK_ESCAPE)
			{
				Logger::GetInstance().WriteLine(LogLevel::Debug, "TunnelPortalTool: cancelled via Escape.");
				ClearFeedback();
				EndInput();
				return true;
			}

			return cSC4BaseViewInputControl::OnKeyDown(vkCode, modifiers);
		}

		bool OnMouseDownR(int32_t x, int32_t z, uint32_t modifiers) override
		{
			Logger::GetInstance().WriteLine(LogLevel::Debug, "TunnelPortalTool: cancelled via right click.");
			ClearFeedback();
			EndInput();
			return true;
		}

		bool OnMouseDownL(int32_t screenX, int32_t screenZ, uint32_t modifiers) override
		{
			if (!IsOnTop())
			{
				return false;
			}

			Endpoint endpoint;
			if (!PickEndpoint(screenX, screenZ, endpoint))
			{
				Logger::GetInstance().WriteLineFormatted(
					LogLevel::Debug,
					"TunnelPortalTool: no compatible network at screen (%d,%d).",
					screenX, screenZ);
				ShowPrompt("No compatible network tile", "Pick an existing surface network tile.");
				return true;
			}

			if (!hasFirstEndpoint)
			{
				firstEndpoint = endpoint;
				hasFirstEndpoint = true;

				Logger::GetInstance().WriteLineFormatted(
					LogLevel::Debug,
					"TunnelPortalTool: first endpoint set - %s at (%u,%u).",
					NetworkTypeName(endpoint.networkType),
					endpoint.x,
					endpoint.z);

				cRZBaseString title;
				title.Sprintf(
					"First %s portal: (%u,%u)",
					NetworkTypeName(endpoint.networkType),
					endpoint.x,
					endpoint.z);

				ShowPrompt(title.Data(), "Select second endpoint on the same network.");
				return true;
			}

			if (endpoint.x == firstEndpoint.x && endpoint.z == firstEndpoint.z)
			{
				Logger::GetInstance().WriteLineFormatted(
					LogLevel::Debug,
					"TunnelPortalTool: second endpoint (%u,%u) is the same tile as the first - rejected.",
					endpoint.x,
					endpoint.z);
				ShowPrompt("Invalid second endpoint", "Select a different network tile.");
				return true;
			}

			if (endpoint.networkType != firstEndpoint.networkType)
			{
				Logger::GetInstance().WriteLineFormatted(
					LogLevel::Debug,
					"TunnelPortalTool: network mismatch - first is %s, second is %s at (%u,%u).",
					NetworkTypeName(firstEndpoint.networkType),
					NetworkTypeName(endpoint.networkType),
					endpoint.x,
					endpoint.z);
				ShowPrompt("Network mismatch", "Select the second endpoint on the same network type.");
				return true;
			}

			const bool placed = PlacePortalPair(firstEndpoint, endpoint);
			ShowPrompt(
				placed ? "Tunnel portals linked" : "Tunnel portal placement failed",
				placed ? "Experimental portal pair committed." : "See NAM.log for details.");

			if (placed)
			{
				EndInput();
			}

			return true;
		}

		void Deactivate() override
		{
			ClearFeedback();
			cSC4BaseViewInputControl::Deactivate();
		}

	private:
		bool PickEndpoint(int32_t screenX, int32_t screenZ, Endpoint& endpoint)
		{
			if (!view3D)
			{
				return false;
			}

			float worldCoords[3] = { 0.0f, 0.0f, 0.0f };
			if (!view3D->PickTerrain(screenX, screenZ, worldCoords, view3D->GetTerrainQueryEnabled()))
			{
				return false;
			}

			cISC4City* city = GetCity();
			if (!city)
			{
				return false;
			}

			const uint32_t maxX = city->CellCountX();
			const uint32_t maxZ = city->CellCountZ();

			if (maxX == 0 || maxZ == 0)
			{
				return false;
			}

			endpoint.x = std::min(static_cast<uint32_t>(std::max(worldCoords[0], 0.0f) / 16.0f), maxX - 1);
			endpoint.z = std::min(static_cast<uint32_t>(std::max(worldCoords[2], 0.0f) / 16.0f), maxZ - 1);

			return TryFindNetworkAtTile(endpoint.x, endpoint.z, endpoint.networkType);
		}

		void ShowPrompt(const char* titleText, const char* detailText)
		{
			if (!view3D)
			{
				return;
			}

			cRZBaseString title(titleText);
			cRZBaseString detail(detailText);
			view3D->SetCursorText(kPrimaryCursorTextID, 0, &detail, &title, 0);

			cRZBaseString hint("Right click or Esc: cancel");
			cRZBaseString mode("NAM tunnel portal tool");
			view3D->SetCursorText(kSecondaryCursorTextID, 0, &hint, &mode, 0);
		}

		void ClearFeedback()
		{
			if (view3D)
			{
				view3D->ClearCursorText(kPrimaryCursorTextID);
				view3D->ClearCursorText(kSecondaryCursorTextID);
			}
		}

		Endpoint firstEndpoint;
		bool hasFirstEndpoint = false;
	};
}

bool TunnelPortalTool::Activate(cISC4View3DWin* view3D)
{
	Logger::GetInstance().WriteLineFormatted(
		LogLevel::Debug,
		"TunnelPortalTool: activation requested, view3D=%p.",
		view3D);

	if (!view3D)
	{
		return false;
	}

	static cRZAutoRefCount<cISC4ViewInputControl> sActiveControl;

	TunnelPortalViewInputControl* control = new TunnelPortalViewInputControl();
	control->AddRef();
	sActiveControl = static_cast<cISC4ViewInputControl*>(control);
	control->Release();

	const bool activated = view3D->SetCurrentViewInputControl(
		control,
		cISC4View3DWin::ViewInputControlStackOperation_RemoveCurrentControl);

	if (!activated)
	{
		Logger::GetInstance().WriteLine(LogLevel::Error, "TunnelPortalTool: failed to set view input control.");
		sActiveControl.Reset();
	}
	else
	{
		Logger::GetInstance().WriteLine(LogLevel::Debug, "TunnelPortalTool: view input control installed.");
	}

	return activated;
}
