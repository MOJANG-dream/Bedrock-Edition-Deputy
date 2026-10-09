#include "mod/Offhands.h"

#include "ll/api/memory/Hook.h"
#include "ll/api/mod/NativeMod.h"

#include "mc/deps/shared_types/legacy/item/UseAnimation.h"
#include "mc/entity/components/ItemInUseComponent.h"
#include "mc/world/Container.h"
#include "mc/world/ContainerID.h"
#include "mc/world/SimpleContainer.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/actor/player/PlayerInventory.h"
#include "mc/world/actor/player/PlayerItemInUse.h"
#include "mc/world/actor/player/PlayerInventorySlotData.h"
#include "mc/world/actor/provider/ActorEquipment.h"
#include "mc/world/inventory/transaction/ComplexInventoryTransaction.h"
#include "mc/world/inventory/transaction/ItemReleaseInventoryTransaction.h"
#include "mc/world/inventory/transaction/ItemUseInventoryTransaction.h"
#include "mc/world/inventory/transaction/ItemUseOnActorInventoryTransaction.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/registry/ItemRegistry.h"
#include "mc/world/item/registry/ItemRegistryRef.h"

#include <fstream>
#include <string>
#include <vector>

// 全局作用域前向声明：buildServerRegistry 钩子的参数类型（放进命名空间会变成不同类型）。
class BaseGameVersion;
class IMinecraftEventing;
class LinkedAssetValidator;
class ResourcePackManager;

