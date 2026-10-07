#include "mod/Offhands.h"

#include "ll/api/memory/Hook.h"

#include "mc/deps/shared_types/legacy/item/UseAnimation.h"
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

#include <vector>

namespace bedrock_edition_deputy::offhands {

namespace {

// 不同玩家的作用域可以嵌套，因此每次查询都沿链条上溯。
thread_local HandSwapScope* gActiveScope = nullptr;

void exchange(ItemStack& lhs, ItemStack& rhs) {
    ItemStack temporary = lhs;
    lhs                 = rhs;
    rhs                 = temporary;
}

// 物品注册结束（双端各一次）后把所有物品的 mAllowOffhand 打开：Java 版副手可放任何物品。
LL_TYPE_INSTANCE_HOOK(
    AllowAllOffhandHook,
    HookPriority::Normal,
    ItemRegistry,
    &ItemRegistry::finishedRegistration,
    void,
    ::Experiments const& experiments
) {
    origin(experiments);
    for (auto const& item : mItemRegistry.get()) {
        if (!item) {
            continue;
        }
        item->mAllowOffhand = ::Item::OffhandAllowed::Yes;
        // Java 版 ShieldItem#getUseDuration；基岩版盾牌从不「被使用」，因此没有时长。
        if (item->mUseAnim == ::SharedTypes::Legacy::UseAnimation::Block && item->mMaxUseDuration == 0) {
            item->mMaxUseDuration = 72000;
        }
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

    PlayerItemInUse& itemInUse         = mPlayer.mItemInUse.get();
    bool const       usingSelectedSlot = isUsingItem(mPlayer)
        && itemInUse.mSlot.get().mContainerId == ContainerID::Inventory
        && itemInUse.mSlot.get().mSlot == mSelectedSlot;
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

void install() { AllowAllOffhandHook::hook(); }

void uninstall() {
    AllowAllOffhandHook::unhook();
    gActiveScope = nullptr;
}

} // namespace bedrock_edition_deputy::offhands
