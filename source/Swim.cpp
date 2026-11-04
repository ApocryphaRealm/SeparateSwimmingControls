#include "PCH.h"

#include "Swim.h"

#include "Settings.h"
#include "utils/Logger.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <format>

namespace swim
{
	namespace
	{
		constexpr std::int64_t kHeldForgetMs = 300;  // a held button repeats every frame; silence means its release was lost
		constexpr std::int64_t kDiveCooldownMs = 800;
		constexpr float kPoseFallHeight = 600.0f;      // over 500: Simple Diving System's High Height and Forward conditions, its two head-down clips
		constexpr const char* kDivingVar = "bSimpleDiving_IsDiving";

		std::int64_t NowMs()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		std::array<std::array<std::atomic<std::int64_t>, settings::kDeviceCount>, settings::kRowCount> g_seen{};  // last press frame, by key; 0 = up
		std::atomic<std::int64_t> g_driveUntil[settings::kRowCount]{};

		std::atomic<bool> g_inWater{ false };          // the player's swimming flag at the last movement frame
		std::atomic<std::int64_t> g_sinkUntil{ 0 };
		std::atomic<std::int64_t> g_lastDive{ 0 };
		std::atomic<bool> g_posing{ false };           // the dive's body turn is playing
		std::atomic<bool> g_poseRefusalLogged{ false };
		std::atomic<bool> g_fallSawVar{ false };       // Simple Diving System's variable went to 1 during this fall
		float g_savedFallStart = 0.0f;                 // main thread: the controller's values, put back when the pose ends
		float g_savedFallTime = 0.0f;

		// counters for the state tool
		std::atomic<int> g_verticalFrames{ 0 };
		std::atomic<float> g_lastStep{ 0.0f };
		std::atomic<int> g_stoppedAtSurface{ 0 };
		std::atomic<int> g_stoppedAtFloor{ 0 };
		std::atomic<int> g_sneakClears{ 0 };
		std::atomic<int> g_blockedPresses{ 0 };
		std::atomic<int> g_waterDives{ 0 };
		std::atomic<int> g_diveRefused{ 0 };
		std::atomic<int> g_varWrites{ 0 };
		std::string g_lastDiveResult = "none";  // main thread

		int DeviceOf(const RE::ButtonEvent* a_button)
		{
			switch (a_button->GetDevice())
			{
			case RE::INPUT_DEVICE::kKeyboard: return settings::kKeyboard;
			case RE::INPUT_DEVICE::kGamepad: return settings::kGamepad;
			default: return -1;
			}
		}

		bool GameplayHasInput()
		{
			auto* ui = RE::UI::GetSingleton();
			if (!ui || ui->GameIsPaused() || ui->IsMenuOpen(RE::Console::MENU_NAME)) { return false; }
			const auto* map = RE::ControlMap::GetSingleton();
			return map && map->IsMovementControlsEnabled();
		}

		bool Held(int a_row, std::int64_t a_now)
		{
			if (g_driveUntil[a_row].load(std::memory_order_relaxed) > a_now) { return true; }
			for (const auto& seen : { std::cref(g_seen[a_row][0]), std::cref(g_seen[a_row][1]) })
			{
				const auto at = seen.get().load(std::memory_order_relaxed);
				if (at != 0 && a_now - at <= kHeldForgetMs) { return true; }
			}
			return false;
		}

		// The game's own "stop sneaking" (Actor's sneak setter, 1.5.97 id 36926, off branch): the iIsInSneak graph variable and
		// the state bit. SneakStop asks the behaviour graph to leave any sneak state it holds. Main thread only.
		void ClearSneakIfSet(const char* a_reason)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* state = player ? player->AsActorState() : nullptr;
			if (!state || !state->IsSneaking()) { return; }
			player->SetGraphVariableInt("iIsInSneak", 0);
			state->actorState1.sneaking = 0;
			player->NotifyAnimationGraph("SneakStop");
			g_sneakClears.fetch_add(1);
			logger::info("{}: the player was sneaking; sneak cleared", a_reason);
		}

