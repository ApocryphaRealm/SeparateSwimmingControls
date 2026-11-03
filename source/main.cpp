// Separate Swimming Controls - own code, GPL-3.0-or-later. Hold-to-rise and hold-to-sink in water, Sneak that does not sneak
// in water, and a dive key that calls on Simple Diving System's animation when that mod is installed. See include/Swim.h.
#include "PCH.h"

#include "DevBenchTool.h"
#include "Page.h"
#include "Settings.h"
#include "Strings.h"
#include "Swim.h"

#include "utils/AddressLibraryGuard.h"
#include "utils/Logger.h"

namespace
{
	void MessageHandler(SKSE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) { return; }
		switch (a_msg->type)
		{
		case SKSE::MessagingInterface::kPostLoad:
			DevBenchTool::Init(false);
			break;
		case SKSE::MessagingInterface::kPostPostLoad:
			page::Register();
			break;
		case SKSE::MessagingInterface::kInputLoaded:
			swim::RegisterInputSink();
			break;
		case SKSE::MessagingInterface::kDataLoaded:
			ssc::Strings::Load();
			DevBenchTool::Init(true);
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			swim::OnGameLoaded();
			break;
		default:
			break;
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::log::init("SeparateSwimmingControls");
	// Address Library pre-check, BEFORE SKSE::Init, which opens the Address Library itself: a missing file gets a message
	// naming it and the plugin loads inert instead of CommonLibSSE-NG's bare failure line.
	if (!AddressLibraryGuard::Guard("Separate Swimming Controls")) { return true; }
	SKSE::Init(a_skse);

	settings::Load();
	settings::ApplyLogLevel();
	SKSE::log::describe_level("SeparateSwimmingControls.ini");
	logger::info("Separate Swimming Controls {} loading", SKSE::PluginDeclaration::GetSingleton()->GetVersion().string("."));

	swim::InstallHandlerWraps();

	auto* messaging = SKSE::GetMessagingInterface();
	if (!messaging || !messaging->RegisterListener(MessageHandler))
	{
		logger::error("could not register the SKSE message listener; the keys will not work");
		return true;
	}
	return true;
}
