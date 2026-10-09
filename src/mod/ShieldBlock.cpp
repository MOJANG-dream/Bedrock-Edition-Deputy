#include "mod/ShieldBlock.h"

#include "mod/Config.h"
#include "mod/Offhands.h"

#include "ll/api/memory/Hook.h"
#include "ll/api/mod/NativeMod.h"

#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/shared_types/legacy/item/UseAnimation.h"
#include "mc/entity/components/ItemInUseComponent.h"
#include "mc/server/ServerPlayer.h"
#include "mc/world/actor/ActorFlags.h"
#include "mc/world/actor/provider/ActorEquipment.h"
#include "mc/world/actor/provider/SynchedActorDataAccess.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/actor/player/PlayerInventory.h"
#include "mc/world/actor/player/PlayerItemInUse.h"
#include "mc/world/ContainerID.h"
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
// 注意：Actor::isClientSide() 在本版头文件中是 `return !mLevel` 的占位实现，恒为 false，
// 不能用来区分端；用 isLocalPlayer() 与对象指针区分 LocalPlayer / ServerPlayer。
std::string usingState(::Player const& player) {
    auto        component = const_cast<::Player&>(player).getEntityContext().tryGetComponent<ItemInUseComponent>();
    int const   duration  = component ? component->mDuration : -1;
    bool const  legacy    = !player.mItemInUse.get().mItem.get().isNull();
    int const   container = static_cast<int>(player.mItemInUse.get().mSlot.get().mContainerId);
    bool const  usingFlag = SynchedActorDataAccess::getActorFlag(
        const_cast<::Player&>(player).getEntityContext(),
        ActorFlags::Usingitem
    );
    return std::string("local=") + (player.isLocalPlayer() ? "1" : "0")
        + " this=" + std::to_string(reinterpret_cast<uintptr_t>(&player))
        + " ecsDur=" + std::to_string(duration) + " legacy=" + (legacy ? "1" : "0")
        + " cid=" + std::to_string(container) + " uf=" + (usingFlag ? "1" : "0");
}

bool isShield(Item const* item) { return item != nullptr && item->mUseAnim == UseAnimation::Block; }

std::string itemName(Item const* item) {
    return item != nullptr ? item->mFullName->getString() : std::string("-");
}

std::string itemProps(Item const* item) {
    if (item == nullptr) {
        return " anim=- maxDur=-";
    }
    return " anim=" + std::to_string(static_cast<int>(item->mUseAnim))
        + " maxDur=" + std::to_string(item->mMaxUseDuration);
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
    ::Player&   player   = mPlayer;
    Item const* type     = item.mItem.get();
    bool const  wasUsing = offhands::isUsingItem(player);

    bool const used = origin(item, handSlot);

    debugLog(
        std::string("[hook] useItem fired ")
        + "item=" + itemName(type) + itemProps(type)
        + " isShield=" + (isShield(type) ? "1" : "0") + " hasItem=" + (offhands::hasItem(item) ? "1" : "0")
        + " wasUsing=" + (wasUsing ? "1" : "0")
        + " cd=" + std::to_string(type != nullptr && isOnCooldown(player, *type) ? 1 : 0) + " " + usingState(player)
    );

    if (!modConfig().enableShieldRightClick) {
        return used;
    }

    // 服务端使用中每刻会自动调 useItem。wasUsing 为真说明这次调用发生在使用期间，
    // 若 origin 把使用掐掉了也不能重启——否则快速点击时盾牌会反复闪起（僵尸重启循环）。
    //
    // 副手路径（外层已有 HandSwapScope，盾已被换到选中槽）：直接启动即可，
    // 外层作用域析构时会把使用槽位改写到 Offhand 容器。
    // 主手路径不能再套 HandSwapScope：交换会把盾换进副手槽，startUsingItem
    // 记录的使用槽位对不上实物所在槽，normalTick 的「使用物品不在记录槽」检查
    // 会在下一刻收盾（这正是主手盾「点按秒收、无法格挡」的根因）。
    // 参考模组 FrederoxDev/Offhand 的 ShieldBlocking 也不交换，直接 startUsingItem。
    if (!wasUsing && isShield(type) && offhands::hasItem(item) && !offhands::isUsingItem(player)
        && !isOnCooldown(player, *type)) {
        bool const swapped = offhands::HandSwapScope::isActive(player);
        player.startUsingItem(item, kShieldUseDuration);
        if (!swapped && offhands::isUsingItem(player)) {
            // 兜底：把使用槽位显式指向当前选中槽（startUsingItem 的槽位解析在
            // 非原版调用路径下可能记录成 slot=0，导致 normalTick 每刻掐掉使用）。
            auto& slot        = player.mItemInUse.get().mSlot.get();
            slot.mSlot        = player.mInventory->mSelected;
            slot.mContainerId = ::ContainerID::Inventory;
        }
        debugLog(
            std::string("[hook] startUsingItem done swapped=") + (swapped ? "1" : "0") + " " + usingState(player)
        );
    }

    return used;
}

