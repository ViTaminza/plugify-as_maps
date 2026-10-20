// as_maps — Plugify C++ port of the Pisex admin_system module "Maps" (Metamod plugin as_maps).
//
// What it does (unchanged from the original):
//   * "mm_map <map>"            console command, @admin/maps, five seconds later a level change
//   * admin menu item "maps"    category "server", @admin/maps, shows the maps of maps.ini;
//                                selecting one changes the map, the back button returns to the
//                                last category menu
//   * a chat announcement when core.ini's message_type is 1 (map) or 2 (admin + map)
//   * an OnAction("map", <map>) notification for the acting admin
//
// Metamod objects dropped in the port:
//   * IVEngineServer2::IsMapValid/ChangeLevel and ServerCommand -> s2sdk::IsMapValid +
//     s2sdk::ServerCommand ("changelevel" / "ds_workshop_changelevel"), still issued from a
//     five second one-shot timer exactly like the original;
//   * CCSPlayerController/SchemaEntity and the whole entity-system startup dance: the module
//     never used the entity system, it only needed the admin's name, which comes from
//     pisexcompat (IPlayersApi::GetPlayerName);
//   * g_pUtils/g_pMenus/g_pPlayers -> pisexcompat (same call sites);
//   * g_pAdminApi (IAdminApi from the admin_system Metamod plugin) -> the admin_system consumer
//     wrapper (admin_api.hpp), which keeps the method names, argument defaults and const char*
//     returns of the original.
//
// Everything runs on the game thread: the command handler and the menu callback are dispatched
// by the engine, the level change is a deferred timer. No client query happens in OnPluginStart
// (only the maps.ini file is read there, like Pisex's AllPluginsLoaded did).

#include <chrono>
#include <cstdint>
#include <exception>
#include <map>
#include <string>

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include <admin_api.hpp>
#include <pisex/pisex.hpp>

