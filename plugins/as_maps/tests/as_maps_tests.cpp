// as_maps plugin tests — no engine: the plugin TU is compiled in (its handlers live in an
// anonymous namespace) and every __s2sdk_* / __admin_system_* / __hudmenu_* binding slot is a
// recording fake.
//
// Covered:
//   * PITFALLS A1/B1/C2: OnPluginStart never fails, makes no client query, registers the map
//     listeners; the map gate is latched by OnMapStart, by the first update of a plugin loaded
//     into a running map, and dropped by OnMapEnd (a plugin reloaded into the map-change window
//     does not latch);
//   * PITFALLS C11: "mm_map" is registered without ClientCanExecute, only caller == -1 acts (a
//     client slot and an out-of-range negative value such as -2 are refused silently), a chat
//     "!mm_map" answers Continue and nothing is executed; OnPluginEnd calls RemoveCommand only when
//     this plugin created the name and RemoveCommandListener otherwise;
//   * the delayed changelevel: nothing is executed from the command callback, the five second
//     s2sdk timer only marks the command due (the ServerCommand runs from OnPluginUpdate, outside
//     the timer lock), IsMapValid picks "changelevel" / "ds_workshop_changelevel", the map name is
//     validated before it is spliced into the command (no space, ';', newline, '../' or over-long
//     name), a second request replaces the pending one (only the latest changelevel is ever
//     issued), and the scheduled request carries its author: right before the ServerCommand the
//     author must still be the live SteamID64 + controller handle with ready admin data and
//     @admin/maps, otherwise the request is dropped (the server console -1 stays valid);
//   * the admin menu: the "server"/"maps" item with the "@admin/maps" flag, the five-second-delayed
//     announcement of message_type 0/1/2 (with the console name and the player name), the
//     SendAction("map") notification, the maps.ini display names and the map-file fallback for an
//     unknown map;
//   * the map menu: built deferred from the admin item callback (the handler runs under the menu
//     manager's lock), the complete menu+cleanup binding set (including SetMenuType/GetMenuType)
//     required before the id-0 dummy and every real menu, s2sdk menu id 0 burned before the first
//     real menu, the exact pisexcompat menu-type table (the player's "!menu" choice through the
//     optional hudmenu fake; an explicit hud/wasd with a missing backend -> builtin button;
//     HUD_LAYOUT default -> hud if registered else button, never wasd), the maps.ini order, five
//     items per page, the back button
//     returning to ShowAdminLastCategoryMenu, the menu destroyed outside the handler, a menu
//     cancelled (Disconnect) and destroyed at map end, a refused cancel re-checked and the menu
//     switched to the builtin "chat" Close before destroy (or A7_UNRESOLVED logged and the menu
//     left alone when that is impossible), no menu backend close over the dying client layer, no
//     menu for a bot/SourceTV/not-in-game slot;
//   * deferred identity: SteamID64 + controller CEntityHandle (the serial catches a recycled slot)
//     are carried by every queued show/select/back and re-checked, together with the admin_system
//     1.1.0 IsClientAdminDataReady() answer and "@admin/maps", before the action is applied;
//   * PITFALLS A7: OnPluginEnd removes the command, the OnCoreLoaded callback, both map listeners,
//     the timer, every menu and the reserved id 0, retires the admin item with null callbacks,
//     drops the admin API, and a stale callback after the unload runs nothing; every registration
//     is paired (the raw-registered category has a null display callback, the item display/select
//     are nulled on unload) and a reload re-registers safely;
//   * maps.ini: the raw cwd Metamod spelling wins over the configs/data candidates and a malformed
//     first-existing file is authoritative (not hidden by a later valid candidate);
//   * fail-soft starts: no admin_system bindings -> loaded but idle; no s2sdk bindings -> no crash;
//     a missing readiness binding / pending SELECT -> the critical action is denied; a missing
//     maps.ini -> an empty menu, not a failed plugin.
//
// Run by ./test.sh (build/tests/as_maps_tests).

#include "../src/plugin.cpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string& what) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL: %s\n", what.c_str());
	}
}

// --- fake log ------------------------------------------------------------------------------------

struct FakeLogLine {
	plg::Severity severity = plg::Severity::Info;
	std::string message;
};

std::vector<FakeLogLine> g_log;

void FakeLog(std::string_view message, plg::Severity severity, const plg::source_location&) {
	g_log.push_back(FakeLogLine{severity, std::string(message)});
}

bool LoggedError(const std::string& needle) {
	for (const FakeLogLine& line : g_log) {
		if (line.severity == plg::Severity::Error && line.message.find(needle) != std::string::npos) {
			return true;
		}
	}
	return false;
}

// --- fake clients --------------------------------------------------------------------------------

struct FakeClient {
	bool connected = false;
	bool inGame = false;
	bool sourceTV = false;
	bool fake = false;
	bool controller = true;
	// Bumped to simulate a recycled slot / a new controller for the same player: the
	// CEntityHandle encodes the generation, the SteamID64 alone does not.
	int32_t generation = 0;
	uint64_t steamid = 0;
	std::string name;
};

std::array<FakeClient, 64> g_clients{};
int g_clientCalls = 0; // every s2sdk client query counts (a map-end handler must not call any)

const FakeClient& Client(int32_t slot) {
	static const FakeClient empty;
	return slot >= 0 && slot < static_cast<int32_t>(g_clients.size()) ? g_clients[static_cast<std::size_t>(slot)]
																	 : empty;
}

void SetClient(int32_t slot, bool inGame, uint64_t steamid, const char* name, bool fake = false) {
	FakeClient& client = g_clients[static_cast<std::size_t>(slot)];
	client.connected = true;
	client.inGame = inGame;
	client.fake = fake;
	client.steamid = steamid;
	client.name = name != nullptr ? name : "";
}

bool FakeIsClientConnected(int32_t slot) {
	++g_clientCalls;
	return Client(slot).connected;
}

bool FakeIsClientInGame(int32_t slot) {
	++g_clientCalls;
	return Client(slot).inGame;
}

bool FakeIsClientSourceTV(int32_t slot) {
	++g_clientCalls;
	return Client(slot).sourceTV;
}

bool FakeIsFakeClient(int32_t slot) {
	++g_clientCalls;
	return Client(slot).fake;
}

uint64_t FakeGetClientSteamID64(int32_t slot) {
	++g_clientCalls;
	return Client(slot).steamid;
}

plg::string FakeGetClientName(int32_t slot) {
	++g_clientCalls;
	return plg::string(Client(slot).name);
}

void* FakePlayerSlotToEntPointer(int32_t slot) {
	++g_clientCalls;
	if (!Client(slot).controller || slot < 0 || slot >= static_cast<int32_t>(g_clients.size())) {
		return nullptr;
	}
	return const_cast<FakeClient*>(&g_clients[static_cast<std::size_t>(slot)]);
}

int32_t FakePlayerSlotToEntHandle(int32_t slot) {
	++g_clientCalls;
	if (!Client(slot).controller) {
		return -1;
	}
	return Client(slot).generation * 100000 + slot + 1; // a CEntityHandle-like value with a serial
}

bool FakeIsValidEntHandle(int32_t handle) {
	return handle > 0;
}

bool FakeIsValidEntPointer(void* pointer) {
	return pointer != nullptr;
}

// --- fake output ---------------------------------------------------------------------------------

std::vector<std::string> g_chatPrints;
std::vector<std::string> g_serverPrints;

void FakePrintToChatAll(const plg::string& text) {
	g_chatPrints.push_back(ToStd(text));
}

void FakePrintToServer(const plg::string& text) {
	g_serverPrints.push_back(ToStd(text));
}

void FakePrintToConsole(int32_t slot, const plg::string& text) {
	g_chatPrints.push_back(std::to_string(slot) + ":" + ToStd(text));
}

// --- fake map lifecycle --------------------------------------------------------------------------

s2sdk::OnMapStartCallback g_onMapStart = nullptr;
s2sdk::OnMapEndCallback g_onMapEnd = nullptr;
int g_mapStartRegisters = 0;
int g_mapStartUnregisters = 0;
int g_mapEndRegisters = 0;
int g_mapEndUnregisters = 0;

void FakeMapStartRegister(s2sdk::OnMapStartCallback callback) {
	g_onMapStart = callback;
	++g_mapStartRegisters;
}

void FakeMapStartUnregister(s2sdk::OnMapStartCallback callback) {
	if (g_onMapStart == callback) {
		g_onMapStart = nullptr;
	}
	++g_mapStartUnregisters;
}

void FakeMapEndRegister(s2sdk::OnMapEndCallback callback) {
	g_onMapEnd = callback;
	++g_mapEndRegisters;
}

void FakeMapEndUnregister(s2sdk::OnMapEndCallback callback) {
	if (g_onMapEnd == callback) {
		g_onMapEnd = nullptr;
	}
	++g_mapEndUnregisters;
}

// --- fake engine ---------------------------------------------------------------------------------

std::set<std::string> g_validMaps;
std::vector<std::string> g_serverCommands;
std::vector<std::string> g_mapValidCalls;

bool FakeIsMapValid(const plg::string& name) {
	const std::string mapName = ToStd(name);
	g_mapValidCalls.push_back(mapName);
	return g_validMaps.count(mapName) != 0;
}

void FakeServerCommand(const plg::string& command) {
	g_serverCommands.push_back(ToStd(command));
}

// --- fake timers ---------------------------------------------------------------------------------

struct FakeTimer {
	s2sdk::TimerId id = 0;
	double delay = 0.0;
	s2sdk::TimerFlag flags = s2sdk::TimerFlag::Default;
	s2sdk::TimerCallback callback = nullptr;
	bool active = true;
};

std::vector<FakeTimer> g_timers;
std::vector<s2sdk::TimerId> g_killedTimers;
s2sdk::TimerId g_nextTimerId = 1;

s2sdk::TimerId FakeCreateTimer(double delay, s2sdk::TimerCallback callback, s2sdk::TimerFlag flags,
							   const plg::vector<plg::any>&) {
	FakeTimer timer;
	timer.id = g_nextTimerId++;
	timer.delay = delay;
	timer.flags = flags;
	timer.callback = callback;
	g_timers.push_back(timer);
	return timer.id;
}

void FakeKillsTimer(s2sdk::TimerId id) {
	g_killedTimers.push_back(id);
	for (FakeTimer& timer : g_timers) {
		if (timer.id == id) {
			timer.active = false;
		}
	}
}

// What TimerSystem::RunFrame does to a due one-shot timer: the callback runs, then the timer is
// erased. The plugin must only mark the command due here.
void FireTimer(s2sdk::TimerId id) {
	for (std::size_t i = 0; i < g_timers.size(); ++i) {
		if (g_timers[i].id != id) {
			continue;
		}
		if (!g_timers[i].active) {
			return;
		}
		const s2sdk::TimerCallback callback = g_timers[i].callback;
		g_timers[i].active = false;
		if (callback != nullptr) {
			const plg::vector<plg::any> userData;
			callback(id, userData);
		}
		return;
	}
}

// --- fake commands -------------------------------------------------------------------------------

struct FakeCommand {
	std::string name;
	std::string description;
	s2sdk::ConVarFlag flags = s2sdk::ConVarFlag::None;
	s2sdk::ConCommandCallback callback = nullptr;
	s2sdk::HookMode mode = s2sdk::HookMode::Pre;
};

std::vector<FakeCommand> g_commands;
std::set<std::string> g_existingCommands; // AddConsoleCommand answers false for these
std::vector<std::pair<std::string, s2sdk::ConCommandCallback>> g_removedCommands;
std::vector<std::string> g_removedListeners;

