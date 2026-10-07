#pragma once

#include "mc/world/item/ItemStack.h"

#include <vector>

namespace bedrock_edition_deputy::offhands {

// 手部 SimpleContainer 中副手所在槽位（0 = 主手，1 = 副手）。
inline constexpr int kHandContainerOffhandSlot = 1;

// 副手容器（ContainerEnumName::OffhandContainer）中的槽位索引。
inline constexpr int kOffhandContainerSlot = 0;

// 打在副手发起的库存事务 mSlot 上的标记，服务端 handle 时据此还原。
inline constexpr int kTransactionSlotMarker = -2;

class Player;
class Container;
class ComplexInventoryTransaction;

bool hasItem(ItemStack const& stack);

ItemStack const& getItem(Player& player);

bool isUsingItem(Player const& player);
bool isUsingOffhandItem(Player const& player);

void setItemInUseSlotToOffhand(Player& player);

// 作用域内产生的库存事务其实是关于副手的，接收端也要交换双手来处理。
void markTransaction(ComplexInventoryTransaction& transaction);

// 原版所有「手持物品」路径都读写背包选中槽，把它和副手槽临时物理对调，
// 这些路径就会作用于副手。作用域外看不到这次对调：两个槽的变更通知都被
// 暂时拦下，退出时只上报真正发生变化的那个槽。
class HandSwapScope {
public:
    explicit HandSwapScope(Player& player);
    ~HandSwapScope();

    HandSwapScope(HandSwapScope const&)            = delete;
    HandSwapScope& operator=(HandSwapScope const&) = delete;

    [[nodiscard]] bool isSwapped() const { return mSwapped; }

    static bool isActive(Player const& player);

    // 活动作用域任一之手对应的槽位。
    static bool holdsNotification(Container const& container, int slot);

    // 为真时，玩家所在作用域会在结束时自行发送库存。
    static bool deferInventorySend(Player const& player, bool shouldSelectSlot);

private:
    Player&        mPlayer;
    HandSwapScope* mOuter{nullptr};

    Container*                mInventory{nullptr};
    Container*                mHand{nullptr};
    std::vector<::ItemStack>* mInventoryItems{nullptr};
    std::vector<::ItemStack>* mHandItems{nullptr};
    int                       mSelectedSlot{0};

    bool mSwapped{false};
    bool mWasUsingItem{false};
    bool mWasUsingOffhandItem{false};
    bool mInventorySendDeferred{false};
    bool mDeferredSendSelectsSlot{false};

    ItemStack mMainhandBefore;
    ItemStack mOffhandBefore;
};

void install();
void uninstall();

} // namespace bedrock_edition_deputy::offhands
