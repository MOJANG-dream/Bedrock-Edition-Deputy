#include "mod/OffhandInput.h"

#include "mod/Config.h"
#include "mod/Offhands.h"

#include "ll/api/memory/Hook.h"

#include "mc/client/game/ClientInputCallbacks.h"
#include "mc/client/game/IClientInstance.h"
#include "mc/client/input/BuildActionIntention.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/deps/shared_types/legacy/item/UseAnimation.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/gamemode/GameMode.h"
#include "mc/world/gamemode/InteractionResult.h"
#include "mc/world/gamemode/SurvivalMode.h"
#include "mc/world/inventory/transaction/ComplexInventoryTransaction.h"
#include "mc/world/item/HandSlot.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/phys/HitResult.h"
#include "mc/world/phys/HitResultType.h"

#include <chrono>
#include <memory>

// Java 版 Minecraft#startUseItem 的手部迭代逻辑：先试主手，主手没有消耗这次点击时，
// 再在 HandSwapScope 内对副手重试同一套 GameMode 调用。原版该管线只认主手，因此
// 在 ClientInputCallbacks::handleBuildAction 外面套一层。
//
// 注意：游戏实际使用的是 SurvivalMode 的覆写（GameMode::$xxx 在生存模式下不会被调用），
// 因此监视主手行为的钩子必须挂在 SurvivalMode 上。

namespace bedrock_edition_deputy {
Config& modConfig();
}

namespace bedrock_edition_deputy::offhand_input {

namespace {

using SharedTypes::Legacy::UseAnimation;

// BuildActionIntention 的动作位。
constexpr int kBuildIntent    = 1 | 32;   // Build | FirstBuild
constexpr int kInteractIntent = 16 | 128; // Interact | FirstInteract
constexpr int kUseIntent      = kBuildIntent | kInteractIntent;

// 与 Java 版 Minecraft#startUseItem 的交互间隔一致：输入帧比游戏刻密，
// 没有节流会让副手在一次点按里被重复使用。
constexpr auto kOffhandActionDelay = std::chrono::milliseconds(200);

// 主手这次点击做了什么，决定是否还要走副手。
struct MainhandAttempt {
    Actor* interactTarget = nullptr;
    Vec3   interactLocation;
    bool   interacted = false;
    bool   built      = false;
    bool   used       = false;
};

thread_local MainhandAttempt*                      gMainhandAttempt    = nullptr;
thread_local bool                                  gOffhandBuildResult = false;
thread_local std::chrono::steady_clock::time_point gLastOffhandAction{};

Player& playerOf(GameMode& gameMode) { return gameMode.mPlayer; }

bool isShieldStack(ItemStack const& stack) {
    Item const* item = stack.mItem.get();
    return item != nullptr && item->mUseAnim == UseAnimation::Block;
}

// 副手动作的节流窗口。
bool offhandActionAllowed() {
    auto const now = std::chrono::steady_clock::now();
    if (now - gLastOffhandAction < kOffhandActionDelay) {
        return false;
    }
    gLastOffhandAction = now;
    return true;
}

// Java 版 ShieldItem#use 在手部迭代里会先于其他物品返回成功，所以举盾要排在副手重试之前。
// 准星对着实体时原版只会走 interact，不会调用 useItem，因此这里主动补一次。
void startShieldUseIfHeld(LocalPlayer& player, GameMode& gameMode) {
    if (offhands::isUsingItem(player)) {
        return;
    }

    ItemStack const& mainhand = player.getSelectedItem();
    if (isShieldStack(mainhand)) {
        gameMode.useItem(const_cast<ItemStack&>(mainhand), HandSlot::Mainhand);
        return;
    }

    if (!isShieldStack(offhands::getItem(player))) {
        return;
    }

    offhands::HandSwapScope scope(player);
    if (scope.isSwapped()) {
        gameMode.useItem(const_cast<ItemStack&>(player.getSelectedItem()), HandSlot::Mainhand);
    }
}

// Minecraft#startUseItem 的 OFF_HAND 轮次，走与主手相同的 GameMode 调用。
void useOffhand(LocalPlayer& player, int intent, MainhandAttempt const& attempt, HitResult const& solidHitResult) {
    if (!offhandActionAllowed()) {
        return;
    }

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
// 注意 handleBuildAction 是静态成员函数（无 this），必须用静态 hook。
LL_STATIC_HOOK(
    HandleBuildActionHook,
    HookPriority::Normal,
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

    bool const mainhandConsumed = attempt.interacted || attempt.built || attempt.used || offhands::isUsingItem(*player);
    if (wasUsingItem || mainhandConsumed || player->isSpectator()) {
        return resetBai;
    }

    if (modConfig().enableShieldRightClick) {
        startShieldUseIfHeld(*player, *player->mGameMode);
        if (offhands::isUsingItem(*player)) {
            return resetBai;
        }
    }

    if (!offhands::hasItem(offhands::getItem(*player))) {
        return resetBai;
    }

    useOffhand(*player, intent, attempt, solidHitResult);
    return resetBai;
}

// 记录主手是否放置了方块；副手轮次里则回填结果。
// 注意：生存模式下真正被调用的是 SurvivalMode 的覆写，必须挂派生类的 thunk。
LL_TYPE_INSTANCE_HOOK(
    OffhandBuildBlockHook,
    HookPriority::Normal,
    SurvivalMode,
    &SurvivalMode::$buildBlock,
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

// 记录主手是否成功「对着方块使用物品」——原版放置方块多数走这条路径，
// 不记录就会被误判成主手没有消耗，进而对副手重复使用。
LL_TYPE_INSTANCE_HOOK(
    OffhandUseItemOnHook,
    HookPriority::Normal,
    SurvivalMode,
    &SurvivalMode::$useItemOn,
    ::InteractionResult,
    ::ItemStack&      item,
    ::BlockPos const& at,
    uchar             face,
    ::Vec3 const&     hit,
    ::HandSlot        handSlot,
    ::Block const*    targetBlock,
    bool              isFirstEvent
) {
    ::InteractionResult const result = origin(item, at, face, hit, handSlot, targetBlock, isFirstEvent);

    if (gMainhandAttempt != nullptr && !offhands::HandSwapScope::isActive(playerOf(*this)) && result.mSuccess) {
        gMainhandAttempt->used = true;
    }

    return result;
}

// 记录主手是否与实体交互；副手轮次里交换双手后重试。
LL_TYPE_INSTANCE_HOOK(
    OffhandInteractHook,
    HookPriority::Normal,
    SurvivalMode,
    &SurvivalMode::$interact,
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
    OffhandUseItemOnHook::hook();
    OffhandInteractHook::hook();
    OffhandBaseUseItemHook::hook();
    LocalSendComplexTransactionHook::hook();
}

void uninstall() {
    LocalSendComplexTransactionHook::unhook();
    OffhandBaseUseItemHook::unhook();
    OffhandInteractHook::unhook();
    OffhandUseItemOnHook::unhook();
    OffhandBuildBlockHook::unhook();
    HandleBuildActionHook::unhook();
    gMainhandAttempt = nullptr;
}

} // namespace bedrock_edition_deputy::offhand_input
