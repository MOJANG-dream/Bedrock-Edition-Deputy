#include "mod/DebugLog.h"

#include "mod/Config.h"

#include "ll/api/io/Logger.h"
#include "ll/api/mod/NativeMod.h"

#include <fstream>
#include <string>

namespace bedrock_edition_deputy {

bool debugLogEnabled() { return modConfig().enableDebugLog; }

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
        out << "== bedrock-edition-deputy debug ==\n";
    }
}

bool reportHook(std::string_view name, bool installed) {
    if (!installed) {
        if (auto mod = ll::mod::NativeMod::current()) {
            // 钩子找不到目标函数通常意味着游戏换了实现：这时模组不会崩，
            // 但对应功能会静默失效，所以必须以错误级别留在日志里。
            mod->getLogger().error("Cannot hook {}: the target function was not found", name);
        }
    }
    // appendDebugLog 自己会检查开关，这里不必重复判断。
    appendDebugLog(std::string("[hook] ") + std::string(name) + (installed ? " ok" : " FAILED"));
    return installed;
}

} // namespace bedrock_edition_deputy