// 双端：参考模组确认 baseUseItem 在客户端（输入路径）与服务端（使用事务路径）都会走到。
// 客户端的 SurvivalMode::$useItem 实测不会被 baseUseItem 调到（日志里从无 side=c），
// 导致客户端从不 startUsingItem，服务端同步 USINGITEM 标志后客户端把使用 reconcile 掉。
// 因此在 baseUseItem 返回后补一次 startUsingItem；useItem hook 先启动过的话这里会被
// !isUsingItem 检查跳过，不会重复。
LL_TYPE_INSTANCE_HOOK(
    ShieldBaseUseItemHook,
    HookPriority::Normal,
    GameMode,
    &GameMode::baseUseItem,
    bool,
    ::ItemStack const& item,
    ::HandSlot         handSlot
) {
    ::Player&   player   = mPlayer;
    Item const* type     = item.mItem.get();
    bool const  wasUsing = offhands::isUsingItem(player);

    bool const used = origin(item, handSlot);

    if (!modConfig().enableShieldRightClick) {
        return used;
    }

    if (!wasUsing && isShield(type) && offhands::hasItem(item) && !offhands::isUsingItem(player)
        && !isOnCooldown(player, *type)) {
        // 同 useItem 侧：副手路径外层已有交换则直接启动；主手路径不交换，
        // 启动后把使用槽位显式指向当前选中槽。
        bool const swapped = offhands::HandSwapScope::isActive(player);
        player.startUsingItem(item, kShieldUseDuration);
        if (!swapped && offhands::isUsingItem(player)) {
            auto& slot        = player.mItemInUse.get().mSlot.get();
            slot.mSlot        = player.mInventory->mSelected;
            slot.mContainerId = ::ContainerID::Inventory;
        }
        debugLog(
            std::string("[hook] baseUseItem startUsingItem swapped=") + (swapped ? "1" : "0")
            + " item=" + itemName(type) + " " + usingState(player)
        );
    }

    return used;
}

// 双端：Java 版 Player#disableShield 会停止使用盾牌。Player::tryDisableShield 只启动冷却，
// 客户端也会收到该冷却，因此每端在冷却开始时自行停止使用。
LL_TYPE_INSTANCE_HOOK(
    ShieldStartCooldownHook,
    HookPriority::Normal,
    Player,
    &Player::startItemCooldown,
    void,
    ::HashedString const& type,
    int                   tickDuration,
    bool                  updateClient
) {
    origin(type, tickDuration, updateClient);

    if (!offhands::isUsingItem(*this)) {
        return;
    }

    Item const* item = inUseItem(*this);
    if (isShield(item) && isOnCooldown(*this, *item)) {
        debugLog(std::string("[shield] cooldown started, stopUsingItem ") + usingState(*this));
        this->stopUsingItem();
    }
}

// 诊断：追踪谁在停止使用，定位「快速点击后使用被掐掉」的凶手。
LL_TYPE_INSTANCE_HOOK(
    ShieldStopUsingHook,
    HookPriority::Normal,
    Player,
    &Player::stopUsingItem,
    void
) {
    if (offhands::isUsingItem(*this)) {
        debugLog(std::string("[hook] stopUsingItem ") + usingState(*this));
    }
    origin();
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
        if (isUsingShield(*this)) {
            stopUsingItem();
        }
        SynchedActorDataAccess::setActorFlag(entity, ActorFlags::TransitionBlocking, false);
        SynchedActorDataAccess::setActorFlag(entity, ActorFlags::Blocking, false);
        return;
    }

    ItemStack const& shield              = getCurrentActiveShield();
    bool const       shieldRaisedChanged = offhands::hasItem(shield) && shield.mBlockingTick.tickID != previousTick;
    bool const       blocking            = isUsingShield(*this);

    // 诊断：使用中每刻都输出，确认 ServerPlayer::normalTick 是否运行、blocking 如何计算。
    if (blocking != wasBlocking || offhands::isUsingItem(*this)) {
        debugLog(
            std::string("[srv] blocking=") + (blocking ? "1" : "0") + " was=" + (wasBlocking ? "1" : "0")
            + " " + usingState(*this)
        );
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
    debugLog(std::string("[hook] install baseUseItem rc=") + std::to_string(ShieldBaseUseItemHook::hook()));
    debugLog(std::string("[hook] install normalTick rc=") + std::to_string(ShieldBlockingTickHook::hook()));
    debugLog(std::string("[hook] install cooldown rc=") + std::to_string(ShieldStartCooldownHook::hook()));
    debugLog(std::string("[hook] install stopUsing rc=") + std::to_string(ShieldStopUsingHook::hook()));
}

void uninstall() {
    ShieldStopUsingHook::unhook();
    ShieldStartCooldownHook::unhook();
    ShieldBlockingTickHook::unhook();
    ShieldBaseUseItemHook::unhook();
    ShieldUseItemHook::unhook();
}

} // namespace bedrock_edition_deputy::shield_block
