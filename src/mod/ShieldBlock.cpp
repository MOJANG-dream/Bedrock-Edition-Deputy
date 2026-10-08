#include "mod/ShieldBlock.h"

#include "mod/Config.h"
#include "mod/Offhands.h"

#include "ll/api/memory/Hook.h"
#include "ll/api/mod/NativeMod.h"

#include "mc/deps/shared_types/legacy/item/UseAnimation.h"
#include "mc/entity/components/ItemInUseComponent.h"
#include "mc/server/ServerPlayer.h"
#include "mc/world/actor/ActorFlags.h"
#include "mc/world/actor/provider/ActorEquipment.h"
#include "mc/world/actor/provider/SynchedActorDataAccess.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/actor/player/PlayerItemInUse.h"
#include "mc/world/gamemode/GameMode.h"
#include "mc/world/gamemode/SurvivalMode.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/ItemStack.h"

#include <fstream>
#include <string>

// 基岩版服务端只在「潜行」时判定格挡，且盾牌自身从不进入「使用中」状态。
// 这里改为完全按 Java 版语义驱动：右键开始使用盾牌，服务端每刻根据
// 「是否正在使用盾牌且未冷却」重算 BLOCKING 标志，不再伪造潜行。
// 因此本地视角、碰撞箱、移动输入与右键交互全部保持原样。

namespace bedrock_edition_deputy {
Config& modConfig();
}

