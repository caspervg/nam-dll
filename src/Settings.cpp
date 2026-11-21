#include "Settings.h"
#include "Logger.h"
#include "mini/ini.h"
#include <algorithm>
#include <cstdio>

#include <algorithm>
#include <cctype>
#include <string>

namespace
{
	std::string Trim(std::string value)
	{
		auto isSpace = [](const unsigned char ch)
		{
			return std::isspace(ch) != 0;
		};

		value.erase(
			value.begin(),
			std::find_if_not(value.begin(), value.end(), isSpace));
		value.erase(
			std::find_if_not(value.rbegin(), value.rend(), isSpace).base(),
			value.end());
		return value;
	}

	std::string ToUpper(std::string value)
	{
		std::transform(
			value.begin(),
			value.end(),
			value.begin(),
			[](const unsigned char ch)
			{
				return static_cast<char>(std::toupper(ch));
			});
		return value;
	}

	void ReadNeighborConnectionNetworks(
		const std::string& networkList,
		bool& enableRHW,
		bool& enableOWR,
		bool& enableNWM)
	{
		enableRHW = false;
		enableOWR = false;
		enableNWM = false;

		size_t tokenStart = 0;
		while (tokenStart <= networkList.size())
		{
			const size_t tokenEnd = networkList.find(',', tokenStart);
			const std::string token = ToUpper(Trim(networkList.substr(
				tokenStart,
				tokenEnd == std::string::npos ? std::string::npos : tokenEnd - tokenStart)));

			if (token == "RHW")
			{
				enableRHW = true;
			}
			else if (token == "OWR")
			{
				enableOWR = true;
			}
			else if (token == "NWM")
			{
				enableNWM = true;
			}

			if (tokenEnd == std::string::npos)
			{
				break;
			}
			tokenStart = tokenEnd + 1;
		}
	}
}

Settings::Settings() :
	enableDiagonalStreets(true),
	disableAutoconnect(true),
	enableTunnels(true),
	reduceFerryBridgeHeightPatch(true),
	enableRUL2EnginePatch(true),
	enableRUL2FastLoading(true),
	verifyRUL2FastLoading(false),
	enableNetworkSlopePatch(true),
	enableFlexPuzzlePiecePatch(true),
	enableCommuteLoopPatch(true),
	enableDirtRoadAccessPatch(false),
	enableTransitAccessPatch(true),
	enableNeighborConnectionPatch(false),
	enableRHWNeighborConnections(false),
	enableOWRNeighborConnections(false),
	enableNWMNeighborConnections(false),
	neighborConnectionMaxSearchDistance(8),
	neighborConnectionMaxGroupingGap(2),
	enableVerticalPathSeparationPatch(true),
	enableDualNetworkTransitPatch(false),
	enableTransportMenuPatch(true),
	enableGhostPreviewPatch(true),
	ghostPreviewOverlay(true),
	ghostPreviewGrading(true),
	ghostPreviewWireframe(true),
	ghostPreviewColor{140, 204, 255, 191},
	ghostPreviewCutColor{239, 28, 36, 102},  // NAM hole digger red
	ghostPreviewFillColor{63, 73, 206, 102},  // NAM raiser blue
	enableDirtRoadRegionalTransportMapPatch(true),
	enableUndergroundPlaceholderPatch(true),
	enableBridgeShoreAbutmentPatch(true),
	enableBridgeFarAbutmentClearancePatch(true),
	enableParallelBridgesPatch(false),
	enableBridgeDebugLog(false),
	enableKeyboardShortcuts(true) {};

