#include "mod/OffhandSync.h"

#include "mod/Offhands.h"

#include "ll/api/memory/Hook.h"

#include "mc/network/packet/LegacySetSlot.h"
#include "mc/server/ServerPlayer.h"
#include "mc/world/Container.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/actor/player/PlayerInventory.h"
#include "mc/world/containers/ContainerEnumName.h"
#include "mc/world/inventory/network/ItemStackNetManagerServer.h"
#include "mc/world/inventory/transaction/ComplexInventoryTransaction.h"
#include "mc/world/inventory/transaction/InventoryTransactionError.h"
#include "mc/world/inventory/transaction/ItemReleaseInventoryTransaction.h"
#include "mc/world/inventory/transaction/ItemUseInventoryTransaction.h"
#include "mc/world/inventory/transaction/ItemUseOnActorInventoryTransaction.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

// 让服务端以「双手已交换」的视角处理副手事务，并在旧式槽位回执里还原槽位。

namespace bedrock_edition_deputy::offhand_sync {

namespace {

// 双端：监听者（HUD、容器模型、服务端物品堆叠 id）不应看到 HandSwapScope 的对调，
// 作用域结束时才会上报净变化。
LL_TYPE_INSTANCE_HOOK(
    ContainerSetChangedHook,
    HookPriority::Normal,
    Container,
    &Container::$setContainerChanged,
    void,
    int slot
) {
    if (offhands::HandSwapScope::holdsNotification(*this, slot)) {
        return;
    }
    origin(slot);
}

// 服务端：原版会在 HandSwapScope 包裹的调用内部重发库存，那会把「已交换的双手」
// 展示给客户端；由作用域在还原之后再发。
LL_TYPE_INSTANCE_HOOK(
    ServerSendInventoryHook,
    HookPriority::Normal,
    ServerPlayer,
    &ServerPlayer::$sendInventory,
    void,
    bool shouldSelectSlot
) {
    if (offhands::HandSwapScope::deferInventorySend(*this, shouldSelectSlot)) {
        return;
    }
    origin(shouldSelectSlot);
}

// 服务端：作用域内发出的事务（例如 Player::completeUsingItem 的）其实是关于副手的，
// 客户端也要以交换过的双手来处理。
LL_TYPE_INSTANCE_HOOK(
    ServerSendComplexTransactionHook,
    HookPriority::Normal,
    ServerPlayer,
    &ServerPlayer::$sendComplexInventoryTransaction,
    void,
    ::std::unique_ptr<::ComplexInventoryTransaction> transaction
) {
    if (transaction != nullptr && offhands::HandSwapScope::isActive(*this)) {
        offhands::markTransaction(*transaction);
    }
    origin(std::move(transaction));
}

// 客户端把副手事务的变化槽位按「双手已交换」列出（副手的变更落在选中槽上，反之亦然）。
// _handleLegacyTransactionRequest 在 handle 已还原双手之后才运行，若不映射，
// 每个槽都会拿到另一只手的净 id，客户端后续针对该物品的 ItemStackRequest 会被拒绝。
struct SwappedLegacySlots {
    Player const* player       = nullptr;
    int           selectedSlot = 0;
};

thread_local SwappedLegacySlots gSwappedLegacySlots;

LL_TYPE_INSTANCE_HOOK(
    ItemUseHandleHook,
    HookPriority::Normal,
    ItemUseInventoryTransaction,
    &ItemUseInventoryTransaction::$handle,
    ::InventoryTransactionError,
    ::Player& player,
    bool      isSenderAuthority
) {
    gSwappedLegacySlots = {};

    if (mSlot != offhands::kTransactionSlotMarker) {
        return origin(player, isSenderAuthority);
    }

    int& slot = mSlot;
    slot      = player.mInventory->mSelected;

    ::InventoryTransactionError error;
    {
        offhands::HandSwapScope scope(player);
        error = origin(player, isSenderAuthority);
    }

    // _handleLegacyTransactionRequest 只跟随成功的事务。
    if (error == ::InventoryTransactionError::NoError) {
        gSwappedLegacySlots = {&player, slot};
    }

    slot = offhands::kTransactionSlotMarker;
    return error;
}

LL_TYPE_INSTANCE_HOOK(
    ItemUseOnActorHandleHook,
    HookPriority::Normal,
    ItemUseOnActorInventoryTransaction,
    &ItemUseOnActorInventoryTransaction::$handle,
    ::InventoryTransactionError,
    ::Player& player,
    bool      isSenderAuthority
) {
    gSwappedLegacySlots = {};

    if (mSlot != offhands::kTransactionSlotMarker) {
        return origin(player, isSenderAuthority);
    }

    int& slot = mSlot;
    slot      = player.mInventory->mSelected;

    ::InventoryTransactionError error;
    {
        offhands::HandSwapScope scope(player);
        error = origin(player, isSenderAuthority);
    }

    if (error == ::InventoryTransactionError::NoError) {
        gSwappedLegacySlots = {&player, slot};
    }

    slot = offhands::kTransactionSlotMarker;
    return error;
}

LL_TYPE_INSTANCE_HOOK(
    ItemReleaseHandleHook,
    HookPriority::Normal,
    ItemReleaseInventoryTransaction,
    &ItemReleaseInventoryTransaction::$handle,
    ::InventoryTransactionError,
    ::Player& player,
    bool      isSenderAuthority
) {
    gSwappedLegacySlots = {};

    if (mSlot != offhands::kTransactionSlotMarker) {
        return origin(player, isSenderAuthority);
    }

    int& slot = mSlot;
    slot      = player.mInventory->mSelected;

    ::InventoryTransactionError error;
    {
        offhands::HandSwapScope scope(player);
        error = origin(player, isSenderAuthority);
    }

    if (error == ::InventoryTransactionError::NoError) {
        gSwappedLegacySlots = {&player, slot};
    }

    slot = offhands::kTransactionSlotMarker;
    return error;
}

// 旧式槽位把副手记为 HandContainer 的槽 1，而不是 OffhandContainer 的槽 0。
LL_TYPE_INSTANCE_HOOK(
    LegacyTransactionRequestHook,
    HookPriority::Normal,
    ItemStackNetManagerServer,
    &ItemStackNetManagerServer::_handleLegacyTransactionRequest,
    void,
    ::ItemStackLegacyRequestId const&     legacyClientRequestId,
    ::std::vector<::LegacySetSlot> const& legacySetItemSlots
) {
    SwappedLegacySlots swapped = gSwappedLegacySlots;
    gSwappedLegacySlots        = {};

    if (swapped.player != &mPlayer) {
        origin(legacyClientRequestId, legacySetItemSlots);
        return;
    }

    std::vector<::LegacySetSlot> unswapped;
    auto const                   add = [&unswapped](::ContainerEnumName container, uchar slot) {
        auto entry = std::find_if(
            unswapped.begin(),
            unswapped.end(),
            [container](::LegacySetSlot const& candidate) { return candidate.mContainerEnum == container; }
        );
        if (entry == unswapped.end()) {
            entry = unswapped.insert(unswapped.end(), ::LegacySetSlot{container, {}});
        }
        entry->mSlots.get().push_back(slot);
    };

    for (::LegacySetSlot const& entry : legacySetItemSlots) {
        for (uchar const slot : entry.mSlots.get()) {
            if (entry.mContainerEnum == ::ContainerEnumName::InventoryContainer
                && slot == static_cast<uchar>(swapped.selectedSlot)) {
                add(::ContainerEnumName::OffhandContainer, static_cast<uchar>(offhands::kHandContainerOffhandSlot));
            } else if (
                entry.mContainerEnum == ::ContainerEnumName::OffhandContainer
                && slot == static_cast<uchar>(offhands::kHandContainerOffhandSlot)
            ) {
                add(::ContainerEnumName::InventoryContainer, static_cast<uchar>(swapped.selectedSlot));
            } else {
                add(entry.mContainerEnum, slot);
            }
        }
    }

    origin(legacyClientRequestId, unswapped);
}

} // namespace

void install() {
    ContainerSetChangedHook::hook();
    ServerSendInventoryHook::hook();
    ServerSendComplexTransactionHook::hook();
    ItemUseHandleHook::hook();
    ItemUseOnActorHandleHook::hook();
    ItemReleaseHandleHook::hook();
    LegacyTransactionRequestHook::hook();
}

void uninstall() {
    LegacyTransactionRequestHook::unhook();
    ItemReleaseHandleHook::unhook();
    ItemUseOnActorHandleHook::unhook();
    ItemUseHandleHook::unhook();
    ServerSendComplexTransactionHook::unhook();
    ServerSendInventoryHook::unhook();
    ContainerSetChangedHook::unhook();
    gSwappedLegacySlots = {};
}

} // namespace bedrock_edition_deputy::offhand_sync
