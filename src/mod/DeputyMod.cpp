#include "mod/DeputyMod.h"

#include "mod/Config.h"
#include "mod/InventoryActions.h"
#include "mod/SettingsScreen.h"
#include "mod/ShieldBlock.h"

#include "ll/api/Config.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/input/KeyInputEvent.h"
#include "ll/api/event/input/MouseInputEvent.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/service/TargetedBedrock.h"
#include "ll/api/thread/ClientThreadExecutor.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/deps/input/Keyboard.h"
#include "mc/deps/input/MouseAction.h"

#include <atomic>
#include <functional>
#include <vector>

namespace bedrock_edition_deputy {

namespace {

Config                       config;
std::atomic<bool>            altHeld{false};
std::vector<ll::event::ListenerPtr> listeners;

void runOnClientThread(std::function<void()> task) {
    ll::thread::ClientThreadExecutor::getDefault().execute(std::move(task));
}

} // namespace

Config& modConfig() { return config; }

// ---------------------------------------------------------------------------
// 输入处理（回调来自窗口输入线程，只做状态记录与线程切换）
// ---------------------------------------------------------------------------

static void onKey(ll::event::input::KeyInputEvent& event) {
    int  key  = event.keyCode();
    bool down = event.isDown();

    if (key == Keyboard::Menu) {
        altHeld.store(down, std::memory_order_relaxed);
        return;
    }

    if (key != Keyboard::F) {
        return;
    }

    if (!down) {
        return; // 抬起放行，防止卡键
    }

    // Alt+F：打开配置界面；已打开时关闭
    if (altHeld.load(std::memory_order_relaxed)) {
        if (config.enableMenuKey) {
            event.cancel();
            runOnClientThread([] {
                if (settings_screen::isOpen()) {
                    settings_screen::close();
                } else {
                    settings_screen::open();
                }
            });
        }
        return;
    }

    // 设置界面打开时，界面自己处理 F（不交换）。
    if (settings_screen::isOpen()) {
        return;
    }

    // 背包/容器界面悬停在玩家物品上：送入副手
    if (config.enableInventoryOffhand && inventory_actions::hasHoveredPlayerSlot()) {
        event.cancel();
        runOnClientThread([] { inventory_actions::swapHoveredToOffhand(); });
        return;
    }

    // HUD 下：交换主副手
    if (config.enableSwapKey) {
        auto clientInstance = ll::service::bedrock::getClientInstance();
        if (clientInstance && clientInstance->isInGameInputEnabled()) {
            event.cancel();
            runOnClientThread([] { inventory_actions::swapHotbarOffhand(); });
        }
    }
}

static void onMouse(ll::event::input::MouseInputEvent& event) {
    if (event.actionButtonId() == MouseAction::ActionRight) {
        shield_block::setRightHeld(event.buttonData() == MouseAction::DataDown);
    }
}

// ---------------------------------------------------------------------------
// 模组生命周期
// ---------------------------------------------------------------------------

DeputyMod& DeputyMod::getInstance() {
    static DeputyMod instance;
    return instance;
}

bool DeputyMod::load() {
    auto& logger = getSelf().getLogger();
    logger.debug("Loading...");

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

    inventory_actions::install();
    shield_block::install();
    settings_screen::setSaveCallback([this] {
        const auto& configFilePath = getSelf().getConfigDir() / "config.json";
        if (!ll::config::saveConfig(config, configFilePath)) {
            getSelf().getLogger().error("Cannot save configurations to {}", configFilePath);
        }
    });
    settings_screen::install();

    auto& bus = ll::event::EventBus::getInstance();
    listeners.emplace_back(bus.emplaceListener<ll::event::input::KeyInputEvent>(onKey));
    listeners.emplace_back(bus.emplaceListener<ll::event::input::MouseInputEvent>(onMouse));

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
    shield_block::uninstall();
    inventory_actions::uninstall();
    altHeld.store(false, std::memory_order_relaxed);

    return true;
}

} // namespace bedrock_edition_deputy

LL_REGISTER_MOD(bedrock_edition_deputy::DeputyMod, bedrock_edition_deputy::DeputyMod::getInstance());