bool FakeAddConsoleCommand(const plg::string& name, const plg::string& description, s2sdk::ConVarFlag flags,
						   s2sdk::ConCommandCallback callback, s2sdk::HookMode mode) {
	const std::string key = ToStd(name);
	g_commands.push_back(FakeCommand{key, ToStd(description), flags, callback, mode});
	return g_existingCommands.count(key) == 0;
}

bool FakeRemoveCommand(const plg::string& name, s2sdk::ConCommandCallback callback) {
	g_removedCommands.emplace_back(ToStd(name), callback);
	return true;
}

bool FakeRemoveCommandListener(const plg::string& name, s2sdk::ConCommandCallback callback, s2sdk::HookMode) {
	g_removedListeners.push_back(ToStd(name));
	(void)callback;
	return true;
}

const FakeCommand* FindCommand(const std::string& name) {
	for (const FakeCommand& command : g_commands) {
		if (command.name == name) {
			return &command;
		}
	}
	return nullptr;
}

// noinline: GCC 14's constant propagation of the callers below trips a false
// -Werror=stringop-overflow inside plg::vector's inline storage; the helper is test-only.
__attribute__((noipa)) s2sdk::ResultType DispatchMmMap(int32_t caller, s2sdk::ConCommandContext context,
														 std::initializer_list<const char*> args) {
	const FakeCommand* command = FindCommand("mm_map");
	Check(command != nullptr, "mm_map is registered with s2sdk");
	if (command == nullptr) {
		return s2sdk::ResultType::Continue;
	}
	plg::vector<plg::string> argv;
	for (const char* arg : args) {
		argv.emplace_back(arg);
	}
	return command->callback(caller, context, argv);
}

// --- fake s2sdk menus ----------------------------------------------------------------------------

struct FakeMenu {
	s2sdk::MenuId id = 0;
	std::string title;
	std::string type;
	s2sdk::MenuHandlerCallback handler = nullptr;
	std::vector<std::pair<std::string, std::string>> items; // info, display
	int32_t pagination = 7;
	bool exitButton = true;
	bool backButton = false;
	bool displayCalled = false;
};

std::vector<FakeMenu> g_menus;
std::vector<s2sdk::MenuId> g_destroyedMenus;
std::vector<std::string> g_destroyedMenuTypes;
std::vector<std::pair<s2sdk::MenuId, std::string>> g_menuTypeSwitches;
int g_backendCloseCalls = 0; // a destroy of a still-displayed menu with a custom backend
bool g_cancelFails = false;
bool g_cancelFailsButCloses = false;
std::set<std::string> g_menuTypes;
s2sdk::MenuId g_nextMenuId = 0;
std::array<bool, 64> g_menuOpen{};
std::array<s2sdk::MenuId, 64> g_openMenuId{};
int g_createMenuCalls = 0;

s2sdk::MenuId FakeCreateMenu(const plg::string& title, s2sdk::MenuHandlerCallback handler, const plg::string& type) {
	++g_createMenuCalls;
	FakeMenu menu;
	menu.id = g_nextMenuId++;
	menu.title = ToStd(title);
	menu.type = ToStd(type);
	menu.handler = handler;
	g_menus.push_back(menu);
	return menu.id;
}

int32_t FakeAddMenuItem(s2sdk::MenuId id, const plg::string& info, const plg::string& display,
						s2sdk::MenuItemStyle) {
	for (FakeMenu& menu : g_menus) {
		if (menu.id == id) {
			menu.items.emplace_back(ToStd(info), ToStd(display));
			return static_cast<int32_t>(menu.items.size()) - 1;
		}
	}
	return -1;
}

bool FakeDisplayMenu(s2sdk::MenuId id, int32_t slot, int32_t) {
	for (FakeMenu& menu : g_menus) {
		if (menu.id == id) {
			menu.displayCalled = true;
			g_menuOpen[static_cast<std::size_t>(slot)] = true;
			g_openMenuId[static_cast<std::size_t>(slot)] = id;
			return true;
		}
	}
	return false;
}

bool FakeDestroyMenu(s2sdk::MenuId id) {
	g_destroyedMenus.push_back(id);
	for (std::size_t i = 0; i < g_menus.size(); ++i) {
		if (g_menus[i].id != id) {
			continue;
		}
		// Model the s2sdk menu type backend's Close(): a custom (hudmenu) backend touches the
		// client layer for a slot that still has the menu open, the builtin chat/console Close is
		// a no-op. This is the callback PITFALLS B2 is about.
		bool displayed = false;
		for (std::size_t slot = 0; slot < g_menuOpen.size(); ++slot) {
			if (g_menuOpen[slot] && g_openMenuId[slot] == id) {
				displayed = true;
				g_menuOpen[slot] = false;
			}
		}
		g_destroyedMenuTypes.push_back(g_menus[i].type);
		if (displayed && g_menus[i].type != "chat" && g_menus[i].type != "console") {
			++g_backendCloseCalls;
		}
		g_menus.erase(g_menus.begin() + static_cast<std::ptrdiff_t>(i));
		return true;
	}
	return false;
}

bool FakeSetMenuType(s2sdk::MenuId id, const plg::string& type) {
	for (FakeMenu& menu : g_menus) {
		if (menu.id == id) {
			menu.type = ToStd(type);
			g_menuTypeSwitches.emplace_back(id, menu.type);
			return true;
		}
	}
	return false;
}

plg::string FakeGetMenuType(s2sdk::MenuId id) {
	for (const FakeMenu& menu : g_menus) {
		if (menu.id == id) {
			return plg::string(menu.type);
		}
	}
	return plg::string();
}

bool FakeIsValidMenu(s2sdk::MenuId id) {
	for (const FakeMenu& menu : g_menus) {
		if (menu.id == id) {
			return true;
		}
	}
	return false;
}

bool FakeIsMenuTypeRegistered(const plg::string& name) {
	return g_menuTypes.count(ToStd(name)) != 0;
}

bool FakeSetMenuPagination(s2sdk::MenuId id, int32_t itemsPerPage) {
	for (FakeMenu& menu : g_menus) {
		if (menu.id == id) {
			menu.pagination = itemsPerPage;
			return true;
		}
	}
	return false;
}

bool FakeSetMenuExitButton(s2sdk::MenuId id, bool enabled) {
	for (FakeMenu& menu : g_menus) {
		if (menu.id == id) {
			menu.exitButton = enabled;
			return true;
		}
	}
	return false;
}

bool FakeSetMenuExitBackButton(s2sdk::MenuId id, bool enabled) {
	for (FakeMenu& menu : g_menus) {
		if (menu.id == id) {
			menu.backButton = enabled;
			return true;
		}
	}
	return false;
}

const FakeMenu* FindMenu(s2sdk::MenuId id) {
	for (const FakeMenu& menu : g_menus) {
		if (menu.id == id) {
			return &menu;
		}
	}
	return nullptr;
}

void InvokeMenu(s2sdk::MenuId id, s2sdk::MenuAction action, int32_t slot, int32_t param) {
	const FakeMenu* menu = FindMenu(id);
	Check(menu != nullptr, "the menu id exists");
	if (menu != nullptr && menu->handler != nullptr) {
		menu->handler(id, action, slot, param);
	}
}

std::vector<std::pair<int32_t, s2sdk::MenuCancelReason>> g_cancelledClients;

s2sdk::MenuId FakeGetClientMenu(int32_t slot) {
	return g_menuOpen[static_cast<std::size_t>(slot)] ? g_openMenuId[static_cast<std::size_t>(slot)] : 0;
}

bool FakeCancelClientMenu(int32_t slot, s2sdk::MenuCancelReason reason) {
	g_cancelledClients.emplace_back(slot, reason);
	if (!g_menuOpen[static_cast<std::size_t>(slot)]) {
		return false;
	}
	if (g_cancelFails && !g_cancelFailsButCloses) {
		// The s2sdk call refused and left the display session in place.
		return false;
	}
	const s2sdk::MenuId id = g_openMenuId[static_cast<std::size_t>(slot)];
	g_menuOpen[static_cast<std::size_t>(slot)] = false;
	// What s2sdk's EndClientMenu does: Cancel then End; the backend close is skipped for the
	// Disconnect reason.
	if (const FakeMenu* menu = FindMenu(id)) {
		if (menu->handler != nullptr) {
			menu->handler(id, s2sdk::MenuAction::Cancel, slot, static_cast<int32_t>(reason));
			menu->handler(id, s2sdk::MenuAction::End, slot, 0);
		}
	}
	return !g_cancelFails; // a configured failure may still have cleared the display
}

// --- fake admin_system bindings ------------------------------------------------------------------

std::map<std::string, std::string> g_translations;
int32_t g_messageType = 1;
bool g_coreLoaded = true;
std::vector<std::string> g_registeredCategories;
std::vector<admin_system::CategoryDisplayCallback> g_registeredCategoryCallbacks;
struct RegisteredItem {
	std::string identity;
	std::string name;
	std::string category;
	std::string flags;
	admin_system::ItemDisplayCallback display = nullptr;
	admin_system::ItemSelectCallback select = nullptr;
};
std::vector<RegisteredItem> g_registeredItems;
admin_system::ItemSelectCallback g_itemSelect = nullptr;
admin_system::CoreLoadedCallback g_coreLoadedCallback = nullptr;
int g_coreLoadedRegisters = 0;
int g_coreLoadedUnregisters = 0;
struct FakeAction {
	int32_t slot = 0;
	std::string action;
	std::string param;
};
std::vector<FakeAction> g_actions;
std::vector<int32_t> g_lastCategoryCalls;
std::vector<std::pair<int32_t, std::string>> g_permissionChecks;

float FakeGetPluginVersion() {
	return 1.1f;
}

plg::string FakeGetTranslation(const plg::string& key) {
	const auto it = g_translations.find(ToStd(key));
	return plg::string(it != g_translations.end() ? it->second : ToStd(key));
}

int32_t FakeGetMessageType() {
	return g_messageType;
}

bool FakeIsCoreLoaded() {
	return g_coreLoaded;
}

void FakeRegisterCategory(const plg::string& identity, const plg::string& name,
						  admin_system::CategoryDisplayCallback callback) {
	g_registeredCategories.push_back(ToStd(identity));
	g_registeredCategoryCallbacks.push_back(callback);
	(void)name;
}

void FakeRegisterItem(const plg::string& identity, const plg::string& name, const plg::string& category,
					  const plg::string& flags, admin_system::ItemDisplayCallback display,
					  admin_system::ItemSelectCallback select) {
	RegisteredItem item;
	item.identity = ToStd(identity);
	item.name = ToStd(name);
	item.category = ToStd(category);
	item.flags = ToStd(flags);
	item.display = display;
	item.select = select;
	g_registeredItems.push_back(item);
	g_itemSelect = select;
}

std::set<int32_t> g_deniedSlots;

bool FakeHasPermission(int32_t slot, const plg::string& permission) {
	g_permissionChecks.emplace_back(slot, ToStd(permission));
	return g_deniedSlots.count(slot) == 0;
}

// --- fake hudmenu bindings (optional dependency) -------------------------------------------------

std::map<int32_t, std::string> g_clientMenuTypes; // unset: the fake answers the server default "hud"

plg::string FakeGetClientMenuType(int32_t slot) {
	const auto it = g_clientMenuTypes.find(slot);
	return plg::string(it != g_clientMenuTypes.end() ? it->second : std::string("hud"));
}

// admin_system 1.1.0 readiness: every (slot, steamid) is ready unless a test marks it pending.
std::set<std::pair<int32_t, uint64_t>> g_notReadyPairs;
bool g_readyThrows = false;

bool FakeIsClientAdminDataReady(int32_t slot, uint64_t steamid) {
	if (g_readyThrows) {
		throw std::runtime_error("fake IsClientAdminDataReady failure");
	}
	return g_notReadyPairs.count(std::make_pair(slot, steamid)) == 0;
}

