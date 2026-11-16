# BoonPanel (Nexus + ArcDPS)

A movable Guild Wars 2 boon panel for the local player. It is built as one C++ DLL with two load interfaces:

- **Nexus** draws the movable ImGui panel.
- **ArcDPS** supplies live local-player boon events through its `combat_local` extension callback.

The DLL is built once, then copied under two names so both loaders can load their own module instance:

```text
Guild Wars 2/
├── addons/
│   └── BoonPanel.dll
└── bin64/
    └── arcdps_BoonPanel.dll
```

## What it currently does

- Displays the 12 standard boons: Aegis, Alacrity, Fury, Might, Protection, Quickness, Regeneration, Resistance, Resolution, Stability, Swiftness and Vigor. The boon IDs are declared in `src/boons.h`.
- Uses a draggable Nexus/ImGui window; the position is managed by ImGui.
- Shows remaining duration for duration-stacking boons and stack counts for Might/Stability.
- Has panel settings for tile size, columns, inactive boons, durations and stack counts.
- Uses ArcDPS local-player combat events, not process-memory scanning. Expiration timestamps are shared between the two DLL instances so stack counts and duration tiles tick down without waiting for another callback.
- Works in WvW to the extent ArcDPS exposes the local player's boon events in that situation.

This is a starter implementation, not a pixel-perfect Reffect replacement. It uses compact text tiles rather than Reffect's original artwork and does not implement every Reffect layout/customization feature.

## Important WvW/data limitation

Nexus' public shared game data does not include a complete active-boon snapshot. The data bridge therefore uses ArcDPS' `combat_local` callback. ArcDPS documents boon apply/change/remove events as visibility-limited; initial boon state outside an instance is limited to squad information. As a result:

- Start tracking reliably by applying/reapplying boons after ArcDPS and BoonPanel have loaded.
- A boon already active before tracking starts may not appear until it is applied again; ArcDPS initial/active-stack events are not a complete player-wide snapshot in every mode.
- Boons that the combat event feed does not expose cannot be inferred by this addon.
- The panel is for **your character's** boons, not every ally/enemy on screen.

No memory scanning, injection, or hidden-state reading is implemented. The addon does not bypass ArenaNet/ArcDPS visibility limits. Review the Guild Wars 2 third-party program policy and the current ArcDPS/Nexus addon policies before use.

## Build on GitHub

1. Create an empty GitHub repository.
2. Upload/push the contents of this folder to the repository's root.
3. Open **Actions** and enable workflows if GitHub asks.
4. Run **Build BoonPanel** using **Run workflow**, or push a commit.
5. Open the completed workflow run and download the `BoonPanel-Windows-x64` artifact.
6. Extract the ZIP into your Guild Wars 2 installation directory, keeping the `addons` and `bin64` folders intact.
7. Restart the game. Ensure both Nexus and ArcDPS are installed and enabled.

The workflow uses `windows-2022`, CMake, the official `RaidcoreGG/Nexus-API` header repository and Dear ImGui v1.80. Nexus requires the exact ImGui 1.80 structure layout; do not change this dependency to a newer ImGui version unless Nexus updates its ABI requirements.

## Build locally (Windows x64)

Requirements: Visual Studio 2022 C++ workload, Git, and CMake 3.25 or newer.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
```

The install layout is produced at `build/package/`.

## Troubleshooting

- **Nexus does not list the addon:** confirm `addons/BoonPanel.dll` exists, is 64-bit, and was built against the current Nexus SDK header/API version.
- **ArcDPS does not list the extension:** confirm the file is named exactly `arcdps_BoonPanel.dll` and is in `bin64` (or the GW2 root directory). Confirm ArcDPS itself is installed.
- **Panel says it is waiting for events:** apply a boon to your character after both modules load. If it still does not update, check the ArcDPS log and ensure the current ArcDPS build supports the extension API revision.
- **Some boon timers/stacks are missing:** the feed is not a guaranteed full-state API; refer to the limitation above.

## Project layout

```text
src/
  arcdps_api.h       ArcDPS ABI declarations used by this project
  arc_extension.cpp ArcDPS callback, boon tracking and DLL exports
  boons.h            Boon names, IDs and stacking metadata
  nexus_addon.cpp    Nexus export and ImGui panel
  shared_state.h/.cpp Named process-local shared-memory snapshot
.github/workflows/
  build.yml          GitHub Actions Windows x64 build
CMakeLists.txt       Dependency fetch and DLL/package build
```

## License

This project is provided under the MIT License. It is an independent implementation and does not include Reffect source or assets.
