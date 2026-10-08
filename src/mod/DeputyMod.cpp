#include "mod/DeputyMod.h"

#include "mod/Config.h"
#include "mod/InventoryActions.h"
#include "mod/OffhandInput.h"
#include "mod/OffhandRender.h"
#include "mod/OffhandSync.h"
#include "mod/OffhandUse.h"
#include "mod/Offhands.h"
#include "mod/SettingsScreen.h"
#include "mod/ShieldBlock.h"

#include "ll/api/Config.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/input/KeyInputEvent.h"
#include "ll/api/io/Logger.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/service/TargetedBedrock.h"
#include "ll/api/thread/ClientThreadExecutor.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/deps/input/Keyboard.h"

#include <atomic>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

// 不包含 <windows.h>：它会引入 ERROR/interface/small/min/max 等大量宏，
// 与 LeviLamina / MC 头文件冲突。这里仅前向声明所需的 user32 函数。
// 必须用 __declspec(dllimport)，否则符号名与 user32.lib 的导出不匹配（LNK2019）。
extern "C" __declspec(dllimport) short __stdcall GetAsyncKeyState(int vKey);
#pragma comment(lib, "user32.lib")
#ifndef VK_MENU
#define VK_MENU 0x12
#endif

namespace bedrock_edition_deputy {

namespace {

Config                       config;
std::atomic<bool>            altHeld{false};
std::atomic<bool>            swapKeyHeld{false};
std::atomic<bool>            menuKeyHeld{false};
std::vector<ll::event::ListenerPtr> listeners;

void menuDebugLog(std::string const& line) {
    auto mod = ll::mod::NativeMod::current();
    if (!mod) {
        return;
    }
    std::ofstream out(mod->getModDir() / "offhand-debug.log", std::ios::app);
    if (out) {
        out << line << '\n';
    }
}

bool altDownNow() {
    return altHeld.load(std::memory_order_relaxed) || (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
}

void runOnClientThread(std::function<void()> task) {
    ll::thread::ClientThreadExecutor::getDefault().execute(std::move(task));
}

} // namespace

Config& modConfig() { return config; }

ll::io::Logger& modLogger() { return DeputyMod::getInstance().getSelf().getLogger(); }

static void onKey(ll::event::input::KeyInputEvent& event) {
    int  key  = event.keyCode();
    bool down = event.isDown();

    if (key == Keyboard::Menu || key == 0x46) {
        menuDebugLog(
            std::string("[key] key=") + std::to_string(key) + " down=" + (down ? "1" : "0")
            + " altHeld=" + (altHeld.load(std::memory_order_relaxed) ? "1" : "0")
            + " altAsync=" + ((GetAsyncKeyState(VK_MENU) & 0x8000) ? "1" : "0")
        );
    }

    if (key == Keyboard::Menu) {
        altHeld.store(down, std::memory_order_relaxed);
        return;
    }

    if (settings_screen::isCapturingKey()) {
        return;
    }

    bool const isMenuKey = key == config.menuKey;
    bool const isSwapKey = key == config.swapKey;

    if (!down) {
        if (isMenuKey) {
            menuKeyHeld.store(false, std::memory_order_relaxed);
        }
        if (isSwapKey) {
            swapKeyHeld.store(false, std::memory_order_relaxed);
        }
        return;
    }

    if (isMenuKey && altDownNow()) {
        if (menuKeyHeld.exchange(true, std::memory_order_relaxed)) {
            return;
        }
        event.cancel();
        menuDebugLog(std::string("[menu] Alt+") + std::to_string(key) + " -> toggle settings screen");
        runOnClientThread([] {
            if (settings_screen::isOpen()) {
                settings_screen::close();
            } else {
                settings_screen::open();
            }
        });
        return;
    }

    if (settings_screen::isOpen()) {
        return;
    }

    if (!isSwapKey || !config.enableSwapKey) {
        return;
    }
    if (swapKeyHeld.exchange(true, std::memory_order_relaxed)) {
        return;
    }

    if (config.enableInventoryOffhand && inventory_actions::hasHoveredPlayerSlot()) {
        event.cancel();
        runOnClientThread([] { inventory_actions::swapHoveredToOffhand(); });
        return;
    }

    auto clientInstance = ll::service::bedrock::getClientInstance();
    if (clientInstance && clientInstance->isInGameInputEnabled()) {
        event.cancel();
        runOnClientThread([] { inventory_actions::swapHotbarOffhand(); });
    } else {
        modLogger().debug("swap key pressed with no in-game input and no hovered player slot");
    }
}

DeputyMod& DeputyMod::getInstance() {
    static DeputyMod instance;
    return instance;
}

bool DeputyMod::load() {
    auto& logger = getSelf().getLogger();
    logger.debug("Loading...");

    {
        std::ofstream out(getSelf().getModDir() / "offhand-debug.log", std::ios::trunc);
        if (out) {
            out << "== bedrock-edition-deputy offhand debug ==\n";
        }
    }

    const auto& configFilePath = getSelf().getConfigDir() / "config.json";
    if (!ll::config::loadConfig(config, configFilePath)) {
        logger.warn("Cannot load configurations from {}", configFilePath);
        if (!ll::config::saveConfig(config, configFilePath)) {
            logger.error("Cannot save default configurations to {}", configFilePath);
        }
    }

    return true;
}

bool DeputyMod::enable() {
    auto& logger = getSelf().getLogger();
    logger.debug("Enabling...");

    offhands::install();
    offhand_sync::install();
    offhand_use::install();
    offhand_input::install();
    offhand_render::install();
    shield_block::install();
    inventory_actions::install();
    settings_screen::setSaveCallback([this] {
        const auto& configFilePath = getSelf().getConfigDir() / "config.json";
        if (!ll::config::saveConfig(config, configFilePath)) {
            getSelf().getLogger().error("Cannot save configurations to {}", configFilePath);
        }
    });
    settings_screen::install();

    auto& bus = ll::event::EventBus::getInstance();
    listeners.emplace_back(bus.emplaceListener<ll::event::input::KeyInputEvent>(onKey));

    return true;
}

bool DeputyMod::disable() {
    auto& logger = getSelf().getLogger();
    logger.debug("Disabling...");

    auto& bus = ll::event::EventBus::getInstance();
    for (auto& listener : listeners) {
        bus.removeListener(listener);
    }
    listeners.clear();

    settings_screen::uninstall();
    settings_screen::setSaveCallback(nullptr);
    inventory_actions::uninstall();
    shield_block::uninstall();
    offhand_render::uninstall();
    offhand_input::uninstall();
    offhand_use::uninstall();
    offhand_sync::uninstall();
    offhands::uninstall();
    altHeld.store(false, std::memory_order_relaxed);
    swapKeyHeld.store(false, std::memory_order_relaxed);
    menuKeyHeld.store(false, std::memory_order_relaxed);

    return true;
}

} // namespace bedrock_edition_deputy

LL_REGISTER_MOD(bedrock_edition_deputy::DeputyMod, bedrock_edition_deputy::DeputyMod::getInstance());