# SeparateSwimmingControls - changelog

Rule 61: this mod's own history, kept beside the code it describes.

## 1.0.0 - 2026-11-03 - untested

### Added
- Settings page in the Apocrypha Menu Framework (toggles, precise sliders, keyboard and controller Bind and Clear on every row, AMF's reserved keys and one key per action refused at capture), eleven languages, DevBench tool ssc.control.
- Dive (D-pad left, Y). On the ground with deep water within 450 units ahead and nothing solid in the way: a leap through the character controller (in-air state plus velocity), then a push under on reaching the water. Swimming: the push under. Already in the air: the push under on reaching the water. Otherwise the key keeps its game use. Simple Diving System (Nexus 154534) is a soft dependency: when its behaviour graph variable bSimpleDiving_IsDiving exists, its pose plays in the air; without it the game's own fall.
- Sneak does not sneak in water and the player never leaves it sneaking; a key the rows hold does nothing else in water. Done by wrapping CanProcess on the game's 13 button handlers, so the control map is never edited and nothing fights a mod that edits it. Jump keeps its handler and alone leaves the water.
- Ascend and Descend, held to rise and sink at any camera angle: right trigger and T, left trigger and Left Ctrl, read raw from the game's input events and added to the player's own movement each frame (the player's ModifyMovementData slot, installed at the first load or new game so it adds to what other mods' hooks do). Rising stops when the engine's kShouldersUnderWater flag clears, sinking on kSupport. bSwimVertical, fSwimVerticalSpeed.
