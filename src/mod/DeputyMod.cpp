#include "mod/DeputyMod.h"

#include "mod/Config.h"
#include "mod/OffhandPlayerModel.h"
#include "mod/OffhandResourcePack.h"
#include "mod/OffhandSync.h"
#include "mod/OffhandUse.h"
#include "mod/Offhands.h"
#include "mod/ShieldBlock.h"

#include "ll/api/Config.h"
#include "ll/api/io/Logger.h"
#include "ll/api/mod/RegisterHelper.h"

#include <fstream>
#include <string>

// 以下为仅客户端的部分：输入、渲染、设置界面与玩家模型动画。
#ifdef LL_PLAT_C

#include "mod/InventoryActions.h"
#include "mod/OffhandInput.h"
#include "mod/OffhandRender.h"
#include "mod/SettingsScreen.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/input/KeyInputEvent.h"
#include "ll/api/service/TargetedBedrock.h"
#include "ll/api/thread/ClientThreadExecutor.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/deps/input/Keyboard.h"

#include <atomic>
#include <functional>
#include <vector>

// 不包含 <windows.h>：它会引入 ERROR/interface/small/min/max 等大量宏，
// 与 LeviLamina / MC 头文件冲突。这里仅前向声明所需的 user32 函数。
// 必须用 __declspec(dllimport)，否则符号名与 user32.lib 的导出不匹配（LNK2019）。
extern "C" __declspec(dllimport) short __stdcall GetAsyncKeyState(int vKey);
#pragma comment(lib, "user32.lib")
#ifndef VK_MENU
#define VK_MENU 0x12
#endif

#endif // LL_PLAT_C

