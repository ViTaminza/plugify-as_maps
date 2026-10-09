# as_maps (Plugify)

Maps module of the Pisex admin_system (Metamod plugin as_maps), ported to Plugify. Adds the 'mm_map <map>' console command (@admin/maps) and the 'maps' item of the admin menu's 'server' category: the maps of maps.ini, a five second delayed level change (changelevel / ds_workshop_changelevel, depending on the map's validity), the Changing_Map / Changing_Map_Admin chat announcement and an OnAction('map') notification.

Plugify port (fork) of **Maps** by Pisex (Metamod (C++)).
Original: https://github.com/Pisex/cs2-admin_system_modules
The code was rewritten for Plugify / s2sdk; behaviour follows the original.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugify dependencies: s2sdk, admin_system, hudmenu (optional)

## Installation
Download `as_maps-1.0.0.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/as_maps`.

`plugins/admin_system` carry only the API headers of the plugins this one uses.

## License
GPL-3.0, see LICENSE.

