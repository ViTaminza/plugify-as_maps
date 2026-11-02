# as_maps (Plugify)

Maps module of the Pisex admin_system (Metamod plugin as_maps), ported to Plugify on the native s2sdk API and the admin_system plugin API (no pisexcompat). Adds the server-console 'mm_map <map>' command (@admin/maps) and the 'maps' item of the admin menu's 'server' category: the maps of maps.ini shown in an on-screen menu whose rendering follows the player's own '!menu' choice when the optional hudmenu plugin is installed, a five second delayed level change (changelevel for a server map, ds_workshop_changelevel otherwise), the Changing_Map / Changing_Map_Admin chat announcement and an OnAction('map') notification.

Plugify port (fork) of **Maps** by Pisex (Metamod (C++)).
Original: https://github.com/Pisex/cs2-admin_system_modules
The code was rewritten for Plugify / s2sdk.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugin version: 1.1.0
- Plugify dependencies: s2sdk (>=2.18.2), admin_system (>=1.1.0), hudmenu (>=1.0.0, optional)
- Self-contained s2sdk integration: uses s2sdk directly, without the pisexcompat library.

## Installation
Download `as_maps-1.1.0.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/as_maps`.

`plugins/admin_system` carry only the API headers of the plugins this one uses.

## License
GPL-3.0, see LICENSE.

