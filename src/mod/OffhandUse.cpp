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

// 副手物品的「使用中」维持逻辑：原版 Player::normalTick 里的物品使用块只认
// 背包选中槽，这里补一份针对副手的等价处理（含进食粒子与进食结算）。

namespace bedrock_edition_deputy::offhand_use {

namespace {

using SharedTypes::Legacy::UseAnimation;

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
        || !offhandItem.sameItem(itemInUse.mItem.get(), ItemStackBase::COMPARISONOPTIONS_RELEVANTUSERDATA())) {
        player.stopUsingItem();
        return;
    }

    if (offhandItem.mCount != itemInUse.mItem.get().mCount || !offhandItem.matchesItem(itemInUse.mItem.get())) {
        itemInUse.mItem.get() = offhandItem;
    }

    bool const clientSide = player.isClientSide();
    auto       component  = player.getEntityContext().tryGetComponent<ItemInUseComponent>();
    Item const* item      = offhandItem.mItem.get();

    if (clientSide && component && component->mDuration < kFeedParticleDuration
        && component->mDuration % kFeedParticleInterval == 0 && hasFeedingAnimation(item->mUseAnim)) {
        player.feed(offhandItem.getIdAux());
    }

    if (component && component->mDuration != 0) {
        return;
    }

    if (clientSide && item->isFood()) {
        player.eat(offhandItem);
    }

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

    // Player::normalTick 会停掉「物品不在选中槽」的使用。把数量清零只骗过这一处检查：
    // isUsingItem / getItemInUse 走 isNull，不看数量。
    ItemStack&  itemInUse = mItemInUse.get().mItem.get();
    uchar const count     = itemInUse.mCount;
    itemInUse.mCount      = 0;

    origin();

    if (!offhands::isUsingOffhandItem(*this)) {
        return;
    }

    itemInUse.mCount = count;
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
