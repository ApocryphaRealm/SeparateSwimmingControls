#include "PCH.h"

#include "SKSEMenuFramework.h"
#include "utils/PreciseSlider.h"
#include "utils/Toggle.h"

#include "Page.h"

#include "Settings.h"
#include "Strings.h"
#include "Swim.h"
#include "utils/Logger.h"

#include <atomic>
#include <chrono>
#include <format>
#include <mutex>
#include <vector>

namespace page
{
	namespace
	{
		constexpr int kNoCapture = -1;
		std::atomic<int> g_capture{ kNoCapture };        // row * 2 + device, while a Bind button waits for a press
		std::atomic<std::int64_t> g_lastDrawn{ 0 };      // the page's last frame, so a stale arm never eats a game press
		std::mutex g_statusLock;
		std::string g_status;
		std::atomic<bool> g_saveWanted{ false };

		std::int64_t NowMs()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		void SetStatus(std::string a_text)
		{
			std::scoped_lock l(g_statusLock);
			g_status = std::move(a_text);
		}

		std::string Status()
		{
			std::scoped_lock l(g_statusLock);
			return g_status;
		}

		// The keys AMF reserves right now, asked at each capture so the answer follows its live configuration (DEFAULT-KEYS.md).
		bool FrameworkReserves(std::int32_t a_code)
		{
			using func_t = std::uint32_t (*)(std::int32_t*, std::uint32_t);
			static const auto func = GetMenuFrameworkFunction<func_t>("SMF_GetReservedKeyCodes");
			if (!func) { return false; }
			const std::uint32_t needed = func(nullptr, 0);
			if (needed == 0) { return false; }
			std::vector<std::int32_t> codes(needed);
			const std::uint32_t written = func(codes.data(), needed);
			for (std::uint32_t i = 0; i < written && i < needed; ++i)
			{
				if (codes[i] == a_code) { return true; }
			}
			return false;
		}

		std::string RowLabel(int a_row)
		{
			switch (a_row)
			{
			case settings::kAscend: return TR("SWIM_RowAscend", "Ascend");
			case settings::kDescend: return TR("SWIM_RowDescend", "Descend");
			default: return TR("SWIM_RowDive", "Dive");
			}
		}

		// The framework's input hook, on its input thread: the one press a Bind button is waiting for.
		bool __stdcall OnInput(RE::InputEvent* a_event)
		{
			const int capture = g_capture.load();
			if (capture == kNoCapture || !a_event) { return false; }
			if (NowMs() - g_lastDrawn.load() > 1000)
			{
				g_capture = kNoCapture;  // the page is closed; this arm is left over
				return false;
			}
			const auto* button = a_event->AsButtonEvent();
			if (!button || !button->IsDown()) { return false; }
			const int row = capture / 2;
			const int device = capture % 2;
			const auto wanted = device == settings::kKeyboard ? RE::INPUT_DEVICE::kKeyboard : RE::INPUT_DEVICE::kGamepad;
			if (button->GetDevice() != wanted) { return false; }
			const auto code = button->GetIDCode();
			if (code == 0) { return false; }
			if (device == settings::kKeyboard && FrameworkReserves(static_cast<std::int32_t>(code)))
			{
				SetStatus(TR("SWIM_StatusReserved", "The menu framework reserves that key. Press another."));
				return false;
			}
			for (int r = 0; r < settings::kRowCount; ++r)
			{
				if (r != row && settings::keys[r][device].load() == code)
				{
					SetStatus(std::format("{} {}", TR("SWIM_StatusTaken", "That key is already"), RowLabel(r)));
					return false;
				}
			}
			settings::keys[row][device] = code;
			g_capture = kNoCapture;
			g_saveWanted = true;
			SetStatus(TR("SWIM_StatusSaved", "Saved."));
			logger::info("page: {} {} bound to 0x{:X}", settings::RowName(row), device == settings::kKeyboard ? "keyboard" : "gamepad", code);
			return true;  // so binding a key does not also do whatever that key does
		}