namespace bedrock_edition_deputy::offhands {

namespace {

// 不同玩家的作用域可以嵌套，因此每次查询都沿链条上溯。
thread_local HandSwapScope* gActiveScope = nullptr;

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

void exchange(ItemStack& lhs, ItemStack& rhs) {
    ItemStack temporary = lhs;
    lhs                 = rhs;
    rhs                 = temporary;
}

// 物品注册结束后把所有物品的 mAllowOffhand 打开：Java 版副手可放任何物品。
void applyOffhandFlags(ItemRegistry& registry) {
    for (auto const& item : registry.mItemRegistry.get()) {
        if (!item) {
            continue;
        }
        item->mAllowOffhand = ::Item::OffhandAllowed::Yes;
        // Java 版 ShieldItem#getUseDuration / TridentItem#getUseDuration 均为 72000；
        // 基岩版盾牌与长矛从不「被使用」，因此没有时长（蓄力时长为 0 会立刻完成使用）。
        if ((item->mUseAnim == ::SharedTypes::Legacy::UseAnimation::Block
             || item->mUseAnim == ::SharedTypes::Legacy::UseAnimation::Spear)
            && item->mMaxUseDuration == 0) {
            item->mMaxUseDuration = 72000;
        }
        // 诊断：转储所有带使用动画的物品，确认新版「矛」的动画值与蓄力时长。
        if (item->mUseAnim != ::SharedTypes::Legacy::UseAnimation::None) {
            debugLog(
                std::string("[reg] item=") + item->mFullName->getString()
                + " anim=" + std::to_string(static_cast<int>(item->mUseAnim))
                + " maxDur=" + std::to_string(item->mMaxUseDuration)
            );
        }
    }
}

#ifdef LL_PLAT_C
// 客户端：ItemRegistry::finishedRegistration 仅客户端导出（#ifdef LL_PLAT_C）。
LL_TYPE_INSTANCE_HOOK(
    AllowAllOffhandHook,
    HookPriority::Normal,
    ItemRegistry,
    &ItemRegistry::finishedRegistration,
    void,
    ::Experiments const& experiments
) {
    origin(experiments);
    applyOffhandFlags(*this);
}
#endif

// 服务端：注册由 ItemRegistryRef::buildServerRegistry 完成（双端均导出，
// 客户端的 IntegratedServer 也走这条路径，重复设置幂等）。
LL_TYPE_INSTANCE_HOOK(
    ServerBuildRegistryHook,
    HookPriority::Normal,
    ItemRegistryRef,
    &ItemRegistryRef::buildServerRegistry,
    void,
    ::Experiments const&                               experiments,
    ::BaseGameVersion const&                           baseGameVersion,
    ::ResourcePackManager*                             rpm,
    ::Bedrock::NonOwnerPointer<::LinkedAssetValidator> validator,
    ::IMinecraftEventing&                              eventing
) {
    origin(experiments, baseGameVersion, rpm, validator, eventing);
    // TypedStorage<8,16,weak_ptr<T>> 直接退化为 weak_ptr<T>。
    if (auto registry = mWeakRegistry.lock()) {
        applyOffhandFlags(*registry);
    }
}

} // namespace

bool hasItem(ItemStack const& stack) {
    return !stack.isNull() && stack.mItem.get() != nullptr && stack.mCount != 0;
}

ItemStack const& getItem(Player& player) {
    return ActorEquipment::getHandContainer(player.getEntityContext()).getItem(kHandContainerOffhandSlot);
}

bool isUsingItem(Player const& player) {
    // 参考模组只查旧字段：startUsingItem 设置它，stopUsingItem 清空它。
    // ECS ItemInUseComponent 由 ItemInUseComponentRemoveSystem 异步管理，不可靠。
    return !player.mItemInUse.get().mItem.get().isNull();
}

bool isUsingOffhandItem(Player const& player) {
    return isUsingItem(player) && player.mItemInUse.get().mSlot.get().mContainerId == ContainerID::Offhand;
}

void setItemInUseSlotToOffhand(Player& player) {
    player.mItemInUse.get().mSlot.get().mSlot        = kOffhandContainerSlot;
    player.mItemInUse.get().mSlot.get().mContainerId = ContainerID::Offhand;
}

void markTransaction(ComplexInventoryTransaction& transaction) {
    switch (transaction.mType) {
    case ComplexInventoryTransaction::Type::ItemUseTransaction:
        static_cast<ItemUseInventoryTransaction&>(transaction).mSlot = kTransactionSlotMarker;
        break;
    case ComplexInventoryTransaction::Type::ItemUseOnEntityTransaction:
        static_cast<ItemUseOnActorInventoryTransaction&>(transaction).mSlot = kTransactionSlotMarker;
        break;
    case ComplexInventoryTransaction::Type::ItemReleaseTransaction:
        static_cast<ItemReleaseInventoryTransaction&>(transaction).mSlot = kTransactionSlotMarker;
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// HandSwapScope
// ---------------------------------------------------------------------------

HandSwapScope::HandSwapScope(Player& player) : mPlayer(player), mOuter(gActiveScope) {
    if (isActive(player)) {
        return;
    }

    PlayerInventory& supplies = *player.mInventory;
    if (supplies.mSelectedContainerId != ContainerID::Inventory) {
        return;
    }

    auto& inventory = *supplies.mInventory;
    auto& hand      = ActorEquipment::getHandContainer(player.getEntityContext());

    auto& inventoryItems = inventory.mItems.get();
    auto& handItems      = hand.mItems.get();

    int const selected = supplies.mSelected;
    if (selected < 0 || selected >= static_cast<int>(inventoryItems.size())
        || kHandContainerOffhandSlot >= static_cast<int>(handItems.size())) {
        return;
    }

    mInventory      = &inventory;
    mHand           = &hand;
    mInventoryItems = &inventoryItems;
    mHandItems      = &handItems;
    mSelectedSlot   = selected;

    mMainhandBefore = inventoryItems[selected];
    mOffhandBefore  = handItems[kHandContainerOffhandSlot];
    exchange(inventoryItems[selected], handItems[kHandContainerOffhandSlot]);

    mWasUsingItem        = isUsingItem(player);
    mWasUsingOffhandItem = isUsingOffhandItem(player);
    if (mWasUsingOffhandItem) {
        player.mItemInUse.get().mSlot.get().mSlot        = selected;
        player.mItemInUse.get().mSlot.get().mContainerId = ContainerID::Inventory;
    }

    mSwapped     = true;
    gActiveScope = this;
}

HandSwapScope::~HandSwapScope() {
    if (!mSwapped) {
        return;
    }

    exchange((*mInventoryItems)[mSelectedSlot], (*mHandItems)[kHandContainerOffhandSlot]);

    PlayerItemInUse& itemInUse = mPlayer.mItemInUse.get();
    // 使用槽位只要指向 Inventory 容器（主手侧）就改写到 Offhand，不校验具体槽号：
    // startUsingItem 的实现可能用 slot=0 而不是当前选中槽索引。
    bool const usingSelectedSlot = isUsingItem(mPlayer)
        && itemInUse.mSlot.get().mContainerId == ContainerID::Inventory;
    if (usingSelectedSlot && (mWasUsingOffhandItem || !mWasUsingItem)) {
        setItemInUseSlotToOffhand(mPlayer);
    }

    gActiveScope = mOuter;

    // 监听者只会看到双手各自的净变化，永远看不到这次对调本身。
    if ((*mInventoryItems)[mSelectedSlot] != mMainhandBefore) {
        mInventory->setContainerChanged(mSelectedSlot);
    }
    if ((*mHandItems)[kHandContainerOffhandSlot] != mOffhandBefore) {
        mHand->setContainerChanged(kHandContainerOffhandSlot);
    }

    if (mInventorySendDeferred) {
        mPlayer.sendInventory(mDeferredSendSelectsSlot);
    }
}

bool HandSwapScope::isActive(Player const& player) {
    for (HandSwapScope const* scope = gActiveScope; scope != nullptr; scope = scope->mOuter) {
        if (&scope->mPlayer == &player) {
            return true;
        }
    }
    return false;
}

bool HandSwapScope::holdsNotification(Container const& container, int slot) {
    for (HandSwapScope const* scope = gActiveScope; scope != nullptr; scope = scope->mOuter) {
        if ((&container == scope->mInventory && slot == scope->mSelectedSlot)
            || (&container == scope->mHand && slot == kHandContainerOffhandSlot)) {
            return true;
        }
    }
    return false;
}

bool HandSwapScope::deferInventorySend(Player const& player, bool shouldSelectSlot) {
    for (HandSwapScope* scope = gActiveScope; scope != nullptr; scope = scope->mOuter) {
        if (&scope->mPlayer != &player) {
            continue;
        }
        scope->mInventorySendDeferred   = true;
        scope->mDeferredSendSelectsSlot = scope->mDeferredSendSelectsSlot || shouldSelectSlot;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 安装 / 卸载
// ---------------------------------------------------------------------------

void install() {
#ifdef LL_PLAT_C
    AllowAllOffhandHook::hook();
#endif
    ServerBuildRegistryHook::hook();
}

void uninstall() {
    ServerBuildRegistryHook::unhook();
#ifdef LL_PLAT_C
    AllowAllOffhandHook::unhook();
#endif
    gActiveScope = nullptr;
}

} // namespace bedrock_edition_deputy::offhands
