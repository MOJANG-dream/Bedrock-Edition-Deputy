#include "mod/ShieldBlock.h"

#include "mod/Config.h"

#include "ll/api/memory/Hook.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/entity/systems/ClientInputUpdateSystem.h"
#include "mc/client/game/ClientInstance.h"
#include "mc/client/input/ClientMoveInputHandler.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/deps/ecs/Optional.h"
#include "mc/deps/ecs/gamerefs_entity/EntityContext.h"
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
#include "mc/world/actor/ActorFlags.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/ItemStackBase.h"

#include <atomic>

// 设计说明（基岩版服务端以「潜行 + 持盾」作为格挡判定条件，无法绕开）：
// 1) 不改动本地输入管线 —— 本地玩家不会真的蹲下，视角/碰撞箱保持站立，
//    右键的放置方块、使用物品等交互完全不受影响。
// 2) 只在发往服务端的 PlayerAuthInputPacket 里伪造完整的潜行位
//    （状态位 Sneaking/SneakDown/SneakCurrentRaw + 边沿 StartSneaking /
//    StopSneaking），服务端按原生规则判定格挡并减伤。
// 3) 本地格挡动画通过直接写 ActorFlags::Blocking 实现。
// 4) 举盾减速（Java 版 = 潜行速度）通过在输入提取后缩放移动向量实现。

namespace bedrock_edition_deputy {
Config& modConfig();
}

namespace bedrock_edition_deputy::shield_block {

namespace {

// Java 版举盾移动速度 ≈ 潜行速度（约 30%）。
constexpr float kBlockMoveScale = 0.3f;

std::atomic<bool> gRightHeld{false};
bool              gWasBlocking{false};
bool              gBlockVisualOwned{false};

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
    // 仅副手持盾：主手拿着有右键行为的物品时保持 Java 语义——
    // 先使用主手物品，不举盾。
    if (mainHand.isNull()) {
        return true;
    }
    if (mainHand.mItem->mUseAnim != SharedTypes::Legacy::UseAnimation::None) {
        return false; // 食物、药水、弓、弩、三叉戟等
    }
    if (mainHand.mItem->mBlockType != nullptr) {
        return false; // 方块：右键应当放置
    }
    return true;
}

bool holdingShield(Player& player) {
    return isShield(player.getSelectedItem()) || isShield(player->getOffhandSlot());
}

bool physicallySneaking(IClientInstance& clientInstance) {
    auto* moveInput = ClientMoveInputHandler::getMoveInput(clientInstance);
    return moveInput && moveInput->mRawInputState->mFlagValues->test(
                            static_cast<size_t>(MoveInputState::Flag::SneakInputCurrentlyDown)
                        );
}

// 本地格挡动画：只在我们自己置位时负责清除，不干扰原版潜行举盾的动画。
void updateBlockVisual(LocalPlayer& player, bool blocking) {
    auto component = player.getEntityContext().tryGetComponent<ActorDataFlagComponent>();
    if (!component) {
        return;
    }
    if (blocking) {
        component->mValue.set(static_cast<size_t>(ActorFlags::Blocking), true);
        gBlockVisualOwned = true;
    } else if (gBlockVisualOwned) {
        component->mValue.set(static_cast<size_t>(ActorFlags::Blocking), false);
        gBlockVisualOwned = false;
    }
}

// 举盾减速：输入提取完成后缩放移动向量（本地预测与发包共用这份结果）。
LL_STATIC_HOOK(
    ExtractShieldSlowdownHook,
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
    origin(abilities, moveInput, flags, rawMoveInput, sneaking, isInWater);

    auto clientInstance = ll::service::bedrock::getClientInstance();
    if (!clientInstance || ClientMoveInputHandler::getMoveInput(*clientInstance) != &moveInput) {
        return;
    }
    if (!wantBlock()) {
        return;
    }
    rawMoveInput.mRawMove->x *= kBlockMoveScale;
    rawMoveInput.mRawMove->y *= kBlockMoveScale;
    rawMoveInput.mRawInput->mAnalogMoveVector->x *= kBlockMoveScale;
    rawMoveInput.mRawInput->mAnalogMoveVector->y *= kBlockMoveScale;
}

// 网络包修正：举盾时伪造潜行位让服务端判定格挡；持盾真蹲（且没有在右键
// 举盾）时清除全部潜行输入位，实现「蹲下不格挡」。
LL_TYPE_INSTANCE_HOOK(
    AuthInputShieldHook,
    HookPriority::Normal,
    PlayerAuthInputPacketPayload,
    &PlayerAuthInputPacketPayload::setFromComponent,
    void,
    ::PlayerActionComponent& input
) {
    origin(input);

    auto clientInstance = ll::service::bedrock::getClientInstance();
    if (!clientInstance) {
        return;
    }
    auto* player = clientInstance->getLocalPlayer();
    if (!player) {
        gWasBlocking = false;
        return;
    }

    auto&       config  = modConfig();
    bool const  shield  = holdingShield(*player);
    bool const  blocking = shield && wantBlock();
    using InputData     = PlayerAuthInputPacketPayload::InputData;
    auto& bits          = mInputData.get().mContainer;

    updateBlockVisual(*player, blocking);

    if (blocking) {
        bits.set(static_cast<size_t>(InputData::Sneaking), true);
        bits.set(static_cast<size_t>(InputData::SneakDown), true);
        bits.set(static_cast<size_t>(InputData::SneakCurrentRaw), true);
        // 潜行与冲刺互斥。
        bits.set(static_cast<size_t>(InputData::Sprinting), false);
        bits.set(static_cast<size_t>(InputData::SprintDown), false);
        bits.set(static_cast<size_t>(InputData::StartSprinting), false);
        if (!gWasBlocking) {
            bits.set(static_cast<size_t>(InputData::StartSneaking), true);
            bits.set(static_cast<size_t>(InputData::SneakPressedRaw), true);
            bits.set(static_cast<size_t>(InputData::StopSprinting), true);
        }
    } else if (gWasBlocking && !physicallySneaking(*clientInstance)) {
        // 格挡刚结束且物理上没有在蹲：发送结束潜行边沿。
        bits.set(static_cast<size_t>(InputData::Sneaking), false);
        bits.set(static_cast<size_t>(InputData::SneakDown), false);
        bits.set(static_cast<size_t>(InputData::SneakCurrentRaw), false);
        bits.set(static_cast<size_t>(InputData::StopSneaking), true);
        bits.set(static_cast<size_t>(InputData::SneakReleasedRaw), true);
    }

    if (!blocking && config.disableShieldSneakBlock && shield && physicallySneaking(*clientInstance)) {
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

    gWasBlocking = blocking;
}

} // namespace

void setRightHeld(bool held) { gRightHeld.store(held, std::memory_order_relaxed); }

void install() {
    ExtractShieldSlowdownHook::hook();
    AuthInputShieldHook::hook();
}

void uninstall() {
    AuthInputShieldHook::unhook();
    ExtractShieldSlowdownHook::unhook();
    gRightHeld.store(false, std::memory_order_relaxed);
    gWasBlocking      = false;
    gBlockVisualOwned = false;
}

} // namespace bedrock_edition_deputy::shield_block
