// as_maps — standalone Plugify C++ port of the Pisex admin_system module "Maps" (the Metamod
// plugin as_maps), on top of the s2sdk API and the admin_system plugin API only.
//
// Behaviour of the original, kept:
//   * "mm_map <map>"            console command of the original CON_COMMAND_F(mm_map, "", FCVAR_NONE):
//                                server console only; five seconds later a level change.
//   * admin menu item "maps"    category "server", flag "@admin/maps": the maps of maps.ini in one
//                                page of five; selecting a map changes it, the back button returns
//                                to the admin core's last category menu.
//   * mapcycle / workshop       engine->IsMapValid(map) ? "changelevel <map>" :
//                                "ds_workshop_changelevel <map>", both issued five seconds after
//                                the command or the selection.
//   * chat announcement         core.ini "message_type": 2 = Changing_Map_Admin (admin name +
//                                map), 1 = Changing_Map (map), anything else = silent.
//   * OnAction                  SendAction(slot, "map", <map file name>) for the acting admin
//                                (-1 when the server console ran the command).
//   * maps.ini                  the Pisex file, read unchanged; the display name of a map that is
//                                not in maps.ini falls back to the map file name (the previous port
//                                changed the original's std::map::operator[] pollution this way).
//
// Standalone (docs/PLUGIN_STANDARD.md §7): no pisexcompat and no helper library. The optional
// hudmenu plugin is a normal plugin dependency, used only to honour the player's own "!menu"
// choice (GetClientMenuType); the map menu works without it. The guards of
// handoff/weaponskins/PITFALLS.md live in this file:
//
//   * A1/A7  OnPluginStart() never returns Failed (that stops every Plugify plugin); OnPluginEnd()
//            symmetrically removes the console command (RemoveCommand only when this plugin
//            created it, RemoveCommandListener for a pre-existing name), the OnCoreLoaded
//            callback, both map listeners, the pending level-change timer, every menu this plugin
//            created (including the reserved id 0) and drops the admin API pointer and the state.
//            The admin core has no item-unregister call, so the "maps" item is re-registered with
//            null callbacks on unload and the core no longer holds a pointer into this .so. The
//            "server" category is registered through the raw binding with a null display callback
//            from the start (the wrapper would store its CategoryDisplayTrampoline, which has no
//            unregister counterpart); only this module's own "maps" item is ever written.
//            Menus still displayed at that point are closed through CancelClientMenu when no map
//            is up, so DestroyMenu cannot reach the client layer being torn down.
//   * B1/B2  the map gate g_mapReady. No s2sdk client call is made before the first map or in the
//            window between OnMapEnd and the next OnMapStart: the map readiness is latched by
//            OnMapStart, by the first OnPluginUpdate of a plugin loaded into a running map (unless
//            an OnMapEnd was seen since OnPluginStart), and dropped by OnMapEnd. The delayed
//            change is dropped, never executed, when no map is up.
//   * C2     every client query goes through ConnectedClient/HasController: slot 0..63,
//            IsClientConnected, !IsClientSourceTV, a valid controller handle+pointer for the name;
//            the menu only opens for a connected, in-game, non-bot, non-SourceTV slot with a
//            SteamID64. s2sdk's "<unknown>" name becomes "" (pisexcompat's GetPlayerName did that).
//            Every deferred menu action also carries the SteamID64 and the controller CEntityHandle
//            (the serial catches a reused slot) captured when the action was queued, and re-checks
//            both, the admin_system 1.1.0 IsClientAdminDataReady(slot, steamid) answer (a pending
//            or failed admin SELECT makes HasPermission stale) and "@admin/maps" before it is
//            applied. A missing readiness binding is a fail-closed deny of that action.
//   * C9/B4  the level-change delay is 5 s (>= 1/64) and the timer carries TimerFlag::NoMapChange,
//            so a map change for another reason cancels the pending command instead of running a
//            second changelevel on the new map. The scheduled request keeps its author (server
//            console -1, or slot + SteamID64 + controller CEntityHandle); right before the
//            ServerCommand the author is re-validated (live map, IsRealPlayer, exact identity,
//            IsClientAdminDataReady and @admin/maps) - losing the permission, disconnecting or a
//            reused slot/controller drops the request. The console stays valid.
//   * C10    menus: s2sdk never destroys a closed menu, so this plugin destroys every menu it
//            creates; the destruction is deferred, because the menu handler runs under s2sdk's
//            menu-manager lock. A menu is only created when CreateMenu, AddMenuItem, DisplayMenu,
//            DestroyMenu, GetClientMenu, CancelClientMenu, SetMenuType and GetMenuType all exist;
//            at OnMapEnd and on an unload with no map up the display sessions are ended first with
//            CancelClientMenu(Disconnect) (s2sdk then skips the backend close that would reach the
//            client layer of PITFALLS B2) and the answer is re-checked with GetClientMenu. A
//            session that is still displayed after that gets its menu type switched to a builtin
//            ("chat", then "console"), verified with GetMenuType, because those Close callbacks are
//            no-ops; only then is the menu destroyed. If even that switch is impossible the cleanup
//            logs "A7_UNRESOLVED" and leaves the menu alone instead of running a custom backend's
//            close over the dying client layer. Menu id 0 is reserved by s2sdk as "no menu" while
//            being a valid id: the plugin burns it with a dummy menu at the first display and
//            destroys it on unload (the same workaround pisexcompat used).
//   * C11    command ownership: AddConsoleCommand() answering false means the name already existed
//            and only a listener was attached, so OnPluginEnd must detach the listener and never
//            RemoveCommand a command this plugin did not create. The command is registered
//            without ClientCanExecute (the original's FCVAR_NONE = server console only); the
//            server-console bypass is strictly caller == -1, any other caller (a client slot or an
//            out-of-range value such as -2) is refused without running anything, and a chat
//            "!mm_map" is not this command and answers Continue, leaving the chat line to the
//            engine.
//   * engine input: the map name is the only caller input that is spliced into a server command,
//            so IsSafeMapName() restricts it to [A-Za-z0-9_./-] with an alphanumeric first
//            character (no space, ';', newline, a leading '-' or a "../" path). The original
//            formatted the argument unchecked; the port keeps the changelevel behaviour and
//            refuses anything that is not a map name.
//   * mutexes the s2sdk callbacks run under: the console-command dispatch holds the concommand
//            manager's non-recursive lock, the menu handler holds the menu manager's lock and a
//            menu timeout runs inside TimerSystem::RunFrame's timer lock. ServerCommand
//            ("changelevel") re-enters the concommand dispatcher, and KillTimer/CreateTimer
//            re-enter the timer system, so no callback performs an action: the command callback,
//            the admin item callback, the menu handler and the timer callback only append to
//            g_pendingActions / set g_levelChangeDue, and OnPluginUpdate performs the actual
//            work outside all of those locks.
//
// Porting notes (Metamod -> s2sdk):
//   * IVEngineServer2::IsMapValid/ChangeLevel and engine->ServerCommand -> s2sdk::IsMapValid +
//     s2sdk::ServerCommand; the five second one-shot timer is an s2sdk timer whose callback only
//     marks the command due (see above), the ServerCommand runs from OnPluginUpdate.
//   * IUtilsApi/IMenusApi/IPlayersApi -> s2sdk directly: PrintToChatAll (with the cs2-menus colour
//     table of the phrase files), PrintToServer for the server console, GetClientName behind the
//     C2 guards, and the s2sdk menu API for the map list. The menu type follows pisexcompat's
//     ResolveS2sdkMenuTypeName() table exactly: the player's own "!menu" choice via the optional
//     hudmenu API (GetClientMenuType) is used when its backend is registered, an explicit "hud" or
//     "wasd" with a missing backend becomes the builtin "button" (never the other custom type),
//     and the HUD_LAYOUT default (no choice / unknown answer / no hudmenu) is "hud" when
//     registered, otherwise "button" - the wasd backend is never a HUD_LAYOUT fallback.
//   * IAdminApi -> the admin_system consumer wrapper (admin_api.hpp), which keeps the method
//     names, argument defaults and const char* returns of the original; the flag "@admin/maps",
//     the "server" category and the "maps"/"Item_Maps" identities are unchanged.
//   * maps.ini keeps pisexcompat's ResolvePath() order and semantics: the raw Metamod path relative
//     to the process working directory first, then <GetConfigsDir()>/admin_system/maps.ini,
//     <GetConfigsDir()>/admin_system/translations/maps.ini and <GetDataDir()>/admin_system/maps.ini.
//     The first file that *exists* is authoritative (KeyValues::LoadFromFile parsed only that one),
//     so a malformed raw copy is logged and leaves the list empty instead of being hidden by a
//     later candidate. The KV1 reader and the colour table are plugin-local headers (kv1.hpp,
//     maps_config.hpp, chat_text.hpp), like the other standalone ports; the phrase texts
//     themselves come from the admin_system core's GetTranslation (its ServerLang handling is
//     therefore unchanged).
//
// The plugin makes no random map change and runs no console command of its own; the only
// ServerCommand it ever issues is the changelevel/ds_workshop_changelevel of an explicit
// mm_map/menu request, five seconds later.

