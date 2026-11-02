#include "Settings.h"
#include "Logger.h"
#include "mini/ini.h"
#include <algorithm>
#include <cstdio>

Settings::Settings() :
	enableDiagonalStreets(true),
	disableAutoconnect(true),
	enableTunnels(true),
	reduceFerryBridgeHeightPatch(true),
	enableRUL2EnginePatch(true),
	enableNetworkSlopePatch(true),
	enableFlexPuzzlePiecePatch(true),
	enableCommuteLoopPatch(true),
	enableDirtRoadAccessPatch(false),
	enableVerticalPathSeparationPatch(true),
	enableDualNetworkTransitPatch(false),
	enableTransportMenuPatch(true),
	enableGhostPreviewPatch(true),
	ghostPreviewOverlay(true),
	ghostPreviewGrading(false),
	ghostPreviewColor{140, 204, 255},
	ghostPreviewAlphaScale(0.75f),
	ghostPreviewDepthTolerance(1.0f),
	ghostPreviewGradingOpacity(0.4f),
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
			auto readFloatProp = [&ini](const std::string propName, float &propValue) {
				const std::string& value = ini.get("NAM").get(propName);
				if (!value.empty()) {
					propValue = std::stof(value);
				}
			};
			auto readColorProp = [&ini](const std::string propName, int (&propValue)[3]) {
				int r, g, b;
				if (std::sscanf(ini.get("NAM").get(propName).c_str(), "%d,%d,%d", &r, &g, &b) == 3) {
					propValue[0] = std::clamp(r, 0, 255);
					propValue[1] = std::clamp(g, 0, 255);
					propValue[2] = std::clamp(b, 0, 255);
				}
			};
			readBoolProp("EnableKeyboardShortcuts", enableKeyboardShortcuts);
			readBoolProp("EnableDiagonalStreets", enableDiagonalStreets);
			readBoolProp("DisableAutoconnect", disableAutoconnect);
			readBoolProp("EnableTunnels", enableTunnels);
			readBoolProp("ReduceFerryBridgeHeight", reduceFerryBridgeHeightPatch);
			readBoolProp("EnableRUL2EnginePatch", enableRUL2EnginePatch);
			readBoolProp("EnableNetworkSlopePatch", enableNetworkSlopePatch);
			readBoolProp("EnableFlexPuzzlePiecePatch", enableFlexPuzzlePiecePatch);
			readBoolProp("EnableCommuteLoopPatch", enableCommuteLoopPatch);
			readBoolProp("EnableDirtRoadAccessPatch", enableDirtRoadAccessPatch);
			readBoolProp("EnableVerticalPathSeparationPatch", enableVerticalPathSeparationPatch);
			readBoolProp("EnableDualNetworkTransitPatch", enableDualNetworkTransitPatch);
			readBoolProp("EnableTransportMenuPatch", enableTransportMenuPatch);
			readBoolProp("EnableGhostPreviewPatch", enableGhostPreviewPatch);
			if (const std::string& mode = ini.get("NAM").get("GhostPreviewMode"); !mode.empty()) {
				ghostPreviewOverlay = mode == "overlay" || mode == "both";
				ghostPreviewGrading = mode == "grading" || mode == "both";
			}
			readColorProp("GhostPreviewColor", ghostPreviewColor);
			readFloatProp("GhostPreviewAlphaScale", ghostPreviewAlphaScale);
			readFloatProp("GhostPreviewDepthTolerance", ghostPreviewDepthTolerance);
			readFloatProp("GhostPreviewGradingOpacity", ghostPreviewGradingOpacity);
		} else {
			logger.WriteLine(LogLevel::Info, "Using default settings, as no NAM.ini configuration file was detected.");
		}
	} catch (const std::exception &e) {
		logger.WriteLineFormatted(LogLevel::Error, "Error reading the settings file: %s", e.what());
	}
}
