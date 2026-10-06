#include "mod/ShieldBlock.h"

#include "mod/Config.h"

#include "ll/api/memory/Hook.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/entity/systems/ClientInputUpdateSystem.h"
#include "mc/client/game/ClientInstance.h"
#include "mc/client/input/ClientMoveInputHandler.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/deps/ecs/Optional.h"
#include "mc/deps/shared_types/legacy/item/UseAnimation.h"
#include "mc/deps/vanilla_components/ActorDataFlagComponent.h"
#include "mc/deps/vanilla_components/MovementAbilitiesComponent.h"
#include "mc/entity/components/MoveInputComponent.h"
#include "mc/entity/components/PlayerActionComponent.h"
#include "mc/entity/components/RawMoveInputComponent.h"
#include "mc/entity/components/SneakingComponent.h"
#include "mc/entity/components/WasInWaterFlagComponent.h"
#include "mc/input/MoveInputState.h"
#include "mc/network/packet/PlayerAuthInputPacketPayload.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/ItemStackBase.h"

#include <atomic>

// 实现参考开源客户端模组 Lamium（LGPL-3.0，amatouhake/Lamium）的
// PermanentSneak.cpp：在输入系统上游 extractRawHIDInput 给原版喂一份带
// SneakDown 的临时输入副本，使客户端预测与发送给服务端的 auth input 完全
// 一致，服务端按基岩版原生规则（潜行 + 持盾）判定格挡。

namespace bedrock_edition_deputy {
Config& modConfig();
}

namespace bedrock_edition_deputy::shield_block {

namespace {

std::atomic<bool> gRightHeld{false};

bool isShield(ItemStack const& stack) {
    return !stack.isNull() && stack.getTypeName() == "minecraft:shield";
}

// 当前右键按住时是否应当进入格挡姿态。
bool wantBlock() {
    auto& config = modConfig();
    if (!config.enableShieldRightClick || !gRightHeld.load(std::memory_order_relaxed)) {
        return false;
    }
    auto clientInstance = ll::service::bedrock::getClientInstance();
    if (!clientInstance || !clientInstance->isInGameInputEnabled()) {
        return false;
    }
    auto* player = clientInstance->getLocalPlayer();
    if (!player) {
        return false;
    }

    auto& mainHand = player->getSelectedItem();
    if (isShield(mainHand)) {
        return true; // 主手持盾
    }
    if (!isShield(player->getOffhandSlot())) {
        return false;
    }
    // 仅副手持盾：主手拿着可右键使用的物品（食物、药水、弓、方块等）时
    // 保持 Java 语义——先使用主手物品，不举盾。
    if (mainHand.isNull()) {
        return true;
    }
    return mainHand.mItem->mUseAnim == SharedTypes::Legacy::UseAnimation::None;
}

bool holdingShield(Player& player) {
    return isShield(player.getSelectedItem()) || isShield(player.getOffhandSlot());
}

// 上游输入注入：右键持盾时给原版一份 SneakDown 副本。
LL_STATIC_HOOK(
    ExtractShieldSneakHook,
    HookPriority::Normal,
    &ClientInputUpdateSystem::extractRawHIDInput,
    void,
    ::MovementAbilitiesComponent const&       abilities,
    ::MoveInputComponent const&               moveInput,
    ::ActorDataFlagComponent const&           flags,
    ::RawMoveInputComponent&                  rawMoveInput,
    ::Optional<::SneakingComponent const>     sneaking,
    ::Optional<::WasInWaterFlagComponent const> isInWater
) {
    auto clientInstance = ll::service::bedrock::getClientInstance();
    bool inject = false;
    if (clientInstance) {
        auto* player = clientInstance->getLocalPlayer();
        inject = player && holdingShield(*player) && wantBlock()
            && ClientMoveInputHandler::getMoveInput(*clientInstance) == &moveInput;
    }
    if (!inject) {
        origin(abilities, moveInput, flags, rawMoveInput, sneaking, isInWater);
        return;
    }
    // 只改临时副本，绝不污染玩家真实 HID 状态。
    auto augmented = moveInput;
    augmented.mRawInputState->mFlagValues->set(static_cast<size_t>(MoveInputState::Flag::SneakDown));
    origin(abilities, augmented, flags, rawMoveInput, sneaking, isInWater);
}

// 网络包修正：持盾真蹲（且没有在右键举盾）时清除全部潜行输入位，
// 使服务端不把这次蹲判定为格挡；本地玩家仍处于真实蹲姿（减速/弯身）。
LL_TYPE_INSTANCE_HOOK(
    AuthInputShieldHook,
    HookPriority::Normal,
    PlayerAuthInputPacketPayload,
    &PlayerAuthInputPacketPayload::setFromComponent,
    void,
    ::PlayerActionComponent& input
) {
    origin(input);

    auto& config = modConfig();
    if (!config.disableShieldSneakBlock) {
        return;
    }
    auto clientInstance = ll::service::bedrock::getClientInstance();
    if (!clientInstance) {
        return;
    }
    auto* player = clientInstance->getLocalPlayer();
    if (!player || !holdingShield(*player) || wantBlock()) {
        return;
    }
    auto* moveInput = ClientMoveInputHandler::getMoveInput(*clientInstance);
    if (!moveInput || !moveInput->mRawInputState->mFlagValues->test(
                        static_cast<size_t>(MoveInputState::Flag::SneakInputCurrentlyDown)
                    )) {
        return; // 物理上没有在蹲，无需处理
    }

    using InputData = PlayerAuthInputPacketPayload::InputData;
    auto& bits = mInputData.get().mContainer;
    bits.set(static_cast<size_t>(InputData::Sneaking), false);
    bits.set(static_cast<size_t>(InputData::SneakDown), false);
    bits.set(static_cast<size_t>(InputData::SneakToggleDown), false);
    bits.set(static_cast<size_t>(InputData::PersistSneak), false);
    bits.set(static_cast<size_t>(InputData::StartSneaking), false);
    bits.set(static_cast<size_t>(InputData::StopSneaking), false);
    bits.set(static_cast<size_t>(InputData::SneakPressedRaw), false);
    bits.set(static_cast<size_t>(InputData::SneakCurrentRaw), false);
    bits.set(static_cast<size_t>(InputData::SneakReleasedRaw), false);
}

} // namespace

void setRightHeld(bool held) { gRightHeld.store(held, std::memory_order_relaxed); }

void install() {
    ExtractShieldSneakHook::hook();
    AuthInputShieldHook::hook();
}

void uninstall() {
    AuthInputShieldHook::unhook();
    ExtractShieldSneakHook::unhook();
    gRightHeld.store(false, std::memory_order_relaxed);
}

} // namespace bedrock_edition_deputy::shield_block
