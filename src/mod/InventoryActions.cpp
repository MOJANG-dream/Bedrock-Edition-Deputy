#include "mod/InventoryActions.h"

#include "ll/api/memory/Hook.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/gui/ViewRequest.h"
#include "mc/client/gui/screens/controllers/ContainerScreenController.h"
#include "mc/client/gui/screens/controllers/CraftingScreenController.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/world/ContainerID.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/containers/SlotData.h"
#include "mc/world/containers/managers/controllers/ContainerManagerController.h"
#include "mc/world/inventory/network/ItemStackNetManagerBase.h"
#include "mc/world/inventory/network/ItemStackNetManagerClient.h"
#include "mc/world/inventory/transaction/InventoryAction.h"
#include "mc/world/inventory/transaction/InventorySource.h"
#include "mc/world/inventory/transaction/InventorySourceType.h"
#include "mc/world/inventory/transaction/InventoryTransactionManager.h"
#include "mc/world/item/ItemStack.h"

#include <mutex>
#include <stdexcept>

// 实现参考开源客户端模组 Lamium（LGPL-3.0，amatouhake/Lamium）的
// InventoryMove.cpp 与 HoverTracker.cpp：物品移动一律走游戏自己的容器
// 事务/请求路径，不手工构造网络包。

namespace bedrock_edition_deputy::inventory_actions {

namespace {

// ---------------------------------------------------------------------------
// HUD 下的无界面移动：legacy inventory transaction + 原版容器 setter
// ---------------------------------------------------------------------------

enum class Place { Inventory, Offhand };

struct Location {
    Place place;
    int   slot;
};

bool gMoving          = false;
bool gMoveHookReady   = false;
bool gRecordedInv     = false;
bool gRecordedOffhand = false;

ContainerID containerIdOf(Place place) { return place == Place::Offhand ? ContainerID::Offhand : ContainerID::Inventory; }

ItemStack const& itemAt(LocalPlayer& player, Location at) {
    if (at.place == Place::Offhand) {
        return player.getOffhandSlot();
    }
    return player.getInventory().getItem(at.slot);
}

void setSlot(LocalPlayer& player, Location at, ItemStack const& item) {
    if (at.place == Place::Offhand) {
        player.setOffhandSlot(item);
    } else {
        player.getInventory().$setItem(at.slot, item);
    }
}

LL_TYPE_INSTANCE_HOOK(
    MoveAddActionHook,
    HookPriority::Normal,
    InventoryTransactionManager,
    &InventoryTransactionManager::addAction,
    void,
    ::InventoryAction const& action,
    bool                      forceBalanced
) {
    if (gMoving) {
        auto const& source = action.mSource.get();
        if (source.mType == InventorySourceType::ContainerInventory) {
            if (source.mContainerId == ContainerID::Inventory) {
                gRecordedInv = true;
            }
            if (source.mContainerId == ContainerID::Offhand) {
                gRecordedOffhand = true;
            }
        }
    }
    origin(action, forceBalanced);
}

// 让原版客户端 setter 记录动作，再刷出平衡的 InventoryTransaction。
bool movePair(LocalPlayer& player, Location a, ItemStack const& newA, Location b, ItemStack const& newB) {
    if (gMoving || !gMoveHookReady) {
        return false;
    }
    auto& manager = player.mTransactionManager.get();
    auto* base    = player.mItemStackNetManager.get();
    if (manager.mCurrentTransaction.get()) {
        return false;
    }
    if (!base || !base->mIsEnabled || !base->mIsClientSide) {
        return false;
    }
    if (static_cast<ItemStackNetManagerClient*>(base)->mRequest.get()) {
        return false;
    }

    ItemStack oldA = itemAt(player, a);
    ItemStack oldB = itemAt(player, b);

    struct ActiveGuard {
        ActiveGuard() { gMoving = true; }
        ~ActiveGuard() { gMoving = false; }
    } guard;
    gRecordedInv = false;
    gRecordedOffhand = false;

    auto scope = ItemStackNetManagerBase::_tryBeginClientLegacyTransactionRequest(&player);
    if (!base->mLegacyTransactionRequestId->mRawId) {
        return false;
    }

    setSlot(player, a, newA);
    setSlot(player, b, newB);

    // 副手 setter 不一定记录事务动作；缺哪一侧就手动补哪一侧，保证事务平衡。
    if (manager.mCurrentTransaction.get()) {
        Location  locs[2] = {a, b};
        ItemStack olds[2] = {std::move(oldA), std::move(oldB)};
        ItemStack news[2] = {newA, newB};
        for (int i = 0; i < 2; ++i) {
            bool recorded = locs[i].place == Place::Offhand ? gRecordedOffhand : gRecordedInv;
            if (recorded || locs[i].place == Place::Inventory) {
                continue;
            }
            InventorySource source;
            source.mType       = InventorySourceType::ContainerInventory;
            source.mContainerId = containerIdOf(locs[i].place);
            source.mFlags      = InventorySource::InventorySourceFlags::NoFlag;
            manager.addAction(
                ::InventoryAction{source, static_cast<uint>(0), olds[i], news[i]},
                false
            );
        }
    }

    if (manager.mCurrentTransaction.get()) {
        player.updateInventoryTransactions();
    }
    if (manager.mCurrentTransaction.get()) {
        throw std::runtime_error("offhand move transaction stayed unbalanced");
    }
    return true;
}

// ---------------------------------------------------------------------------
// 容器屏幕：悬停槽跟踪
// ---------------------------------------------------------------------------

struct Hovered {
    ContainerScreenController* controller{nullptr};
    std::string                collection;
    int                        index{-1};
};

std::mutex gHoverMutex;
Hovered    gHovered;

bool isPlayerInventoryCollection(std::string const& name) {
    return name == "inventory_items" || name == "hotbar_items";
}

LL_TYPE_INSTANCE_HOOK(
    ContainerHoverHook,
    HookPriority::Normal,
    ContainerScreenController,
    &ContainerScreenController::$_onContainerSlotHovered,
    ::ui::ViewRequest,
    ::std::string const& collectionName,
    int                  index
) {
    {
        std::lock_guard lock(gHoverMutex);
        gHovered = {this, collectionName, index};
    }
    return origin(collectionName, index);
}

LL_TYPE_INSTANCE_HOOK(
    CraftingHoverHook,
    HookPriority::Normal,
    CraftingScreenController,
    &CraftingScreenController::$_onContainerSlotHovered,
    ::ui::ViewRequest,
    ::std::string const& collectionName,
    int                  index
) {
    {
        std::lock_guard lock(gHoverMutex);
        gHovered = {this, collectionName, index};
    }
    return origin(collectionName, index);
}

LL_TYPE_INSTANCE_HOOK(
    ContainerUnhoverHook,
    HookPriority::Normal,
    ContainerScreenController,
    &ContainerScreenController::$_onContainerSlotUnhovered,
    ::ui::ViewRequest,
    ::std::string const& collectionName,
    int                  index
) {
    {
        std::lock_guard lock(gHoverMutex);
        if (gHovered.controller == this && gHovered.collection == collectionName && gHovered.index == index) {
            gHovered = {};
        }
    }
    return origin(collectionName, index);
}

LL_TYPE_INSTANCE_HOOK(
    ContainerLeaveHook,
    HookPriority::Normal,
    ContainerScreenController,
    &ContainerScreenController::$onLeave,
    void
) {
    {
        std::lock_guard lock(gHoverMutex);
        if (gHovered.controller == this) {
            gHovered = {};
        }
    }
    origin();
}

} // namespace

void swapHotbarOffhand() {
    auto clientInstance = ll::service::bedrock::getClientInstance();
    if (!clientInstance) {
        return;
    }
    auto* player = clientInstance->getLocalPlayer();
    if (!player || player->isSpectator()) {
        return;
    }

    int selected = player->getSelectedItemSlot();
    if (selected < 0 || selected >= 9) {
        return;
    }

    Location hand{Place::Inventory, selected};
    Location offhand{Place::Offhand, 0};

    ItemStack held = itemAt(*player, hand);
    ItemStack second = itemAt(*player, offhand);
    if (held.isNull() && second.isNull()) {
        return;
    }

    try {
        movePair(*player, hand, second, offhand, held);
    } catch (std::exception const&) {
        // 事务无法平衡时不动任何状态；原版会在随后用库存同步纠正。
    }
}

bool hasHoveredPlayerSlot() {
    std::lock_guard lock(gHoverMutex);
    return gHovered.controller != nullptr && isPlayerInventoryCollection(gHovered.collection)
        && gHovered.index >= 0;
}

void swapHoveredToOffhand() {
    Hovered target;
    {
        std::lock_guard lock(gHoverMutex);
        target = gHovered;
    }
    if (!target.controller || !isPlayerInventoryCollection(target.collection) || target.index < 0) {
        return;
    }

    auto& controller = *target.controller;
    auto* manager    = controller.mContainerManagerController.get();
    if (!manager || manager->mContainersClosed) {
        return;
    }
    if (controller._isCursorSelectedActive()) {
        return; // 光标正拿着一摞物品，交给原版点击处理
    }
    if (!manager->hasContainerController(target.collection)) {
        return;
    }
    if (manager->getItemStack(target.collection, target.index).isNull()) {
        return;
    }

    // 首选：屏幕自己的容器交换请求（界面动画与校验完全原生）。
    if (manager->hasContainerController("offhand_items")) {
        manager->handleSwap(
            ::SlotData{target.collection, target.index},
            ::SlotData{"offhand_items", 0}
        );
        return;
    }

    // 回退：该屏幕没有暴露副手 collection（UI 名称因版本而异）时，
    // 直接映射到玩家库存槽位走与 HUD F 交换相同的 legacy transaction。
    // hotbar_items[i] == Inventory[i]；inventory_items[i] == Inventory[i+9]。
    auto clientInstance = ll::service::bedrock::getClientInstance();
    if (!clientInstance) {
        return;
    }
    auto* player = clientInstance->getLocalPlayer();
    if (!player || player->isSpectator()) {
        return;
    }
    int invSlot = target.collection == "hotbar_items" ? target.index : target.index + 9;
    if (invSlot < 0 || invSlot >= player->getInventory().getContainerSize()) {
        return;
    }

    Location hand{Place::Inventory, invSlot};
    Location offhand{Place::Offhand, 0};
    ItemStack held   = itemAt(*player, hand);
    ItemStack second = itemAt(*player, offhand);
    if (held.isNull() && second.isNull()) {
        return;
    }
    try {
        movePair(*player, hand, second, offhand, held);
    } catch (std::exception const&) {
        // 事务不平衡时放弃；等待原版库存同步纠正。
    }
}

void install() {
    if (!gMoveHookReady) {
        MoveAddActionHook::hook();
        gMoveHookReady = true;
    }
    ContainerHoverHook::hook();
    CraftingHoverHook::hook();
    ContainerUnhoverHook::hook();
    ContainerLeaveHook::hook();
}

void uninstall() {
    ContainerLeaveHook::unhook();
    ContainerUnhoverHook::unhook();
    CraftingHoverHook::unhook();
    ContainerHoverHook::unhook();
    if (gMoveHookReady) {
        MoveAddActionHook::unhook();
        gMoveHookReady = false;
    }
    std::lock_guard lock(gHoverMutex);
    gHovered = {};
}

} // namespace bedrock_edition_deputy::inventory_actions