namespace {

using namespace pisex;

// The Pisex getters, same names: pisexcompat restores IUtilsApi/IMenusApi/IPlayersApi and
// admin_api.hpp restores IAdminApi (std::function callbacks, std::string, default arguments).
admin_system::AdminApi* g_pAdminApi = &admin_system::AdminApi::Get();

// "Mirage" "de_mirage" pairs of maps.ini, keyed by map file name - the original's
// std::map<std::string, std::string> g_mapMaps (map name -> display name).
std::map<std::string, std::string> g_mapMaps;

// Pisex issued the level change from CreateTimer(5.0f, ...): five seconds after the command or
// the menu selection.
constexpr float kChangeMapDelay = 5.0f;
// The log tag of the original plugin (GetLogTag()).
constexpr const char* kLogTag = "Maps";
// The original hard-coded "addons/configs/admin_system/maps.ini"; pisexcompat resolves the
// Metamod spelling to <plugify configs>/admin_system/maps.ini.
constexpr const char* kMapsConfigPath = "addons/configs/admin_system/maps.ini";

// The binding slots exist only when the admin_system plugin was loaded before us (it is declared
// as a dependency, so normally it was). Everything that has to do real work is behind this check
// because a module must never return Failed from OnPluginStart (that stops every plugin).
bool AdminApiAvailable() {
	return __admin_system_OnCoreLoaded_Register != nullptr && __admin_system_OnCoreLoaded_Unregister != nullptr &&
		   __admin_system_IsCoreLoaded != nullptr && __admin_system_RegisterCategory != nullptr &&
		   __admin_system_RegisterItem != nullptr && __admin_system_HasPermission != nullptr &&
		   __admin_system_GetTranslation != nullptr && __admin_system_GetMessageType != nullptr &&
		   __admin_system_SendAction != nullptr && __admin_system_ShowAdminLastCategoryMenu != nullptr;
}

// Reads maps.ini (KV1: `"Display name" "map_file"` pairs) into g_mapMaps. A missing or broken
// file is logged and leaves the map list empty - Pisex returned from AllPluginsLoaded the same
// way, it never failed the plugin.
void LoadMapsConfig() {
	KeyValues kv("Maps");
	if (!kv.LoadFromFile(kMapsConfigPath)) {
		g_pUtils->ErrorLog("[%s] Failed to load %s", kLogTag, kMapsConfigPath);
		return;
	}
	g_mapMaps.clear();
	FOR_EACH_VALUE(&kv, pValue) {
		const char* szMapName = pValue->GetString(nullptr);
		if (szMapName && *szMapName) {
			g_mapMaps[szMapName] = pValue->GetName();
		}
	}
}

// Runs the level change command once, five seconds later (the original created the same timer and
// returned -1.0f to stop it after the first run). No exception may escape a timer callback.
void ScheduleLevelChange(std::string command) {
	CreateTimer(kChangeMapDelay, [command = std::move(command)]() {
		try {
			if (__s2sdk_ServerCommand) {
				s2sdk::ServerCommand(plg::string(command));
			}
		} catch (...) {
			// The level change is best-effort; nothing in the plugin can react to a failure.
		}
		return -1.0f; // one-shot
	});
}

// The original: engine->IsMapValid -> "ds_workshop_changelevel <map>" through ServerCommand,
// otherwise IVEngineServer2::ChangeLevel. s2sdk has no ChangeLevel export, so both paths go
// through the equivalent server command (changelevel / ds_workshop_changelevel), still five
// seconds later.
void ChangeMap(int iSlot, const char* mapname) {
	if (!mapname || !*mapname) {
		return;
	}
	// A direct s2sdk call is not covered by the no-op degradation of pisexcompat (which guards its
	// own entry points): without the s2sdk plugin the binding slots are null and calling them
	// would crash the server.
	if (!__s2sdk_IsMapValid || !__s2sdk_ServerCommand) {
		g_pUtils->ErrorLog("[%s] the s2sdk bindings are not available - cannot change the map", kLogTag);
		return;
	}
	const std::string mapName(mapname);

	if (!s2sdk::IsMapValid(plg::string(mapName))) {
		ScheduleLevelChange("ds_workshop_changelevel " + mapName);
	} else {
		ScheduleLevelChange("changelevel " + mapName);
	}

	// core.ini "message_type": 2 = "[admin] changed the map to <name>", 1 = "<name> changed the
	// map", anything else = silent. g_mapMaps[mapName] inserts an empty display name for a map
	// that is not in maps.ini - the original did the same.
	if (g_pAdminApi->GetMessageType() == 2) {
		g_pUtils->PrintToChatAll(g_pAdminApi->GetTranslation("Changing_Map_Admin"),
								 iSlot == -1 ? "Console" : g_pPlayers->GetPlayerName(iSlot), g_mapMaps[mapName].c_str());
	} else if (g_pAdminApi->GetMessageType() == 1) {
		g_pUtils->PrintToChatAll(g_pAdminApi->GetTranslation("Changing_Map"), g_mapMaps[mapName].c_str());
	}
	g_pAdminApi->SendAction(iSlot, "map", mapName.c_str());
}

// mm_map <map> — a console command of the original.
void HandleMapCommand(int iSlot, const char* szContent) {
	const bool bConsole = iSlot == -1;
	if (!g_pAdminApi->HasPermission(iSlot, "@admin/maps") && !bConsole) {
		return;
	}
	CommandArgs args;
	args.Tokenize(szContent);
	if (args.ArgC() < 2) {
		// Pisex formatted UsageChangeMap with the command name and printed it to the caller's
		// console (a negative slot is the server console).
		const std::string text = FormatString(g_pAdminApi->GetTranslation("UsageChangeMap"), args.Arg(0));
		g_pUtils->PrintToConsole(iSlot, "%s\n", text.c_str());
		return;
	}
	ChangeMap(iSlot, args.Arg(1));
}

// The select callback of the registered "maps" item: the menu of maps.ini entries. The item
// numbers are the Pisex ones (1..5 for the current page, 7 = exit-back, 9 = exit), so the
// original's item/exit-back handling keeps working unchanged.
void OnMapsSelect(int iSlot, const char* szCategory, const char* szIdentity, const char* szItem) {
	(void)szCategory;
	(void)szIdentity;
	(void)szItem;
	Menu hMenu;
	g_pMenus->SetTitleMenu(hMenu, g_pAdminApi->GetTranslation("SelectMap"));
	for (const auto& it : g_mapMaps) {
		g_pMenus->AddItemMenu(hMenu, it.first.c_str(), it.second.c_str());
	}
	g_pMenus->SetBackMenu(hMenu, true);
	g_pMenus->SetExitMenu(hMenu, true);
	g_pMenus->SetCallback(hMenu, [](const char* szBack, const char* szFront, int iItem, int iSlot) {
		(void)szFront;
		if (iItem < 7) {
			ChangeMap(iSlot, szBack);
		} else if (iItem == 7) {
			g_pAdminApi->ShowAdminLastCategoryMenu(iSlot);
		}
	});
	g_pMenus->DisplayPlayerMenu(hMenu, iSlot);
}

class AsMapsPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

private:
	// Registers the "server" category and the "maps" item; runs when the admin core is ready
	// (OnCoreLoaded), or immediately when the core finished loading before this plugin started.
	// Idempotent: the core replaces an item registered twice, but the guard keeps the intended
	// "exactly once" behaviour.
	void RegisterAdminMenu();
	// Drops the OnCoreLoaded listener registered with the admin core.
	void UnregisterCoreCallback();

