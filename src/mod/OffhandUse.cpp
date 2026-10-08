#include "mod/OffhandUse.h"

#include "mod/Offhands.h"

#include "ll/api/memory/Hook.h"

#include "mc/world/actor/player/Player.h"
#include "mc/world/gamemode/GameMode.h"

// 副手物品的「使用中」维持逻辑：原版 Player::normalTick 里的物品使用块只认
// 背包选中槽。用 HandSwapScope 把副手物品临时换到选中槽，让 normalTick 原生
// 地处理使用 tick（含进食粒子、使用结算），scope 析构时再恢复槽位映射。

namespace bedrock_edition_deputy::offhand_use {

namespace {

LL_TYPE_INSTANCE_HOOK(
    OffhandItemTickHook,
    HookPriority::Normal,
    Player,
    &Player::$normalTick,
    void
) {
    if (!offhands::isUsingOffhandItem(*this) || offhands::HandSwapScope::isActive(*this)) {
        origin();
        return;
    }

    // 把副手物品临时换到主手选中槽，使 normalTick 的一致性检查通过，
    // 使用 tick 原生地递减 ECS 时长、发送进食粒子、完成使用。
    // 比 count-zeroing 更可靠——不依赖 isNull 是否检查 count。
    offhands::HandSwapScope scope(*this);
    origin();
}

LL_TYPE_INSTANCE_HOOK(
    OffhandReleaseUseHook,
    HookPriority::Normal,
    GameMode,
    &GameMode::$releaseUsingItem,
    void
) {
    Player& player = mPlayer;
    if (offhands::HandSwapScope::isActive(player) || !offhands::isUsingOffhandItem(player)) {
        origin();
        return;
    }

    offhands::HandSwapScope scope(player);
    origin();
}

} // namespace

void install() {
    OffhandItemTickHook::hook();
    OffhandReleaseUseHook::hook();
}

void uninstall() {
    OffhandReleaseUseHook::unhook();
    OffhandItemTickHook::unhook();
}

} // namespace bedrock_edition_deputy::offhand_use