		bool Loaded(const wchar_t* a_dll)
		{
			return REX::W32::GetModuleHandleW(a_dll) != nullptr;
		}

		// What every one of Simple Diving System's clips asks of OAR. SDS's own plugin writes the variable 0 from every camera
		// update while the player is at or below the water, so this runs after that write: see ThirdPersonUpdate. The fall
		// distance picks the clip: from 500 up, the two head-down ones (moving forward, or not moving). At the swimming player's
		// distance of 0 only the hands-up clip qualifies. OAR's FallDistance counts only while fallTime is above 0, and the
		// engine's distance is fallStartHeight (game units) less the controller's height, so both are written.
		void HoldPose(RE::PlayerCharacter* a_player)
		{
			if (a_player->SetGraphVariableInt(kDivingVar, 1) || a_player->SetGraphVariableBool(kDivingVar, true))
			{
				g_varWrites.fetch_add(1, std::memory_order_relaxed);
			}
			if (auto* controller = a_player->GetCharController())
			{
				controller->fallStartHeight = a_player->GetPositionZ() + kPoseFallHeight;
				controller->fallTime = 1.0f;
			}
		}

		// Main thread, from a task or the Dive key. SwimStart is what the game itself sends on entering water.
		void EndPose(bool a_stillSwimming)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) { return; }
			if (auto* controller = player->GetCharController())
			{
				controller->fallStartHeight = g_savedFallStart;
				controller->fallTime = g_savedFallTime;
			}
			const bool sent = player->NotifyAnimationGraph(a_stillSwimming ? "SwimStart" : "JumpLand");
			if (!player->SetGraphVariableInt(kDivingVar, 0)) { player->SetGraphVariableBool(kDivingVar, false); }
			logger::info("dive: pose ended, {} sent, the graph {}", a_stillSwimming ? "SwimStart" : "JumpLand", sent ? "took it" : "refused it");
		}

		// The dive's body turn. Simple Diving System's clips are variants of the fall clip (MT_JumpFall), so the graph has to be
		// in the fall state for them to play; the graph's root takes JumpFall from any state, swimming included, and SwimStart
		// brings it back. a_player is swimming. Says why not, once a load, when the pose cannot play.
		void StartPose(RE::PlayerCharacter* a_player)
		{
			if (!settings::diveAnimation.load() || g_posing.load()) { return; }
			const auto d = DiveAnimationStatus();
			if (!d.Ready())
			{
				if (!g_poseRefusalLogged.exchange(true))
				{
					logger::info("dive animation: no pose, the push still works: SimpleDiving.dll {}, OpenAnimationReplacer.dll {}, graph variable {}",
								 d.dll ? "loaded" : "not loaded", d.oar ? "loaded" : "not loaded", d.graphVar ? "present" : "absent");
				}
				return;
			}
			const auto* camera = RE::PlayerCamera::GetSingleton();
			if (camera && camera->IsInFirstPerson())
			{
				logger::debug("dive: first person, no pose: Simple Diving System ships no first-person clips");
				return;
			}
			auto* controller = a_player->GetCharController();
			if (!controller) { return; }
			g_savedFallStart = controller->fallStartHeight;
			g_savedFallTime = controller->fallTime;
			HoldPose(a_player);
			if (!a_player->NotifyAnimationGraph("JumpFall"))
			{
				logger::warn("dive: the graph refused JumpFall, no pose");
				EndPose(true);
				return;
			}
			g_posing = true;
			logger::info("dive: pose started, JumpFall taken, graph variable present");
		}

		void DoDive()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* state = player ? player->AsActorState() : nullptr;
			const auto refuse = [](const char* a_why) {
				g_lastDiveResult = a_why;
				g_diveRefused.fetch_add(1);
				logger::debug("dive: {}", a_why);
			};
			if (!state) { return refuse("no player"); }
			if (!settings::enabled || !settings::dive) { return refuse("switched off"); }
			if (!GameplayHasInput()) { return refuse("the game is not taking input"); }
			if (player->IsOnMount()) { return refuse("mounted"); }
			if (!state->IsSwimming()) { return refuse("not in water"); }
			const auto now = NowMs();
			if (now - g_lastDive.load() < kDiveCooldownMs) { return refuse("cooldown"); }

			g_lastDive = now;
			g_sinkUntil = now + static_cast<std::int64_t>(settings::diveSinkSeconds * 1000.0f);
			g_waterDives.fetch_add(1);
			g_lastDiveResult = "under, from the surface";
			logger::info("dive: under, from the surface");
			StartPose(player);
		}

		// Ends the pose as soon as its reason is gone: the push is over, or the player left the water or the third-person
		// camera. A task, because the graph is not asked for changes in the middle of movement.
		void EndPoseIfDone(bool a_swimming, std::int64_t a_now)
		{
			const auto* camera = RE::PlayerCamera::GetSingleton();
			const bool over = !a_swimming || a_now >= g_sinkUntil.load(std::memory_order_relaxed) || !settings::enabled.load(std::memory_order_relaxed) ||
			                  !settings::diveAnimation.load(std::memory_order_relaxed) || (camera && camera->IsInFirstPerson());
			if (!over) { return; }
			g_posing = false;
			if (auto* tasks = SKSE::GetTaskInterface())
			{
				tasks->AddTask([a_swimming]() { EndPose(a_swimming); });
			}
		}

		// Whether Simple Diving System's own write reaches the graph during an ordinary fall: nothing in this mod touches a fall,
		// so this line is the check that it is not in the way. Falling only; on the ground it costs one flag.
		void WatchFall(RE::PlayerCharacter* a_player)
		{
			static const bool s_sds = Loaded(L"SimpleDiving.dll");
			if (!s_sds) { return; }
			const auto* controller = a_player->GetCharController();
			if (!controller || controller->context.currentState != RE::hkpCharacterStateType::kInAir)
			{
				g_fallSawVar = false;
				return;
			}
			if (g_fallSawVar) { return; }
			std::int32_t value = 0;
			bool flag = false;
			const bool on = (a_player->GetGraphVariableInt(kDivingVar, value) && value != 0) || (a_player->GetGraphVariableBool(kDivingVar, flag) && flag);
			if (on)
			{
				g_fallSawVar = true;
				logger::info("fall: Simple Diving System's variable went to 1 above deep water, so its fall clip should be playing");
			}
		}

		void UpdateMovement(RE::PlayerCharacter* a_player, float a_delta, RE::NiPoint3& a_translation)
		{
			const auto* state = a_player->AsActorState();
			if (!state) { return; }
			const bool swimming = state->IsSwimming() && !a_player->IsOnMount();
			if (g_posing.load(std::memory_order_relaxed)) { EndPoseIfDone(swimming, NowMs()); }
			if (a_delta <= 0.0f || !settings::enabled.load(std::memory_order_relaxed)) { return; }
			if (swimming != g_inWater.load(std::memory_order_relaxed))
			{
				g_inWater = swimming;
				logger::debug("swimming: {}", swimming);
				if (swimming && Loaded(L"SimpleDiving.dll"))
				{
					logger::info("into the water; during the fall Simple Diving System's variable {}", g_fallSawVar.load() ? "was 1" : "stayed 0 or there was no fall");
				}
				// The player never leaves the water sneaking: cleared on the way in, and on the way out for a press that
				// landed in the moment before. A task, because the graph is not asked for changes in the middle of movement.
				if (settings::swimVertical)
				{
					if (auto* tasks = SKSE::GetTaskInterface())
					{
						tasks->AddTask([swimming]() { ClearSneakIfSet(swimming ? "into the water" : "out of the water"); });
					}
				}
			}
			if (!swimming)
			{
				WatchFall(a_player);
				return;
			}
			const auto now = NowMs();

			auto* controller = a_player->GetCharController();
			if (!controller) { return; }
			const bool onFloor = controller->flags.any(RE::CHARACTER_FLAGS::kSupport);
			if (now < g_sinkUntil.load(std::memory_order_relaxed) && !onFloor)
			{
				a_translation.z -= settings::diveSinkSpeed.load(std::memory_order_relaxed) * a_delta;
			}
			if (!settings::swimVertical.load(std::memory_order_relaxed)) { return; }
			const bool up = Held(settings::kAscend, now);
			const bool down = Held(settings::kDescend, now);
			if (up == down) { return; }  // neither, or both, which cancel
			if (up && !controller->flags.any(RE::CHARACTER_FLAGS::kShouldersUnderWater))
			{
				// The engine's own surface: it sets this flag at 70% submerged and floats the player there. Ascend stops here;
				// only Jump, whose handler this never touches by default, leaves the water.
				g_stoppedAtSurface.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			if (down && onFloor)
			{
				g_stoppedAtFloor.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			const float step = settings::swimVerticalSpeed.load(std::memory_order_relaxed) * a_delta;
			a_translation.z += up ? step : -step;
			g_verticalFrames.fetch_add(1, std::memory_order_relaxed);
			g_lastStep.store(up ? step : -step, std::memory_order_relaxed);
		}

		// The player's slot of Actor::ModifyMovementData (PlayerCharacter's vtable, slot 0x11A). The translation is this frame's
		// movement in game units and the character controller moves it, so walls, the floor and the water's physics stay the
		// engine's. On the ground out of water this costs the swimming flag and a look at the controller's state.
		struct PlayerMovementData
		{
			static void thunk(RE::PlayerCharacter* a_this, float a_delta, RE::NiPoint3& a_translation, RE::NiPoint3& a_rotation)
			{
				func(a_this, a_delta, a_translation, a_rotation);
				if (a_this) { UpdateMovement(a_this, a_delta, a_translation); }
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// Simple Diving System writes its variable from this same slot on every camera update, 0 in water, and OAR reads it during
		// the graph's update. Wrapping the slot and writing after the call makes this write the last one whichever plugin wrapped
		// first. Third person only: the pose is never held in first person, where that mod ships no clips.
		struct ThirdPersonUpdate
		{
			static void thunk(RE::ThirdPersonState* a_this, RE::BSTSmartPointer<RE::TESCameraState>& a_next)
			{
				func(a_this, a_next);
				if (!g_posing.load(std::memory_order_relaxed)) { return; }
				if (auto* player = RE::PlayerCharacter::GetSingleton()) { HoldPose(player); }
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// Whether a press a handler is about to take belongs to this mod. Runs for every event and handler, so the common case
		// (not in water) is a few reads. Main thread: PlayerControls and MenuControls process input there.
		bool ShouldBlock(RE::InputEvent* a_event, bool a_isSneakHandler)
		{
			if (!a_event || a_event->GetEventType() != RE::INPUT_EVENT_TYPE::kButton || !settings::enabled.load(std::memory_order_relaxed)) { return false; }
			const auto* button = a_event->AsButtonEvent();
			const int device = button ? DeviceOf(button) : -1;
			const bool sneakBlock = a_isSneakHandler && g_inWater.load(std::memory_order_relaxed) && settings::swimVertical.load(std::memory_order_relaxed);
			if (device < 0 && !sneakBlock) { return false; }
			if (auto* ui = RE::UI::GetSingleton(); !ui || ui->GameIsPaused()) { return false; }
			if (sneakBlock) { return true; }
			const auto code = button->GetIDCode();
			if (code == 0) { return false; }
			if (!g_inWater.load(std::memory_order_relaxed)) { return false; }  // on land every key keeps its game use
			if (settings::dive.load(std::memory_order_relaxed) && code == settings::keys[settings::kDive][device].load(std::memory_order_relaxed)) { return true; }
			if (!settings::swimVertical.load(std::memory_order_relaxed)) { return false; }
			return code == settings::keys[settings::kAscend][device].load(std::memory_order_relaxed) ||
			       code == settings::keys[settings::kDescend][device].load(std::memory_order_relaxed);
		}

		template <class Handler, bool kIsSneak = false>
		struct CanProcessWrap
		{
			static bool thunk(Handler* a_this, RE::InputEvent* a_event)
			{
				if (ShouldBlock(a_event, kIsSneak))
				{
					if (a_event->AsButtonEvent() && a_event->AsButtonEvent()->IsDown()) { g_blockedPresses.fetch_add(1, std::memory_order_relaxed); }
					return false;
				}
				return func(a_this, a_event);
			}
			static inline REL::Relocation<decltype(thunk)> func;
			static void Install()
			{
				REL::Relocation<std::uintptr_t> vtbl{ Handler::VTABLE[0] };
				func = vtbl.write_vfunc(0x1, thunk);
			}
		};

		class InputSink final : public RE::BSTEventSink<RE::InputEvent*>
		{
		public:
			static InputSink* GetSingleton()
			{
				static InputSink singleton;
				return &singleton;
			}

			RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_events, RE::BSTEventSource<RE::InputEvent*>*) override
			{
				if (!a_events) { return RE::BSEventNotifyControl::kContinue; }
				const auto now = NowMs();
				for (auto* e = *a_events; e; e = e->next)
				{
					const auto* button = e->AsButtonEvent();
					const int device = button ? DeviceOf(button) : -1;
					if (device < 0) { continue; }
					const auto code = button->GetIDCode();
					if (code == 0) { continue; }
					for (int row = 0; row < settings::kRowCount; ++row)
					{
						if (code != settings::keys[row][device].load(std::memory_order_relaxed)) { continue; }
						g_seen[row][device].store(button->IsUp() ? 0 : now, std::memory_order_relaxed);
						if (row == settings::kDive && button->IsDown() && g_inWater.load(std::memory_order_relaxed) && settings::enabled && settings::dive && GameplayHasInput())
						{
							if (auto* tasks = SKSE::GetTaskInterface()) { tasks->AddTask([]() { DoDive(); }); }
						}
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void InstallHandlerWraps()
	{
		CanProcessWrap<RE::SneakHandler, true>::Install();
		CanProcessWrap<RE::AttackBlockHandler>::Install();
		CanProcessWrap<RE::ActivateHandler>::Install();
		CanProcessWrap<RE::AutoMoveHandler>::Install();
		CanProcessWrap<RE::JumpHandler>::Install();
		CanProcessWrap<RE::ReadyWeaponHandler>::Install();
		CanProcessWrap<RE::RunHandler>::Install();
		CanProcessWrap<RE::ToggleRunHandler>::Install();
		CanProcessWrap<RE::ShoutHandler>::Install();
		CanProcessWrap<RE::SprintHandler>::Install();
		CanProcessWrap<RE::TogglePOVHandler>::Install();
		CanProcessWrap<RE::MenuOpenHandler>::Install();
		CanProcessWrap<RE::FavoritesHandler>::Install();
		logger::info("hook: CanProcess wrapped on the 13 button handlers");
	}

	void InstallMovementWrap()
	{
		static bool s_installed = false;
		if (s_installed) { return; }
		s_installed = true;
		REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_PlayerCharacter[0] };
		PlayerMovementData::func = vtbl.write_vfunc(0x11A, PlayerMovementData::thunk);
		logger::info("hook: PlayerCharacter::ModifyMovementData (vtable {:X}, slot 0x11A) wrapped, after every plugin's data-loaded hooks", vtbl.address());
		REL::Relocation<std::uintptr_t> camera{ RE::VTABLE_ThirdPersonState[0] };
		ThirdPersonUpdate::func = camera.write_vfunc(0x3, ThirdPersonUpdate::thunk);
		logger::info("hook: ThirdPersonState::Update (vtable {:X}, slot 0x3) wrapped", camera.address());
	}

	void RegisterInputSink()
	{
		auto* manager = RE::BSInputDeviceManager::GetSingleton();
		if (!manager)
		{
			logger::warn("the input device manager is not there; the keys will not work");
			return;
		}
		manager->AddEventSink<RE::InputEvent*>(InputSink::GetSingleton());
		logger::info("input sink registered");
	}

	SoftDependency DiveAnimationStatus()
	{
		SoftDependency d;
		d.dll = Loaded(L"SimpleDiving.dll");
		d.oar = Loaded(L"OpenAnimationReplacer.dll");
		if (auto* player = RE::PlayerCharacter::GetSingleton())
		{
			std::int32_t value = 0;
			bool flag = false;
			d.graphVar = player->GetGraphVariableInt(kDivingVar, value) || player->GetGraphVariableBool(kDivingVar, flag);
		}
		return d;
	}

	void OnGameLoaded()
	{
		InstallMovementWrap();
		g_posing = false;
		g_poseRefusalLogged = false;
		g_sinkUntil = 0;
		// Only the DLLs here: the player's behaviour graph is not loaded yet at this message (it read as absent on a list that has
		// the variable compiled in), so the graph is asked at the first dive.
		const auto d = DiveAnimationStatus();
		logger::info("dive animation: SimpleDiving.dll {}, OpenAnimationReplacer.dll {}; the behaviour graph is asked at the first dive",
					 d.dll ? "loaded" : "not loaded", d.oar ? "loaded" : "not loaded");
	}

	std::string StateJson()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		const auto* state = player ? player->AsActorState() : nullptr;
		auto* controller = player ? player->GetCharController() : nullptr;
		const auto now = NowMs();
		const auto d = DiveAnimationStatus();
		std::int32_t var = -1;
		if (player) { player->GetGraphVariableInt(kDivingVar, var); }
		std::string keys;
		for (int r = 0; r < settings::kRowCount; ++r)
		{
			keys += std::format(R"({}"{}":{{"keyboard":"0x{:02X}","gamepad":"0x{:04X}","held":{}}})", r ? "," : "", settings::RowName(r), settings::keys[r][settings::kKeyboard].load(),
								settings::keys[r][settings::kGamepad].load(), Held(r, now) ? "true" : "false");
		}
		return std::format(
			R"("enabled":{},"swimVertical":{},"speed":{},"isSwimming":{},"inWaterSeen":{},"sneaking":{},"mounted":{},"shouldersUnder":{},"support":{},"keys":{{{}}},)"
			R"("frames":{},"lastStep":{},"stoppedAtSurface":{},"stoppedAtFloor":{},"sneakClears":{},"blockedPresses":{},)"
			R"("dive":{{"on":{},"fromSurface":{},"refused":{},"posing":{},"sinking":{},"lastResult":"{}","varWrites":{},"sds":{{"dll":{},"oar":{},"graphVar":{},"value":{}}}}})",
			settings::enabled.load() ? "true" : "false", settings::swimVertical.load() ? "true" : "false", settings::swimVerticalSpeed.load(),
			state && state->IsSwimming() ? "true" : "false", g_inWater.load() ? "true" : "false", state && state->IsSneaking() ? "true" : "false",
			player && player->IsOnMount() ? "true" : "false",
			controller && controller->flags.any(RE::CHARACTER_FLAGS::kShouldersUnderWater) ? "true" : "false",
			controller && controller->flags.any(RE::CHARACTER_FLAGS::kSupport) ? "true" : "false", keys,
			g_verticalFrames.load(), g_lastStep.load(), g_stoppedAtSurface.load(), g_stoppedAtFloor.load(), g_sneakClears.load(), g_blockedPresses.load(),
			settings::dive.load() ? "true" : "false", g_waterDives.load(), g_diveRefused.load(), g_posing.load() ? "true" : "false",
			now < g_sinkUntil.load() ? "true" : "false", g_lastDiveResult, g_varWrites.load(), d.dll ? "true" : "false", d.oar ? "true" : "false", d.graphVar ? "true" : "false", var);
	}

	void DriveHold(int a_row, int a_ms)
	{
		if (a_row >= 0 && a_row < settings::kRowCount) { g_driveUntil[a_row] = NowMs() + a_ms; }
	}

	void DriveDive()
	{
		if (auto* tasks = SKSE::GetTaskInterface()) { tasks->AddTask([]() { DoDive(); }); }
	}
}
