#pragma once

#include "mc/world/item/ItemStack.h"

#include <vector>

class Container;
class ComplexInventoryTransaction;
class Player;

namespace bedrock_edition_deputy::offhands {

inline constexpr int kHandContainerOffhandSlot = 1;
inline constexpr int kOffhandContainerSlot = 0;
inline constexpr int kTransactionSlotMarker = -2;

bool hasItem(ItemStack const& stack);

ItemStack const& getItem(::Player& player);

bool isUsingItem(::Player const& player);
bool isUsingOffhandItem(::Player const& player);

void setItemInUseSlotToOffhand(::Player& player);

void markTransaction(::ComplexInventoryTransaction& transaction);

class HandSwapScope {
public:
    explicit HandSwapScope(Player& player);
    ~HandSwapScope();

    HandSwapScope(HandSwapScope const&)            = delete;
    HandSwapScope& operator=(HandSwapScope const&) = delete;

    [[nodiscard]] bool isSwapped() const { return mSwapped; }

    static bool isActive(Player const& player);

    static bool holdsNotification(Container const& container, int slot);

    static bool deferInventorySend(Player const& player, bool shouldSelectSlot);

private:
    Player&        mPlayer;
    HandSwapScope* mOuter{nullptr};

    Container*                     mInventory{nullptr};
    Container*                     mHand{nullptr};
    std::vector<::ItemStack>*      mInventoryItems{nullptr};
    std::vector<::ItemStack>*      mHandItems{nullptr};
    int                            mSelectedSlot{0};

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