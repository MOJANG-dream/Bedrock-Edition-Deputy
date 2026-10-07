#include "mod/OffhandInput.h"

#include "mod/Offhands.h"

#include "ll/api/memory/Hook.h"

#include "mc/client/game/ClientInputCallbacks.h"
#include "mc/client/game/IClientInstance.h"
#include "mc/client/input/BuildActionIntention.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/gamemode/GameMode.h"
#include "mc/world/inventory/transaction/ComplexInventoryTransaction.h"
#include "mc/world/item/HandSlot.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/phys/HitResult.h"
#include "mc/world/phys/HitResultType.h"

#include <memory>

// Java 版 Minecraft#startUseItem 的手部迭代逻辑：先试主手，主手没有消耗这次点击时，
// 再在 HandSwapScope 内对副手重试同一套 GameMode 调用。原版该管线只认主手，因此
// 在 ClientInputCallbacks::handleBuildAction 外面套一层。

namespace bedrock_edition_deputy::offhand_input {

namespace {

// BuildActionIntention 的动作位。
constexpr int kBuildIntent    = 1 | 32;   // Build | FirstBuild
constexpr int kInteractIntent = 16 | 128; // Interact | FirstInteract
constexpr int kUseIntent      = kBuildIntent | kInteractIntent;

// 主手这次点击做了什么，决定是否还要走副手。
struct MainhandAttempt {
    Actor* interactTarget = nullptr;
    Vec3   interactLocation;
    bool   interacted = false;
    bool   built      = false;
    bool   used       = false;
};

thread_local MainhandAttempt* gMainhandAttempt    = nullptr;
thread_local bool             gOffhandBuildResult = false;

Player& playerOf(GameMode& gameMode) { return gameMode.mPlayer; }

// Minecraft#startUseItem 的 OFF_HAND 轮次，走与主手相同的 GameMode 调用。
void useOffhand(LocalPlayer& player, int intent, MainhandAttempt const& attempt, HitResult const& solidHitResult) {
    GameMode& gameMode = *player.mGameMode;

    if (solidHitResult.mType == HitResultType::Entity) {
        if (attempt.interactTarget != nullptr) {
            offhands::HandSwapScope scope(player);
            if (scope.isSwapped()) {
                gameMode.interact(*attempt.interactTarget, attempt.interactLocation, HandSlot::Mainhand);
            }
        }
        return;
    }

    if ((intent & kBuildIntent) != 0 && solidHitResult.mType == HitResultType::Tile) {
        gOffhandBuildResult = false;
        {
            offhands::HandSwapScope scope(player);
            if (scope.isSwapped()) {
                gameMode.startBuildBlock(solidHitResult.mBlock, solidHitResult.mFacing, HandSlot::Mainhand);
            }
        }
        if (gOffhandBuildResult) {
            return;
        }
    }

    if ((intent & kInteractIntent) == 0) {
        return;
    }

    offhands::HandSwapScope scope(player);
    if (scope.isSwapped()) {
        gameMode.baseUseItem(player.getSelectedItem(), HandSlot::Mainhand);
    }
}

// 主手轮次的入口。原版 Minecraft#handleKeybinds 在「物品使用中」时会吞掉所有攻击/使用点击。
LL_TYPE_INSTANCE_HOOK(
    HandleBuildActionHook,
    HookPriority::Normal,
    ClientInputCallbacks,
    &ClientInputCallbacks::handleBuildAction,
    bool,
    ::IClientInstance&      client,
    ::BuildActionIntention& bai,
    ::HitResult const&      solidHitResult,
    ::HitResult const&      liquidHitResult
) {
    LocalPlayer* player = client.getLocalPlayer();
    if (player == nullptr || (bai.mAction & kUseIntent) == 0 || offhands::HandSwapScope::isActive(*player)) {
        return origin(client, bai, solidHitResult, liquidHitResult);
    }

    int const  intent       = bai.mAction;
    bool const wasUsingItem = offhands::isUsingItem(*player);

    MainhandAttempt attempt;
    gMainhandAttempt    = &attempt;
    bool const resetBai = origin(client, bai, solidHitResult, liquidHitResult);
    gMainhandAttempt    = nullptr;

    bool const mainhandConsumed =
        attempt.interacted || attempt.built || attempt.used || offhands::isUsingItem(*player);
    if (wasUsingItem || mainhandConsumed || player->isSpectator() || !offhands::hasItem(offhands::getItem(*player))) {
        return resetBai;
    }

    useOffhand(*player, intent, attempt, solidHitResult);
    return resetBai;
}

// 记录主手是否放置了方块；副手轮次里则回填结果。
LL_TYPE_INSTANCE_HOOK(
    OffhandBuildBlockHook,
    HookPriority::Normal,
    GameMode,
    &GameMode::$buildBlock,
    bool,
    ::BlockPos const& pos,
    uchar             face,
    ::HandSlot        handSlot,
    bool const        isSimTick
) {
    Player& player = playerOf(*this);

    if (offhands::HandSwapScope::isActive(player)) {
        gOffhandBuildResult = origin(pos, face, handSlot, isSimTick);
        return gOffhandBuildResult;
    }

    if (gMainhandAttempt != nullptr && !isSimTick) {
        gMainhandAttempt->built = origin(pos, face, handSlot, isSimTick);
        return gMainhandAttempt->built;
    }

    return origin(pos, face, handSlot, isSimTick);
}

// 记录主手是否与实体交互；副手轮次里交换双手后重试。
LL_TYPE_INSTANCE_HOOK(
    OffhandInteractHook,
    HookPriority::Normal,
    GameMode,
    &GameMode::$interact,
    bool,
    ::Actor&      entity,
    ::Vec3 const& location,
    ::HandSlot    handSlot
) {
    Player& player = playerOf(*this);

    if (gMainhandAttempt == nullptr || offhands::HandSwapScope::isActive(player)) {
        return origin(entity, location, handSlot);
    }

    gMainhandAttempt->interactTarget   = &entity;
    gMainhandAttempt->interactLocation = location;
    gMainhandAttempt->interacted       = origin(entity, location, handSlot);
    return gMainhandAttempt->interacted;
}

// 记录主手是否使用了物品。
LL_TYPE_INSTANCE_HOOK(
    OffhandBaseUseItemHook,
    HookPriority::Normal,
    GameMode,
    &GameMode::baseUseItem,
    bool,
    ::ItemStack const& item,
    ::HandSlot         handSlot
) {
    if (gMainhandAttempt == nullptr || offhands::HandSwapScope::isActive(playerOf(*this))) {
        return origin(item, handSlot);
    }

    gMainhandAttempt->used = origin(item, handSlot);
    return gMainhandAttempt->used;
}

// 客户端发出的、产生于 HandSwapScope 内的事务其实是关于副手的，打上标记让服务端也交换双手。
LL_TYPE_INSTANCE_HOOK(
    LocalSendComplexTransactionHook,
    HookPriority::Normal,
    LocalPlayer,
    &LocalPlayer::$sendComplexInventoryTransaction,
    void,
    ::std::unique_ptr<::ComplexInventoryTransaction> transaction
) {
    if (transaction != nullptr && offhands::HandSwapScope::isActive(*this)) {
        offhands::markTransaction(*transaction);
    }
    origin(std::move(transaction));
}

} // namespace

void install() {
    HandleBuildActionHook::hook();
    OffhandBuildBlockHook::hook();
    OffhandInteractHook::hook();
    OffhandBaseUseItemHook::hook();
    LocalSendComplexTransactionHook::hook();
}

void uninstall() {
    LocalSendComplexTransactionHook::unhook();
    OffhandBaseUseItemHook::unhook();
    OffhandInteractHook::unhook();
    OffhandBuildBlockHook::unhook();
    HandleBuildActionHook::unhook();
    gMainhandAttempt = nullptr;
}

} // namespace bedrock_edition_deputy::offhand_input
