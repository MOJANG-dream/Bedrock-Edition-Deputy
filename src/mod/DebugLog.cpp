#include "mod/DebugLog.h"

#include "mod/Config.h"

#include "ll/api/io/Logger.h"
#include "ll/api/mod/NativeMod.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace bedrock_edition_deputy {

bool debugLogEnabled() {
    // 【临时诊断构建】本版无条件写日志：启动器会在每次启动时把模组目录里的
    // config.json 重置成默认值并删掉 debug.on，开关一律失效。定位完问题后恢复。
    return true;
}

void appendDebugLog(std::string_view line) {
    if (!debugLogEnabled()) {
        return;
    }
    auto mod = ll::mod::NativeMod::current();
    if (!mod) {
        return;
    }
    std::ofstream out(mod->getModDir() / "offhand-debug.log", std::ios::app);
    if (out) {
        out << line << '\n';
    }
}

void resetDebugLog() {
    if (!debugLogEnabled()) {
        return;
    }
    auto mod = ll::mod::NativeMod::current();
    if (!mod) {
        return;
    }
    std::ofstream out(mod->getModDir() / "offhand-debug.log", std::ios::trunc);
    if (out) {
        auto const& cfg = modConfig();
        out << "== bedrock-edition-deputy debug ==\n";
        out << "[cfg] dir=" << mod->getModDir().string() << '\n';
        out << "[cfg] enableSwapKey=" << cfg.enableSwapKey << " swapKey=" << cfg.swapKey
            << " menuKey=" << cfg.menuKey << " shieldRightClick=" << cfg.enableShieldRightClick
            << " invOffhand=" << cfg.enableInventoryOffhand << " offhandSlot=" << cfg.enableOffhandSlot
            << " debug=" << cfg.enableDebugLog << '\n';
    }
}

bool reportHook(std::string_view name, int hookResult) {
    // hookEx 的约定是 0 表示成功，非 0 是错误码；直接当 bool 会把成功记成失败。
    bool const installed = hookResult == 0;
    if (!installed) {
        if (auto mod = ll::mod::NativeMod::current()) {
            // 钩子找不到目标函数通常意味着游戏换了实现：这时模组不会崩，
            // 但对应功能会静默失效，所以必须以错误级别留在日志里。
            mod->getLogger().error(
                "Cannot hook {}: the target function was not found (code {})",
                name,
                hookResult
            );
        }
    }
    // appendDebugLog 自己会检查开关，这里不必重复判断。
    appendDebugLog(std::string("[hook] ") + std::string(name) + (installed ? " ok" : " FAILED"));
    return installed;
}

} // namespace bedrock_edition_deputy
