#include "mod/OffhandPlayerModel.h"

#include "mod/Offhands.h"

#include "ll/api/memory/Hook.h"

// 仅客户端：Molang 变量 API 只在客户端导出（LL_PLAT_C）。
#ifdef LL_PLAT_C

#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/shared_types/legacy/item/UseAnimation.h"
#include "mc/entity/components/ItemInUseComponent.h"
#include "mc/util/MolangScriptArg.h"
#include "mc/util/MolangScriptArgType.h"
#include "mc/util/MolangVariableMap.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorSwingSource.h"
#include "mc/world/actor/LegacyMolangVariableUpdate.h"
#include "mc/world/actor/Mob.h"
#include "mc/world/actor/RenderParams.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/actor/player/PlayerItemInUse.h"
#include "mc/world/item/HandSlot.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/ItemStack.h"

#include <algorithm>
#include <mutex>
#include <unordered_set>

// 移植自参考模组 OffhandPlayerModel.cpp：在 LegacyMolangVariableUpdate 写好主手
// 相关的 Molang 变量之后，把副手的使用状态改写/补充进去，让资源包里的动画
// （attack.rotations、use_item_progress、各持械姿态）把对应动作镜像到左臂。

namespace bedrock_edition_deputy::offhand_player_model {

namespace {

using SharedTypes::Legacy::UseAnimation;

struct MolangVariableName {
    uint64      hash;
    char const* name;
};

constexpr MolangVariableName variable(char const* name) { return {HashedString::computeHash(name), name}; }

// 由 LegacyMolangVariableUpdate::updateEntitySpecificMolangVariables 写入的变量。
constexpr MolangVariableName kAttackTime                = variable("variable.attack_time");
constexpr MolangVariableName kLeftArmSwimAmount         = variable("variable.left_arm_swim_amount");
constexpr MolangVariableName kRightArmSwimAmount        = variable("variable.right_arm_swim_amount");
constexpr MolangVariableName kUseItemIntervalProgress   = variable("variable.use_item_interval_progress");
constexpr MolangVariableName kUseItemStartupProgress    = variable("variable.use_item_startup_progress");
constexpr MolangVariableName kChargeAmount              = variable("variable.charge_amount");
constexpr MolangVariableName kIsBrandishingSpear        = variable("variable.is_brandishing_spear");
constexpr MolangVariableName kIsHoldingSpyglass         = variable("variable.is_holding_spyglass");
constexpr MolangVariableName kIsTootingGoatHorn         = variable("variable.is_tooting_goat_horn");
constexpr MolangVariableName kIsUsingBrush              = variable("variable.is_using_brush");

// 资源包 animation.player.attack.rotations / animation.humanoid.use_item_progress 读取。
constexpr MolangVariableName kOffhandAttackTime              = variable("variable.offhand_attack_time");
constexpr MolangVariableName kOffhandUseItemIntervalProgress = variable("variable.offhand_use_item_interval_progress");
constexpr MolangVariableName kOffhandUseItemStartupProgress  = variable("variable.offhand_use_item_startup_progress");

// 资源包 offhand_poses.animation.json 与玩家动画控制器读取。
constexpr MolangVariableName kOffhandPose                 = variable("variable.offhand_pose");
constexpr MolangVariableName kOffhandItemUse              = variable("variable.offhand_item_use");
constexpr MolangVariableName kOffhandItemUseNormalized    = variable("variable.offhand_item_use_normalized");
constexpr MolangVariableName kOffhandCrossbowCharged      = variable("variable.offhand_crossbow_charged");

// TridentItem.THROW_THRESHOLD_TIME，variable.charge_amount 按它充能。
constexpr float kTridentThrowThresholdTime = 10.0f;

// controller.animation.player.root 只在 variable.use_item_interval_progress > 0 时播放
// use_item_progress；该常量让它为副手保持播放，又不让右臂出现可见动作。
constexpr float kUseItemProgressPlaying = 0.0001f;

float getFloat(MolangVariableMap const& variables, MolangVariableName const& name) {
    MolangScriptArg const& value = variables.getMolangVariable(name.hash, name.name);
    return value.mType == MolangScriptArgType::Float ? value.mPOD.mFloat : 0.0f;
}

void setFloat(MolangVariableMap& variables, MolangVariableName const& name, float value) {
    variables.setMolangVariable(name.hash, name.name, MolangScriptArg{value});
}

// ---------------------------------------------------------------------------
// 副手挥臂追踪（移植自 OffhandSwing.cpp，去掉自定义包——1.26 的 Mob::swing
// 自带 HandSlot，主手挥臂落钩时清除标记即可）。
// ---------------------------------------------------------------------------

std::mutex                    gOffhandSwingersMutex;
std::unordered_set<Mob const*> gOffhandSwingers;

void setOffhandSwing(Mob const& mob, bool offhand) {
    std::scoped_lock lock(gOffhandSwingersMutex);
    if (offhand) {
        gOffhandSwingers.insert(&mob);
    } else {
        gOffhandSwingers.erase(&mob);
    }
}

bool isOffhandSwing(Mob const& mob) {
    if (!mob.mSwinging) {
        return false;
    }
    std::scoped_lock lock(gOffhandSwingersMutex);
    return gOffhandSwingers.contains(&mob);
}

// HumanoidModel#setupAttackAnimation 挥动 LivingEntity#swingingArm；左臂时身体反方向转。
void updateSwing(Player const& player, MolangVariableMap& variables) {
    if (!isOffhandSwing(player)) {
        setFloat(variables, kOffhandAttackTime, 0.0f);
        return;
    }

    float const attackTime = getFloat(variables, kAttackTime);
    float const swimAmount = getFloat(variables, kLeftArmSwimAmount);
    setFloat(variables, kAttackTime, 0.0f);
    setFloat(variables, kOffhandAttackTime, attackTime);
    setFloat(variables, kRightArmSwimAmount, swimAmount);
    setFloat(variables, kLeftArmSwimAmount, attackTime > 0.0f ? 0.0f : swimAmount);
}

bool isChargedCrossbow(ItemStack const& item) {
    return offhands::hasItem(item) && item.mItem.get()->mUseAnim == UseAnimation::Crossbow && item.hasChargedItem();
}

bool isEatingAnimation(UseAnimation animation) {
    return animation == UseAnimation::Eat || animation == UseAnimation::Drink
        || animation == UseAnimation::GlowStick || animation == UseAnimation::Sparkler;
}

// 必须与 offhand_poses.animation.json 提供副手姿态的动画集合一致。
bool hasHeldPose(UseAnimation animation) {
    return animation == UseAnimation::Bow || animation == UseAnimation::Crossbow || animation == UseAnimation::Spear
        || animation == UseAnimation::Spyglass || animation == UseAnimation::GoatHorn || animation == UseAnimation::Brush;
}

// Java 版把使用姿态放在使用它的那只手上（PlayerRenderer#getArmPose 分手判定），
// 副手已充能弩也走持握姿态。
void updateItemUse(Player& player, MolangVariableMap& variables) {
    bool const usingItem         = offhands::isUsingItem(player);
    bool const usingOffhandItem  = offhands::isUsingOffhandItem(player);
    ItemStack const& itemInUseStack = player.mItemInUse.get().mItem.get();
    Item const*      itemInUse   = usingItem ? itemInUseStack.mItem.get() : nullptr;
    UseAnimation const animation = itemInUse != nullptr ? itemInUse->mUseAnim : UseAnimation::None;

    // HumanoidModel#setupAnim 最后处理正在使用物品的手，所以它的姿态胜出；
    // 否则充能弩的持握姿态胜出，再否则主手优先。
    bool const mainhandPosed          = !usingOffhandItem && hasHeldPose(animation);
    bool const offhandCrossbowCharged = isChargedCrossbow(offhands::getItem(player));
    bool const offhandPose            = usingOffhandItem
            ? hasHeldPose(animation)
            : offhandCrossbowCharged && !mainhandPosed && !isChargedCrossbow(player.getSelectedItem());

    auto const  component   = player.getEntityContext().tryGetComponent<ItemInUseComponent>();
    float const remaining   = component != nullptr ? static_cast<float>(component->mDuration) : 0.0f;
    float const maxDuration = itemInUse != nullptr ? static_cast<float>(itemInUse->getMaxUseDuration(&itemInUseStack)) : 0.0f;

    setFloat(variables, kOffhandPose, offhandPose ? 1.0f : 0.0f);
    setFloat(variables, kOffhandItemUse, usingOffhandItem ? 1.0f : 0.0f);
    setFloat(variables, kOffhandItemUseNormalized, usingOffhandItem && maxDuration > 0.0f ? remaining / maxDuration : 0.0f);
    setFloat(variables, kOffhandCrossbowCharged, offhandPose && !usingOffhandItem ? 1.0f : 0.0f);

    // 使用中的盾牌由格挡姿态表现；否则 Player::getItemUseStartupProgress 会让手臂像进食一样动。
    if (animation == UseAnimation::Block) {
        setFloat(variables, kUseItemIntervalProgress, 0.0f);
        setFloat(variables, kUseItemStartupProgress, 0.0f);
        setFloat(variables, kOffhandUseItemIntervalProgress, 0.0f);
        setFloat(variables, kOffhandUseItemStartupProgress, 0.0f);
        return;
    }

    if (!usingOffhandItem) {
        setFloat(variables, kOffhandUseItemIntervalProgress, 0.0f);
        setFloat(variables, kOffhandUseItemStartupProgress, 0.0f);
        return;
    }

    // 只有进食/饮用通过 use_item_progress 抬手；其他使用类型有自己的姿态。
    float const interval = getFloat(variables, kUseItemIntervalProgress);
    float const startup  = getFloat(variables, kUseItemStartupProgress);
    bool const eating    = isEatingAnimation(animation);
    setFloat(variables, kOffhandUseItemIntervalProgress, eating ? interval : 0.0f);
    setFloat(variables, kOffhandUseItemStartupProgress, eating ? startup : 0.0f);
    setFloat(variables, kUseItemIntervalProgress, eating && (interval > 0.0f || startup > 0.0f) ? kUseItemProgressPlaying : 0.0f);
    setFloat(variables, kUseItemStartupProgress, 0.0f);

    // LegacyMolangVariableUpdate 按主手物品计算这些变量；它们选出的姿态由
    // variable.offhand_pose 镜像到左臂。
    float const charge = std::clamp((maxDuration - remaining) / kTridentThrowThresholdTime, 0.0f, 1.0f);
    setFloat(variables, kChargeAmount, animation == UseAnimation::Spear ? charge : 0.0f);
    setFloat(variables, kIsBrandishingSpear, animation == UseAnimation::Spear ? 1.0f : 0.0f);
    setFloat(variables, kIsHoldingSpyglass, animation == UseAnimation::Spyglass ? 1.0f : 0.0f);
    setFloat(variables, kIsTootingGoatHorn, animation == UseAnimation::GoatHorn ? 1.0f : 0.0f);
    setFloat(variables, kIsUsingBrush, animation == UseAnimation::Brush ? 1.0f : 0.0f);
}

// 客户端：每个玩家的第三/第一人称模型都会经过这里（本地玩家也在内）。
// 该函数在命名空间内（非成员函数），必须用静态 hook。
LL_STATIC_HOOK(
    UpdateEntitySpecificMolangVariablesHook,
    HookPriority::Normal,
    &LegacyMolangVariableUpdate::updateEntitySpecificMolangVariables,
    void,
    Actor&        actor,
    RenderParams& renderParams
) {
    origin(actor, renderParams);

    if (!actor.isPlayer()) {
        return;
    }

    Player&            player    = static_cast<Player&>(actor);
    MolangVariableMap& variables = actor.mMolangVariables.get();
    updateSwing(player, variables);
    updateItemUse(player, variables);
}

// 任何由 Mob::swing 开始的挥臂按 HandSlot 归属：副手打标，主手清除。
LL_TYPE_INSTANCE_HOOK(OffhandSwingHook, HookPriority::Normal, Mob, &Mob::$swing, bool, ActorSwingSource swingSource, HandSlot handSlot) {
    bool const started = origin(swingSource, handSlot);
    if (started) {
        setOffhandSwing(*this, handSlot == HandSlot::Offhand);
    }
    return started;
}

LL_TYPE_INSTANCE_HOOK(OffhandSwingAiStepHook, HookPriority::Normal, Player, &Player::$aiStep, void) {
    origin();
    if (!mSwinging) {
        setOffhandSwing(*this, false);
    }
}

} // namespace

void markOffhandSwing(Mob& mob) { setOffhandSwing(mob, true); }

void install() {
    UpdateEntitySpecificMolangVariablesHook::hook();
    OffhandSwingHook::hook();
    OffhandSwingAiStepHook::hook();
}

void uninstall() {
    OffhandSwingAiStepHook::unhook();
    OffhandSwingHook::unhook();
    UpdateEntitySpecificMolangVariablesHook::unhook();
}

} // namespace bedrock_edition_deputy::offhand_player_model

#else // LL_PLAT_C

namespace bedrock_edition_deputy::offhand_player_model {

void markOffhandSwing(Mob&) {}

void install() {}
void uninstall() {}

} // namespace bedrock_edition_deputy::offhand_player_model

#endif