void FakeSendAction(int32_t slot, const plg::string& action, const plg::string& param) {
	g_actions.push_back(FakeAction{slot, ToStd(action), ToStd(param)});
}

void FakeShowAdminLastCategoryMenu(int32_t slot) {
	g_lastCategoryCalls.push_back(slot);
}

void FakeOnCoreLoadedRegister(admin_system::CoreLoadedCallback callback) {
	g_coreLoadedCallback = callback;
	++g_coreLoadedRegisters;
}

void FakeOnCoreLoadedUnregister(admin_system::CoreLoadedCallback callback) {
	if (g_coreLoadedCallback == callback) {
		g_coreLoadedCallback = nullptr;
	}
	++g_coreLoadedUnregisters;
}

// --- the world -----------------------------------------------------------------------------------

std::string g_configsDir = AS_MAPS_TEST_DATA_DIR;
// The plugin's data candidate (<data>/admin_system/maps.ini) must stay out of the normal tests.
constexpr const char* kMissingDataDir = "/nonexistent-as-maps-test-data";
std::string g_dataDir = kMissingDataDir;

plg::string FakeGetConfigsDir() {
	return plg::string(g_configsDir);
}

plg::string FakeGetDataDir() {
	return plg::string(g_dataDir);
}

void InstallDefaultTranslations() {
	g_translations["SelectMap"] = "Выберите карту";
	g_translations["Changing_Map"] = "Смена карты на {LIGHTRED}%s{DEFAULT}...";
	g_translations["Changing_Map_Admin"] = "Админ {LIGHTRED}%s{DEFAULT} сменил карту на {LIGHTRED}%s{DEFAULT}";
	g_translations["UsageChangeMap"] = "Используйте: {LIGHTRED}%s{DEFAULT} <map>";
}

void InstallFakes() {
	plg::GetConfigsDir = &FakeGetConfigsDir;
	plg::GetDataDir = &FakeGetDataDir;
	plg::Log = &FakeLog;

	__s2sdk_OnMapStart_Register = &FakeMapStartRegister;
	__s2sdk_OnMapStart_Unregister = &FakeMapStartUnregister;
	__s2sdk_OnMapEnd_Register = &FakeMapEndRegister;
	__s2sdk_OnMapEnd_Unregister = &FakeMapEndUnregister;

	__s2sdk_IsClientConnected = &FakeIsClientConnected;
	__s2sdk_IsClientInGame = &FakeIsClientInGame;
	__s2sdk_IsClientSourceTV = &FakeIsClientSourceTV;
	__s2sdk_IsFakeClient = &FakeIsFakeClient;
	__s2sdk_GetClientSteamID64 = &FakeGetClientSteamID64;
	__s2sdk_GetClientName = &FakeGetClientName;
	__s2sdk_PlayerSlotToEntPointer = &FakePlayerSlotToEntPointer;
	__s2sdk_PlayerSlotToEntHandle = &FakePlayerSlotToEntHandle;
	__s2sdk_IsValidEntHandle = &FakeIsValidEntHandle;
	__s2sdk_IsValidEntPointer = &FakeIsValidEntPointer;

	__s2sdk_PrintToChatAll = &FakePrintToChatAll;
	__s2sdk_PrintToServer = &FakePrintToServer;
	__s2sdk_PrintToConsole = &FakePrintToConsole;

	__s2sdk_IsMapValid = &FakeIsMapValid;
	__s2sdk_ServerCommand = &FakeServerCommand;

	__s2sdk_CreateTimer = &FakeCreateTimer;
	__s2sdk_KillsTimer = &FakeKillsTimer;

	__s2sdk_AddConsoleCommand = &FakeAddConsoleCommand;
	__s2sdk_RemoveCommand = &FakeRemoveCommand;
	__s2sdk_RemoveCommandListener = &FakeRemoveCommandListener;

	__s2sdk_CreateMenu = &FakeCreateMenu;
	__s2sdk_AddMenuItem = &FakeAddMenuItem;
	__s2sdk_DisplayMenu = &FakeDisplayMenu;
	__s2sdk_DestroyMenu = &FakeDestroyMenu;
	__s2sdk_IsValidMenu = &FakeIsValidMenu;
	__s2sdk_IsMenuTypeRegistered = &FakeIsMenuTypeRegistered;
	__s2sdk_SetMenuPagination = &FakeSetMenuPagination;
	__s2sdk_SetMenuExitButton = &FakeSetMenuExitButton;
	__s2sdk_SetMenuExitBackButton = &FakeSetMenuExitBackButton;
	__s2sdk_GetClientMenu = &FakeGetClientMenu;
	__s2sdk_CancelClientMenu = &FakeCancelClientMenu;
	__s2sdk_SetMenuType = &FakeSetMenuType;
	__s2sdk_GetMenuType = &FakeGetMenuType;

	__admin_system_GetPluginVersion = &FakeGetPluginVersion;
	__admin_system_GetTranslation = &FakeGetTranslation;
	__admin_system_GetMessageType = &FakeGetMessageType;
	__admin_system_IsCoreLoaded = &FakeIsCoreLoaded;
	__admin_system_RegisterCategory = &FakeRegisterCategory;
	__admin_system_RegisterItem = &FakeRegisterItem;
	__admin_system_HasPermission = &FakeHasPermission;
	__admin_system_SendAction = &FakeSendAction;
	__admin_system_ShowAdminLastCategoryMenu = &FakeShowAdminLastCategoryMenu;
	__admin_system_OnCoreLoaded_Register = &FakeOnCoreLoadedRegister;
	__admin_system_OnCoreLoaded_Unregister = &FakeOnCoreLoadedUnregister;
	__admin_system_IsClientAdminDataReady = &FakeIsClientAdminDataReady;

	__hudmenu_GetClientMenuType = &FakeGetClientMenuType;
	__hudmenu_SetClientMenuType = nullptr;
}

// Forgets every fake registration and all recorded state. The plugin must have been stopped first
// (PrepareTest/FinishTest do that), otherwise it still believes its command is registered.
void ResetWorld() {
	g_clients.fill(FakeClient{});
	g_clientCalls = 0;
	g_deniedSlots.clear();
	g_clientMenuTypes.clear();
	g_notReadyPairs.clear();
	g_readyThrows = false;

	g_chatPrints.clear();
	g_serverPrints.clear();
	g_log.clear();

	g_onMapStart = nullptr;
	g_onMapEnd = nullptr;
	g_mapStartRegisters = 0;
	g_mapStartUnregisters = 0;
	g_mapEndRegisters = 0;
	g_mapEndUnregisters = 0;

	g_validMaps = {"de_dust2", "de_mirage"};
	g_serverCommands.clear();
	g_mapValidCalls.clear();

	g_timers.clear();
	g_killedTimers.clear();
	g_nextTimerId = 1;

	g_commands.clear();
	g_existingCommands.clear();
	g_removedCommands.clear();
	g_removedListeners.clear();

	g_menus.clear();
	g_destroyedMenus.clear();
	g_destroyedMenuTypes.clear();
	g_cancelledClients.clear();
	g_menuTypeSwitches.clear();
	g_backendCloseCalls = 0;
	g_cancelFails = false;
	g_cancelFailsButCloses = false;
	g_menuTypes = {"hud", "wasd", "button"};
	g_nextMenuId = 0;
	g_menuOpen.fill(false);
	g_openMenuId.fill(0);
	g_createMenuCalls = 0;

	g_translations.clear();
	InstallDefaultTranslations();
	g_messageType = 1;
	g_coreLoaded = true;
	g_registeredCategories.clear();
	g_registeredCategoryCallbacks.clear();
	g_registeredItems.clear();
	g_itemSelect = nullptr;
	g_coreLoadedCallback = nullptr;
	g_coreLoadedRegisters = 0;
	g_coreLoadedUnregisters = 0;
	g_actions.clear();
	g_lastCategoryCalls.clear();
	g_permissionChecks.clear();
	g_configsDir = AS_MAPS_TEST_DATA_DIR;
	g_dataDir = kMissingDataDir;

	// The admin API wrapper caches every phrase per key.
	admin_system::AdminApi::Get().ReloadTranslations();

	// Bindings last: the unload of the previous test already ran with the previous fakes.
	plg::GetConfigsDir = nullptr;
	plg::GetDataDir = nullptr;
	plg::Log = nullptr;
	__s2sdk_OnMapStart_Register = nullptr;
	__s2sdk_OnMapStart_Unregister = nullptr;
	__s2sdk_OnMapEnd_Register = nullptr;
	__s2sdk_OnMapEnd_Unregister = nullptr;
	__s2sdk_IsClientConnected = nullptr;
	__s2sdk_IsClientInGame = nullptr;
	__s2sdk_IsClientSourceTV = nullptr;
	__s2sdk_IsFakeClient = nullptr;
	__s2sdk_GetClientSteamID64 = nullptr;
	__s2sdk_GetClientName = nullptr;
	__s2sdk_PlayerSlotToEntPointer = nullptr;
	__s2sdk_PlayerSlotToEntHandle = nullptr;
	__s2sdk_IsValidEntHandle = nullptr;
	__s2sdk_IsValidEntPointer = nullptr;
	__s2sdk_PrintToChatAll = nullptr;
	__s2sdk_PrintToServer = nullptr;
	__s2sdk_PrintToConsole = nullptr;
	__s2sdk_IsMapValid = nullptr;
	__s2sdk_ServerCommand = nullptr;
	__s2sdk_CreateTimer = nullptr;
	__s2sdk_KillsTimer = nullptr;
	__s2sdk_AddConsoleCommand = nullptr;
	__s2sdk_RemoveCommand = nullptr;
	__s2sdk_RemoveCommandListener = nullptr;
	__s2sdk_CreateMenu = nullptr;
	__s2sdk_AddMenuItem = nullptr;
	__s2sdk_DisplayMenu = nullptr;
	__s2sdk_DestroyMenu = nullptr;
	__s2sdk_IsValidMenu = nullptr;
	__s2sdk_IsMenuTypeRegistered = nullptr;
	__s2sdk_SetMenuPagination = nullptr;
	__s2sdk_SetMenuExitButton = nullptr;
	__s2sdk_SetMenuExitBackButton = nullptr;
	__s2sdk_GetClientMenu = nullptr;
	__s2sdk_CancelClientMenu = nullptr;
	__s2sdk_SetMenuType = nullptr;
	__s2sdk_GetMenuType = nullptr;
	__admin_system_GetPluginVersion = nullptr;
	__admin_system_GetTranslation = nullptr;
	__admin_system_GetMessageType = nullptr;
	__admin_system_IsCoreLoaded = nullptr;
	__admin_system_RegisterCategory = nullptr;
	__admin_system_RegisterItem = nullptr;
	__admin_system_HasPermission = nullptr;
	__admin_system_SendAction = nullptr;
	__admin_system_ShowAdminLastCategoryMenu = nullptr;
	__admin_system_OnCoreLoaded_Register = nullptr;
	__admin_system_OnCoreLoaded_Unregister = nullptr;
	__admin_system_IsClientAdminDataReady = nullptr;
	__hudmenu_GetClientMenuType = nullptr;
	__hudmenu_SetClientMenuType = nullptr;

	InstallFakes();
}

void PrepareTest() {
	g_asMapsPlugin.OnPluginEnd();
	ResetWorld();
}

void FinishTest() {
	g_asMapsPlugin.OnPluginEnd();
}

void StartPlugin() {
	const plg::PluginResult result = g_asMapsPlugin.OnPluginStart();
	Check(static_cast<bool>(result), "OnPluginStart never returns Failed");
}

void Update() {
	(void)g_asMapsPlugin.OnPluginUpdate(std::chrono::milliseconds(100));
}

void MapStart() {
	Check(g_onMapStart != nullptr, "the map start listener is registered");
	if (g_onMapStart != nullptr) {
		g_onMapStart();
	}
}

