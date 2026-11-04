# SeparateSwimmingControls - changelog

Rule 61: this mod's own history, kept beside the code it describes.

## 1.0.0 - 2026-11-03 - untested

### Added
- Settings page in the Apocrypha Menu Framework (toggles, precise sliders, keyboard and controller Bind and Clear on every row, AMF's reserved keys and one key per action refused at capture), eleven languages, DevBench tool ssc.control.
- The dive pose: Simple Diving System (Nexus 154534) is a soft dependency. Its clips are Open Animation Replacer variants of the fall clip MT_JumpFall, so in third person the pose sends JumpFall (the graph root takes it from Swim_State), holds bSimpleDiving_IsDiving at 1 after that mod own camera-update write of 0, sets the fall distance that picks its head-down clips, and sends SwimStart to end. The graph variable is asked at the first use, not at load, when the graph is not there yet. Without that mod nothing plays and it logs why once.
- Sneak does not sneak in water and the player never leaves it sneaking; a key the rows hold does nothing else in water. Done by wrapping CanProcess on the game's 13 button handlers, so the control map is never edited and nothing fights a mod that edits it. Jump keeps its handler and alone leaves the water.
- Ascend and Descend, held to rise and sink at any camera angle: right trigger and T, left trigger and Left Ctrl, read raw from the game's input events and added to the player's own movement each frame (the player's ModifyMovementData slot, installed at the first load or new game so it adds to what other mods' hooks do). Rising stops when the engine's kShouldersUnderWater flag clears, sinking on kSupport. bSwimVertical, fSwimVerticalSpeed.

### Removed
- The Dive button and its push: iDiveKey, iDivePad, bDive, fDiveSinkSpeed, fDiveSinkSeconds, the Keys row, the page's rows and the DevBench op=dive. Those lines in an older INI are ignored and left in place.
### Changed
- The dive pose plays while Descend is held in water, in third person: JumpFall once per press (never again until release, 800 ms apart at the least, so a tap or a flickering swim flag cannot loop it), SwimStart on release, on Ascend cancelling it, or at the bottom. bDiveAnimation still switches it off; the same 11 languages.
### Fixed
- A load no longer inherits the last game's in-water flag, which for one movement frame could read as climbing out and clear the sneak of a save made sneaking.