namespace bedrock_edition_deputy {

namespace {

Config config;

#ifdef LL_PLAT_C

std::atomic<bool>            altHeld{false};
std::atomic<bool>            swapKeyHeld{false}; // 边沿触发：忽略长按的键盘重复事件
std::atomic<bool>            menuKeyHeld{false};
std::vector<ll::event::ListenerPtr> listeners;

// 临时诊断：菜单链路追踪，追加到 <模组目录>/offhand-debug.log。
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

// Alt 是系统修饰键，游戏/事件层可能不投递它自身的按下事件，
// 导致 altHeld 永远为 false（Alt+F 打不开菜单）。这里直接读 Win32 异步键态兜底。
bool altDownNow() {
    return altHeld.load(std::memory_order_relaxed) || (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
}

void runOnClientThread(std::function<void()> task) {
    ll::thread::ClientThreadExecutor::getDefault().execute(std::move(task));
}

#endif // LL_PLAT_C

} // namespace

Config& modConfig() { return config; }

ll::io::Logger& modLogger() { return DeputyMod::getInstance().getSelf().getLogger(); }

#ifdef LL_PLAT_C

// ---------------------------------------------------------------------------
// 输入处理（回调来自窗口输入线程，只做状态记录与线程切换）
// ---------------------------------------------------------------------------

static void onKey(ll::event::input::KeyInputEvent& event) {
    int  key  = event.keyCode();
    bool down = event.isDown();

    // 诊断：记录所有 F 键与 Alt 键事件，确认输入通路与键码。
    if (key == Keyboard::Menu || key == 0x46 /* F */) {
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

    // 设置界面正在捕获键位：一切按键交给界面处理。
    if (settings_screen::isCapturingKey()) {
        return;
    }

    bool const isMenuKey = key == config.menuKey;
    bool const isSwapKey = key == config.swapKey;

    if (!down) {
        // 抬起：复位边沿状态并放行，防止卡键。
        if (isMenuKey) {
            bool const wasHeld = menuKeyHeld.exchange(false, std::memory_order_relaxed);
            // Alt+F 组合下 F 的 keydown 会被系统当加速键吞掉，只能看到 keyup。
            // keydown 没处理过（wasHeld=false）且 Alt 仍按住时，用 keyup 兜底触发菜单。
            if (!wasHeld && altDownNow()) {
                event.cancel();
                menuDebugLog(
                    std::string("[menu] Alt+") + std::to_string(key) + " (keyup fallback) -> toggle settings screen"
                );
                runOnClientThread([] {
                    if (settings_screen::isOpen()) {
                        settings_screen::close();
                    } else {
                        settings_screen::open();
                    }
                });
            }
        }
        if (isSwapKey) {
            swapKeyHeld.store(false, std::memory_order_relaxed);
        }
        return;
    }

    // Alt+菜单键：打开/关闭配置界面（快捷键固定，不可改绑）
    if (isMenuKey && altDownNow()) {
        if (menuKeyHeld.exchange(true, std::memory_order_relaxed)) {
            return; // 长按重复
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

    // 设置界面打开时，界面自己处理按键（不交换）。
    if (settings_screen::isOpen()) {
        return;
    }

    if (!isSwapKey || !config.enableSwapKey) {
        return;
    }
    if (swapKeyHeld.exchange(true, std::memory_order_relaxed)) {
        return; // 长按重复
    }

    // 背包/容器界面悬停在玩家物品上：送入副手
    if (config.enableInventoryOffhand && inventory_actions::hasHoveredPlayerSlot()) {
        event.cancel();
        runOnClientThread([] { inventory_actions::swapHoveredToOffhand(); });
        return;
    }

    // HUD 下：交换主副手
    auto clientInstance = ll::service::bedrock::getClientInstance();
    if (clientInstance && clientInstance->isInGameInputEnabled()) {
        event.cancel();
        runOnClientThread([] { inventory_actions::swapHotbarOffhand(); });
    } else {
        modLogger().debug("swap key pressed with no in-game input and no hovered player slot");
    }
}

#endif // LL_PLAT_C

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

    // 每次开启游戏都清空诊断日志，保证日志只包含本次会话。
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

    // 顺序：先装物品注册/双手交换基础设施，再装依赖它的使用管线与同步，
    // 最后装与它们协作的格挡。以下为双端共享部分。
    offhands::install();
    offhand_sync::install();
    offhand_use::install();
    shield_block::install();

#ifdef LL_PLAT_C
    // 仅客户端：输入、渲染、库存操作、设置界面、副手模型动画与资源包注入。
    offhand_input::install();
    offhand_render::install();
    inventory_actions::install();
    offhand_resource_pack::install();
    offhand_player_model::install();
    settings_screen::setSaveCallback([this] {
        const auto& configFilePath = getSelf().getConfigDir() / "config.json";
        if (!ll::config::saveConfig(config, configFilePath)) {
            getSelf().getLogger().error("Cannot save configurations to {}", configFilePath);
        }
    });
    settings_screen::install();

    auto& bus = ll::event::EventBus::getInstance();
    listeners.emplace_back(bus.emplaceListener<ll::event::input::KeyInputEvent>(onKey));
#endif

    return true;
}

bool DeputyMod::disable() {
    auto& logger = getSelf().getLogger();
    logger.debug("Disabling...");

#ifdef LL_PLAT_C
    auto& bus = ll::event::EventBus::getInstance();
    for (auto& listener : listeners) {
        bus.removeListener(listener);
    }
    listeners.clear();

    settings_screen::uninstall();
    settings_screen::setSaveCallback(nullptr);
    offhand_player_model::uninstall();
    offhand_resource_pack::uninstall();
    inventory_actions::uninstall();
    offhand_render::uninstall();
    offhand_input::uninstall();
    altHeld.store(false, std::memory_order_relaxed);
    swapKeyHeld.store(false, std::memory_order_relaxed);
    menuKeyHeld.store(false, std::memory_order_relaxed);
#endif

    shield_block::uninstall();
    offhand_use::uninstall();
    offhand_sync::uninstall();
    offhands::uninstall();

    return true;
}

} // namespace bedrock_edition_deputy

LL_REGISTER_MOD(bedrock_edition_deputy::DeputyMod, bedrock_edition_deputy::DeputyMod::getInstance());