void MapEnd() {
	Check(g_onMapEnd != nullptr, "the map end listener is registered");
	if (g_onMapEnd != nullptr) {
		g_onMapEnd();
	}
}

// A map is up and the plugin's update has run (the gate is open).
void ReadyMap() {
	MapStart();
	Update();
}

void SelectAdminMapsItem(int32_t slot) {
	Check(g_itemSelect != nullptr, "the admin 'maps' item is registered");
	if (g_itemSelect != nullptr) {
		g_itemSelect(slot, plg::string("server"), plg::string("maps"), plg::string("Item_Maps"));
	}
}

void FireCoreLoaded() {
	Check(g_coreLoadedCallback != nullptr, "the OnCoreLoaded callback is registered");
	if (g_coreLoadedCallback != nullptr) {
		g_coreLoadedCallback();
	}
}

// Opens the map menu through the admin item and returns the real (non-reserved) menu id.
s2sdk::MenuId OpenMapMenu(int32_t slot) {
	SelectAdminMapsItem(slot);
	Update();
	for (const FakeMenu& menu : g_menus) {
		if (menu.title != "as_maps:reserved-id-0") {
			return menu.id;
		}
	}
	Check(false, "the map menu was created");
	return 0;
}

// --- tests ---------------------------------------------------------------------------------------

void TestStartRegistersEverythingWithoutClientQueries() {
	PrepareTest();
	SetClient(3, true, 76561198000000003, "Admin");

	StartPlugin();

	Check(g_clientCalls == 0, "OnPluginStart makes no client query (PITFALLS B1)");
	Check(g_mapStartRegisters == 1 && g_mapEndRegisters == 1, "both map listeners are registered");
	Check(g_commands.size() == 1 && g_commands.front().name == "mm_map", "mm_map is registered once");
	Check((static_cast<int64_t>(g_commands.front().flags) & static_cast<int64_t>(s2sdk::ConVarFlag::Release)) != 0,
		  "mm_map carries Release");
	Check((static_cast<int64_t>(g_commands.front().flags) &
		   static_cast<int64_t>(s2sdk::ConVarFlag::ClientCanExecute)) == 0,
		  "mm_map is not client-executable (the original's FCVAR_NONE)");
	Check(g_coreLoadedRegisters == 1, "the OnCoreLoaded callback is registered");
	Check(g_registeredCategories.size() == 1 && g_registeredCategories.front() == "server",
		  "the 'server' category is registered immediately when the core is already loaded");
	Check(g_registeredCategoryCallbacks.size() == 1 && g_registeredCategoryCallbacks.front() == nullptr,
		  "the category is registered through the raw binding with a null display callback");
	Check(g_registeredItems.size() == 1, "the 'maps' item is registered");
	if (!g_registeredItems.empty()) {
		const RegisteredItem& item = g_registeredItems.front();
		Check(item.identity == "maps" && item.name == "Item_Maps" && item.category == "server",
			  "the item keeps the original identities");
		Check(item.flags == "@admin/maps", "the item keeps the @admin/maps permission");
		Check(item.select != nullptr, "the item has its select callback");
		Check(item.display != nullptr, "the loaded item keeps the wrapper's display trampoline");
	}

	Check(g_menus.empty(), "no menu is created at start");
	Check(g_timers.empty(), "no timer is created at start");

	FinishTest();
}

void TestCoreLoadedLaterAndIdempotent() {
	PrepareTest();
	g_coreLoaded = false;

	StartPlugin();
	Check(g_registeredItems.empty(), "nothing is registered before OnCoreLoaded");
	Check(g_coreLoadedRegisters == 1, "the callback is registered");

	FireCoreLoaded();
	Check(g_registeredItems.size() == 1, "the item is registered when the core loads");
	FireCoreLoaded();
	Check(g_registeredItems.size() == 1, "a repeated OnCoreLoaded does not register twice");

	FinishTest();
}

void TestMapGateMidMapLatch() {
	PrepareTest();
	SetClient(3, true, 76561198000000003, "Admin");
	StartPlugin();
	// The plugin was loaded into a running map: no OnMapEnd since start, so the first update is the
	// "map is up" signal (PITFALLS B1).
	Update();
	SelectAdminMapsItem(3);
	Update();
	Check(g_menus.size() == 2, "a plugin loaded mid-map can open the menu after its first update");

	FinishTest();
}

void TestMapGateChangeWindowDoesNotLatch() {
	PrepareTest();
	SetClient(3, true, 76561198000000003, "Admin");
	StartPlugin();
	// The plugin was loaded during a map change: OnMapEnd arrives before the first update.
	MapEnd();
	Update();
	Check(!g_mapReady, "the gate stays closed when an OnMapEnd was seen before the first update");

	const int menuCalls = g_createMenuCalls;
	SelectAdminMapsItem(3);
	Update();
	Check(g_createMenuCalls == menuCalls, "no menu is built while the gate is closed");
	Check(g_chatPrints.empty() && g_timers.empty(), "no client output and no timer while the gate is closed");

	MapStart();
	Update();
	SelectAdminMapsItem(3);
	Update();
	Check(g_createMenuCalls == menuCalls + 2, "the next map opens the gate again (reserved id + menu)");

	FinishTest();
}

void TestCommandServerConsoleUsage() {
	PrepareTest();
	StartPlugin();
	ReadyMap();

	Check(DispatchMmMap(-1, s2sdk::ConCommandContext::Console, {"mm_map"}) == s2sdk::ResultType::Handled,
		  "the server console command is handled");
	Update();

	Check(g_serverPrints.size() == 1, "the usage goes to the server console");
	Check(g_serverPrints.front() == "Используйте: mm_map <map>", "the phrase is formatted and stripped");
	Check(g_chatPrints.empty(), "no chat output for the usage");
	Check(g_timers.empty() && g_serverCommands.empty(), "a usage error schedules nothing");

	FinishTest();
}

void TestCommandChangeLevel() {
	PrepareTest();
	StartPlugin();
	ReadyMap();

	Check(DispatchMmMap(-1, s2sdk::ConCommandContext::Console, {"mm_map", "de_dust2"}) ==
			  s2sdk::ResultType::Handled,
		  "mm_map is handled");
	Check(g_serverCommands.empty(), "nothing is executed from the command callback");
	Check(g_timers.empty(), "the timer is scheduled from OnPluginUpdate, not from the callback");

	Update();
	Check(g_timers.size() == 1, "one delayed change is scheduled");
	if (!g_timers.empty()) {
		Check(g_timers.front().delay == 5.0, "the Pisex five seconds are kept");
		Check(g_timers.front().flags == s2sdk::TimerFlag::NoMapChange,
			  "the timer is cancelled by a map change (NoMapChange)");
	}
	Check(g_serverCommands.empty(), "the engine command is not issued before the timer fires");
	Check(g_chatPrints.size() == 1, "message_type 1 announces the change");
	Check(g_chatPrints.front() == "Смена карты на \x0F" "Dust2\x01" "...",
		  "the announcement uses the display name and the chat colour table");
	Check(g_actions.size() == 1 && g_actions.front().slot == -1 && g_actions.front().action == "map" &&
			  g_actions.front().param == "de_dust2",
		  "SendAction('map') reports the console change");

	const s2sdk::TimerId timer = g_timers.front().id;
	FireTimer(timer);
	Check(g_serverCommands.empty(), "the timer callback runs no server command (it holds the timer lock)");
	Update();
	Check(g_serverCommands.size() == 1 && g_serverCommands.front() == "changelevel de_dust2",
		  "the changelevel runs from OnPluginUpdate after the timer fired");
	Update();
	Check(g_serverCommands.size() == 1, "the one-shot command is issued exactly once");

	FinishTest();
}

void TestUnsafeMapNamesRejected() {
	PrepareTest();
	StartPlugin();
	ReadyMap();

	// The map name is the only caller input that is spliced into a server command: a space, a
	// command separator, a newline, a path traversal or a leading '-' must never reach the engine.
	for (const char* bad : {"de_dust2; rcon_password x", "de dust2", "../../etc/passwd",
							"de_dust2\nquit", "-de_dust2", ".de_dust2", ""}) {
		DispatchMmMap(-1, s2sdk::ConCommandContext::Console, {"mm_map", bad});
		Update();
	}
	Check(g_timers.empty() && g_serverCommands.empty() && g_chatPrints.empty() && g_actions.empty(),
		  "an invalid map name is refused before IsMapValid/announce/timer");
	Check(LoggedError("invalid map name"), "the invalid map name is logged");

	// A workshop path is a normal map name (alphanumerics plus '/', '_' and '-').
	DispatchMmMap(-1, s2sdk::ConCommandContext::Console, {"mm_map", "workshop/3070901615/de_dust2"});
	Update();
	Check(g_timers.size() == 1, "a valid workshop path is accepted");
	FireTimer(g_timers.front().id);
	Update();
	Check(g_serverCommands.size() == 1 &&
			  g_serverCommands.front() == "ds_workshop_changelevel workshop/3070901615/de_dust2",
		  "the workshop command is built unchanged");

	FinishTest();
}

void TestCommandWorkshopMap() {
	PrepareTest();
	StartPlugin();
	ReadyMap();

	// awp_lego_2 is not a server map in the fake: IsMapValid answers false.
	DispatchMmMap(-1, s2sdk::ConCommandContext::Console, {"mm_map", "awp_lego_2"});
	Update();
	FireTimer(g_timers.front().id);
	Update();

	Check(g_serverCommands.size() == 1 && g_serverCommands.front() == "ds_workshop_changelevel awp_lego_2",
		  "an invalid (workshop) map uses ds_workshop_changelevel");
	Check(g_chatPrints.size() == 1 && g_chatPrints.front().find("AWP LEGO 2") != std::string::npos,
		  "the maps.ini display name is used");

	FinishTest();
}

void TestCommandRefusesClientCaller() {
	PrepareTest();
	SetClient(3, true, 76561198000000003, "Admin");
	StartPlugin();
	ReadyMap();

	Check(DispatchMmMap(3, s2sdk::ConCommandContext::Console, {"mm_map", "de_dust2"}) ==
			  s2sdk::ResultType::Handled,
		  "a client console caller is swallowed");
	Update();
	Check(g_timers.empty() && g_serverCommands.empty() && g_chatPrints.empty() && g_actions.empty(),
		  "a client caller runs nothing");
	Check(g_permissionChecks.empty(), "the server-only command does not need a permission lookup");

	Check(DispatchMmMap(3, s2sdk::ConCommandContext::Chat, {"mm_map", "de_dust2"}) ==
			  s2sdk::ResultType::Continue,
		  "a chat !mm_map is left to the engine (Continue)");
	Update();
	Check(g_timers.empty() && g_serverCommands.empty(), "the chat spelling runs nothing");

	// The server-console bypass is strictly caller == -1: any other negative value fails closed.
	for (const int32_t caller : {-2, -3, -64, 64}) {
		Check(DispatchMmMap(caller, s2sdk::ConCommandContext::Console, {"mm_map", "de_dust2"}) ==
				  s2sdk::ResultType::Handled,
			  "a non-console caller is refused (caller " + std::to_string(caller) + ")");
	}
	Update();
	Check(g_timers.empty() && g_serverCommands.empty() && g_chatPrints.empty(),
		  "an out-of-range caller runs nothing");

	FinishTest();
}

