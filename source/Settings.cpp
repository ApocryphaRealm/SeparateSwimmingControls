#include "PCH.h"

#include "Settings.h"

#include "utils/Logger.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <vector>

namespace settings
{
	namespace
	{
		std::string g_iniPath;

		std::string Lower(std::string a_text)
		{
			std::ranges::transform(a_text, a_text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return a_text;
		}

		std::string Trim(const std::string& a_text)
		{
			const auto first = a_text.find_first_not_of(" \t\r\n");
			if (first == std::string::npos) { return {}; }
			return a_text.substr(first, a_text.find_last_not_of(" \t\r\n") - first + 1);
		}

		constexpr const char* kKeyNames[kRowCount][kDeviceCount] = {
			{ "iAscendKey", "iAscendPad" },
			{ "iDescendKey", "iDescendPad" },
		};

		std::vector<std::string> ReadLines(const std::string& a_path)
		{
			std::vector<std::string> lines;
			std::ifstream file(a_path);
			for (std::string line; std::getline(file, line);)
			{
				if (!line.empty() && line.back() == '\r') { line.pop_back(); }
				lines.push_back(line);
			}
			return lines;
		}

		// "section:key" (lower case) -> value
		std::map<std::string, std::string> ReadKeys(const std::string& a_path)
		{
			std::map<std::string, std::string> out;
			std::string section;
			for (const auto& raw : ReadLines(a_path))
			{
				const std::string line = Trim(raw);
				if (line.empty() || line[0] == ';' || line[0] == '#') { continue; }
				if (line.front() == '[' && line.back() == ']')
				{
					section = Lower(line.substr(1, line.size() - 2));
					continue;
				}
				const auto eq = line.find('=');
				if (eq == std::string::npos) { continue; }
				out[Lower(Trim(line.substr(0, eq))) + ":" + section] = Trim(line.substr(eq + 1));
			}
			return out;
		}

		void ReadBool(const std::map<std::string, std::string>& a_keys, const char* a_key, std::atomic<bool>& a_out)
		{
			const auto it = a_keys.find(a_key);
			if (it == a_keys.end()) { return; }
			const std::string v = Lower(it->second);
			if (v == "1" || v == "true") { a_out = true; }
			else if (v == "0" || v == "false") { a_out = false; }
			else { logger::warn("INI {}: \"{}\" is not 0 or 1; the default stays", a_key, it->second); }
		}

		void ReadFloat(const std::map<std::string, std::string>& a_keys, const char* a_key, std::atomic<float>& a_out, float a_min, float a_max)
		{
			const auto it = a_keys.find(a_key);
			if (it == a_keys.end()) { return; }
			try
			{
				const float v = std::stof(it->second);
				if (v >= a_min && v <= a_max) { a_out = v; return; }
			}
			catch (...)
			{
			}
			logger::warn("INI {}: \"{}\" is not a number from {} to {}; the default stays", a_key, it->second, a_min, a_max);
		}

		void ReadUInt(const std::map<std::string, std::string>& a_keys, const std::string& a_key, std::atomic<std::uint32_t>& a_out)
		{
			const auto it = a_keys.find(a_key);
			if (it == a_keys.end()) { return; }
			try { a_out = static_cast<std::uint32_t>(std::stoul(it->second, nullptr, 0)); }
			catch (...) { logger::warn("INI {}: \"{}\" is not a number; the default stays", a_key, it->second); }
		}

		// One key's value, in the file's own line; the section and the line are added when the file has neither.
		void WriteKey(std::vector<std::string>& a_lines, const std::string& a_section, const std::string& a_key, const std::string& a_value)
		{
			const std::string wantSection = "[" + Lower(a_section) + "]";
			const std::string wantKey = Lower(a_key);
			std::size_t sectionEnd = a_lines.size();
			bool inSection = false, haveSection = false;
			for (std::size_t i = 0; i < a_lines.size(); ++i)
			{
				const std::string line = Trim(a_lines[i]);
				if (!line.empty() && line.front() == '[')
				{
					if (inSection) { sectionEnd = i; break; }
					inSection = Lower(line) == wantSection;
					haveSection = haveSection || inSection;
					continue;
				}
				if (!inSection || line.empty() || line[0] == ';') { continue; }
				const auto eq = line.find('=');
				if (eq != std::string::npos && Lower(Trim(line.substr(0, eq))) == wantKey)
				{
					a_lines[i] = a_key + "=" + a_value;
					return;
				}
			}
			if (!haveSection)
			{
				a_lines.push_back("[" + a_section + "]");
				a_lines.push_back(a_key + "=" + a_value);
				return;
			}
			// before the blank lines that end the section
			while (sectionEnd > 0 && Trim(a_lines[sectionEnd - 1]).empty()) { --sectionEnd; }
			a_lines.insert(a_lines.begin() + static_cast<std::ptrdiff_t>(sectionEnd), a_key + "=" + a_value);
		}
	}

	const char* RowName(int a_row)
	{
		switch (a_row)
		{
		case kAscend: return "Ascend";
		case kDescend: return "Descend";
		default: return "?";
		}
	}

