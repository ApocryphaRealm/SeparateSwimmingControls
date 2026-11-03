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
		constexpr std::int64_t kLeapPushMs = 500;    // how long a leap keeps its forward speed; the air takes over after it
		constexpr std::int64_t kLeapMaxMs = 12000;   // a dive that never met water or ground gives up
		constexpr std::int64_t kDiveCooldownMs = 800;
		constexpr std::int64_t kDiveCheckCacheMs = 150;
		constexpr float kMinWaterDepth = 60.0f;      // below the surface, for a leap to be safe
		constexpr float kMaxDiveHeight = 6000.0f;    // above the water, for the key to count at all
		constexpr float kProbes[] = { 150.0f, 300.0f, 450.0f };  // how far ahead water may start
		constexpr const char* kDivingVar = "bSimpleDiving_IsDiving";

		std::int64_t NowMs()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		std::array<std::array<std::atomic<std::int64_t>, settings::kDeviceCount>, settings::kRowCount> g_seen{};  // last press frame, by key; 0 = up
		std::atomic<std::int64_t> g_driveUntil[settings::kRowCount]{};

		std::atomic<bool> g_inWater{ false };          // the player's swimming flag at the last movement frame
		std::atomic<bool> g_leaping{ false };          // a leap is in the air
		std::atomic<std::int64_t> g_leapStart{ 0 };
		std::atomic<float> g_leapX{ 0.0f };            // havok units a second
		std::atomic<float> g_leapY{ 0.0f };
		std::atomic<std::int64_t> g_sinkUntil{ 0 };
		std::atomic<std::int64_t> g_lastDive{ 0 };

		// counters for the state tool
		std::atomic<int> g_verticalFrames{ 0 };
		std::atomic<float> g_lastStep{ 0.0f };
		std::atomic<int> g_stoppedAtSurface{ 0 };
		std::atomic<int> g_stoppedAtFloor{ 0 };
		std::atomic<int> g_sneakClears{ 0 };
		std::atomic<int> g_blockedPresses{ 0 };
		std::atomic<int> g_leaps{ 0 };
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

		// One ray, ignoring the player's capsule, on the LOS layer (which water does not block).
		bool RayHit(RE::Actor* a_actor, RE::TESObjectCELL* a_cell, const RE::NiPoint3& a_from, const RE::NiPoint3& a_to, float& a_distance)
		{
			RE::bhkWorld* world = a_cell ? a_cell->GetbhkWorld() : nullptr;
			if (!world) { return false; }
			const float s = RE::bhkWorld::GetWorldScale();
			RE::bhkPickData pick;
			pick.rayInput.from = RE::hkVector4(a_from.x * s, a_from.y * s, a_from.z * s, 0.0f);
			pick.rayInput.to = RE::hkVector4(a_to.x * s, a_to.y * s, a_to.z * s, 0.0f);
#if RUNTIME_LINE == 17
			RE::CFilter filter;
			a_actor->GetCollisionFilterInfo(filter);
			filter.SetCollisionLayer(RE::COL_LAYER::kLOS);
			pick.rayInput.filterInfo = filter;
#else
			std::uint32_t filter = 0;
			a_actor->GetCollisionFilterInfo(filter);
			pick.rayInput.filterInfo = (filter & 0xFFFF0000u) | static_cast<std::uint32_t>(RE::COL_LAYER::kLOS);
#endif
			world->PickObject(pick);
			if (!pick.rayOutput.HasHit()) { return false; }
			a_distance = pick.rayOutput.hitFraction * a_from.GetDistance(a_to);
			return true;
		}

		RE::NiPoint3 Forward(const RE::Actor* a_actor)
		{
			const float z = a_actor->GetAngleZ();
			return { std::sin(z), std::cos(z), 0.0f };
		}

		// Deep water ahead, with nothing solid between: the condition for a leap. a_why gets the reason for the state tool.
		bool WaterAhead(RE::PlayerCharacter* a_player, const char*& a_why)
		{
			auto* cell = a_player->GetParentCell();
			if (!cell) { a_why = "no cell"; return false; }
			const RE::NiPoint3 pos = a_player->GetPosition();
			const RE::NiPoint3 forward = Forward(a_player);
			const RE::NiPoint3 chest = pos + RE::NiPoint3{ 0.0f, 0.0f, 80.0f };
			a_why = "no deep water ahead";
			for (const float reach : kProbes)
			{
				const RE::NiPoint3 probe = pos + forward * reach;
				float waterZ = -RE::NI_INFINITY;
				if (!cell->GetWaterHeight(probe, waterZ) || waterZ <= -RE::NI_INFINITY) { continue; }
				const float gap = pos.z - waterZ;
				if (gap > kMaxDiveHeight) { continue; }
				float distance = 0.0f;
				const RE::NiPoint3 chestAhead = chest + forward * reach;
				if (RayHit(a_player, cell, chest, chestAhead, distance))
				{
					a_why = "something solid in the way";
					return false;
				}
				const float toBottom = (gap > 0.0f ? gap : 0.0f) + 80.0f + kMinWaterDepth;
				const RE::NiPoint3 down = chestAhead - RE::NiPoint3{ 0.0f, 0.0f, toBottom };
				if (RayHit(a_player, cell, chestAhead, down, distance)) { continue; }  // ground above or too near the surface
				return true;
			}
			return false;
		}

		// Is the Dive key about to do something? Answers a burst of CanProcess calls from one press with one raycast. Main thread.
		bool DiveWouldLeap(RE::PlayerCharacter* a_player)
		{
			static std::int64_t s_at = 0;
			static bool s_answer = false;
			const auto now = NowMs();
			if (now - s_at < kDiveCheckCacheMs) { return s_answer; }
			s_at = now;
			const char* why = nullptr;
			s_answer = !a_player->IsOnMount() && WaterAhead(a_player, why);
			return s_answer;
		}

		void DoDive()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* state = player ? player->AsActorState() : nullptr;
			auto* controller = player ? player->GetCharController() : nullptr;
			const auto refuse = [](const char* a_why) {
				g_lastDiveResult = a_why;
				g_diveRefused.fetch_add(1);
				logger::debug("dive: {}", a_why);
			};
			if (!state || !controller) { return refuse("no player or character controller"); }
			if (!settings::enabled || !settings::dive) { return refuse("switched off"); }
			if (!GameplayHasInput()) { return refuse("the game is not taking input"); }
			if (player->IsOnMount()) { return refuse("mounted"); }
			const auto now = NowMs();
			if (now - g_lastDive.load() < kDiveCooldownMs) { return refuse("cooldown"); }

			if (state->IsSwimming())
			{
				g_lastDive = now;
				g_sinkUntil = now + static_cast<std::int64_t>(settings::diveSinkSeconds * 1000.0f);
				g_waterDives.fetch_add(1);
				g_lastDiveResult = "under, from the surface";
				logger::info("dive: under, from the surface");
				return;
			}
			if (controller->context.currentState != RE::hkpCharacterStateType::kOnGround)
			{
				// Already in the air (a jump, a fall): no leap, but the same push down on reaching water.
				g_lastDive = now;
				g_leapX = 0.0f;
				g_leapY = 0.0f;
				g_leapStart = now - kLeapPushMs;
				g_leaping = true;
				g_lastDiveResult = "in the air: the push down waits for the water";
				return;
			}
			const char* why = nullptr;
			if (!WaterAhead(player, why)) { return refuse(why); }

			g_lastDive = now;
			const float s = RE::bhkWorld::GetWorldScale();
			const RE::NiPoint3 forward = Forward(player);
			const float speed = settings::diveForward * s;
			g_leapX = forward.x * speed;
			g_leapY = forward.y * speed;
			controller->flags.set(RE::CHARACTER_FLAGS::kJumping);
			controller->flags.reset(RE::CHARACTER_FLAGS::kSupport);
			controller->wantState = RE::hkpCharacterStateType::kInAir;
			controller->context.currentState = RE::hkpCharacterStateType::kInAir;
			controller->fallTime = 0.0f;
			controller->fallStartHeight = player->GetPositionZ();
			RE::hkVector4 velocity(g_leapX.load(), g_leapY.load(), settings::diveUp * s, 0.0f);
			controller->SetLinearVelocityImpl(velocity);
			controller->outVelocity = velocity;
			controller->velocityMod = velocity;
			player->NotifyAnimationGraph("JumpFall");
			g_leapStart = now;
			g_leaping = true;
			g_leaps.fetch_add(1);
			g_lastDiveResult = "leap";
			logger::info("dive: leap, {} forward and {} up", settings::diveForward.load(), settings::diveUp.load());
		}

		void UpdateLeap(RE::PlayerCharacter* a_player, bool a_swimming, std::int64_t a_now)
		{
			const auto elapsed = a_now - g_leapStart.load(std::memory_order_relaxed);
			if (a_swimming)
			{
				g_leaping = false;
				g_sinkUntil = a_now + static_cast<std::int64_t>(settings::diveSinkSeconds * 1000.0f);
				g_lastDiveResult = "reached the water";
				logger::info("dive: reached the water after {} ms", elapsed);
				return;
			}
			auto* controller = a_player->GetCharController();
			if (!controller || elapsed > kLeapMaxMs || (elapsed > 200 && controller->flags.any(RE::CHARACTER_FLAGS::kSupport)))
			{
				g_leaping = false;
				g_lastDiveResult = elapsed > kLeapMaxMs ? "gave up in the air" : "landed, not in water";
				if (settings::diveAnimation) { a_player->SetGraphVariableInt(kDivingVar, 0); }
				return;
			}
			if (elapsed < kLeapPushMs && (g_leapX.load(std::memory_order_relaxed) != 0.0f || g_leapY.load(std::memory_order_relaxed) != 0.0f))
			{
				RE::hkVector4 velocity;
				controller->GetLinearVelocityImpl(velocity);
				velocity.quad.m128_f32[0] = g_leapX.load(std::memory_order_relaxed);
				velocity.quad.m128_f32[1] = g_leapY.load(std::memory_order_relaxed);
				controller->SetLinearVelocityImpl(velocity);
			}
			// Simple Diving System writes this itself from the camera's update whenever the player is above deep water, so
			// this only matters before the player is over it. GetGraphVariableInt answers false when the graph has no such
			// variable, and Set is then a no-op, so this is safe without that mod.
			if (settings::diveAnimation && a_player->SetGraphVariableInt(kDivingVar, 1)) { g_varWrites.fetch_add(1, std::memory_order_relaxed); }
		}

		void UpdateMovement(RE::PlayerCharacter* a_player, float a_delta, RE::NiPoint3& a_translation)
		{
			const auto* state = a_player->AsActorState();
			if (!state || a_delta <= 0.0f || !settings::enabled.load(std::memory_order_relaxed)) { return; }
			const bool swimming = state->IsSwimming() && !a_player->IsOnMount();
			if (swimming != g_inWater.load(std::memory_order_relaxed))
			{
				g_inWater = swimming;
				logger::debug("swimming: {}", swimming);
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
			const bool leaping = g_leaping.load(std::memory_order_relaxed);
			if (!swimming && !leaping) { return; }
			const auto now = NowMs();
			if (leaping) { UpdateLeap(a_player, swimming, now); }
			if (!swimming) { return; }

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
		// engine's. Out of water and not leaping this costs two atomic reads and the swimming flag.
		struct PlayerMovementData
		{
			static void thunk(RE::PlayerCharacter* a_this, float a_delta, RE::NiPoint3& a_translation, RE::NiPoint3& a_rotation)
			{
				func(a_this, a_delta, a_translation, a_rotation);
				if (a_this) { UpdateMovement(a_this, a_delta, a_translation); }
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// Whether a press a handler is about to take belongs to this mod. Runs for every event and handler, so the common case
		// (not in water, not the Dive key) is a few reads. Main thread: PlayerControls and MenuControls process input there.
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
			const bool diveKey = settings::dive.load(std::memory_order_relaxed) && code == settings::keys[settings::kDive][device].load(std::memory_order_relaxed);
			if (g_inWater.load(std::memory_order_relaxed))
			{
				if (diveKey) { return true; }
				if (!settings::swimVertical.load(std::memory_order_relaxed)) { return false; }
				return code == settings::keys[settings::kAscend][device].load(std::memory_order_relaxed) ||
				       code == settings::keys[settings::kDescend][device].load(std::memory_order_relaxed);
			}
			// On land the Dive key keeps its game use unless it is about to leap, so D-pad left stays a favourites hotkey away from water.
			auto* player = diveKey ? RE::PlayerCharacter::GetSingleton() : nullptr;
			return player && DiveWouldLeap(player);
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
						if (row == settings::kDive && button->IsDown() && settings::enabled && settings::dive && GameplayHasInput())
						{
							if (auto* tasks = SKSE::GetTaskInterface()) { tasks->AddTask([]() { DoDive(); }); }
						}
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		bool Loaded(const wchar_t* a_dll)
		{
			return REX::W32::GetModuleHandleW(a_dll) != nullptr;
		}
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
			d.graphVar = player->GetGraphVariableInt(kDivingVar, value);
		}
		return d;
	}

	void OnGameLoaded()
	{
		InstallMovementWrap();
		g_leaping = false;
		g_sinkUntil = 0;
		const auto d = DiveAnimationStatus();
		logger::info("dive animation: SimpleDiving.dll {}, OpenAnimationReplacer.dll {}, graph variable {} -> {}",
					 d.dll ? "loaded" : "not loaded", d.oar ? "loaded" : "not loaded", d.graphVar ? "present" : "absent",
					 d.Ready() ? "Simple Diving System's pose will play" : "the leap plays the game's own fall");
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
			R"("dive":{{"on":{},"leaps":{},"fromSurface":{},"refused":{},"leaping":{},"sinking":{},"lastResult":"{}","varWrites":{},"sds":{{"dll":{},"oar":{},"graphVar":{},"value":{}}}}})",
			settings::enabled.load() ? "true" : "false", settings::swimVertical.load() ? "true" : "false", settings::swimVerticalSpeed.load(),
			state && state->IsSwimming() ? "true" : "false", g_inWater.load() ? "true" : "false", state && state->IsSneaking() ? "true" : "false",
			player && player->IsOnMount() ? "true" : "false",
			controller && controller->flags.any(RE::CHARACTER_FLAGS::kShouldersUnderWater) ? "true" : "false",
			controller && controller->flags.any(RE::CHARACTER_FLAGS::kSupport) ? "true" : "false", keys,
			g_verticalFrames.load(), g_lastStep.load(), g_stoppedAtSurface.load(), g_stoppedAtFloor.load(), g_sneakClears.load(), g_blockedPresses.load(),
			settings::dive.load() ? "true" : "false", g_leaps.load(), g_waterDives.load(), g_diveRefused.load(), g_leaping.load() ? "true" : "false",
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
