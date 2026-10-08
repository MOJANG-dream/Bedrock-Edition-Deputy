#include "mod/OffhandUse.h"

#include "mod/Offhands.h"

#include "ll/api/memory/Hook.h"

#include "mc/deps/shared_types/legacy/item/UseAnimation.h"
#include "mc/entity/components/ItemInUseComponent.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/actor/player/PlayerItemInUse.h"
#include "mc/world/gamemode/GameMode.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/ItemStackBase.h"

#include <fstream>
#include <string>

#include "ll/api/mod/NativeMod.h"

// 副手物品的「使用中」维持逻辑：原版 Player::normalTick 里的物品使用块只认
// 背包选中槽，这里补一份针对副手的等价处理（含进食粒子与进食结算）。

namespace bedrock_edition_deputy::offhand_use {

namespace {

using SharedTypes::Legacy::UseAnimation;

void debugLog(std::string const& line) {
    auto mod = ll::mod::NativeMod::current();
    if (!mod) {
        return;
    }
    std::ofstream out(mod->getModDir() / "offhand-debug.log", std::ios::app);
    if (out) {
        out << line << '\n';
    }
}

// Player::normalTick 发送进食粒子的剩余时长窗口与间隔。
constexpr int kFeedParticleDuration = 26;
constexpr int kFeedParticleInterval = 4;

bool hasFeedingAnimation(UseAnimation animation) {
    return animation == UseAnimation::Eat || animation == UseAnimation::Drink
        || animation == UseAnimation::GlowStick || animation == UseAnimation::Sparkler;
}

// Player::normalTick 中物品使用块的副手版本。
void tickOffhandItemInUse(Player& player) {
    PlayerItemInUse& itemInUse   = player.mItemInUse.get();
    ItemStack const& offhandItem = offhands::getItem(player);

    if (!offhands::hasItem(offhandItem)
        || !offhandItem.sameItem(
            itemInUse.mItem.get(),
            ItemStackBase::COMPARISONOPTIONS_RELEVANTUSERDATA()
        )) {
        debugLog("[use] tick stop: offhand mismatch");
        player.stopUsingItem();
        return;
    }

    if (offhandItem.mCount != itemInUse.mItem.get().mCount || !offhandItem.matchesItem(itemInUse.mItem.get())) {
        itemInUse.mItem.get() = offhandItem;
    }

    bool const clientSide = player.isClientSide();
    auto       component  = player.getEntityContext().tryGetComponent<ItemInUseComponent>();
    Item const* item      = offhandItem.mItem.get();

    // 组件在 OffhandItemTickHook 的 HandSwapScope 内已重建，这里应存在。
    // 若仍缺失（极端情况），直接返回，等下一帧重建。
    if (!component) {
        return;
    }

    if (clientSide && component->mDuration < kFeedParticleDuration
        && component->mDuration % kFeedParticleInterval == 0 && hasFeedingAnimation(item->mUseAnim)) {
        player.feed(offhandItem.getIdAux());
    }

    if (!component || component->mDuration != 0) {
        return;
    }

    if (item->mMaxUseDuration > 0) {
        return;
    }

    if (clientSide && item->isFood()) {
        player.eat(offhandItem);
    }

    debugLog("[use] tick complete: duration hit 0");
    offhands::HandSwapScope scope(player);
    player.completeUsingItem();
}

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

    {
        offhands::HandSwapScope scope(*this);

        // 在 origin 之前重建被清掉的组件：必须在 HandSwapScope 内调用，
        // 这样 startUsingItem 会把 mItemInUse.mSlot 设到选中槽，而选中槽此刻
        // 物理持有副手物品——物品、槽位、组件三者一致，origin 内部的任何检查都能通过。
        auto component = getEntityContext().tryGetComponent<ItemInUseComponent>();
        if (!component) {
            ItemStack const& offhandItem = offhands::getItem(*this);
            Item const*      item        = offhandItem.mItem.get();
            int              duration    = item ? item->mMaxUseDuration : 0;
            if (duration <= 0) {
                duration = 72000;
            }
            debugLog(std::string("[use] re-add component dur=") + std::to_string(duration));
            startUsingItem(const_cast<ItemStack&>(offhandItem), duration);
        }

        origin();
    }

    if (!offhands::isUsingOffhandItem(*this)) {
        return;
    }

    tickOffhandItemInUse(*this);
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