#include "chat_text.hpp"
#include "maps_config.hpp"

#include <admin_api.hpp>
#include <hudmenu/hudmenu.hpp>

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plg/vector.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr char kLogTag[] = "Maps";
// CS2 has 64 player slots; the original modules iterate 0..63.
constexpr int32_t kMaxSlots = 64;
// Pisex issued the level change from CreateTimer(5.0f, ...): five seconds after the command or the
// menu selection. 5 >= 1/64, the minimum delay of PITFALLS C9.
constexpr double kChangeMapDelay = 5.0;
// cs2-menus/Pisex showed five entries per page; the Pisex menu library numbered them 1..5.
constexpr int32_t kMenuItemsPerPage = 5;
// pisexcompat's default menu timeout (g_menuTimeout = 60): a menu that is left alone closes.
constexpr int32_t kMenuDisplayTimeSeconds = 60;

// The Metamod flags of the original CON_COMMAND_F(mm_map, "", FCVAR_NONE) with the ConVar_Register
// FCVAR_* flags of the plugin: Release makes the name visible to the engine, no ClientCanExecute
// (server-console-only, exactly what pisexcompat's RegServerCommand produced).
constexpr s2sdk::ConVarFlag kMmMapFlags =
		static_cast<s2sdk::ConVarFlag>(static_cast<int64_t>(s2sdk::ConVarFlag::Release));

// The admin_system consumer API (the IAdminApi of the original). Bound in OnPluginStart and
// dropped in OnPluginEnd, so a stale callback cannot reach an unloaded API.
admin_system::AdminApi* g_pAdminApi = nullptr;

// maps.ini: std::map keyed by the map file name, exactly like the original's
// std::map<std::string, std::string> g_mapMaps; its ordering is the menu ordering.
std::map<std::string, std::string> g_mapMaps;

// --- registrations the unload has to take back (PITFALLS A7) -------------------------------------

bool g_coreCallbackRegistered = false; // OnCoreLoaded listener of the admin core
bool g_menuRegistered = false;         // the "server"/"maps" item is registered with the core
bool g_commandRegistered = false;      // "mm_map" is hooked (created here or pre-existing)
bool g_commandCreated = false;         // AddConsoleCommand returned true: the name is ours to remove
bool g_mapListeners = false;           // OnMapStart/OnMapEnd are registered with s2sdk

// --- PITFALLS B1/B2: the map gate ----------------------------------------------------------------

bool g_mapReady = false;           // a map is up: s2sdk client/entity calls are safe
bool g_mapEndedSinceStart = false; // OnMapEnd was seen since OnPluginStart
bool g_firstUpdateDone = false;    // the mid-map latch has already been decided

// --- the delayed level change --------------------------------------------------------------------

// The request the timer carries. The command string alone is not enough: the author can lose the
// permission, disconnect or have the slot reused during the five seconds, and ProcessLevelChange()
// runs ServerCommand() later, outside the check that created the request. The author identity is
// therefore stored with the command and re-validated immediately before the engine call.
struct ScheduledChange {
	bool set = false;
	std::string command;      // "changelevel <map>" / "ds_workshop_changelevel <map>"
	int32_t slot = -1;        // -1 = server console, otherwise the acting admin's slot
	uint64_t steamid = 0;     // the author the request was made for
	int32_t controllerHandle = 0; // controller CEntityHandle (serial) at schedule time
};

bool g_levelChangeDue = false; // the timer fired; OnPluginUpdate must run the command
bool g_levelChangeTimerActive = false;
s2sdk::TimerId g_levelChangeTimer = 0;
ScheduledChange g_scheduledChange;

// --- deferred actions ----------------------------------------------------------------------------
// Every s2sdk callback (command dispatch, admin item select, menu handler, timer) runs under a
// non-recursive lock of its subsystem and must not perform the action itself (see the header
// comment). They only queue here; OnPluginUpdate drains the queue on the game thread.

enum class PendingKind : uint8_t {
	ShowMapsMenu,  // open the maps.ini menu for a slot (admin menu item select)
	SelectMap,     // a map was selected in that menu
	BackToCategory,// the menu's back button was pressed
	ChangeMap,     // "mm_map <map>" from the server console
};

struct PendingAction {
	PendingKind kind = PendingKind::ShowMapsMenu;
	int32_t slot = -1;
	uint64_t steamid = 0;        // menu actions: the identity the menu was displayed for
	int32_t controllerHandle = 0; // ... and the controller handle (CEntityHandle, includes serial)
	std::string mapName;
};

std::deque<PendingAction> g_pendingActions;

// --- the map menu --------------------------------------------------------------------------------

struct OpenMenu {
	bool active = false;
	s2sdk::MenuId id = 0;
	uint64_t steamid = 0;         // identity of the slot the menu was shown to
	int32_t controllerHandle = 0; // controller CEntityHandle (serial blocks slot reuse)
	std::vector<std::string> maps; // item index -> map file name (the info text of the item)
};

std::array<OpenMenu, kMaxSlots> g_openMenus{};
// Menus whose display ended and which still have to go to s2sdk::DestroyMenu. The handler may not
// destroy them itself (it runs under the menu manager's lock).
std::vector<s2sdk::MenuId> g_destroyQueue;
bool g_menuIdZeroChecked = false; // the reserved-id-0 workaround has run
bool g_menuIdZeroReserved = false; // this plugin owns menu id 0 and must destroy it on unload

// True from the first line of OnPluginEnd: every callback that may still be invoked for a moment
// returns without touching the plugin state.
bool g_shuttingDown = false;

// --- forward declarations ------------------------------------------------------------------------

void QueueMenuDestroy(s2sdk::MenuId id);
void RegisterAdminMenu();
void OnMapsItemSelect(int32_t slot, const char* category, const char* identity, const char* item);
void OnCoreLoadedCallback();

// --- logging -------------------------------------------------------------------------------------

void LogMessage(std::string_view message, plg::Severity severity) {
	if (!plg::Log) {
		return;
	}
	try {
		plg::Log(std::string("[") + kLogTag + "] " + std::string(message), severity,
				 plg::source_location::current());
	} catch (...) {
	}
}

void LogInfo(std::string_view message) {
	LogMessage(message, plg::Severity::Info);
}

void LogError(std::string_view message) {
	LogMessage(message, plg::Severity::Error);
}

// --- small helpers -------------------------------------------------------------------------------

std::string ToStd(const plg::string& text) {
	return std::string(text.data(), text.size());
}

bool ValidSlot(int32_t slot) {
	return slot >= 0 && slot < kMaxSlots;
}

// The map name is spliced into "changelevel <map>" / "ds_workshop_changelevel <map>" and handed to
// the engine's command buffer, so it is restricted to the characters a real map name (including a
// workshop path such as workshop/3070901615/de_dust2) is made of. This is the only input of the
// plugin that reaches the engine: a quoted argument with a space, a ';', a newline or a "../"
// path can never become part of a server command. The original built the same string from the
// argument unchecked; the port keeps the changelevel behaviour and rejects everything that is not
// a map name instead of passing it on.
bool IsSafeMapName(std::string_view name) {
	constexpr std::size_t kMaxMapNameLength = 128;
	if (name.empty() || name.size() > kMaxMapNameLength) {
		return false;
	}
	const unsigned char first = static_cast<unsigned char>(name.front());
	if (!std::isalnum(first)) {
		return false; // no leading '-', '.', '/' or a control byte
	}
	if (name.find("..") != std::string_view::npos) {
		return false; // no path traversal
	}
	for (const char c : name) {
		const unsigned char value = static_cast<unsigned char>(c);
		if (std::isalnum(value) || c == '_' || c == '-' || c == '.' || c == '/') {
			continue;
		}
		return false;
	}
	return true;
}

