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
    if (modConfig().enableDebugLog) {
        return true;
    }
    // 兜底开关：模组目录下放一个 debug.on 文件即强制开启。配置在版本迁移时可能
    // 被重置成默认值，这时光靠 config.json 里的开关无法打开诊断日志。
    static bool const marker = [] {
        auto mod = ll::mod::NativeMod::current();
        if (!mod) {
            return false;
        }
        std::error_code ec;
        return std::filesystem::exists(mod->getModDir() / "debug.on", ec);
    }();
    return marker;
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
        out << "== bedrock-edition-deputy debug ==\n";
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
#include "mod/DebugLog.h"#include "mod/DebugLog.h"

#include "mod/Config.h"

#include "ll/api/io/Logger.h"
#include "ll/api/mod/NativeMod.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace bedrock_edition_deputy {

bool debugLogEnabled() {
    if (modConfig().enableDebugLog) {
        return true;
    }
    // 兜底开关：模组目录下放一个 debug.on 文件即强制开启。配置在版本迁移时可能
    // 被重置成默认值，这时光靠 config.json 里的开关无法打开诊断日志。
    static bool const marker = [] {
        auto mod = ll::mod::NativeMod::current();
        if (!mod) {
            return false;
        }
        std::error_code ec;
        return std::filesystem::exists(mod->getModDir() / "debug.on", ec);
    }();
    return marker;
}

void appendDebugLog(std::string_view line) {
    if (!debugLogEnabled()) {
        return;
    }
    auto mod = ll::mod::NativeMod::current();
#include "mod/DebugLog.h"    if (!mod) {
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
