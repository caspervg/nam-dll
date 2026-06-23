#include "Settings.h"
#include "Logger.h"
#include "mini/ini.h"

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

		if (enableOWR)
		{
			enableRHW = true;
		}
	}
}

Settings::Settings() :
	enableDiagonalStreets(true),
	disableAutoconnect(true),
	enableTunnels(true),
	reduceFerryBridgeHeightPatch(true),
	enableRUL2EnginePatch(true),
	enableNetworkSlopePatch(true),
	enableFlexPuzzlePiecePatch(true),
	enableCommuteLoopPatch(true),
	enableDirtRoadAccessPatch(true),
	enableTransitAccessPatch(true),
	enableNeighborConnectionPatch(false),
	enableRHWNeighborConnections(false),
	enableOWRNeighborConnections(false),
	enableNWMNeighborConnections(false),
	neighborConnectionMaxSearchDistance(8),
	neighborConnectionMaxGroupingGap(2),
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
			readBoolProp("EnableTransitAccessPatch", enableTransitAccessPatch);
			readBoolProp("EnableNeighborConnectionPatch", enableNeighborConnectionPatch);

			ReadNeighborConnectionNetworks(
				ini.get("NAM").get("NeighborConnectionNetworks"),
				enableRHWNeighborConnections,
				enableOWRNeighborConnections,
				enableNWMNeighborConnections);

			const std::string maxSearchDistance =
				ini.get("NAM").get("NeighborConnectionMaxSearchDistance");
			if (!maxSearchDistance.empty())
			{
				neighborConnectionMaxSearchDistance = std::stoul(maxSearchDistance);
			}

			const std::string maxGroupingGap =
				ini.get("NAM").get("NeighborConnectionMaxGroupingGap");
			if (!maxGroupingGap.empty())
			{
				neighborConnectionMaxGroupingGap = std::stoul(maxGroupingGap);
			}
		} else {
			logger.WriteLine(LogLevel::Info, "Using default settings, as no NAM.ini configuration file was detected.");
		}
	} catch (const std::exception &e) {
		logger.WriteLineFormatted(LogLevel::Error, "Error reading the settings file: %s", e.what());
	}
}