// printf into a std::string (pisex::FormatString of the original). The format is a phrase from the
// admin_system core, so it is not a literal and cannot be checked by the compiler.
std::string FormatString(const char* format, ...) __attribute__((format(printf, 1, 2)));
std::string FormatString(const char* format, ...) {
	if (format == nullptr) {
		return {};
	}
	va_list args;
	va_start(args, format);
	va_list copy;
	va_copy(copy, args);
	const int size = std::vsnprintf(nullptr, 0, format, copy);
	va_end(copy);
	if (size <= 0) {
		va_end(args);
		return {};
	}
	std::vector<char> buffer(static_cast<std::size_t>(size) + 1, '\0');
	std::vsnprintf(buffer.data(), buffer.size(), format, args);
	va_end(args);
	return std::string(buffer.data(), static_cast<std::size_t>(size));
}

// --- menu destroy bookkeeping --------------------------------------------------------------------

void QueueMenuDestroy(s2sdk::MenuId id) {
	// Menu id 0 is the reserved "no menu" id: this plugin keeps the dummy menu that owns it until
	// OnPluginEnd and destroys it there.
	if (id == 0) {
		return;
	}
	if (std::find(g_destroyQueue.begin(), g_destroyQueue.end(), id) != g_destroyQueue.end()) {
		return;
	}
	g_destroyQueue.push_back(id);
}

// True when a menu display session can be ended without the menu backend's close callback (which
// reaches the client layer): CancelClientMenu(Disconnect) skips it in s2sdk. A menu is only ever
// created when these bindings exist, so a missing one here means the s2sdk module itself is gone.
bool MenuSessionBindingsAvailable() {
	return __s2sdk_GetClientMenu != nullptr && __s2sdk_CancelClientMenu != nullptr &&
		   __s2sdk_DestroyMenu != nullptr;
}

// A menu type whose backend Close() has no client side effect (s2sdk's builtin menu types: the
// chat and console close callbacks are empty). Used when a display session refuses to end between
// maps: destroying the menu would otherwise run the custom backend's close (hudmenu's
// HideForSlot) over the client layer that is being torn down (PITFALLS B2).
bool SwitchMenuToSafeBuiltinType(s2sdk::MenuId id) {
	if (id == 0 || __s2sdk_SetMenuType == nullptr || __s2sdk_GetMenuType == nullptr) {
		return false;
	}
	for (const char* const typeName : {"chat", "console"}) {
		try {
			if (!s2sdk::SetMenuType(id, plg::string(typeName))) {
				continue;
			}
			if (ToStd(s2sdk::GetMenuType(id)) == typeName) {
				return true; // switched and verified
			}
		} catch (...) {
		}
	}
	return false;
}

// Ends every display session of |id| so that DestroyMenu cannot run a custom backend's close over
// a dying client layer. The reasons are handled in order:
//   1. CancelClientMenu(Disconnect) for each slot that currently displays the menu. Even a false
//      answer is re-checked with GetClientMenu (the session may have ended anyway).
//   2. If the menu is still displayed, switch its type to a builtin whose Close is a no-op and
//      verify with GetMenuType; DestroyMenu then invokes that empty close.
// Returns false when the display cannot be ended safely (A7_UNRESOLVED): the caller must not
// destroy the menu then. With a map up no such care is needed (clients are valid and the backend
// close is what hides the UI), so this is only used for the map-down/unload path.
bool EndMenuSessionsForSafeDestroy(s2sdk::MenuId id) {
	if (id == 0) {
		return true;
	}
	if (__s2sdk_GetClientMenu == nullptr || __s2sdk_CancelClientMenu == nullptr) {
		LogError("A7_UNRESOLVED: menu " + std::to_string(id) +
				 " cannot be checked/cancelled (s2sdk session bindings missing); not destroying it");
		return false;
	}
	for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
		try {
			if (s2sdk::GetClientMenu(slot) != id) {
				continue;
			}
			s2sdk::CancelClientMenu(slot, s2sdk::MenuCancelReason::Disconnect);
			// CancelClientMenu's answer is not trusted on its own: a session that is still
			// displayed after the call must get the builtin-type fallback below.
			if (s2sdk::GetClientMenu(slot) != id) {
				continue;
			}
		} catch (...) {
			// A throwing query is treated as "still displayed".
		}
		if (!SwitchMenuToSafeBuiltinType(id)) {
			LogError("A7_UNRESOLVED: menu " + std::to_string(id) +
					 " is still displayed but its backend cannot be switched to a builtin type; not destroying it");
			return false;
		}
	}
	return true;
}

// Frees a menu whose display sessions are over. Only safe after EndMenuSessionsForSafeDestroy()
// (or while a map is up, where the backend close is what hides the UI).
void DestroyMenuNow(s2sdk::MenuId id) {
	if (id == 0 || __s2sdk_DestroyMenu == nullptr) {
		return;
	}
	try {
		if (!__s2sdk_IsValidMenu || s2sdk::IsValidMenu(id)) {
			s2sdk::DestroyMenu(id);
		}
	} catch (...) {
	}
}

// --- PITFALLS B1/B2: the map lifecycle callbacks -------------------------------------------------
// Plain function pointers called on the game thread; they must not throw and OnMapEnd must not
// touch a client (PITFALLS B2).

void OnMapStartListener() {
	// From here on the clients and entities of the new map are in place.
	g_mapReady = true;
	// The NoMapChange timer of the previous map is gone, and a command it carried belongs to that
	// map: nothing may be issued against the new one.
	g_levelChangeDue = false;
	g_levelChangeTimerActive = false;
	g_scheduledChange = ScheduledChange{};
}

void OnMapEndListener() {
	// From here until the next map start every s2sdk client query can crash (PITFALLS B2). The
	// s2sdk timer system removes NoMapChange timers *before* it calls this listener, so the timer
	// must not (and cannot) be killed here - only the state is dropped.
	g_mapReady = false;
	g_mapEndedSinceStart = true;
	g_levelChangeDue = false;
	g_levelChangeTimerActive = false;
	g_scheduledChange = ScheduledChange{};

	// Every menu display session is ended first with the Disconnect reason (s2sdk then skips the
	// menu backend's close callback, which would reach the client layer being torn down), and the
	// menu is freed right after: no menu survives a map change. Without the session bindings the
	// destroy is queued for the next map instead of risking a backend close here.
	std::vector<s2sdk::MenuId> ids;
	ids.reserve(static_cast<std::size_t>(kMaxSlots) + g_destroyQueue.size());
	for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
		OpenMenu& menu = g_openMenus[static_cast<std::size_t>(slot)];
		if (!menu.active) {
			continue;
		}
		ids.push_back(menu.id);
		menu = OpenMenu{};
	}
	ids.insert(ids.end(), g_destroyQueue.begin(), g_destroyQueue.end());
	g_destroyQueue.clear();
	std::sort(ids.begin(), ids.end());
	ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

	const bool sessionsAvailable = MenuSessionBindingsAvailable();
	std::vector<s2sdk::MenuId> retry;
	for (const s2sdk::MenuId id : ids) {
		if (id == 0) {
			continue;
		}
		if (!sessionsAvailable) {
			retry.push_back(id);
			continue;
		}
		if (!EndMenuSessionsForSafeDestroy(id)) {
			// A7_UNRESOLVED (already logged): the menu is still displayed and cannot be given a
			// builtin no-op Close. Keep it for the next map start / OnPluginEnd instead of running
			// the custom backend's close over the dying client layer.
			retry.push_back(id);
			continue;
		}
		DestroyMenuNow(id);
	}
	// The Cancel/End callbacks the destroys just delivered may have queued the same ids again; the
	// ids that were destroyed are dropped, the unresolved ones stay queued for a retry.
	g_destroyQueue = std::move(retry);
}

// --- PITFALLS C2: the client guards --------------------------------------------------------------

// The map gate and the slot range are the only requirements for a function that addresses a
// client without reading its state.
bool CanQueryClient(int32_t slot) {
	return g_mapReady && ValidSlot(slot);
}

// A connected slot with a client object that is not the SourceTV proxy. This is pisexcompat's
// ClientExists() - the guard of the original's GetPlayerName.
bool ConnectedClient(int32_t slot) {
	if (!CanQueryClient(slot) || !__s2sdk_IsClientConnected || !__s2sdk_IsClientSourceTV) {
		return false;
	}
	return s2sdk::IsClientConnected(slot) && !s2sdk::IsClientSourceTV(slot);
}