		bool HasRequiredExports()
		{
			// Every export this page, Toggle.h and PreciseSlider.h resolve; the header does not null-check.
			constexpr const char* required[] = {
				"AddSectionItem", "igTextV", "igTextDisabledV", "igTextWrappedV", "igSetTooltipV", "igSeparatorText", "igSliderInt", "igSliderFloat",
				"igIsItemHovered", "igButton", "igSameLine", "igSpacing", "igPushItemWidth", "igPopItemWidth", "igIsAnyItemActive", "igIsKeyDown_Nil",
				"igIsMouseDown_Nil", "igGetCursorScreenPos", "igGetWindowDrawList", "igGetFrameHeight", "igInvisibleButton", "igPushID_Str", "igPopID",
				"ImDrawList_AddRectFilled", "ImDrawList_AddCircleFilled"
			};
			for (const char* name : required)
			{
				if (!GetMenuFrameworkFunction<void*>(name))
				{
					logger::warn("the menu framework does not export \"{}\"; the settings page is not registered (the INI still works)", name);
					return false;
				}
			}
			return true;
		}

		void Help(const std::string& a_text)
		{
			ImGuiMCP::SameLine();
			ImGuiMCP::TextDisabled("%s", TR("SWIM_HelpMark", "(?)").c_str());
			if (ImGuiMCP::IsItemHovered()) { ImGuiMCP::SetTooltip("%s", a_text.c_str()); }
		}

		bool ToggleRow(const std::string& a_label, std::atomic<bool>& a_value, const std::string& a_help)
		{
			bool v = a_value.load();
			const bool changed = ImGuiMCP::Toggle(a_label.c_str(), &v);
			if (changed) { a_value = v; }
			Help(a_help);
			return changed;
		}

		bool SliderRow(const std::string& a_label, std::atomic<float>& a_value, float a_min, float a_max, const char* a_format, const std::string& a_help)
		{
			float v = a_value.load();
			const bool changed = precise::SliderFloat(a_label.c_str(), &v, a_min, a_max, a_format);
			if (changed) { a_value = v; }
			Help(a_help);
			return changed;
		}

		// One device's key: its name, a Bind button, a Clear button.
		bool KeyCell(int a_row, int a_device)
		{
			using namespace ImGuiMCP;
			bool changed = false;
			const int id = a_row * 2 + a_device;
			const auto key = settings::keys[a_row][a_device].load();
			const std::string name = key ? settings::KeyName(key, a_device) : TR("SWIM_KeyNone", "(none)");
			Text("%s", (a_device == settings::kKeyboard ? TR("SWIM_DeviceKeyboard", "Keyboard") : TR("SWIM_DeviceGamepad", "Controller")).c_str());
			SameLine();
			Text("%s", name.c_str());
			SameLine();
			if (g_capture.load() == id)
			{
				if (Button(std::format("{}##cancel{}", TR("SWIM_PressKey", "Press a key... (cancel)"), id).c_str()))
				{
					g_capture = kNoCapture;
					SetStatus({});
				}
			}
			else if (Button(std::format("{}##bind{}", TR("SWIM_Bind", "Bind"), id).c_str()))
			{
				g_capture = id;
				SetStatus({});
			}
			SameLine();
			if (Button(std::format("{}##clear{}", TR("SWIM_Clear", "Clear"), id).c_str()))
			{
				settings::keys[a_row][a_device] = 0;
				changed = true;
			}
			return changed;
		}