namespace bedrock_edition_deputy::shield_block {

namespace {

using SharedTypes::Legacy::UseAnimation;

// Java 版 ShieldItem#getUseDuration（约 1 小时，等于无限）。
constexpr int kShieldUseDuration = 72000;

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

// 同时打印「旧字段」「ECS 组件」与 USINGITEM 标志三个来源，用来确定哪一个才是权威状态。
std::string usingState(::Player const& player) {
    auto        component = const_cast<::Player&>(player).getEntityContext().tryGetComponent<ItemInUseComponent>();
    int const   duration  = component ? component->mDuration : -1;
    bool const  legacy    = !player.mItemInUse.get().mItem.get().isNull();
    int const   container = static_cast<int>(player.mItemInUse.get().mSlot.get().mContainerId);
    bool const  usingFlag = SynchedActorDataAccess::getActorFlag(
        const_cast<::Player&>(player).getEntityContext(),
        ActorFlags::Usingitem
    );
    return "ecsDur=" + std::to_string(duration) + " legacy=" + (legacy ? "1" : "0")
        + " cid=" + std::to_string(container) + " uf=" + (usingFlag ? "1" : "0");
}

bool isShield(Item const* item) { return item != nullptr && item->mUseAnim == UseAnimation::Block; }

// 检查玩家手中是否仍有盾牌（主手或副手）。
bool playerStillHasShield(::Player const& player) {
    auto& handContainer = ActorEquipment::getHandContainer(const_cast<::Player&>(player).getEntityContext());
    for (int i = 0; i < 2; ++i) {
        ItemStack const& stack = handContainer.getItem(i);
        if (offhands::hasItem(stack) && isShield(stack.mItem.get())) {
            return true;
        }
    }
    return false;
}

// 当前正在使用的物品（没有则为 nullptr）。
Item const* inUseItem(::Player const& player) {
    if (!offhands::isUsingItem(player)) {
        return nullptr;
    }
    return player.mItemInUse.get().mItem.get().mItem.get();
}

bool isOnCooldown(::Player const& player, Item const& item) {
    return player.isItemOnCooldown(item.getCooldownCategory());
}

// Java 版 LivingEntity#isBlocking，去掉其五刻延迟
// （Player::isBlocking 已通过 ShieldItem::EFFECTIVE_BLOCK_DELAY 施加该延迟）。
bool isUsingShield(::Player const& player) {
    Item const* item = inUseItem(player);
    return isShield(item) && !isOnCooldown(player, *item);
}

// 配置关闭时强制收盾的 bypass 标志。
thread_local bool gForceStop = false;

// releaseUsingItem 正在执行时置 true，让 stopUsingItem 守卫放行（玩家松手降盾）。
// HandSwapScope 析构等非松手路径触发 stopUsingItem 时该标志为 false，守卫拦截。
thread_local bool gInReleaseUsingItem = false;

// 双端：客户端经使用输入、服务端经使用事务都会走到这里。
// 注意：实际生效的是 SurvivalMode 的覆写，挂 GameMode::$useItem 生存模式下永远不会被调用。
LL_TYPE_INSTANCE_HOOK(
    ShieldUseItemHook,
    HookPriority::Normal,
    SurvivalMode,
    &SurvivalMode::$useItem,
    bool,
    ::ItemStack& item,
    ::HandSlot   handSlot
) {
    bool const used = origin(item, handSlot);

    ::Player&   player = mPlayer;
    Item const* type   = item.mItem.get();

    debugLog(
        std::string("[hook] useItem fired side=") + (player.isClientSide() ? "c" : "s")
        + " isShield=" + (isShield(type) ? "1" : "0") + " hasItem=" + (offhands::hasItem(item) ? "1" : "0")
        + " cd=" + std::to_string(type != nullptr && isOnCooldown(player, *type) ? 1 : 0) + " " + usingState(player)
    );

    if (!modConfig().enableShieldRightClick) {
        return used;
    }

    if (isShield(type) && offhands::hasItem(item) && !offhands::isUsingItem(player)
        && !isOnCooldown(player, *type)) {
        player.startUsingItem(item, kShieldUseDuration);
        debugLog(std::string("[hook] startUsingItem done ") + usingState(player));
    }

    return used;
}

// 守卫：当玩家正在使用盾牌且盾牌仍在手中时，拦截 stopUsingItem，
// 防止 HandSwapScope 析构、normalTick 等非松手路径杀死使用状态。
// releaseUsingItem（松手降盾）执行时 gInReleaseUsingItem=true，守卫放行。
// 玩家也可以通过切换物品来收盾（切走盾牌后 playerStillHasShield 返回 false，守卫自然失效）。
LL_TYPE_INSTANCE_HOOK(ShieldStopUsingGuardHook, HookPriority::Normal, Player, &Player::stopUsingItem, void) {
    if (!gForceStop && !gInReleaseUsingItem) {
        Item const* item = inUseItem(*this);
        if (isShield(item) && playerStillHasShield(*this)) {
            debugLog(std::string("[guard] blocked stopUsingItem ") + usingState(*this));
            return;
        }
    }
    debugLog(std::string("[trace] stopUsingItem ") + usingState(*this));
    origin();
}

LL_TYPE_INSTANCE_HOOK(ShieldCompleteUsingGuardHook, HookPriority::Normal, Player, &Player::completeUsingItem, void) {
    if (!gForceStop) {
        Item const* item = inUseItem(*this);
        if (isShield(item) && playerStillHasShield(*this)) {
            debugLog(std::string("[guard] blocked completeUsingItem ") + usingState(*this));
            return;
        }
    }
    debugLog(std::string("[trace] completeUsingItem ") + usingState(*this));
    origin();
}

// releaseUsingItem 是玩家松手降盾的正道。置 gInReleaseUsingItem 让 stopUsingItem 守卫放行。
LL_TYPE_INSTANCE_HOOK(ShieldReleaseTraceHook, HookPriority::Normal, GameMode, &GameMode::$releaseUsingItem, void) {
    debugLog(std::string("[trace] releaseUsingItem ") + usingState(mPlayer));
    gInReleaseUsingItem = true;
    origin();
    gInReleaseUsingItem = false;
}

// 服务端：ServerPlayer::normalTick 原本在潜行/骑乘时置 BLOCKING。Java 版只在
// 「正在使用盾牌」时格挡，因此这里改用该条件重算标志。
LL_TYPE_INSTANCE_HOOK(
    ShieldBlockingTickHook,
    HookPriority::Normal,
    ServerPlayer,
    &ServerPlayer::$normalTick,
    void
) {
    EntityContext& entity       = getEntityContext();
    bool const     wasBlocking  = SynchedActorDataAccess::getActorFlag(entity, ActorFlags::Blocking);
    uint64 const   previousTick = mPrevShieldBlockingTick->tickID;

    origin();

    auto& cfg = modConfig();

    // 开关被关掉时立刻收盾，让客户端动画与服务端状态同步回落。
    if (!cfg.enableShieldRightClick) {
        Item const* item = inUseItem(*this);
        if (isShield(item) && isUsingShield(*this)) {
            gForceStop = true;
            stopUsingItem();
            gForceStop = false;
        }
        SynchedActorDataAccess::setActorFlag(entity, ActorFlags::TransitionBlocking, false);
        SynchedActorDataAccess::setActorFlag(entity, ActorFlags::Blocking, false);
        return;
    }

    ItemStack const& shield              = getCurrentActiveShield();
    bool const       shieldRaisedChanged = offhands::hasItem(shield) && shield.mBlockingTick.tickID != previousTick;
    bool const       blocking            = isUsingShield(*this);

    if (blocking != wasBlocking) {
        debugLog(std::string("[srv] blocking ") + (blocking ? "1" : "0") + " " + usingState(*this));
    }

    SynchedActorDataAccess::setActorFlag(
        entity,
        ActorFlags::TransitionBlocking,
        shieldRaisedChanged || (blocking && !wasBlocking)
    );
    SynchedActorDataAccess::setActorFlag(entity, ActorFlags::Blocking, blocking);
}

} // namespace

void install() {
    debugLog(std::string("[hook] install useItem rc=") + std::to_string(ShieldUseItemHook::hook()));
    debugLog(std::string("[hook] install normalTick rc=") + std::to_string(ShieldBlockingTickHook::hook()));
    debugLog(std::string("[hook] install stopGuard rc=") + std::to_string(ShieldStopUsingGuardHook::hook()));
    debugLog(std::string("[hook] install completeGuard rc=") + std::to_string(ShieldCompleteUsingGuardHook::hook()));
    debugLog(std::string("[hook] install releaseTrace rc=") + std::to_string(ShieldReleaseTraceHook::hook()));
}

void uninstall() {
    ShieldReleaseTraceHook::unhook();
    ShieldCompleteUsingGuardHook::unhook();
    ShieldStopUsingGuardHook::unhook();
    ShieldBlockingTickHook::unhook();
    ShieldUseItemHook::unhook();
}

} // namespace bedrock_edition_deputy::shield_block
