#pragma once

#include <cstdint>
#include <string>

// The swimming controls. The game has no vertical swim input: its swim state reads no button, the Sneak press only fires the
// sneak action, and sneaking does nothing for height. So Ascend and Descend are read raw from the game's own input events and
// added to the player's own movement each frame (the player's ModifyMovementData slot; NPCs have another, not touched).
//
// Nothing here edits the control map, so nothing here can fight another mod over it (Unbind Vanilla Controls' [Swimming] and
// [Horseback] layouts swap it): "a key a row holds does nothing else in water" is the game's own input handlers being told
// not to take that press, by a wrap of each handler's CanProcess, which is what also keeps Sneak from sneaking.
//
// DIVE. On land, with deep water ahead, a leap forward through the character controller (the same InAir + velocity path other
// mods use), and on reaching the water a push down. In water at the surface, just the push down. The pose in the air comes
// from Simple Diving System (Nexus 154534) when it is installed: it replaces the falling animation while its behaviour graph
// variable bSimpleDiving_IsDiving is 1. A soft dependency, asked for at run time; without it the leap plays the game's own fall.
namespace swim
{
	void InstallHandlerWraps();    // plugin load: the handlers' vtables
	void InstallMovementWrap();    // first load or new game, once: after every plugin's data-loaded hooks, so it adds to what they did
	void RegisterInputSink();      // kInputLoaded

	struct SoftDependency
	{
		bool dll = false;       // SimpleDiving.dll is loaded
		bool oar = false;       // OpenAnimationReplacer.dll is loaded
		bool graphVar = false;  // the player's behaviour graph has bSimpleDiving_IsDiving (patched by BDI, Nemesis or Pandora)
		bool Ready() const { return dll && oar && graphVar; }
	};
	SoftDependency DiveAnimationStatus();  // asked of the running game
	void OnGameLoaded();                   // logs that answer once per load

	// DevBench (rule 31 and 64)
	std::string StateJson();
	void DriveHold(int a_row, int a_ms);   // as if the row's key were held for a_ms; the movement path, not the input path
	void DriveDive();                      // what the Dive key queues
}