		void __stdcall Render()
		{
			using namespace ImGuiMCP;
			g_lastDrawn = NowMs();
			bool changed = false;

			TextWrapped("%s", TR("SWIM_PageIntro", "Changes apply at once and are saved to the mod's INI.").c_str());
			Spacing();

			changed |= ToggleRow(TR("SWIM_Enabled", "Enabled"), settings::enabled, TR("SWIM_HelpEnabled", "Off: nothing in this mod does anything."));

			SeparatorText(TR("SWIM_SecSwim", "Swimming").c_str());
			changed |= ToggleRow(TR("SWIM_Vertical", "Ascend and Descend keys"), settings::swimVertical,
				TR("SWIM_HelpVertical", "Hold to rise or sink at any camera angle. Sneak does not sneak in water, and you never climb out sneaking. Rising stops at the surface; only Jump leaves the water. Off: Sneak does what it does in the game."));
			PushItemWidth(260.0f);
			changed |= SliderRow(TR("SWIM_Speed", "Speed"), settings::swimVerticalSpeed, 10.0f, 1000.0f, "%.0f",
				TR("SWIM_HelpSpeed", "Game units a second; about 70 is one metre. Added to what your gear and other swimming mods are doing."));
			PopItemWidth();

			SeparatorText(TR("SWIM_SecKeys", "Keys").c_str());
			TextWrapped("%s", TR("SWIM_KeysNote", "In water, a key here does nothing else. On land Ascend and Descend do nothing and their keys keep their game use.").c_str());
			for (int r = 0; r < settings::kRowCount; ++r)
			{
				Spacing();
				Text("%s", RowLabel(r).c_str());
				changed |= KeyCell(r, settings::kKeyboard);
				changed |= KeyCell(r, settings::kGamepad);
			}

			SeparatorText(TR("SWIM_SecDive", "Dive").c_str());
			changed |= ToggleRow(TR("SWIM_Dive", "Dive key"), settings::dive,
				TR("SWIM_HelpDive", "With deep water just ahead, a leap into it and a push under. At the surface, the push under. Anywhere else the key keeps its game use."));
			changed |= ToggleRow(TR("SWIM_DiveAnim", "Use Simple Diving System's dive pose"), settings::diveAnimation,
				TR("SWIM_HelpDiveAnim", "A separate mod. When it is installed its pose plays in the air; without it the leap plays the game's own fall."));
			const auto sds = swim::DiveAnimationStatus();
			TextDisabled("%s", (sds.Ready() ? TR("SWIM_SdsReady", "Simple Diving System: ready.") :
			                    sds.dll     ? TR("SWIM_SdsPartial", "Simple Diving System: installed, but Open Animation Replacer or its behaviour patch is missing, or no game is loaded yet.") :
			                                  TR("SWIM_SdsAbsent", "Simple Diving System: not installed. The dive still works.")).c_str());
			PushItemWidth(260.0f);
			changed |= SliderRow(TR("SWIM_DiveForward", "Leap speed"), settings::diveForward, 0.0f, 1000.0f, "%.0f", TR("SWIM_HelpDiveForward", "How fast the leap carries you forward."));
			changed |= SliderRow(TR("SWIM_DiveUp", "Leap lift"), settings::diveUp, 0.0f, 800.0f, "%.0f", TR("SWIM_HelpDiveUp", "How much the leap lifts you. 0 steps straight off the edge."));
			changed |= SliderRow(TR("SWIM_DiveSink", "Push under: speed"), settings::diveSinkSpeed, 10.0f, 1000.0f, "%.0f", TR("SWIM_HelpDiveSink", "How fast the push takes you under."));
			changed |= SliderRow(TR("SWIM_DiveSinkTime", "Push under: seconds"), settings::diveSinkSeconds, 0.1f, 5.0f, "%.1f", TR("SWIM_HelpDiveSinkTime", "How long the push lasts."));
			PopItemWidth();

			Spacing();
			if (Button(TR("SWIM_Restore", "Restore defaults").c_str()))
			{
				settings::RestoreDefaults();
				changed = true;
				SetStatus(TR("SWIM_StatusRestored", "Defaults restored and saved."));
			}
			else if (changed)
			{
				SetStatus(TR("SWIM_StatusSaved", "Saved."));
			}

			if (changed) { g_saveWanted = true; }
			// One write per change: wait until no control is held (a slider being dragged changes every frame).
			if (g_saveWanted.load() && !IsAnyItemActive())
			{
				g_saveWanted = false;
				if (auto* tasks = SKSE::GetTaskInterface()) { tasks->AddTask([] { settings::Save(); }); }
			}
			if (const auto status = Status(); !status.empty()) { TextDisabled("%s", status.c_str()); }
		}
	}

	void Register()
	{
		if (!SKSEMenuFramework::IsInstalled() || !GetMenuFrameworkModule())
		{
			logger::info("no menu framework is loaded; settings are read from the INI only");
			return;
		}
		if (!HasRequiredExports()) { return; }
		SKSEMenuFramework::SetSection("Swim Controls");
		SKSEMenuFramework::AddSectionItem("Settings", Render);
		if (GetMenuFrameworkFunction<void*>("RegisterInpoutEvent")) { SKSEMenuFramework::AddInputEvent(OnInput); }
		else { logger::warn("the menu framework has no input hook; the Bind buttons will wait for nothing (the INI still sets the keys)"); }
		logger::info("settings page registered");
	}
}