void TestSecondRequestReplacesPendingChange() {
	PrepareTest();
	StartPlugin();
	ReadyMap();

	DispatchMmMap(-1, s2sdk::ConCommandContext::Console, {"mm_map", "de_dust2"});
	Update();
	const s2sdk::TimerId first = g_timers.front().id;

	DispatchMmMap(-1, s2sdk::ConCommandContext::Console, {"mm_map", "de_mirage"});
	Update();
	Check(g_timers.size() == 2, "the second request creates a new timer");
	Check(g_killedTimers.size() == 1 && g_killedTimers.front() == first,
		  "the pending timer is killed instead of running two changelevels");

	// The engine may still call the old callback; its id no longer matches.
	FireTimer(first);
	Update();
	Check(g_serverCommands.empty(), "a stale timer callback runs nothing");

	FireTimer(g_timers.back().id);
	Update();
	Check(g_serverCommands.size() == 1 && g_serverCommands.front() == "changelevel de_mirage",
		  "only the latest change is issued");

	FinishTest();
}

void TestScheduledChangeRevalidatesAuthor() {
	constexpr uint64_t kSteam = 76561198000000003;

	// Schedules a menu selection and returns the pending timer id; the announcement is already out
	// at this point (it must not move to the re-validation).
	const auto scheduleFromMenu = []() -> s2sdk::TimerId {
		PrepareTest();
		SetClient(3, true, kSteam, "Admin");
		StartPlugin();
		ReadyMap();
		const s2sdk::MenuId menu = OpenMapMenu(3);
		InvokeMenu(menu, s2sdk::MenuAction::Select, 3, 1);
		Update();
		Check(g_timers.size() == 1, "the selection scheduled one change timer");
		Check(g_chatPrints.size() == 1, "the announcement happens at schedule time");
		return g_timers.back().id;
	};

	const auto expectDrop = [&scheduleFromMenu](const char* what, auto mutate) {
		const s2sdk::TimerId timer = scheduleFromMenu();
		mutate();
		FireTimer(timer);
		Update();
		Check(g_serverCommands.empty(), std::string("no ServerCommand when the author is invalid: ") + what);
		Check(LoggedError("dropping the delayed level change"),
			  std::string("the dropped request is logged: ") + what);
		FinishTest();
	};

	expectDrop("permission revoked", [] { g_deniedSlots.insert(3); });
	expectDrop("author disconnected", [] { g_clients[3].connected = false; });
	expectDrop("slot reused by another steamid", [] { g_clients[3].steamid = 76561198000000099; });
	expectDrop("controller recreated", [] { g_clients[3].generation = 1; });
	expectDrop("admin data not ready",
			   [] { g_notReadyPairs.insert(std::make_pair(3, static_cast<uint64_t>(76561198000000003))); });

	// The normal owner still changes the map five seconds later.
	{
		const s2sdk::TimerId timer = scheduleFromMenu();
		FireTimer(timer);
		Update();
		Check(g_serverCommands.size() == 1 && g_serverCommands.front() == "changelevel de_dust2",
			  "a still-valid author changes the map");
		FinishTest();
	}

	// The server console keeps its bypass: it has no admin record to lose.
	{
		PrepareTest();
		SetClient(3, true, kSteam, "Admin");
		StartPlugin();
		ReadyMap();
		DispatchMmMap(-1, s2sdk::ConCommandContext::Console, {"mm_map", "de_dust2"});
		Update();
		Check(g_timers.size() == 1, "the console request scheduled a timer");
		g_deniedSlots.insert(3);
		g_clients[3].connected = false;
		FireTimer(g_timers.back().id);
		Update();
		Check(g_serverCommands.size() == 1 && g_serverCommands.front() == "changelevel de_dust2",
			  "the server console request stays valid regardless of client state");
		FinishTest();
	}
}

void TestMapEndDropsPendingChange() {
	PrepareTest();
	StartPlugin();
	ReadyMap();

	DispatchMmMap(-1, s2sdk::ConCommandContext::Console, {"mm_map", "de_dust2"});
	Update();
	const s2sdk::TimerId timer = g_timers.front().id;
	const int clientCallsBefore = g_clientCalls;

	MapEnd();
	Check(g_clientCalls == clientCallsBefore, "OnMapEnd performs no client query (PITFALLS B2)");

	// s2sdk removes NoMapChange timers before the listeners; simulate the engine still delivering
	// the callback of the old map.
	FireTimer(timer);
	Update();
	Check(g_serverCommands.empty(), "a change of the previous map is never issued on the new one");

	MapStart();
	Update();
	Check(g_serverCommands.empty(), "the new map starts without a pending change");

	FinishTest();
}

void TestMessageTypes() {
	PrepareTest();
	SetClient(3, true, 76561198000000003, "Admin");

	// message_type 0: silent, but SendAction still fires.
	g_messageType = 0;
	StartPlugin();
	ReadyMap();
	DispatchMmMap(-1, s2sdk::ConCommandContext::Console, {"mm_map", "de_dust2"});
	Update();
	Check(g_chatPrints.empty(), "message_type 0 prints nothing");
	Check(g_actions.size() == 1, "message_type 0 still sends the action");
	FinishTest();

	// message_type 2 from the server console: "Console" is the actor name.
	PrepareTest();
	SetClient(3, true, 76561198000000003, "Admin");
	g_messageType = 2;
	StartPlugin();
	ReadyMap();
	DispatchMmMap(-1, s2sdk::ConCommandContext::Console, {"mm_map", "de_dust2"});
	Update();
	Check(g_chatPrints.size() == 1 &&
			  g_chatPrints.front() ==
					  "Админ \x0F" "Console\x01" " сменил карту на \x0F" "Dust2\x01",
		  "message_type 2 uses 'Console' for the server console");
	FinishTest();
}

void TestUnknownMapFallsBackToFileName() {
	PrepareTest();
	g_validMaps.insert("de_nuke"); // valid, but not in maps.ini
	StartPlugin();
	ReadyMap();

	DispatchMmMap(-1, s2sdk::ConCommandContext::Console, {"mm_map", "de_nuke"});
	Update();

	Check(g_chatPrints.size() == 1 && g_chatPrints.front().find("de_nuke") != std::string::npos,
		  "a map outside maps.ini is announced by its file name");
	FireTimer(g_timers.front().id);
	Update();
	Check(g_serverCommands.size() == 1 && g_serverCommands.front() == "changelevel de_nuke",
		  "the map still changes");

	FinishTest();
}

void TestMenuDisplay() {
	PrepareTest();
	SetClient(3, true, 76561198000000003, "Admin");
	StartPlugin();
	ReadyMap();

	SelectAdminMapsItem(3);
	Check(g_createMenuCalls == 0, "the admin item callback creates no menu (it runs under the menu lock)");
	Update();

	Check(g_createMenuCalls == 2, "the reserved id 0 and the real menu are created");
	Check(g_menus.size() == 2, "both menus exist");
	Check(g_menus[0].id == 0 && g_menus[0].title == "as_maps:reserved-id-0" && g_menus[0].type == "chat",
		  "menu id 0 is burned with the reserved dummy");
	const FakeMenu& menu = g_menus[1];
	Check(menu.id == 1, "the real menu gets the next id");
	Check(menu.type == "hud", "the hud backend is preferred when hudmenu registered it");
	Check(menu.title == "Выберите карту", "the SelectMap phrase is the menu title");
	Check(menu.items.size() == 3, "the three maps.ini entries are added");
	if (menu.items.size() == 3) {
		Check(menu.items[0] == std::make_pair(std::string("awp_lego_2"), std::string("AWP LEGO 2")),
			  "item 0 is the map file/display pair (alphabetical map-file order)");
		Check(menu.items[1] == std::make_pair(std::string("de_dust2"), std::string("Dust2")), "item 1");
		Check(menu.items[2] == std::make_pair(std::string("de_mirage"), std::string("Mirage")), "item 2");
	}
	Check(menu.pagination == 5, "five entries per page, like the Pisex menu");
	Check(menu.exitButton, "the exit button stays on");
	Check(menu.backButton, "the back button returns to the admin category");
	Check(menu.displayCalled && g_menuOpen[3] && g_openMenuId[3] == menu.id, "the menu is displayed to the slot");

	FinishTest();
}

void TestMenuTypeFallbacks() {
	PrepareTest();
	SetClient(3, true, 76561198000000003, "Admin");
	g_menuTypes = {"button"};
	StartPlugin();
	ReadyMap();
	SelectAdminMapsItem(3);
	Update();
	Check(g_createMenuCalls == 2 && g_menus[1].type == "button", "without hud/wasd the built-in button is used");
	FinishTest();

	PrepareTest();
	SetClient(3, true, 76561198000000003, "Admin");
	g_menuTypes.clear();
	StartPlugin();
	ReadyMap();
	SelectAdminMapsItem(3);
	Update();
	Check(g_createMenuCalls == 0, "no menu is created when no menu type is registered");
	Check(LoggedError("no s2sdk menu type is registered"), "the disabled menu subsystem is logged");
	FinishTest();
}

void TestMenuSelectAndIdentity() {
	PrepareTest();
	SetClient(3, true, 76561198000000003, "Admin");
	g_messageType = 2;
	StartPlugin();
	ReadyMap();

	const s2sdk::MenuId menu = OpenMapMenu(3);
	Check(menu != 0, "the real menu id is not the reserved 0");
	Check(g_destroyedMenus.empty(), "nothing is destroyed inside the menu handler");
	Check(g_actions.empty(), "no action inside the menu handler");

	g_chatPrints.clear();
	InvokeMenu(menu, s2sdk::MenuAction::Select, 3, 1); // de_dust2
	Check(g_destroyedMenus.empty(), "the destroy is deferred outside the handler");
	Check(g_serverCommands.empty() && g_actions.empty(), "the select runs nothing inside the handler");
	Update();
	Check(g_destroyedMenus.size() == 1 && g_destroyedMenus.front() == menu,
		  "the selected menu is destroyed from OnPluginUpdate");
	Check(g_chatPrints.size() == 1 && g_chatPrints.front().find("Admin") != std::string::npos &&
			  g_chatPrints.front().find("Dust2") != std::string::npos,
		  "message_type 2 names the acting admin");
	Check(g_actions.size() == 1 && g_actions.front().slot == 3 && g_actions.front().param == "de_dust2",
		  "the action carries the acting slot and the map file");
	Check(g_timers.size() == 1, "the change is scheduled");
	FireTimer(g_timers.front().id);
	Update();
	Check(g_serverCommands.size() == 1 && g_serverCommands.front() == "changelevel de_dust2",
		  "the selected map changes five seconds later");

	FinishTest();
}

void TestMenuSlotReuse() {
	PrepareTest();
	SetClient(3, true, 76561198000000003, "Admin");
	StartPlugin();
	ReadyMap();
	const s2sdk::MenuId menu = OpenMapMenu(3);

	// The slot is reused by another player before he selects.
	g_clients[3].steamid = 76561198000000099;
	g_clients[3].name = "Other";

	InvokeMenu(menu, s2sdk::MenuAction::Select, 3, 1);
	Update();
	Check(g_chatPrints.empty() && g_actions.empty() && g_timers.empty(),
		  "a reused slot never acts for the new player");

	// The back button is refused for a foreign identity as well.
	const s2sdk::MenuId second = OpenMapMenu(3);
	Check(second != 0, "a new menu is displayed to the new player");
	g_clients[3].steamid = 76561198000000003;
	InvokeMenu(second, s2sdk::MenuAction::Cancel, 3,
			   static_cast<int32_t>(s2sdk::MenuCancelReason::ExitBack));
	Update();
	Check(g_lastCategoryCalls.empty(), "the back button of a stale identity does nothing");

	FinishTest();
}