void Settings::Load(std::filesystem::path settingsFilePath)
{
	Logger& logger = Logger::GetInstance();
	try {
		mINI::INIFile file(settingsFilePath);
		mINI::INIStructure ini;
		if (file.read(ini)) {
			auto readBoolProp = [&ini](const std::string propName, bool &propValue) {
				propValue ^= ini.get("NAM").get(propName) == (propValue ? "false" : "true");  // toggle if opposite of default
			};
			auto readColorProp = [&ini](const std::string propName, uint8_t (&propValue)[4]) {
				int rgba[4];
				if (std::sscanf(ini.get("NAM").get(propName).c_str(), "%d,%d,%d,%d", &rgba[0], &rgba[1], &rgba[2], &rgba[3]) == 4) {
					for (int i = 0; i < 4; i++) {
						propValue[i] = static_cast<uint8_t>(std::clamp(rgba[i], 0, 255));
					}
				}
			};
			readBoolProp("EnableKeyboardShortcuts", enableKeyboardShortcuts);
			readBoolProp("EnableDiagonalStreets", enableDiagonalStreets);
			readBoolProp("DisableAutoconnect", disableAutoconnect);
			readBoolProp("EnableTunnels", enableTunnels);
			readBoolProp("ReduceFerryBridgeHeight", reduceFerryBridgeHeightPatch);
			readBoolProp("EnableRUL2EnginePatch", enableRUL2EnginePatch);
			readBoolProp("EnableRUL2FastLoading", enableRUL2FastLoading);
			readBoolProp("VerifyRUL2FastLoading", verifyRUL2FastLoading);
			readBoolProp("EnableNetworkSlopePatch", enableNetworkSlopePatch);
			readBoolProp("EnableFlexPuzzlePiecePatch", enableFlexPuzzlePiecePatch);
			readBoolProp("EnableCommuteLoopPatch", enableCommuteLoopPatch);
			readBoolProp("EnableDirtRoadAccessPatch", enableDirtRoadAccessPatch);
			readBoolProp("EnableTransitAccessPatch", enableTransitAccessPatch);
			readBoolProp("EnableNeighborConnectionPatch", enableNeighborConnectionPatch);

			ReadNeighborConnectionNetworks(
				ini.get("NAM").get("NeighborConnectionNetworks"),
				enableRHWNeighborConnections,
				enableOWRNeighborConnections,
				enableNWMNeighborConnections);

			// A bad number must not throw out of Load: that would skip every setting read after it.
			auto readUIntProp = [&ini, &logger](const std::string propName, uint32_t &propValue) {
				const std::string value = ini.get("NAM").get(propName);
				if (value.empty()) {
					return;
				}
				try {
					propValue = std::stoul(value);
				} catch (const std::exception&) {
					logger.WriteLineFormatted(LogLevel::Error, "Ignoring invalid %s value: %s", propName.c_str(), value.c_str());
				}
			};
			readUIntProp("NeighborConnectionMaxSearchDistance", neighborConnectionMaxSearchDistance);
			readUIntProp("NeighborConnectionMaxGroupingGap", neighborConnectionMaxGroupingGap);
			readBoolProp("EnableVerticalPathSeparationPatch", enableVerticalPathSeparationPatch);
			readBoolProp("EnableDualNetworkTransitPatch", enableDualNetworkTransitPatch);
			readBoolProp("EnableTransportMenuPatch", enableTransportMenuPatch);
			readBoolProp("EnableGhostPreviewPatch", enableGhostPreviewPatch);
			if (const std::string mode = ini.get("NAM").get("GhostPreviewMode"); mode == "overlay" || mode == "grading" || mode == "both") {
				ghostPreviewOverlay = mode != "grading";
				ghostPreviewGrading = mode != "overlay";
			}
			if (const std::string style = ini.get("NAM").get("GhostPreviewGradingStyle"); style == "wireframe" || style == "terrain") {
				ghostPreviewWireframe = style == "wireframe";
			}
			readColorProp("GhostPreviewColor", ghostPreviewColor);
			readColorProp("GhostPreviewCutColor", ghostPreviewCutColor);
			readColorProp("GhostPreviewFillColor", ghostPreviewFillColor);
			readBoolProp("EnableDirtRoadRegionalTransportMapPatch", enableDirtRoadRegionalTransportMapPatch);
			readBoolProp("EnableUndergroundPlaceholderPatch", enableUndergroundPlaceholderPatch);
			readBoolProp("EnableBridgeShoreAbutmentPatch", enableBridgeShoreAbutmentPatch);
			readBoolProp("EnableBridgeFarAbutmentClearancePatch", enableBridgeFarAbutmentClearancePatch);
			readBoolProp("EnableParallelBridgesPatch", enableParallelBridgesPatch);
			readBoolProp("EnableBridgeDebugLog", enableBridgeDebugLog);
		} else {
			logger.WriteLine(LogLevel::Info, "Using default settings, as no NAM.ini configuration file was detected.");
		}
	} catch (const std::exception &e) {
		logger.WriteLineFormatted(LogLevel::Error, "Error reading the settings file: %s", e.what());
	}
}
