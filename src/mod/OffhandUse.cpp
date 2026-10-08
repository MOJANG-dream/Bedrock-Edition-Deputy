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

    if (clientSide && component && component->mDuration < kFeedParticleDuration
        && component->mDuration % kFeedParticleInterval == 0 && hasFeedingAnimation(item->mUseAnim)) {
        player.feed(offhandItem.getIdAux());
    }

    if (!component || component->mDuration != 0) {
        // 组件不存在 = 当前并没有正在进行的物品使用（不使用时该组件就不存在），
        // 此时绝不能调用 completeUsingItem()，否则会把刚开始的副手使用立刻结束掉
        // （表现为：盾牌刚举起就落下、钓鱼竿鱼线立刻收回、长矛蓄力留不住）。
        return;
    }

    // mDuration==0 但物品有使用时长（食物/弓/盾牌等）时，组件可能刚创建尚未初始化。
    // 贸然 completeUsingItem 会导致使用状态立刻死亡并无限循环。
    // 仅对 mMaxUseDuration==0 的即时型物品（钓鱼竿等）才立即结算。
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

    // 本版本（1.26.51）原版 tick 里的「使用中物品」一致性检查会核对槽位/ECS 组件，
    // 参考模板的「数量清零」只骗得过按物品比对的旧实现：副手使用时选中槽是主手物品，
    // 检查不通过就会每刻 stopUsingItem，ECS 组件与 USINGITEM 标志也被清掉
    // （日志现象：副手鱼竿/食物等刚 startUsingItem 就 ecsDur=-1、uf=0）。
    // 因此在 normalTick 期间直接建立完整 HandSwapScope：ctor 会把 mItemInUse.mSlot
    // 重映射到背包选中槽，且选中槽此时物理持有副手物品——物品、槽位、ECS 三个维度
    // 在 origin 执行期间全部一致，任何口径的检查都能通过；时长归零的结算（吃完食物等）
    // 也自然作用于副手。dtor 负责换回并重映射回副手槽，外界无感。
    {
        offhands::HandSwapScope scope(*this);
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