void TestMenuBackAndTimeout() {
	PrepareTest();
	SetClient(3, true, 76561198000000003, "Admin");
	StartPlugin();
	ReadyMap();

	const s2sdk::MenuId menu = OpenMapMenu(3);
	InvokeMenu(menu, s2sdk::MenuAction::Cancel, 3,
			   static_cast<int32_t>(s2sdk::MenuCancelReason::ExitBack));
	Check(g_lastCategoryCalls.empty(), "the back action is deferred");
	Update();
	Check(g_lastCategoryCalls.size() == 1 && g_lastCategoryCalls.front() == 3,
		  "the back button shows the admin core's last category menu");
	Check(g_actions.empty() && g_timers.empty(), "back changes no map");
	Check(g_destroyedMenus.size() == 1 && g_destroyedMenus.front() == menu, "the menu is destroyed");

	const s2sdk::MenuId timedOut = OpenMapMenu(3);
	InvokeMenu(timedOut, s2sdk::MenuAction::Cancel, 3,
			   static_cast<int32_t>(s2sdk::MenuCancelReason::Timeout));
	InvokeMenu(timedOut, s2sdk::MenuAction::End, 3, 0);
	Update();
	Check(g_lastCategoryCalls.size() == 1, "a timeout never goes back");
	Check(g_destroyedMenus.size() == 2 && g_destroyedMenus[1] == timedOut, "the timed-out menu is destroyed");

	FinishTest();
}

void TestMenuNotShownForInvalidSlots() {
	PrepareTest();
	// Bots, SourceTV and clients that are not in game get no menu.
	SetClient(4, true, 76561198000000004, "Bot", /*fake=*/true);
	SetClient(5, false, 76561198000000005, "Connecting");
	SetClient(6, true, 0, "GOTV");
	g_clients[6].sourceTV = true;

	StartPlugin();
	ReadyMap();
	for (const int32_t slot : {4, 5, 6}) {
		SelectAdminMapsItem(slot);
		Update();
	}
	Check(g_createMenuCalls == 0, "no menu for a bot, a connecting client or SourceTV");
	Check(g_registeredItems.size() == 1, "the admin item itself is registered (the core filters access)");

	FinishTest();
}

void TestMapEndCancelsAndDestroysMenus() {
	PrepareTest();
	SetClient(3, true, 76561198000000003, "Admin");
	StartPlugin();
	ReadyMap();
	const s2sdk::MenuId menu = OpenMapMenu(3);

	const int clientCallsBefore = g_clientCalls;
	MapEnd();
	// OnMapEnd ends the display session without the backend close (CancelClientMenu with the
	// Disconnect reason), then frees the menu; neither touches a client (PITFALLS B2).
	Check(g_clientCalls == clientCallsBefore, "the map-end menu cleanup performs no client query");
	Check(g_cancelledClients.size() == 1 && g_cancelledClients.front().first == 3 &&
			  g_cancelledClients.front().second == s2sdk::MenuCancelReason::Disconnect,
		  "OnMapEnd cancels the display session with the Disconnect reason");
	bool destroyed = false;
	for (const s2sdk::MenuId id : g_destroyedMenus) {
		destroyed = destroyed || id == menu;
	}
	Check(destroyed, "OnMapEnd frees the menu after ending the session");
	Check(g_backendCloseCalls == 0, "no menu backend close runs over the dying client layer");
	Check(g_openMenus[3].active == false, "the menu state is dropped at map end");

	Update();
	MapStart();
	Update();
	Check(g_destroyedMenus.size() == 1, "nothing is destroyed twice on the next map");

	FinishTest();
}

void TestUnloadWithMapDownCancelsAndDestroysQueuedMenus() {
	PrepareTest();
	SetClient(3, true, 76561198000000003, "Admin");
	StartPlugin();
	ReadyMap();
	const s2sdk::MenuId menu = OpenMapMenu(3);

	// Simulate a missing session binding at map end: the cleanup is then deferred instead of
	// destroying a menu whose backend could still close over the dying client layer.
	__s2sdk_CancelClientMenu = nullptr;
	MapEnd();
	Check(g_destroyedMenus.empty(), "without CancelClientMenu the destroy waits");
	__s2sdk_CancelClientMenu = &FakeCancelClientMenu;

	(void)g_asMapsPlugin.OnPluginEnd();
	Check(g_cancelledClients.size() == 1 && g_cancelledClients.front().first == 3,
		  "the map-down unload ends the display session first");
	bool destroyed = false;
	for (const s2sdk::MenuId id : g_destroyedMenus) {
		destroyed = destroyed || id == menu;
	}
	Check(destroyed, "the queued menu is freed by the map-down unload after the cancel");

	FinishTest();
}

void TestMapEndCancelFalseSwitchesToBuiltin() {
	PrepareTest();
	SetClient(3, true, 76561198000000003, "Admin");
	StartPlugin();
	ReadyMap();
	const s2sdk::MenuId menu = OpenMapMenu(3);

	// CancelClientMenu refuses and the display is still active. The map-end cleanup must switch the
	// menu to a builtin type whose Close is a no-op, verify the switch, and only then destroy it.
	g_cancelFails = true;
	MapEnd();
	Check(g_cancelledClients.size() == 1 &&
			  g_cancelledClients.front().second == s2sdk::MenuCancelReason::Disconnect,
		  "the failed cancel was attempted with the Disconnect reason");
	Check(g_menuTypeSwitches.size() == 1 && g_menuTypeSwitches.front().first == menu &&
			  g_menuTypeSwitches.front().second == "chat",
		  "the still-displayed menu is switched to the builtin chat type");
	Check(g_destroyedMenus.size() == 1 && g_destroyedMenus.front() == menu, "the menu is destroyed after the switch");
	Check(g_destroyedMenuTypes.size() == 1 && g_destroyedMenuTypes.front() == "chat",
		  "DestroyMenu saw the verified builtin type");
	Check(g_backendCloseCalls == 0, "the custom backend close never runs over the client layer");
	Check(!LoggedError("A7_UNRESOLVED"), "the fail-safe resolved the cleanup");

	(void)g_asMapsPlugin.OnPluginEnd();
	Check(g_backendCloseCalls == 0, "the unload after the map end runs no backend close");

	FinishTest();
}

void TestMapEndCancelFalseButDisplayGoneDoesNotSwitch() {
	PrepareTest();
	SetClient(3, true, 76561198000000003, "Admin");
	StartPlugin();
	ReadyMap();
	const s2sdk::MenuId menu = OpenMapMenu(3);

	// The cancel answered false but the session ended anyway: the GetClientMenu re-check must avoid
	// an unnecessary type switch and destroy the menu directly (no viewer left).
	g_cancelFails = true;
	g_cancelFailsButCloses = true;
	MapEnd();
	Check(g_menuTypeSwitches.empty(), "a display that is already gone needs no type switch");
	Check(g_destroyedMenus.size() == 1 && g_destroyedMenus.front() == menu, "the menu is destroyed");
	Check(g_backendCloseCalls == 0, "no backend close runs for a menu without viewers");

	FinishTest();
}

void TestMapEndUnresolvedSwitchIsReported() {
	PrepareTest();
	SetClient(3, true, 76561198000000003, "Admin");
	StartPlugin();
	ReadyMap();
	const s2sdk::MenuId menu = OpenMapMenu(3);

	// Cancel fails and SetMenuType is gone: there is no safe way to free the still-displayed menu.
	// A7_UNRESOLVED must be logged and the menu must not be destroyed in the map-change window.
	g_cancelFails = true;
	__s2sdk_SetMenuType = nullptr;
	MapEnd();
	Check(LoggedError("A7_UNRESOLVED"), "the unresolved cleanup is reported explicitly");
	Check(std::find(g_destroyedMenus.begin(), g_destroyedMenus.end(), menu) == g_destroyedMenus.end(),
		  "the still-displayed menu is not destroyed in the map window");
	Check(g_backendCloseCalls == 0, "no backend close runs on the dying client layer");

	// It is retried on the map-down unload: still unresolved, still not destroyed, still no crash.
	// (Only the reserved id-0 dummy is released there.)
	(void)g_asMapsPlugin.OnPluginEnd();
	Check(g_backendCloseCalls == 0, "the unload retry also runs no backend close");
	Check(std::find(g_destroyedMenus.begin(), g_destroyedMenus.end(), menu) == g_destroyedMenus.end(),
		  "the unresolved menu is left alone instead of risking the client layer");

	FinishTest();
}

void TestUnloadMidMapClosesBackendOnce() {
	PrepareTest();
	SetClient(3, true, 76561198000000003, "Admin");
	StartPlugin();
	ReadyMap();
	OpenMapMenu(3);

	// With a map up the clients are valid: the backend close is what hides the on-screen menu.
	(void)g_asMapsPlugin.OnPluginEnd();
	Check(g_backendCloseCalls == 1, "a map-up unload lets the custom backend close the menu once");
	Check(g_cancelledClients.empty(), "a map-up unload does not need the Disconnect cancel");

	FinishTest();
}

void TestTeardown() {
	PrepareTest();
	SetClient(3, true, 76561198000000003, "Admin");
	StartPlugin();
	ReadyMap();
	OpenMapMenu(3);
	DispatchMmMap(-1, s2sdk::ConCommandContext::Console, {"mm_map", "de_dust2"});
	Update();
	const s2sdk::TimerId timer = g_timers.front().id;
	const s2sdk::MenuId menu = g_menus[1].id;
	// The core still holds this callback pointer for a moment after the unload; the retirement
	// call must not be the one the stale-callback test invokes.
	const admin_system::ItemSelectCallback staleItemSelect = g_itemSelect;

	(void)g_asMapsPlugin.OnPluginEnd();

	Check(g_removedCommands.size() == 1 && g_removedCommands.front().first == "mm_map" &&
			  g_removedCommands.front().second == &OnMmMapCommand,
		  "OnPluginEnd removes the command it created");
	Check(g_removedListeners.empty(), "RemoveCommandListener is not used for an owned command");
	Check(g_coreLoadedUnregisters == 1, "the OnCoreLoaded callback is unregistered");
	Check(g_mapStartUnregisters == 1 && g_mapEndUnregisters == 1, "both map listeners are unregistered");
	Check(g_killedTimers.size() == 1 && g_killedTimers.front() == timer, "the pending timer is killed");
	Check(g_destroyedMenus.size() >= 2, "the open menu and the reserved id 0 are destroyed");
	bool destroyedReserved = false;
	bool destroyedMenu = false;
	for (const s2sdk::MenuId id : g_destroyedMenus) {
		destroyedReserved = destroyedReserved || id == 0;
		destroyedMenu = destroyedMenu || id == menu;
	}
	Check(destroyedReserved, "the reserved menu id 0 is released");
	Check(destroyedMenu, "the displayed menu is destroyed");
	Check(g_cancelledClients.empty(), "a map-up unload lets the backend close the menu (no forced Disconnect)");
	Check(g_registeredItems.size() == 2 && g_registeredItems.back().display == nullptr &&
			  g_registeredItems.back().select == nullptr,
		  "the admin item is retired with null callbacks so the core holds no pointer into the .so");
	Check(g_registeredCategoryCallbacks.size() == 1 && g_registeredCategoryCallbacks.front() == nullptr,
		  "the category never holds a trampoline into the .so");
	Check(g_pAdminApi == nullptr, "the admin API pointer is dropped");

	// A callback the engine still holds for a moment must not reach a half-unloaded plugin.
	const std::size_t timersBefore = g_timers.size();
	const int menuCallsBefore = g_createMenuCalls;
	const s2sdk::ResultType stale = DispatchMmMap(-1, s2sdk::ConCommandContext::Console, {"mm_map", "de_dust2"});
	Check(stale == s2sdk::ResultType::Handled, "a stale command callback answers Handled");
	Check(staleItemSelect != nullptr, "the stale admin item callback was captured");
	if (staleItemSelect != nullptr) {
		staleItemSelect(3, plg::string("server"), plg::string("maps"), plg::string("Item_Maps"));
	}
	Update();
	Check(g_createMenuCalls == menuCallsBefore, "a stale admin item callback opens no menu after the unload");
	Check(g_timers.size() == timersBefore, "a stale callback schedules nothing");

	FinishTest();
}