	std::string KeyName(std::uint32_t a_key, int a_device)
	{
		if (a_key == 0) { return {}; }
		if (a_device == kGamepad)
		{
			switch (a_key)
			{
			case 0x0001: return "D-pad Up";
			case 0x0002: return "D-pad Down";
			case 0x0004: return "D-pad Left";
			case 0x0008: return "D-pad Right";
			case 0x0010: return "Start";
			case 0x0020: return "Back";
			case 0x0040: return "L3";
			case 0x0080: return "R3";
			case 0x0100: return "LB";
			case 0x0200: return "RB";
			case 0x0009: return "LT";
			case 0x000A: return "RT";
			case 0x1000: return "A";
			case 0x2000: return "B";
			case 0x4000: return "X";
			case 0x8000: return "Y";
			default: return std::format("0x{:04X}", a_key);
			}
		}
		// The DirectInput scan codes in order, 0x01 to 0x39, then the ones past them a player is likely to use.
		static constexpr const char* kLow[] = { "Esc", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "=", "Backspace", "Tab",
			"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "[", "]", "Enter", "Left Ctrl",
			"A", "S", "D", "F", "G", "H", "J", "K", "L", ";", "'", "`", "Left Shift", "\\",
			"Z", "X", "C", "V", "B", "N", "M", ",", ".", "/", "Right Shift", "Num *", "Left Alt", "Space" };
		if (a_key >= 1 && a_key <= std::size(kLow)) { return kLow[a_key - 1]; }
		switch (a_key)
		{
		case 0x3A: return "Caps Lock";
		case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F: case 0x40: case 0x41: case 0x42: case 0x43: case 0x44:
			return std::format("F{}", a_key - 0x3A);
		case 0x57: return "F11";
		case 0x58: return "F12";
		case 0x9D: return "Right Ctrl";
		case 0xB8: return "Right Alt";
		case 0xC7: return "Home";
		case 0xC8: return "Up";
		case 0xC9: return "Page Up";
		case 0xCB: return "Left";
		case 0xCD: return "Right";
		case 0xCF: return "End";
		case 0xD0: return "Down";
		case 0xD1: return "Page Down";
		case 0xD2: return "Insert";
		case 0xD3: return "Delete";
		default: return std::format("0x{:02X}", a_key);
		}
	}

	void RestoreDefaults()
	{
		logLevel = Defaults::logLevel;
		enabled = Defaults::enabled;
		swimVertical = Defaults::swimVertical;
		swimVerticalSpeed = Defaults::swimVerticalSpeed;
		diveAnimation = Defaults::diveAnimation;
		for (int r = 0; r < kRowCount; ++r)
		{
			for (int d = 0; d < kDeviceCount; ++d) { keys[r][d] = Defaults::keys[r][d]; }
		}
	}

	void Load()
	{
		g_iniPath = (std::filesystem::current_path() / "Data" / "SKSE" / "Plugins" / "SeparateSwimmingControls.ini").string();
		RestoreDefaults();
		if (!std::filesystem::exists(g_iniPath))
		{
			logger::warn("{} is not there; the compiled defaults are in force", g_iniPath);
			return;
		}
		const auto k = ReadKeys(g_iniPath);  // only the keys asked for below are read: a line from an older version (iDiveKey, bDive) is ignored and left as it is
		ReadUInt(k, "uloglevel:debug", logLevel);
		ReadBool(k, "benabled:general", enabled);
		ReadBool(k, "bswimvertical:general", swimVertical);
		ReadFloat(k, "fswimverticalspeed:general", swimVerticalSpeed, 10.0f, 1000.0f);
		ReadBool(k, "bdiveanimation:dive", diveAnimation);
		for (int r = 0; r < kRowCount; ++r)
		{
			for (int d = 0; d < kDeviceCount; ++d) { ReadUInt(k, Lower(kKeyNames[r][d]) + ":keys", keys[r][d]); }
		}
		logger::info("settings: {} read", g_iniPath);
	}

	bool Save()
	{
		if (g_iniPath.empty()) { return false; }
		auto lines = ReadLines(g_iniPath);
		const auto real = [](float a_v) { return std::format("{:g}", a_v); };
		WriteKey(lines, "Debug", "uLogLevel", std::to_string(logLevel.load()));
		WriteKey(lines, "General", "bEnabled", enabled ? "1" : "0");
		WriteKey(lines, "General", "bSwimVertical", swimVertical ? "1" : "0");
		WriteKey(lines, "General", "fSwimVerticalSpeed", real(swimVerticalSpeed));
		for (int r = 0; r < kRowCount; ++r)
		{
			WriteKey(lines, "Keys", kKeyNames[r][kKeyboard], std::format("0x{:02X}", keys[r][kKeyboard].load()));
			WriteKey(lines, "Keys", kKeyNames[r][kGamepad], std::format("0x{:04X}", keys[r][kGamepad].load()));
		}
		WriteKey(lines, "Dive", "bDiveAnimation", diveAnimation ? "1" : "0");
		std::ofstream file(g_iniPath, std::ios::trunc);
		if (!file)
		{
			logger::warn("settings: {} could not be written", g_iniPath);
			return false;
		}
		for (const auto& line : lines) { file << line << '\n'; }
		logger::debug("settings: {} written", g_iniPath);
		return static_cast<bool>(file);
	}

	const std::string& IniPath() { return g_iniPath; }

	void ApplyLogLevel()
	{
		const auto level = logLevel.load();
		SKSE::log::set_level(static_cast<spdlog::level::level_enum>(level <= 6 ? level : 2), spdlog::level::info);
	}
}