// ... and the slot has a controller entity, i.e. a name to read: pisexcompat's ControllerExists(),
// including the C2 checks of the handle and of the pointer.
bool HasController(int32_t slot) {
	if (!ConnectedClient(slot) || !__s2sdk_PlayerSlotToEntPointer || !__s2sdk_PlayerSlotToEntHandle ||
		!__s2sdk_IsValidEntHandle || !__s2sdk_IsValidEntPointer) {
		return false;
	}
	const int32_t handle = s2sdk::PlayerSlotToEntHandle(slot);
	void* const pointer = s2sdk::PlayerSlotToEntPointer(slot);
	return pointer != nullptr && s2sdk::IsValidEntHandle(handle) && s2sdk::IsValidEntPointer(pointer);
}

// The controller CEntityHandle of the slot (index | serial << 15): a deferred menu action stores it
// and re-checks it before acting, so a recycled slot (new player or a new controller of the same
// player) can never inherit an old selection. 0 = no valid controller.
int32_t ControllerHandle(int32_t slot) {
	if (!HasController(slot) || !__s2sdk_PlayerSlotToEntHandle || !__s2sdk_IsValidEntHandle) {
		return 0;
	}
	const int32_t handle = s2sdk::PlayerSlotToEntHandle(slot);
	return s2sdk::IsValidEntHandle(handle) ? handle : 0;
}

// The permission of the "maps" admin item is re-checked when a deferred action is applied (the
// admin core checked it when it built the category menu, but the rights can be reloaded while the
// map menu is open). Only the server console (-1) is the reported bypass; any other out-of-range
// slot is refused instead of being treated like the console.
bool HasMapsPermission(int32_t slot) {
	if (slot == -1) {
		return true;
	}
	if (!ValidSlot(slot) || g_pAdminApi == nullptr) {
		return false;
	}
	return g_pAdminApi->HasPermission(slot, "@admin/maps");
}

// admin_system 1.1.0's authoritative identity check: true only when the admin record loaded for
// |slot| belongs to exactly |steamid|. The core answers false while its asynchronous SELECT is
// pending, after a failed load, and for a steamid the current answer was not computed for (a slot
// that just changed hands), so HasPermission may be stale without it. A missing binding (an older
// core) or a throwing call is a fail-closed deny of the critical action, never a fallback to the
// stale checks; the server console (-1) has no admin record and is handled by the caller.
bool AdminDataReady(int32_t slot, uint64_t steamid) {
	if (slot == -1) {
		return true;
	}
	if (!ValidSlot(slot) || steamid == 0 || g_pAdminApi == nullptr) {
		return false;
	}
	if (__admin_system_IsClientAdminDataReady == nullptr) {
		LogError("the loaded admin_system has no IsClientAdminDataReady (API 1.1.0) - refusing the admin action");
		return false;
	}
	try {
		return g_pAdminApi->IsClientAdminDataReady(slot, steamid);
	} catch (const std::exception& ex) {
		LogError(std::string("IsClientAdminDataReady failed: ") + ex.what());
	} catch (...) {
		LogError("IsClientAdminDataReady failed with an unknown exception");
	}
	return false;
}

// The name of the original's IPlayersApi::GetPlayerName(): read only with a controller entity, ""
// for s2sdk's "<unknown>" placeholder (pisexcompat returned "" there as well).
std::string PlayerName(int32_t slot) {
	if (!HasController(slot) || !__s2sdk_GetClientName) {
		return {};
	}
	try {
		const plg::string name = s2sdk::GetClientName(slot);
		std::string out(name.data(), name.size());
		if (out == "<unknown>") {
			out.clear();
		}
		return out;
	} catch (...) {
		return {};
	}
}

uint64_t ClientSteamID64(int32_t slot) {
	if (!ConnectedClient(slot) || !__s2sdk_GetClientSteamID64) {
		return 0;
	}
	try {
		return s2sdk::GetClientSteamID64(slot);
	} catch (...) {
		return 0;
	}
}

// A slot that may be shown the map menu / whose selection may act: connected, in game, not a bot,
// not SourceTV, with a real SteamID64 (the identity of the menu).
bool IsRealPlayer(int32_t slot) {
	if (!ConnectedClient(slot) || !__s2sdk_IsClientInGame || !__s2sdk_IsFakeClient ||
		!__s2sdk_GetClientSteamID64) {
		return false;
	}
	return s2sdk::IsClientInGame(slot) && !s2sdk::IsFakeClient(slot) && s2sdk::GetClientSteamID64(slot) != 0;
}

// --- chat output ---------------------------------------------------------------------------------

// pisexcompat's PrintToChatAll: the formatted phrase goes through the cs2-menus colour table.
void PrintChatAll(std::string_view text) {
	if (!g_mapReady || !__s2sdk_PrintToChatAll) {
		return;
	}
	try {
		s2sdk::PrintToChatAll(plg::string(as_maps::Colorize(text)));
	} catch (...) {
	}
}

std::string DisplayNameFor(const std::string& mapName) {
	const auto it = g_mapMaps.find(mapName);
	return it != g_mapMaps.end() ? it->second : mapName;
}

// --- the admin_system registration ---------------------------------------------------------------

// Every binding the consumer wrapper and this plugin use. The wrapper's own detail::Available()
// keys on GetPluginVersion, so it is part of the check; without these the plugin stays loaded but
// idle instead of failing OnPluginStart (PITFALLS A1).
bool AdminApiAvailable() {
	return __admin_system_GetPluginVersion != nullptr && __admin_system_OnCoreLoaded_Register != nullptr &&
		   __admin_system_OnCoreLoaded_Unregister != nullptr && __admin_system_IsCoreLoaded != nullptr &&
		   __admin_system_RegisterCategory != nullptr && __admin_system_RegisterItem != nullptr &&
		   __admin_system_HasPermission != nullptr && __admin_system_GetTranslation != nullptr &&
		   __admin_system_GetMessageType != nullptr && __admin_system_SendAction != nullptr &&
		   __admin_system_ShowAdminLastCategoryMenu != nullptr;
}

// The original's OnCoreLoaded block: the "server" category and the "maps" item. Idempotent: the
// core replaces an item registered twice, but the guard keeps the intended "exactly once" behaviour.
void RegisterAdminMenu() {
	if (g_menuRegistered || g_pAdminApi == nullptr) {
		return;
	}
	g_menuRegistered = true;
	// The category is registered through the raw generated binding with a null display callback.
	// The consumer wrapper would always hand over its CategoryDisplayTrampoline (code in this .so)
	// even for a null std::function, and the core has no way to unregister a category: after
	// `plg reload` it would keep calling into the unloaded library (PITFALLS A7). There is no
	// category display callback of this module anyway, and the core checks for null before calling.
	if (__admin_system_RegisterCategory != nullptr) {
		::admin_system::RegisterCategory(plg::string("server"), plg::string("Category_Server"), nullptr);
	}
	// The item select callback is this module's code, so the consumer wrapper is used here (it
	// adapts std::function/const char* to the boundary); OnPluginEnd retires the item with null
	// callbacks through the raw binding (RetireAdminMenuRegistration), which removes the pointers
	// into this .so. Only this module's own "maps" item is touched.
	g_pAdminApi->RegisterItem("maps", "Item_Maps", "server", "@admin/maps", nullptr, &OnMapsItemSelect);
}

void OnCoreLoadedCallback() {
	if (g_shuttingDown || g_pAdminApi == nullptr) {
		return;
	}
	RegisterAdminMenu();
}

// The select callback of the "maps" item. It is invoked by the admin core from its menu handler,
// i.e. under s2sdk's menu-manager lock: only the identity of the slot is captured and the display
// is queued; the menu API is never called here.
void OnMapsItemSelect(int32_t slot, const char* category, const char* identity, const char* item) {
	(void)category;
	(void)identity;
	(void)item;
	if (g_shuttingDown || g_pAdminApi == nullptr || !g_mapReady || !ValidSlot(slot)) {
		return;
	}
	try {
		// The player who opened the admin menu: SteamID64 + controller handle are the identity the
		// deferred display is applied for (the slot may be reused before OnPluginUpdate runs).
		const uint64_t steamid = ClientSteamID64(slot);
		const int32_t controllerHandle = ControllerHandle(slot);
		if (steamid == 0 || controllerHandle == 0) {
			return;
		}
		g_pendingActions.push_back(
				PendingAction{PendingKind::ShowMapsMenu, slot, steamid, controllerHandle, std::string()});
	} catch (...) {
	}
}