void TestTeardownOfPreExistingCommand() {
	PrepareTest();
	g_existingCommands.insert("mm_map");
	StartPlugin();

	Check(g_commands.size() == 1, "the pre-existing name is hooked");
	Check(g_removedCommands.empty(), "nothing removed yet");

	(void)g_asMapsPlugin.OnPluginEnd();
	Check(g_removedCommands.empty(), "RemoveCommand is never called for a name this plugin did not create");
	Check(g_removedListeners.size() == 1 && g_removedListeners.front() == "mm_map",
		  "only the listener is detached instead");

	FinishTest();
}

void TestFailSoftWithoutAdminBindings() {
	PrepareTest();
	__admin_system_GetPluginVersion = nullptr;
	__admin_system_GetTranslation = nullptr;
	__admin_system_GetMessageType = nullptr;
	__admin_system_IsCoreLoaded = nullptr;
	__admin_system_RegisterCategory = nullptr;
	__admin_system_RegisterItem = nullptr;
	__admin_system_HasPermission = nullptr;
	__admin_system_SendAction = nullptr;
	__admin_system_ShowAdminLastCategoryMenu = nullptr;
	__admin_system_OnCoreLoaded_Register = nullptr;
	__admin_system_OnCoreLoaded_Unregister = nullptr;

	SetClient(3, true, 76561198000000003, "Admin");
	StartPlugin();

	Check(g_commands.empty(), "no command without the admin API");
	Check(g_registeredItems.empty(), "no menu item without the admin API");
	Check(LoggedError("admin_system bindings are not available"), "the missing dependency is logged");
	Update();
	Check(g_menus.empty() && g_timers.empty(), "the idle plugin does nothing on update");

	(void)g_asMapsPlugin.OnPluginEnd();
	Check(g_mapStartUnregisters == 1 && g_mapEndUnregisters == 1, "the listeners registered before the check are removed");

	FinishTest();
}

void TestFailSoftWithoutS2sdk() {
	PrepareTest();
	plg::GetConfigsDir = &FakeGetConfigsDir;
	plg::GetDataDir = &FakeGetDataDir;
	plg::Log = &FakeLog;
	__s2sdk_OnMapStart_Register = nullptr;
	__s2sdk_OnMapEnd_Register = nullptr;
	__s2sdk_AddConsoleCommand = nullptr;
	__s2sdk_CreateMenu = nullptr;
	__s2sdk_CreateTimer = nullptr;

	SetClient(3, true, 76561198000000003, "Admin");
	StartPlugin();
	Check(g_commands.empty(), "no command without the s2sdk binding");
	Check(LoggedError("AddConsoleCommand binding is unavailable"), "the missing s2sdk command binding is logged");
	Update();
	Check(!g_mapReady, "the gate stays closed without map listeners");
	SelectAdminMapsItem(3);
	Update();
	Check(g_menus.empty(), "no menu without the s2sdk menu binding");
	(void)g_asMapsPlugin.OnPluginEnd();

	FinishTest();
}

void TestEmptyMapsConfig() {
	PrepareTest();
	g_configsDir = "/nonexistent-as-maps-test-configs";
	SetClient(3, true, 76561198000000003, "Admin");
	StartPlugin();
	ReadyMap();
	Check(LoggedError("Failed to load"), "the missing maps.ini is logged and the plugin stays loaded");

	SelectAdminMapsItem(3);
	Update();
	Check(g_createMenuCalls == 2, "the menu still opens (with no entries) when maps.ini is missing");
	Check(g_menus.size() == 2 && g_menus[1].items.empty(), "the map list is empty, not a crash");

	FinishTest();
}

void TestMenuCreationRequiresCleanupBindings() {
	PrepareTest();
	SetClient(3, true, 76561198000000003, "Admin");
	StartPlugin();
	ReadyMap();

	// The dummy id 0 and every real menu require the whole cleanup chain up front: creating a menu
	// without GetClientMenu/CancelClientMenu would leave the map-end/unload path unable to end the
	// display session before DestroyMenu (PITFALLS B2).
	const auto expectNoMenu = [](const char* what) {
		SelectAdminMapsItem(3);
		Update();
		Check(g_createMenuCalls == 0, std::string("no menu (and no id-0 dummy) without ") + what);
		Check(LoggedError("s2sdk menu bindings"), std::string("the missing binding is logged: ") + what);
	};
	__s2sdk_CreateMenu = nullptr;
	expectNoMenu("CreateMenu");
	__s2sdk_CreateMenu = &FakeCreateMenu;
	__s2sdk_AddMenuItem = nullptr;
	expectNoMenu("AddMenuItem");
	__s2sdk_AddMenuItem = &FakeAddMenuItem;
	__s2sdk_DisplayMenu = nullptr;
	expectNoMenu("DisplayMenu");
	__s2sdk_DisplayMenu = &FakeDisplayMenu;
	__s2sdk_DestroyMenu = nullptr;
	expectNoMenu("DestroyMenu");
	__s2sdk_DestroyMenu = &FakeDestroyMenu;
	__s2sdk_GetClientMenu = nullptr;
	expectNoMenu("GetClientMenu");
	__s2sdk_GetClientMenu = &FakeGetClientMenu;
	__s2sdk_CancelClientMenu = nullptr;
	expectNoMenu("CancelClientMenu");
	__s2sdk_CancelClientMenu = &FakeCancelClientMenu;
	__s2sdk_SetMenuType = nullptr;
	expectNoMenu("SetMenuType");
	__s2sdk_SetMenuType = &FakeSetMenuType;
	__s2sdk_GetMenuType = nullptr;
	expectNoMenu("GetMenuType");
	__s2sdk_GetMenuType = &FakeGetMenuType;

	FinishTest();
}

void TestMenuFollowsPlayerChoice() {
	PrepareTest();
	SetClient(3, true, 76561198000000003, "Admin");
	StartPlugin();
	ReadyMap();
	g_clientMenuTypes[3] = "chat"; // the player's own "!menu" choice through hudmenu
	OpenMapMenu(3);
	Check(g_menus.size() == 2 && g_menus[1].type == "chat", "the player's '!menu' choice is used");
	FinishTest();

	PrepareTest();
	SetClient(3, true, 76561198000000003, "Admin");
	StartPlugin();
	ReadyMap();
	g_clientMenuTypes[3] = "wasd";
	OpenMapMenu(3);
	Check(g_menus.size() == 2 && g_menus[1].type == "wasd", "a registered 'wasd' choice is used");
	FinishTest();

	PrepareTest();
	SetClient(3, true, 76561198000000003, "Admin");
	StartPlugin();
	ReadyMap();
	__hudmenu_GetClientMenuType = nullptr; // hudmenu not installed: pisexcompat's default mapping
	OpenMapMenu(3);
	Check(g_menus.size() == 2 && g_menus[1].type == "hud", "without hudmenu the HUD default is used");
	FinishTest();

	PrepareTest();
	SetClient(3, true, 76561198000000003, "Admin");
	StartPlugin();
	ReadyMap();
	g_clientMenuTypes[3] = "wasd";
	g_menuTypes = {"hud", "button"}; // hudmenu returned a stale wasd while the backend is gone
	OpenMapMenu(3);
	Check(g_menus.size() == 2 && g_menus[1].type == "button",
		  "an explicit wasd choice with the backend gone becomes button, never hud");
	FinishTest();
}

void TestMenuTypeParityTable() {
	constexpr uint64_t kSteam = 76561198000000003;

	// pisexcompat ResolveS2sdkMenuTypeName() table, exercised end to end through the real
	// ResolveMenuType: (hudmenu answer, registered types, hudmenu installed) -> menu type.
	const auto resolved = [&](const char* choice, std::initializer_list<const char*> types, bool hudmenu) {
		PrepareTest();
		SetClient(3, true, kSteam, "Admin");
		g_menuTypes = std::set<std::string>(types.begin(), types.end());
		StartPlugin();
		ReadyMap();
		if (!hudmenu) {
			__hudmenu_GetClientMenuType = nullptr;
		} else if (choice != nullptr) {
			g_clientMenuTypes[3] = choice;
		}
		OpenMapMenu(3);
		const std::string type = g_menus.size() == 2 ? g_menus[1].type : std::string();
		FinishTest();
		return type;
	};

	// The five hudmenu answers when the backend exists.
	Check(resolved("hud", {"hud", "wasd", "button"}, true) == "hud", "'hud' with the backend -> hud");
	Check(resolved("wasd", {"hud", "wasd", "button"}, true) == "wasd", "'wasd' with the backend -> wasd");
	Check(resolved("chat", {"hud", "wasd", "button"}, true) == "chat", "'chat' -> chat");
	Check(resolved("centerhtml", {"hud", "wasd", "button"}, true) == "centerhtml",
		  "'centerhtml' -> centerhtml");
	Check(resolved("button", {"hud", "wasd", "button"}, true) == "button", "'button' -> button");

	// An explicit custom type with its backend gone becomes the builtin button, never the other
	// custom type.
	Check(resolved("wasd", {"hud", "button"}, true) == "button",
		  "'wasd' with the wasd backend gone and hud registered -> button");
	Check(resolved("hud", {"wasd", "button"}, true) == "button",
		  "'hud' with the hud backend gone and wasd registered -> button");
	Check(resolved("wasd", {"button"}, true) == "button", "'wasd' with only the builtin button -> button");

	// Unknown answer -> HUD_LAYOUT default: hud if registered, otherwise button (not wasd).
	Check(resolved("legacy", {"hud", "button"}, true) == "hud", "unknown answer -> HUD_LAYOUT hud");
	Check(resolved("legacy", {"wasd", "button"}, true) == "button",
		  "unknown answer with hud gone and wasd registered -> button");

	// hudmenu not installed: its answer is ignored, HUD_LAYOUT default applies.
	Check(resolved(nullptr, {"hud", "wasd", "button"}, false) == "hud",
		  "no hudmenu, hud registered -> hud");
	Check(resolved(nullptr, {"wasd", "button"}, false) == "button",
		  "no hudmenu, hud gone and wasd registered -> button, never wasd");
	Check(resolved("wasd", {"wasd", "button"}, false) == "button",
		  "no hudmenu, a stale wasd choice is ignored -> HUD_LAYOUT button");
}

void TestMenuPermissionRechecked() {
	PrepareTest();
	SetClient(3, true, 76561198000000003, "Admin");
	StartPlugin();
	ReadyMap();

	g_deniedSlots.insert(3);
	SelectAdminMapsItem(3);
	Update();
	Check(g_createMenuCalls == 0, "no map menu without @admin/maps");
	Check(!g_permissionChecks.empty() && g_permissionChecks.back().second == "@admin/maps",
		  "the permission of the original item is checked");

	g_deniedSlots.clear();
	const s2sdk::MenuId menu = OpenMapMenu(3);
	Check(menu != 0, "the menu opens for a permitted admin");

	g_deniedSlots.insert(3);
	InvokeMenu(menu, s2sdk::MenuAction::Select, 3, 1);
	Update();
	Check(g_actions.empty() && g_timers.empty() && g_chatPrints.empty(),
		  "a selection is refused when the permission was revoked while the menu was open");

	g_deniedSlots.clear();
	const s2sdk::MenuId second = OpenMapMenu(3);
	g_deniedSlots.insert(3);
	InvokeMenu(second, s2sdk::MenuAction::Cancel, 3,
			   static_cast<int32_t>(s2sdk::MenuCancelReason::ExitBack));
	Update();
	Check(g_lastCategoryCalls.empty(), "the back action is refused after the permission was revoked");

	FinishTest();
}

