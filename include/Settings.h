#pragma once

#include <atomic>
#include <cstdint>
#include <string>

// The mod's settings: read from SKSE\Plugins\SeparateSwimmingControls.ini, written back line by line (never with
// WritePrivateProfileString, which reports success and writes nothing under MO2's virtual folder). The page and the input
// run on other threads than the movement hook, so every value is atomic. Compiled defaults match the shipped INI (rule 16).
namespace settings
{
	enum Row : int
	{
		kAscend = 0,
		kDescend = 1,
		kDive = 2,
		kRowCount = 3
	};
	enum Device : int
	{
		kKeyboard = 0,
		kGamepad = 1,
		kDeviceCount = 2
	};

	// A key is the code the game's own ButtonEvent reports: a DirectInput scan code, or the gamepad code below. 0 = no key.
	inline constexpr std::uint32_t kPadLeftTrigger = 0x0009;
	inline constexpr std::uint32_t kPadRightTrigger = 0x000A;
	inline constexpr std::uint32_t kPadX = 0x4000;

	struct Defaults
	{
		static constexpr std::uint32_t logLevel = 2;
		static constexpr bool enabled = true;
		static constexpr bool swimVertical = true;
		static constexpr float swimVerticalSpeed = 150.0f;
		static constexpr bool dive = true;
		static constexpr bool diveAnimation = true;
		static constexpr float diveSinkSpeed = 300.0f;
		static constexpr float diveSinkSeconds = 0.8f;
		static constexpr std::uint32_t keys[kRowCount][kDeviceCount] = {
			{ 0x14, kPadRightTrigger },  // Ascend: T, right trigger
			{ 0x1D, kPadLeftTrigger },   // Descend: Left Ctrl, left trigger
			{ 0x15, kPadX },             // Dive: Y, X button (Square)
		};
	};

	inline std::atomic<std::uint32_t> logLevel{ Defaults::logLevel };
	inline std::atomic<bool> enabled{ Defaults::enabled };                     // bEnabled:General - the whole mod
	inline std::atomic<bool> swimVertical{ Defaults::swimVertical };           // bSwimVertical:General - Ascend, Descend; Sneak does not sneak in water
	inline std::atomic<float> swimVerticalSpeed{ Defaults::swimVerticalSpeed };// fSwimVerticalSpeed:General - game units a second, 10 to 1000
	inline std::atomic<bool> dive{ Defaults::dive };                           // bDive:Dive - the Dive key
	inline std::atomic<bool> diveAnimation{ Defaults::diveAnimation };         // bDiveAnimation:Dive - play Simple Diving System's body turn, when it is there
	inline std::atomic<float> diveSinkSpeed{ Defaults::diveSinkSpeed };        // fDiveSinkSpeed:Dive - how fast a dive takes you under, 10 to 1000
	inline std::atomic<float> diveSinkSeconds{ Defaults::diveSinkSeconds };    // fDiveSinkSeconds:Dive - for how long, 0.1 to 5.0
	inline std::atomic<std::uint32_t> keys[kRowCount][kDeviceCount]{};         // [Keys] iAscendKey, iAscendPad ...

	const char* RowName(int a_row);  // the INI's and the log's name: Ascend, Descend, Dive

	// A key's name for the page ("Left Ctrl", "RT"), or its hex code when it has none; empty for no key.
	std::string KeyName(std::uint32_t a_key, int a_device);

	void Load();             // at plugin load; a missing INI or line keeps the default
	bool Save();             // every value, keeping the file's comments; false when the file cannot be written
	void RestoreDefaults();  // in memory, as the page's button does; the caller saves
	const std::string& IniPath();
	void ApplyLogLevel();
}