// --- the s2sdk menu ------------------------------------------------------------------------------

// A no-op handler for the dummy menu that owns the reserved id 0. The menu is never displayed.
void OnReservedMenuHandler(s2sdk::MenuId, s2sdk::MenuAction, int32_t, int32_t) {}

// s2sdk's client-menu state uses id 0 for "no menu", but 0 is also a valid CreateMenu() answer
// (ids start at 0). The plugin burns the id with a menu nobody can display, exactly like
// pisexcompat's BurnMenuIdZero(); the dummy is destroyed in OnPluginEnd.
bool EnsureMenuIdZeroBurned() {
	if (g_menuIdZeroChecked) {
		return true;
	}
	g_menuIdZeroChecked = true;
	if (!__s2sdk_CreateMenu || !__s2sdk_DestroyMenu) {
		return false;
	}
	try {
		const s2sdk::MenuId id = s2sdk::CreateMenu(plg::string("as_maps:reserved-id-0"),
												   &OnReservedMenuHandler, plg::string("chat"));
		if (id == 0) {
			g_menuIdZeroReserved = true;
		} else {
			// Another plugin already burned id 0: the dummy is not needed.
			s2sdk::DestroyMenu(id);
		}
		return true;
	} catch (...) {
		return false;
	}
}

// pisexcompat's ResolveS2sdkMenuTypeName (libs/pisexcompat/src/menu.cpp), table preserved exactly:
//   * the player's own "!menu" choice from the optional hudmenu plugin:
//       "chat"/"centerhtml"/"button" -> that builtin type;
//       "hud"  -> "hud" when registered, otherwise "button" (never another custom type);
//       "wasd" -> "wasd" when registered, otherwise "button" (never "hud");
//       anything else -> the HUD_LAYOUT default below;
//   * HUD_LAYOUT default -> "hud" when registered, otherwise "button" (the wasd backend is never
//     used as a fallback for HUD_LAYOUT).
// The plugin declares hudmenu as an optional dependency only for the player choice; without it,
// __hudmenu_GetClientMenuType is null and the HUD_LAYOUT default applies. Empty = the menu
// subsystem is disabled (not even the builtin button type exists).
bool IsMenuTypeRegistered(const char* name) {
	if (__s2sdk_IsMenuTypeRegistered == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsMenuTypeRegistered(plg::string(name));
	} catch (...) {
		return false;
	}
}

std::string PlayerMenuTypeChoice(int32_t slot) {
	if (__hudmenu_GetClientMenuType == nullptr || !g_mapReady || !ValidSlot(slot)) {
		return {};
	}
	try {
		return ToStd(hudmenu::GetClientMenuType(slot));
	} catch (...) {
		return {};
	}
}

std::string ResolveMenuType(int32_t slot) {
	const bool hudRegistered = IsMenuTypeRegistered("hud");
	const bool wasdRegistered = IsMenuTypeRegistered("wasd");
	if (!hudRegistered && !wasdRegistered && !IsMenuTypeRegistered("button")) {
		return {}; // the menu subsystem (and its builtin button type) is disabled
	}

	const std::string choice = PlayerMenuTypeChoice(slot);
	if (!choice.empty()) {
		if (choice == "chat" || choice == "centerhtml" || choice == "button") {
			return choice; // always-registered builtins
		}
		if (choice == "hud") {
			return hudRegistered ? std::string("hud") : std::string("button");
		}
		if (choice == "wasd") {
			return wasdRegistered ? std::string("wasd") : std::string("button");
		}
		// An unknown/stale answer falls through to the HUD_LAYOUT default.
	}
	return hudRegistered ? std::string("hud") : std::string("button");
}

// The handler of the map menu. It runs under the menu manager's lock (and, for a timeout, inside
// TimerSystem::RunFrame's lock), so it neither touches the menu API nor runs any engine command:
// every outcome is queued for OnPluginUpdate.
void OnMapsMenuHandler(s2sdk::MenuId id, s2sdk::MenuAction action, int32_t slot, int32_t param) {
	try {
		if (g_shuttingDown || !ValidSlot(slot)) {
			return;
		}
		OpenMenu& menu = g_openMenus[static_cast<std::size_t>(slot)];
		switch (action) {
		case s2sdk::MenuAction::Start:
			return;
		case s2sdk::MenuAction::Select: {
			if (!menu.active || menu.id != id) {
				return; // a stale session (the slot was reused or the menu replaced)
			}
			if (param < 0 || static_cast<std::size_t>(param) >= menu.maps.size()) {
				return;
			}
			const PendingAction pending{PendingKind::SelectMap, slot, menu.steamid, menu.controllerHandle,
										menu.maps[static_cast<std::size_t>(param)]};
			g_pendingActions.push_back(pending);
			menu = OpenMenu{}; // forget the display before the engine's End callback arrives
			QueueMenuDestroy(id);
			return;
		}
		case s2sdk::MenuAction::Cancel: {
			const auto reason = static_cast<s2sdk::MenuCancelReason>(param);
			if (menu.active && menu.id == id) {
				if (reason == s2sdk::MenuCancelReason::ExitBack) {
					// The "back" slot of the s2sdk menu: return to the admin core's category menu,
					// like the original's item 7.
					g_pendingActions.push_back(PendingAction{PendingKind::BackToCategory, slot, menu.steamid,
															 menu.controllerHandle, std::string()});
				}
				menu = OpenMenu{};
			}
			QueueMenuDestroy(id);
			return;
		}
		case s2sdk::MenuAction::End:
			// The display session is fully closed; End always follows Cancel (and is the only event
			// for a disconnect or a destroy).
			if (menu.active && menu.id == id) {
				menu = OpenMenu{};
			}
			QueueMenuDestroy(id);
			return;
		}
	} catch (...) {
	}
}

// Builds and displays the maps.ini menu for a slot. Runs from OnPluginUpdate, outside the menu
// manager's lock, so the s2sdk menu API is safe here. The identity and the controller handle the
// deferred action carried are checked again: a reused slot never gets the previous player's menu,
// and the "@admin/maps" permission is re-checked because the admin core can reload it while the
// admin menu is open.
bool DisplayMapsMenu(int32_t slot, uint64_t steamid, int32_t controllerHandle) {
	if (g_shuttingDown || g_pAdminApi == nullptr || !g_mapReady || !IsRealPlayer(slot)) {
		return false;
	}
	if (steamid == 0 || controllerHandle == 0 || ClientSteamID64(slot) != steamid ||
		ControllerHandle(slot) != controllerHandle) {
		return false;
	}
	if (!AdminDataReady(slot, steamid) || !HasMapsPermission(slot)) {
		return false;
	}
	// Every call the display path and its teardown need must exist before the first menu (the
	// reserved id 0 included): a missing CancelClientMenu or SetMenuType/GetMenuType would leave
	// the map-end/unload path unable to end a still-displayed session before DestroyMenu, whose
	// backend close would then touch the dying client layer (PITFALLS B2).
	if (!__s2sdk_CreateMenu || !__s2sdk_AddMenuItem || !__s2sdk_DisplayMenu || !__s2sdk_DestroyMenu ||
		!__s2sdk_GetClientMenu || !__s2sdk_CancelClientMenu || !__s2sdk_SetMenuType ||
		!__s2sdk_GetMenuType || !__s2sdk_IsMenuTypeRegistered) {
		LogError("the s2sdk menu bindings are incomplete - the map menu is disabled");
		return false;
	}
	const std::string menuType = ResolveMenuType(slot);
	if (menuType.empty()) {
		LogError("no s2sdk menu type is registered (menus are disabled in the s2sdk config)");
		return false;
	}
	// Only burn id 0 when the display can actually happen; a disabled menu subsystem must not
	// leave a reserved menu behind.
	if (!EnsureMenuIdZeroBurned()) {
		LogError("could not reserve the s2sdk menu id 0 - the map menu is disabled");
		return false;
	}

	// Replace an older display of this slot. Its handler gets Interrupted/End and queues the
	// destroy; forgetting the state first keeps the stale events from touching the new menu.
	if (g_openMenus[static_cast<std::size_t>(slot)].active) {
		QueueMenuDestroy(g_openMenus[static_cast<std::size_t>(slot)].id);
		g_openMenus[static_cast<std::size_t>(slot)] = OpenMenu{};
	}

	const char* const title = g_pAdminApi->GetTranslation("SelectMap");
	const s2sdk::MenuId id = s2sdk::CreateMenu(plg::string(title != nullptr ? title : ""),
											   &OnMapsMenuHandler, plg::string(menuType));

	OpenMenu state;
	state.active = true;
	state.id = id;
	state.steamid = steamid;
	state.controllerHandle = controllerHandle;
	state.maps.reserve(g_mapMaps.size());
	for (const auto& [mapName, displayName] : g_mapMaps) {
		s2sdk::AddMenuItem(id, plg::string(mapName), plg::string(displayName), s2sdk::MenuItemStyle::Default);
		state.maps.push_back(mapName);
	}
	if (__s2sdk_SetMenuPagination) {
		s2sdk::SetMenuPagination(id, kMenuItemsPerPage); // five entries per page, like cs2-menus
	}
	if (__s2sdk_SetMenuExitButton) {
		s2sdk::SetMenuExitButton(id, true); // the original's SetExitMenu(hMenu, true)
	}
	if (__s2sdk_SetMenuExitBackButton) {
		// The back button takes over the exit slot in s2sdk's menu model and reports ExitBack:
		// it is the original's item 7 (ShowAdminLastCategoryMenu).
		s2sdk::SetMenuExitBackButton(id, true);
	}
	g_openMenus[static_cast<std::size_t>(slot)] = std::move(state);

	if (!s2sdk::DisplayMenu(id, slot, kMenuDisplayTimeSeconds)) {
		LogError("s2sdk could not display the map menu on slot " + std::to_string(slot));
		g_openMenus[static_cast<std::size_t>(slot)] = OpenMenu{};
		QueueMenuDestroy(id);
		return false;
	}
	return true;
}