	bool m_coreCallbackRegistered = false;
	bool m_menuRegistered = false;
};

AsMapsPlugin g_asMapsPlugin;

void AsMapsPlugin::RegisterAdminMenu() {
	if (m_menuRegistered) {
		return;
	}
	m_menuRegistered = true;
	g_pAdminApi->RegisterCategory("server", "Category_Server", nullptr);
	g_pAdminApi->RegisterItem("maps", "Item_Maps", "server", "@admin/maps", nullptr, OnMapsSelect);
}

void AsMapsPlugin::UnregisterCoreCallback() {
	if (!m_coreCallbackRegistered) {
		return;
	}
	m_coreCallbackRegistered = false;
	if (__admin_system_OnCoreLoaded_Unregister) {
		g_pAdminApi->OnCoreLoadedUnregister();
	}
}

plg::PluginResult AsMapsPlugin::OnPluginStart() {
	try {
		// Config/translations dirs (<configs>/admin_system) and the s2sdk listeners pisexcompat
		// needs. Never fails hard: a missing s2sdk binding leaves the library in no-op mode.
		pisex::Init("admin_system");

		// Pisex loaded maps.ini in AllPluginsLoaded; it is a plain file read, no client or engine
		// query, so it is safe here (and needed before the menu can be built).
		LoadMapsConfig();

		if (!AdminApiAvailable()) {
			// Fail soft: returning Failed from OnPluginStart would stop every plugify plugin on
			// the server. Without the admin_system bindings there is nothing to register, so the
			// plugin stays loaded but idle (no command, no menu item).
			g_pUtils->ErrorLog("[%s] the admin_system bindings are not available (is the admin_system plugin loaded?)", kLogTag);
			return {};
		}

		// mm_map — the Metamod CON_COMMAND_F(mm_map, "", FCVAR_NONE): server console only (the
		// commented-out "!map" chat command of the original is not ported: it was commented out
		// there as well).
		g_pUtils->RegServerCommand({"mm_map"}, [](int iSlot, const char* szContent) {
			HandleMapCommand(iSlot, szContent);
			return true; // handled: no other command handler must see it
		});

		// Registration mirrors the original's OnCoreLoaded block. A core that finished loading
		// before this plugin started never fires the event (documented in the manifest), so the
		// current state is checked as well; RegisterAdminMenu() is idempotent.
		g_pAdminApi->OnCoreLoaded([] { g_asMapsPlugin.RegisterAdminMenu(); });
		m_coreCallbackRegistered = true;
		if (g_pAdminApi->IsCoreLoaded()) {
			RegisterAdminMenu();
		}

		return {};
	} catch (const std::exception& ex) {
		g_pUtils->ErrorLog("[%s] OnPluginStart failed: %s", kLogTag, ex.what());
	} catch (...) {
		g_pUtils->ErrorLog("[%s] OnPluginStart failed with an unknown exception", kLogTag);
	}
	return {};
}

plg::PluginResult AsMapsPlugin::OnPluginUpdate(std::chrono::milliseconds dt) {
	(void)dt;
	try {
		// Pumps the deferred work of pisexcompat (timers, next-frame queue, deferred startup).
		pisex::Poll();
	} catch (const std::exception& ex) {
		g_pUtils->ErrorLog("[%s] update failed: %s", kLogTag, ex.what());
	} catch (...) {
		g_pUtils->ErrorLog("[%s] update failed with an unknown exception", kLogTag);
	}
	return {};
}

plg::PluginResult AsMapsPlugin::OnPluginEnd() {
	try {
		// The core keeps a pointer to our trampoline: drop it before the library goes away.
		UnregisterCoreCallback();
		pisex::Shutdown();
	} catch (const std::exception& ex) {
		g_pUtils->ErrorLog("[%s] OnPluginEnd failed: %s", kLogTag, ex.what());
	} catch (...) {
		g_pUtils->ErrorLog("[%s] OnPluginEnd failed with an unknown exception", kLogTag);
	}
	return {};
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_asMapsPlugin)
