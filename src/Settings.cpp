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
	ghostPreviewGrading(true),
	ghostPreviewWireframe(true),
	ghostPreviewColor{140, 204, 255, 191},
	ghostPreviewCutColor{239, 28, 36, 102},  // NAM hole digger red
	ghostPreviewFillColor{63, 73, 206, 102},  // NAM raiser blue
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
			readBoolProp("EnableNetworkSlopePatch", enableNetworkSlopePatch);
			readBoolProp("EnableFlexPuzzlePiecePatch", enableFlexPuzzlePiecePatch);
			readBoolProp("EnableCommuteLoopPatch", enableCommuteLoopPatch);
			readBoolProp("EnableDirtRoadAccessPatch", enableDirtRoadAccessPatch);
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
		} else {
			logger.WriteLine(LogLevel::Info, "Using default settings, as no NAM.ini configuration file was detected.");
		}
	} catch (const std::exception &e) {
		logger.WriteLineFormatted(LogLevel::Error, "Error reading the settings file: %s", e.what());
	}
}