// Destroys the menus whose display ended. Runs from OnPluginUpdate (outside every menu handler),
// and only with a map up: between maps DestroyMenu would reach the client layer through the menu
// type backend, so those ids wait for the next map (or OnPluginEnd, where nothing is displayed).
void ProcessMenuDestroyQueue() {
	if (!g_mapReady || g_destroyQueue.empty() || !__s2sdk_DestroyMenu) {
		return;
	}
	std::vector<s2sdk::MenuId> ids;
	ids.swap(g_destroyQueue);
	for (const s2sdk::MenuId id : ids) {
		if (id == 0) {
			continue;
		}
		try {
			if (__s2sdk_IsValidMenu && !s2sdk::IsValidMenu(id)) {
				continue;
			}
			s2sdk::DestroyMenu(id);
		} catch (...) {
		}
	}
}

// --- the level change ----------------------------------------------------------------------------

// The s2sdk timer callback. It runs inside TimerSystem::RunFrame with the timer system's
// non-recursive mutex held, so it only marks the command due; OnPluginUpdate runs the
// ServerCommand after the callback returned and the lock is gone.
void OnLevelChangeTimer(s2sdk::TimerId id, const plg::vector<plg::any>& /*userData*/) {
	try {
		if (g_shuttingDown || !g_levelChangeTimerActive || id != g_levelChangeTimer) {
			return; // a replaced/old timer: its command is no longer wanted
		}
		g_levelChangeTimerActive = false; // s2sdk erases a one-shot timer after this call
		g_levelChangeDue = true;
	} catch (...) {
	}
}

// Schedules the change for the author it was requested by. The command executes five seconds later
// only if that author is still the same, ready and permitted at that point (ProcessLevelChange).
void ScheduleLevelChange(const std::string& command, int32_t slot, uint64_t steamid, int32_t controllerHandle) {
	// The timer create/kill pair is required together: a timer whose id cannot be killed on unload
	// would carry a pointer into the unloaded .so (PITFALLS A7).
	if (!__s2sdk_CreateTimer || !__s2sdk_KillsTimer) {
		LogError("the s2sdk timer Create/Kill pair is unavailable - the map is not changed");
		return;
	}
	// A second mm_map/menu selection within the five seconds replaces the pending command (and
	// kills the old timer); the original left both timers running and issued two changelevels.
	if (g_levelChangeTimerActive && __s2sdk_KillsTimer) {
		try {
			s2sdk::KillsTimer(g_levelChangeTimer);
		} catch (...) {
		}
	}
	g_levelChangeTimerActive = false;
	g_levelChangeDue = false;
	g_scheduledChange = ScheduledChange{true, command, slot, steamid, controllerHandle};
	try {
		g_levelChangeTimer = s2sdk::CreateTimer(kChangeMapDelay, &OnLevelChangeTimer,
												s2sdk::TimerFlag::NoMapChange, plg::vector<plg::any>{});
		g_levelChangeTimerActive = true;
	} catch (...) {
		g_scheduledChange = ScheduledChange{};
		LogError("s2sdk::CreateTimer failed - the map is not changed");
	}
}

// The author of a scheduled change is re-checked right before the engine command: the five seconds
// are long enough for the admin to lose the permission, disconnect, or for the slot to be reused
// (including a new controller entity of the same player). The console (-1) has no admin record and
// stays valid. Any failure drops the request, including the s2sdk::ServerCommand call itself.
bool AuthorStillMayChangeMap(const ScheduledChange& request) {
	if (request.slot == -1) {
		return true; // server console request
	}
	if (!ValidSlot(request.slot)) {
		LogError("dropping the delayed level change: the author slot is out of range");
		return false;
	}
	if (!IsRealPlayer(request.slot) || request.steamid == 0 ||
		ClientSteamID64(request.slot) != request.steamid) {
		LogError("dropping the delayed level change: the author left or the slot was reused");
		return false;
	}
	if (request.controllerHandle == 0 || ControllerHandle(request.slot) != request.controllerHandle) {
		LogError("dropping the delayed level change: the author's controller changed");
		return false;
	}
	if (!AdminDataReady(request.slot, request.steamid)) {
		LogError("dropping the delayed level change: the admin data of the author is not ready");
		return false;
	}
	if (!HasMapsPermission(request.slot)) {
		LogError("dropping the delayed level change: the author lost @admin/maps");
		return false;
	}
	return true;
}

void ProcessLevelChange() {
	if (!g_levelChangeDue) {
		return;
	}
	g_levelChangeDue = false;
	const ScheduledChange request = std::move(g_scheduledChange);
	g_scheduledChange = ScheduledChange{};
	if (!request.set || request.command.empty()) {
		return;
	}
	if (!g_mapReady) {
		LogError("dropping the delayed level change: no map is up");
		return;
	}
	if (!AuthorStillMayChangeMap(request)) {
		return;
	}
	if (!__s2sdk_ServerCommand) {
		LogError("the s2sdk ServerCommand binding is unavailable - the map is not changed");
		return;
	}
	try {
		s2sdk::ServerCommand(plg::string(request.command));
	} catch (const std::exception& ex) {
		LogError(std::string("ServerCommand failed: ") + ex.what());
	} catch (...) {
		LogError("ServerCommand failed with an unknown exception");
	}
}

// ChangeMap() of the original: IsMapValid decides between "changelevel" and
// "ds_workshop_changelevel"; the command is issued five seconds later. Runs from OnPluginUpdate.
void ChangeMap(int32_t slot, uint64_t steamid, int32_t controllerHandle, const std::string& mapName) {
	if (g_shuttingDown || g_pAdminApi == nullptr || mapName.empty() || !g_mapReady) {
		return;
	}
	if (!IsSafeMapName(mapName)) {
		LogError("refusing an invalid map name: '" + mapName + "'");
		return;
	}
	if (slot != -1) {
		// A menu lives up to a minute: the slot can belong to another player by now. The menu
		// carries the SteamID64 and the controller CEntityHandle it was displayed for, and both
		// the live slot and the identity are re-checked when the selection is processed (C2, slot
		// reuse); the "@admin/maps" permission is re-checked as well.
		if (!IsRealPlayer(slot) || ClientSteamID64(slot) != steamid || ClientSteamID64(slot) == 0) {
			return;
		}
		if (controllerHandle == 0 || ControllerHandle(slot) != controllerHandle) {
			return;
		}
		if (!AdminDataReady(slot, steamid) || !HasMapsPermission(slot)) {
			return;
		}
	}
	if (!__s2sdk_IsMapValid || !__s2sdk_ServerCommand || !__s2sdk_CreateTimer || !__s2sdk_KillsTimer) {
		LogError("the s2sdk engine/timer bindings are unavailable - cannot change the map");
		return;
	}

	bool valid = false;
	try {
		valid = s2sdk::IsMapValid(plg::string(mapName));
	} catch (...) {
		return;
	}
	const std::string command = (valid ? "changelevel " : "ds_workshop_changelevel ") + mapName;

	// The original scheduled the timer first, then announced the change and sent the action. The
	// request remembers its author; the ServerCommand itself is only issued after the author is
	// re-checked five seconds later (ProcessLevelChange/AuthorStillMayChangeMap).
	ScheduleLevelChange(command, slot, steamid, controllerHandle);

	const std::string displayName = DisplayNameFor(mapName);
	const int messageType = g_pAdminApi->GetMessageType();
	if (messageType == 2) {
		const std::string adminName = slot == -1 ? std::string("Console") : PlayerName(slot);
		PrintChatAll(FormatString(g_pAdminApi->GetTranslation("Changing_Map_Admin"), adminName.c_str(),
								  displayName.c_str()));
	} else if (messageType == 1) {
		PrintChatAll(FormatString(g_pAdminApi->GetTranslation("Changing_Map"), displayName.c_str()));
	}
	g_pAdminApi->SendAction(slot, "map", mapName.c_str());
}

