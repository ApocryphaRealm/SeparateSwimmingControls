# Prior art for Separate Swimming Controls (searched 2026-11-03, rule 24)

Searched Nexus (skyrimspecialedition, GraphQL name search: swim, swimming, dive, diving, underwater, ascend, descend, vertical,
swim up, swim control, buoyancy, water control, surface) and GitHub (repositories: skyrim swim, skyrim swimming, skse swim,
skyrim underwater, skyrim diving; code: ModifyMovementData IsSwimming), plus a general web search.

No mod found with hold-to-rise and hold-to-sink on rebindable keys. The nearest:

| Mod | Where | What it does | Overlap |
|---|---|---|---|
| Swimming Extensions (Svimex) 1.1.3 | Nexus 189821; github.com/Tsptds/skyrim-svimex-skse, AGPL-3.0 | Manual submerge on the Sneak key: a press starts sinking until fully under, then look-pitch swimming; clamps to the surface otherwise. Sprint swim, turn and enter animations. Needs Nemesis or Pandora | The nearest. One-way sink tied to Sneak, no ascend, no dedicated keys, no leap. Its surface clamp would fight Descend |
| Simple Diving System 1.0.4 | Nexus 154534; github.com/Tsptds/skyrim-diving, AGPL-3.0 | Pose while falling toward deep water; no key, no movement | Used as a soft dependency for the dive's body turn: variable and event names only, no code |
| Underwater NG 1.0.1.4 | Nexus 187230 | Buoyancy moves the player vertically by gear weight, in the same movement slot; no keys | Passive buoyancy only; adds beside this mod |
| Sink Or Swim | Nexus 42962; github.com/LXIV-CXXVIII/Sink-Or-Swim | Armour weight slows or sinks you | Passive, no keys |
| Immersive Diving Animation | Nexus 54525 | Hold Shift while jumping for a dive pose (OAR/DAR) | Animation only |
| SWIM - Simulated Water Impact Momentum | Nexus 182273 | Carries momentum underwater at five named locations (scripted) | Local entry momentum, no keys |
| Sprint Swimming; Sprint Swim Redux SKSE | Nexus 67028, 156417; github.com/Monitor221hz/Skyrim-Sprint-Swim | Faster swimming on Sprint | Horizontal only |