void TestMenuAdminDataReadiness() {
	PrepareTest();
	SetClient(3, true, 76561198000000003, "Admin");
	StartPlugin();
	ReadyMap();
	const s2sdk::MenuId menu = OpenMapMenu(3);

	// The admin SELECT for this steam id is still pending: HasPermission may answer with the
	// previous occupant's rights, so the selection is refused until readiness is reported.
	g_notReadyPairs.insert(std::make_pair(3, static_cast<uint64_t>(76561198000000003)));
	InvokeMenu(menu, s2sdk::MenuAction::Select, 3, 1);
	Update();
	Check(g_actions.empty() && g_timers.empty() && g_chatPrints.empty(),
		  "a pending admin SELECT denies the map change");

	g_notReadyPairs.clear();
	const s2sdk::MenuId second = OpenMapMenu(3);
	__admin_system_IsClientAdminDataReady = nullptr; // an older core without API 1.1.0
	InvokeMenu(second, s2sdk::MenuAction::Select, 3, 1);
	Update();
	Check(g_actions.empty() && g_timers.empty(), "a missing readiness binding denies the map change");
	Check(LoggedError("IsClientAdminDataReady"), "the missing/refusing readiness is logged");

	// The console bypass needs no admin record.
	DispatchMmMap(-1, s2sdk::ConCommandContext::Console, {"mm_map", "de_dust2"});
	Update();
	Check(g_timers.size() == 1, "the server console still changes the map without admin readiness");

	FinishTest();
}

void TestMenuAdminDataReadinessFailure() {
	PrepareTest();
	SetClient(3, true, 76561198000000003, "Admin");
	StartPlugin();
	ReadyMap();
	g_readyThrows = true;
	SelectAdminMapsItem(3);
	Update();
	Check(g_createMenuCalls == 0, "a throwing readiness check denies the menu");
	Check(LoggedError("IsClientAdminDataReady failed"), "the readiness failure is logged");
	FinishTest();
}

void TestMenuControllerGeneration() {
	PrepareTest();
	SetClient(3, true, 76561198000000003, "Admin");
	StartPlugin();
	ReadyMap();
	const s2sdk::MenuId menu = OpenMapMenu(3);

	// Same SteamID64, but the controller entity was recreated: its CEntityHandle carries a new
	// serial, so the old menu must not act for the new controller.
	g_clients[3].generation = 1;
	InvokeMenu(menu, s2sdk::MenuAction::Select, 3, 1);
	Update();
	Check(g_actions.empty() && g_timers.empty() && g_chatPrints.empty(),
		  "a recycled controller never acts for the previous menu");

	const s2sdk::MenuId second = OpenMapMenu(3);
	InvokeMenu(second, s2sdk::MenuAction::Select, 3, 1);
	Update();
	Check(g_actions.size() == 1 && g_timers.size() == 1,
		  "the new controller's own menu works");

	FinishTest();
}

void TestRegistrationPairing() {
	PrepareTest();
	StartPlugin();
	Check(g_mapStartRegisters == 1 && g_mapEndRegisters == 1, "one map listener pair registered");
	Check(g_coreLoadedRegisters == 1, "one OnCoreLoaded registration");
	Check(g_mapStartUnregisters == 0 && g_mapEndUnregisters == 0 && g_coreLoadedUnregisters == 0,
		  "nothing unregistered while loaded");

	FinishTest();
	Check(g_mapStartUnregisters == 1 && g_mapEndUnregisters == 1, "the same map listener pair removed");
	Check(g_coreLoadedUnregisters == 1, "the OnCoreLoaded callback unregistered");
	Check(g_removedCommands.size() == 1, "the owned command removed");

	// OnPluginEnd is idempotent: a second call must not unregister/remove anything twice.
	(void)g_asMapsPlugin.OnPluginEnd();
	Check(g_mapStartUnregisters == 1 && g_mapEndUnregisters == 1 && g_coreLoadedUnregisters == 1,
		  "a repeated OnPluginEnd does not unregister twice");
	Check(g_removedCommands.size() == 1, "a repeated OnPluginEnd does not remove the command twice");

	FinishTest();
}

void TestTimerPairing() {
	// A pending timer is killed on unload ...
	PrepareTest();
	StartPlugin();
	ReadyMap();
	DispatchMmMap(-1, s2sdk::ConCommandContext::Console, {"mm_map", "de_dust2"});
	Update();
	Check(g_timers.size() == 1, "the change timer exists");
	FinishTest();
	Check(g_killedTimers.size() == 1 && g_killedTimers.front() == g_timers.front().id,
		  "the pending timer is killed by OnPluginEnd");

	// ... a timer that already fired is not killed again (s2sdk erased it).
	PrepareTest();
	StartPlugin();
	ReadyMap();
	DispatchMmMap(-1, s2sdk::ConCommandContext::Console, {"mm_map", "de_dust2"});
	Update();
	FireTimer(g_timers.front().id);
	Update();
	Check(g_serverCommands.size() == 1, "the fired timer ran its changelevel");
	FinishTest();
	Check(g_killedTimers.empty(), "an already-fired one-shot timer is not killed again");
}

void TestReloadReRegistersSafely() {
	PrepareTest();
	SetClient(3, true, 76561198000000003, "Admin");
	StartPlugin();
	Check(g_registeredItems.size() == 1 && g_registeredItems.back().select != nullptr,
		  "the first load registers the live item");
	Check(g_registeredCategoryCallbacks.size() == 1 && g_registeredCategoryCallbacks.front() == nullptr,
		  "the first load registers the category with null callbacks");

	(void)g_asMapsPlugin.OnPluginEnd();
	Check(g_registeredItems.size() == 2 && g_registeredItems.back().select == nullptr &&
			  g_registeredItems.back().display == nullptr,
		  "unload retires the item callbacks");
	Check(g_registeredCategoryCallbacks.size() == 1,
		  "unload does not need to touch the already-null category");

	StartPlugin(); // plg reload
	Check(g_commands.size() == 2 && g_mapStartRegisters == 2 && g_coreLoadedRegisters == 2,
		  "a reload registers the command, the map listeners and the core callback again");
	Check(g_registeredCategories.size() == 2 && g_registeredCategoryCallbacks.back() == nullptr,
		  "a reload registers the category again with a null callback");
	Check(g_registeredItems.size() == 3 && g_registeredItems.back().select != nullptr &&
			  g_registeredItems.back().display != nullptr,
		  "a reload replaces the retired item with the live callbacks");

	(void)g_asMapsPlugin.OnPluginEnd();
	Check(g_registeredItems.size() == 4 && g_registeredItems.back().select == nullptr &&
			  g_registeredItems.back().display == nullptr,
		  "the second unload retires the reloaded item");
	Check(g_coreLoadedUnregisters == 2 && g_mapStartUnregisters == 2 && g_mapEndUnregisters == 2,
		  "the second unload unregisters its own pairs");

	FinishTest();
}

void TestRawMapsFileWinsAndIsNotHidden() {
	// The raw Metamod spelling is relative to the server working directory. Create it in a
	// disposable temp working directory so the test works even when build/tests is mounted read-only,
	// exercising the ResolvePath() rule that a malformed raw file is authoritative and cannot be
	// hidden by the configs copy.
	namespace fs = std::filesystem;
	std::error_code ignored;
	const fs::path savedCwd = fs::current_path(ignored);
	const fs::path tempRoot = fs::temp_directory_path(ignored) /
							  ("as_maps_raw_cwd_" + std::to_string(static_cast<unsigned long>(::getpid())));
	fs::remove_all(tempRoot, ignored);
	Check(fs::create_directories(tempRoot, ignored), "temp working directory can be created");
	fs::current_path(tempRoot, ignored);

	const fs::path rawDir = fs::path("addons") / "configs" / "admin_system";
	const fs::path rawFile = rawDir / "maps.ini";
	Check(fs::create_directories(rawDir), "the raw cwd maps directory can be created");

	{
		std::ofstream file(rawFile, std::ios::binary | std::ios::trunc);
		file << "\"Maps\"\n{\n    \"Raw map\" \"de_raw\"\n}\n";
	}
	PrepareTest();
	SetClient(3, true, 76561198000000003, "Admin");
	StartPlugin();
	ReadyMap();
	SelectAdminMapsItem(3);
	Update();
	Check(g_menus.size() == 2 && g_menus[1].items.size() == 1,
		  "the raw cwd maps.ini wins over the configs candidate");
	Check(!g_menus.empty() && g_menus[1].items.front().first == "de_raw", "the raw file's map is used");
	FinishTest();

	{
		std::ofstream file(rawFile, std::ios::binary | std::ios::trunc);
		file << "\"Maps\"\n{\n    \"Raw map\" \"de_raw\"\n"; // missing '}'
	}
	PrepareTest();
	SetClient(3, true, 76561198000000003, "Admin");
	StartPlugin();
	Check(LoggedError("Failed to load"), "a malformed raw maps.ini fails the load");
	ReadyMap();
	SelectAdminMapsItem(3);
	Update();
	Check(g_menus.size() == 2 && g_menus[1].items.empty(),
		  "the malformed raw file is not hidden by the valid configs candidate");

	FinishTest();
	if (!savedCwd.empty()) {
		fs::current_path(savedCwd, ignored);
	}
	fs::remove_all(tempRoot, ignored);
}

void TestMalformedConfigIsNotHidden() {
	PrepareTest();
	g_configsDir = std::string(AS_MAPS_TEST_DATA_DIR) + "/malformed";
	SetClient(3, true, 76561198000000003, "Admin");
	StartPlugin();
	Check(LoggedError("Failed to load"), "the malformed first-existing maps.ini fails the load");
	ReadyMap();
	SelectAdminMapsItem(3);
	Update();
	Check(g_createMenuCalls == 2, "the menu still opens");
	Check(g_menus.size() == 2 && g_menus[1].items.empty(),
		  "the translations candidate below the malformed file is not used (no de_nuke)");

	FinishTest();
}

} // namespace

int main() {
	TestStartRegistersEverythingWithoutClientQueries();
	TestCoreLoadedLaterAndIdempotent();
	TestMapGateMidMapLatch();
	TestMapGateChangeWindowDoesNotLatch();
	TestCommandServerConsoleUsage();
	TestCommandChangeLevel();
	TestCommandWorkshopMap();
	TestUnsafeMapNamesRejected();
	TestCommandRefusesClientCaller();
	TestSecondRequestReplacesPendingChange();
	TestScheduledChangeRevalidatesAuthor();
	TestMapEndDropsPendingChange();
	TestMessageTypes();
	TestUnknownMapFallsBackToFileName();
	TestMenuDisplay();
	TestMenuTypeFallbacks();
	TestMenuCreationRequiresCleanupBindings();
	TestMenuFollowsPlayerChoice();
	TestMenuTypeParityTable();
	TestMenuSelectAndIdentity();
	TestMenuSlotReuse();
	TestMenuBackAndTimeout();
	TestMenuPermissionRechecked();
	TestMenuAdminDataReadiness();
	TestMenuAdminDataReadinessFailure();
	TestMenuControllerGeneration();
	TestMenuNotShownForInvalidSlots();
	TestMapEndCancelsAndDestroysMenus();
	TestUnloadWithMapDownCancelsAndDestroysQueuedMenus();
	TestMapEndCancelFalseSwitchesToBuiltin();
	TestMapEndCancelFalseButDisplayGoneDoesNotSwitch();
	TestMapEndUnresolvedSwitchIsReported();
	TestUnloadMidMapClosesBackendOnce();
	TestTeardown();
	TestRegistrationPairing();
	TestTimerPairing();
	TestReloadReRegistersSafely();
	TestMalformedConfigIsNotHidden();
	TestRawMapsFileWinsAndIsNotHidden();
	TestTeardownOfPreExistingCommand();
	TestFailSoftWithoutAdminBindings();
	TestFailSoftWithoutS2sdk();
	TestEmptyMapsConfig();

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