// The original's item 7: ShowAdminLastCategoryMenu of the acting admin.
void ShowLastCategory(int32_t slot, uint64_t steamid, int32_t controllerHandle) {
	if (g_shuttingDown || g_pAdminApi == nullptr || !g_mapReady || !ValidSlot(slot) || !IsRealPlayer(slot)) {
		return;
	}
	if (steamid == 0 || ClientSteamID64(slot) != steamid ||
		controllerHandle == 0 || ControllerHandle(slot) != controllerHandle) {
		return;
	}
	if (!AdminDataReady(slot, steamid) || !HasMapsPermission(slot)) {
		return;
	}
	g_pAdminApi->ShowAdminLastCategoryMenu(slot);
}

// --- the command ---------------------------------------------------------------------------------

// "mm_map <map>": the original's CON_COMMAND_F, registered server-console-only (the Pisex
// RegServerCommand semantics of the previous port). A client caller is refused without running
// anything; a chat "!mm_map" is not this command and is left to the engine.
s2sdk::ResultType OnMmMapCommand(int32_t caller, s2sdk::ConCommandContext context,
								 const plg::vector<plg::string>& args) {
	try {
		if (context == s2sdk::ConCommandContext::Chat) {
			return s2sdk::ResultType::Continue;
		}
		if (caller != -1) {
			return s2sdk::ResultType::Handled;
		}
		if (g_shuttingDown || g_pAdminApi == nullptr || args.empty() || args[0].empty()) {
			return s2sdk::ResultType::Handled;
		}
		if (args.size() < 2) {
			// Pisex printed UsageChangeMap through the caller's console; a negative slot is the
			// server console (META_CONPRINTF of the original), where the colour tags mean nothing.
			const std::string usage =
					FormatString(g_pAdminApi->GetTranslation("UsageChangeMap"), ToStd(args[0]).c_str());
			if (__s2sdk_PrintToServer) {
				try {
					s2sdk::PrintToServer(plg::string(as_maps::StripColorTags(usage)));
				} catch (...) {
				}
			}
			return s2sdk::ResultType::Handled;
		}
		const std::string mapName = ToStd(args[1]);
		if (!mapName.empty()) {
			// The change is announced and scheduled from OnPluginUpdate: this callback runs under
			// the concommand manager's lock, and ServerCommand("changelevel") would re-enter it.
			g_pendingActions.push_back(PendingAction{PendingKind::ChangeMap, -1, 0, 0, mapName});
		}
	} catch (const std::exception& ex) {
		LogError(std::string("mm_map failed: ") + ex.what());
	} catch (...) {
		LogError("mm_map failed with an unknown exception");
	}
	return s2sdk::ResultType::Handled;
}

// --- the deferred queue --------------------------------------------------------------------------

void ProcessPendingActions() {
	// Bounded: a menu handler cannot produce an endless stream, but the update must stay bounded
	// even if a stale callback keeps queueing.
	for (int processed = 0; processed < 256 && !g_pendingActions.empty(); ++processed) {
		PendingAction action = std::move(g_pendingActions.front());
		g_pendingActions.pop_front();
		switch (action.kind) {
		case PendingKind::ShowMapsMenu:
			DisplayMapsMenu(action.slot, action.steamid, action.controllerHandle);
			break;
		case PendingKind::SelectMap:
			ChangeMap(action.slot, action.steamid, action.controllerHandle, action.mapName);
			break;
		case PendingKind::BackToCategory:
			ShowLastCategory(action.slot, action.steamid, action.controllerHandle);
			break;
		case PendingKind::ChangeMap:
			ChangeMap(-1, 0, 0, action.mapName);
			break;
		}
	}
}

// Destroys every menu this plugin still owns. Called from OnPluginEnd with g_shuttingDown set, so
// the handlers return immediately and no callback enters the half-unloaded plugin.
void DestroyOneMenu(s2sdk::MenuId id) {
	if (id == 0 || __s2sdk_DestroyMenu == nullptr) {
		return;
	}
	// With a map up the clients are valid, so DestroyMenu may run the menu backend's close callback
	// (that is what hides the on-screen menu). Between maps the client layer is being torn down:
	// end the display session with the Disconnect reason first, and if s2sdk still reports it as
	// displayed, switch the menu to a builtin type whose Close is a no-op and verify that switch
	// before destroying it. If neither works, A7_UNRESOLVED is logged and the menu is left alone -
	// destroying it would run the custom backend's close over the dying client layer.
	if (!g_mapReady && !EndMenuSessionsForSafeDestroy(id)) {
		return;
	}
	DestroyMenuNow(id);
}

void DestroyAllMenus() {
	std::vector<s2sdk::MenuId> ids;
	ids.reserve(static_cast<std::size_t>(kMaxSlots) + g_destroyQueue.size());
	for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
		OpenMenu& menu = g_openMenus[static_cast<std::size_t>(slot)];
		if (!menu.active) {
			continue;
		}
		ids.push_back(menu.id);
		menu = OpenMenu{};
	}
	ids.insert(ids.end(), g_destroyQueue.begin(), g_destroyQueue.end());
	g_destroyQueue.clear();
	std::sort(ids.begin(), ids.end());
	ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
	for (const s2sdk::MenuId id : ids) {
		DestroyOneMenu(id);
	}
	if (g_menuIdZeroReserved) {
		g_menuIdZeroReserved = false;
		try {
			s2sdk::DestroyMenu(0); // the reserved dummy; nobody can display id 0
		} catch (...) {
		}
	}
	// The Cancel/End callbacks of the destroys above may have queued their ids again; after the
	// unload there is no next map to retry them on.
	g_destroyQueue.clear();
	g_openMenus.fill(OpenMenu{});
}

// The admin core has no item-unregister API. Re-register the item with null callbacks through the
// generated binding, so the core stops holding the trampolines that live in this .so
// (PITFALLS A7); the consumer wrapper cannot be used here because it would install its own
// trampolines again. The item stays visible in the core's "server" category but selecting it is a
// no-op until the plugin is reloaded and registers the real callbacks.
void RetireAdminMenuRegistration() {
	if (!g_menuRegistered || __admin_system_RegisterItem == nullptr) {
		g_menuRegistered = false;
		return;
	}
	g_menuRegistered = false;
	try {
		::admin_system::RegisterItem(plg::string("maps"), plg::string("Item_Maps"), plg::string("server"),
									 plg::string("@admin/maps"), nullptr, nullptr);
	} catch (...) {
	}
}

// --- maps.ini ------------------------------------------------------------------------------------

void LoadMapsConfig() {
	const std::string configsDir = plg::GetConfigsDir ? ToStd(plg::GetConfigsDir()) : std::string();
	const std::string dataDir = plg::GetDataDir ? ToStd(plg::GetDataDir()) : std::string();
	const std::vector<std::string> candidates = as_maps::MapsFileCandidates(configsDir, dataDir);
	const as_maps::LoadResult result = as_maps::LoadMaps(candidates);
	if (!result.ok) {
		const std::string path = result.found ? result.path : candidates.front();
		LogError("Failed to load " + path);
		g_mapMaps.clear();
		return;
	}
	g_mapMaps = result.maps;
	LogInfo("loaded " + std::to_string(g_mapMaps.size()) + " maps from " + result.path);
}

// --- the plugin ----------------------------------------------------------------------------------

class AsMapsPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;
};

AsMapsPlugin g_asMapsPlugin;

plg::PluginResult AsMapsPlugin::OnPluginStart() {
	try {
		g_pAdminApi = &admin_system::AdminApi::Get();
		g_shuttingDown = false;

		g_mapReady = false;
		g_mapEndedSinceStart = false;
		g_firstUpdateDone = false;

		g_coreCallbackRegistered = false;
		g_menuRegistered = false;
		g_commandRegistered = false;
		g_commandCreated = false;

		g_levelChangeDue = false;
		g_levelChangeTimerActive = false;
		g_levelChangeTimer = 0;
		g_scheduledChange = ScheduledChange{};

		g_pendingActions.clear();
		g_destroyQueue.clear();
		g_openMenus.fill(OpenMenu{});
		g_menuIdZeroChecked = false;
		g_menuIdZeroReserved = false;
		g_mapMaps.clear();

		// maps.ini is a plain file read (no client, entity or engine query), so it is safe here
		// and is needed before the admin menu item can be built.
		LoadMapsConfig();

		// PITFALLS B1: registering the map listeners touches no client. Without the listeners
		// nothing could ever close the gate again, so a missing binding keeps it shut instead of
		// opening it blindly. The Register/Unregister pair is required together, so the teardown
		// below can always take back exactly what this start registered.
		if (!g_mapListeners && __s2sdk_OnMapStart_Register && __s2sdk_OnMapStart_Unregister &&
			__s2sdk_OnMapEnd_Register && __s2sdk_OnMapEnd_Unregister) {
			s2sdk::OnMapStart_Register(&OnMapStartListener);
			s2sdk::OnMapEnd_Register(&OnMapEndListener);
			g_mapListeners = true;
		} else if (!__s2sdk_OnMapStart_Register || !__s2sdk_OnMapStart_Unregister ||
				   !__s2sdk_OnMapEnd_Register || !__s2sdk_OnMapEnd_Unregister) {
			LogError("the s2sdk map listener Register/Unregister pair is incomplete - client queries and the map menu stay disabled");
		}

		if (!AdminApiAvailable()) {
			// Fail soft: returning Failed from OnPluginStart would stop every Plugify plugin. The
			// plugin stays loaded but idle (no command, no menu item), like the previous port.
			LogError("the admin_system bindings are not available (is the admin_system plugin loaded?)");
			return {};
		}

		// The command is only registered when both removal arms of the ownership rule exist, so
		// OnPluginEnd can always undo it (RemoveCommand for a created name, RemoveCommandListener
		// for a pre-existing one).
		if (!g_commandRegistered && __s2sdk_AddConsoleCommand && __s2sdk_RemoveCommand &&
			__s2sdk_RemoveCommandListener) {
			// false = the name already existed (the engine or another plugin) and s2sdk only
			// attached a listener to it: OnPluginEnd must then detach the listener and never
			// RemoveCommand a command this plugin did not create (PITFALLS C11).
			g_commandCreated = s2sdk::AddConsoleCommand("mm_map", "", kMmMapFlags, &OnMmMapCommand,
														s2sdk::HookMode::Pre);
			g_commandRegistered = true;
			if (g_commandCreated) {
				LogInfo("ready; 'mm_map' registered and the 'maps' item is armed");
			} else {
				LogInfo("'mm_map' already exists; reusing the entry (the listener is attached)");
			}
		} else if (!__s2sdk_AddConsoleCommand) {
			LogError("the s2sdk AddConsoleCommand binding is unavailable - 'mm_map' is not registered");
		} else if (!__s2sdk_RemoveCommand || !__s2sdk_RemoveCommandListener) {
			LogError("the s2sdk command removal pair is incomplete - 'mm_map' is not registered");
		}

		// The original registered the menu from OnCoreLoaded. A core that finished loading before
		// this plugin started never fires the event (documented in the manifest), so the current
		// state is checked as well; RegisterAdminMenu() is idempotent. The callback pair is
		// required together so OnPluginEnd can always unregister it.
		if (!g_coreCallbackRegistered && __admin_system_OnCoreLoaded_Register &&
			__admin_system_OnCoreLoaded_Unregister) {
			g_pAdminApi->OnCoreLoaded(&OnCoreLoadedCallback);
			g_coreCallbackRegistered = true;
			if (g_pAdminApi->IsCoreLoaded()) {
				RegisterAdminMenu();
			}
		} else if (!g_coreCallbackRegistered) {
			LogError("the admin_system OnCoreLoaded Register/Unregister pair is incomplete - the 'maps' item is not registered");
		}
	} catch (const std::exception& ex) {
		LogError(std::string("OnPluginStart failed: ") + ex.what());
	} catch (...) {
		LogError("OnPluginStart failed with an unknown exception");
	}
	return {}; // never Failed (PITFALLS A1)
}

plg::PluginResult AsMapsPlugin::OnPluginUpdate(std::chrono::milliseconds dt) {
	(void)dt;
	try {
		// A plugin loaded into a running map never saw OnMapStart: its first update is the "a map
		// is up" signal (PITFALLS B1). An OnMapEnd seen since OnPluginStart means the server is
		// between maps, so the latch waits for the next OnMapStart instead.
		if (!g_firstUpdateDone) {
			g_firstUpdateDone = true;
			if (g_mapListeners && !g_mapEndedSinceStart) {
				g_mapReady = true;
			}
		}
		// The actual work happens here, outside the concommand/menu/timer locks the callbacks run
		// under (see the header comment).
		ProcessPendingActions();
		ProcessLevelChange();
		ProcessMenuDestroyQueue();
	} catch (const std::exception& ex) {
		LogError(std::string("update failed: ") + ex.what());
	} catch (...) {
		LogError("update failed with an unknown exception");
	}
	return {};
}

plg::PluginResult AsMapsPlugin::OnPluginEnd() {
	try {
		g_shuttingDown = true;
		g_pendingActions.clear();
		g_scheduledChange = ScheduledChange{};
		g_levelChangeDue = false;
		if (g_levelChangeTimerActive && __s2sdk_KillsTimer) {
			try {
				s2sdk::KillsTimer(g_levelChangeTimer);
			} catch (...) {
			}
		}
		g_levelChangeTimerActive = false;

		if (g_commandRegistered) {
			// PITFALLS C11: remove the command only when this plugin created it; a name that
			// already existed was merely hooked, so only our listener is detached. Both calls
			// also remove the listener AddConsoleCommand attached (RemoveCommand drops Pre/Post).
			if (g_commandCreated) {
				if (__s2sdk_RemoveCommand) {
					s2sdk::RemoveCommand("mm_map", &OnMmMapCommand);
				}
			} else if (__s2sdk_RemoveCommandListener) {
				s2sdk::RemoveCommandListener("mm_map", &OnMmMapCommand, s2sdk::HookMode::Pre);
			}
		}
		g_commandRegistered = false;
		g_commandCreated = false;

		// The admin core keeps a pointer to our trampoline: drop it before the library goes away.
		if (g_coreCallbackRegistered && g_pAdminApi != nullptr) {
			g_pAdminApi->OnCoreLoadedUnregister();
		}
		g_coreCallbackRegistered = false;

		if (g_mapListeners) {
			if (__s2sdk_OnMapStart_Unregister) {
				s2sdk::OnMapStart_Unregister(&OnMapStartListener);
			}
			if (__s2sdk_OnMapEnd_Unregister) {
				s2sdk::OnMapEnd_Unregister(&OnMapEndListener);
			}
			g_mapListeners = false;
		}

		DestroyAllMenus();
		g_menuIdZeroChecked = false;

		// The admin core keeps function pointers into this library for the registered menu item.
		// There is no unregister API, so the item is re-registered with null callbacks (see
		// RetireAdminMenuRegistration) before the API pointer is dropped.
		RetireAdminMenuRegistration();

		// A stale callback (the engine still holds the pointer for a moment) can no longer reach a
		// client, a menu or the admin API.
		g_pAdminApi = nullptr;
		g_mapReady = false;
		g_mapEndedSinceStart = true;
		g_firstUpdateDone = false;
		g_mapMaps.clear();
	} catch (const std::exception& ex) {
		LogError(std::string("OnPluginEnd failed: ") + ex.what());
	} catch (...) {
		LogError("OnPluginEnd failed with an unknown exception");
	}
	return {};
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_asMapsPlugin)
