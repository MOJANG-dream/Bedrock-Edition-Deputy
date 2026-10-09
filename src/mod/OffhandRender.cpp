// 仅客户端：副手第一人称渲染依赖客户端渲染器。服务端构建时本文件编译为空。
#ifdef LL_PLAT_C

#include "mod/OffhandRender.h"

#include "mod/Offhands.h"

#include "ll/api/memory/Hook.h"

#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/client/game/IClientInstance.h"
#include "mc/client/model/models/DataDrivenModel.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/block/BlockGraphics.h"
#include "mc/client/renderer/block/BlockTessellator.h"
#include "mc/client/renderer/block/tessellation_pipeline/BlockSchematic.h"
#include "mc/client/renderer/block/tessellation_pipeline/item_transforms/ItemTransforms.h"
#include "mc/client/renderer/block/tessellation_pipeline/item_transforms/Transform.h"
#include "mc/client/renderer/block/tessellation_pipeline/item_transforms/Type.h"
#include "mc/client/renderer/game/ItemInHandRenderer.h"
#include "mc/deps/core/math/Matrix.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/minecraft_renderer/game/ItemContextFlags.h"
#include "mc/deps/minecraft_renderer/game/ItemRenderCall.h"
#include "mc/deps/renderer/Camera.h"
#include "mc/deps/renderer/MatrixStack.h"
#include "mc/entity/components/ItemInUseComponent.h"
#include "mc/world/actor/RenderParams.h"
#include "mc/world/actor/animation/AttachableSlotIndex.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/components/ComponentItem.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/block/components/BlockComponentDirectData.h"
#include "mc/world/level/block/components/BlockGeometryComponent.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

// 第一人称副手渲染，移植自参考模组 FrederoxDev/Offhand 的 OffhandRendering.cpp：
// 1. renderFirstPerson 捕获完整 ItemContextFlags（renderOffhandItem 只收到子集，
//    缺 FirstPersonPass/InHand 位时 renderItem 不画）；
// 2. 副手使用期间把 ItemInUseComponent.mDuration 置零，避免使用动画套在主手上，
//    自绘副手时临时恢复真实时长（进食/喝药水抬升姿态与弓/矛蓄力帧都读它）；
// 3. 自绘副手用镜像主手姿态（computeHeldItemPose），并钩住 _applyDefaultItemTransforms
//    把物品自身的显示变换一起镜像，否则位置/朝向错误；
// 4. 平面精灵在镜像后会从背面显示，需要再 scale(-1,-1,1) 翻回。
//    ItemRenderCall::mIsFlat 在本版 LeviLamina 头文件中未导出（空结构），
//    改用「非方块且非模型类使用动画（盾/弓/弩/矛）」启发式判定平面物品；
// 5. 方块按 ClientBlockPipeline 的物品显示变换摆放（fix space + JSON display），
//    未导出的 ClientBlockComponentDirectData 分支跳过，直接读几何组件的 schematic；
// 6. 弓的 attachable 恒绑右臂骨骼，第一人称副手弓需要镜像回左臂
//   （setupAttachable/renderAttachable 钩子）；
// 7. mIsMirroredArt 物品（如桶装类对称贴图）非方块姿态下再转 180°。
// 未移植：副手挥动动画（OffhandSwing）、bob view 摆动。

namespace bedrock_edition_deputy::offhand_render {

namespace {

using UseAnimation      = ::SharedTypes::Legacy::UseAnimation;
using ItemTransformType = ::ClientBlockPipeline::ItemTransforms::Type;

constexpr float PI = std::numbers::pi_v<float>;

// BrushItem::SWING_DURATION。
constexpr int kBrushSwingDuration = 20;

// renderOffhandItem 只收到 renderFirstPerson flags 的一个子集。
thread_local ::ItemContextFlags firstPersonItemFlags = ::ItemContextFlags::None;

// renderFirstPerson 期间隐藏副手使用时置位。
thread_local bool hidingOffhandUse   = false;
thread_local int  offhandUseDuration = 0;

// _applyDefaultItemTransforms 镜像期间置位。
thread_local bool mirroringItemTransforms = false;
// 本次镜像的副手物品是否是平面精灵（启发式，见文件头说明）。
thread_local bool mirroringFlatItem = false;

// 仅 renderFirstPerson 期间非空，attachable 钩子据此识别第一人称绘制。
thread_local Player* firstPersonPlayer = nullptr;

// 沿 x 轴镜像矩阵：x 偏移与 Y/Z 旋转取反，整体等价 S * M * S（S=scale(-1,1,1)）。
void mirrorInPlace(Matrix& pose) {
    auto& m = pose._m.get();
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if ((column == 0) != (row == 0)) {
                m[column][row] = -m[column][row];
            }
        }
    }
}

Matrix mirrored(Matrix const& transform) {
    Matrix result = transform;
    mirrorInPlace(result);
    return result;
}

// LL 的 Matrix 没有 mult，用 glm 乘法代替 pose.mult(other)。
void multInPlace(Matrix& pose, Matrix const& other) { pose._m.get() = pose._m.get() * other._m.get(); }

// 组件基类物品（1.21+ 数据驱动物品）的渲染矩阵由 JSON 决定，交回原版路径，
// 避免自绘姿态与 JSON 变换叠加。
bool setsMatrixFromJson(ItemStack const& item) {
    Item const* type = item.mItem.get();
    if (type == nullptr || !type->isComponentBased()) {
        return false;
    }
    return static_cast<::ComponentItem const*>(type)->shouldUseJsonForRenderMatrix();
}

bool isModelUseAnimation(Item const* type) {
    if (type == nullptr) {
        return false;
    }
    auto anim = type->mUseAnim;
    return anim == UseAnimation::Block || anim == UseAnimation::Bow || anim == UseAnimation::Crossbow
        || anim == UseAnimation::Spear;
}

// 旧 tessellation 的方块按平面精灵渲染（参考模组 usesNewTessellation）。
bool usesNewTessellation(::Block const& block) {
    auto& fallback = ::BlockGraphics::mVanillaBlocksWithFallbackToOldTessellation();
    if (block.getBlockType().mDefaultState.get() != nullptr
        && fallback.find(block.getBlockType().mDefaultState.get()->mNetworkId.get()) != fallback.end()) {
        return false;
    }
    return block.mDirectData.get().mUseNewTessellation.get();
}

// ClientBlockLogic::getItemDisplayTransform 的复刻。LL 头里 ClientBlockComponentDirectData
// 是空结构，itemVisual 覆盖分支拿不到，直接读方块几何组件的 schematic。
Matrix getItemDisplayTransform(::Block const& block, ItemTransformType type) {
    auto const* geometry = block.mDirectData.get().mBlockGeometryComponent.get();
    if (geometry != nullptr && geometry->mBlockSchematic.get() != nullptr) {
        auto const& transforms = geometry->mBlockSchematic.get()->mItemTransforms.get();
        auto        match      = std::find_if(transforms.begin(), transforms.end(), [type](auto const& transform) {
            return transform.mType.get() == type;
        });
        if (match != transforms.end()) {
            return match->mTransform.get();
        }
    }
    if (type <= ItemTransformType::Shelf) {
        return ::ClientBlockPipeline::ItemTransforms::getDefaultTransformMatrix(type);
    }
    return Matrix::IDENTITY();
}

// 复刻 ItemInHandRenderer::_shouldRenderOffhandItem。
bool shouldRenderOffhandItem(ItemInHandRenderer& renderer, Player& player) {
    ItemStack const& mainhand = renderer.mItem.get();
    ItemStack const& offhand  = renderer.mOffHandItem.get();

    if (mainhand.isInstance(::HashedString{"minecraft:filled_map"}, false)
        || mainhand.isInstance(::HashedString{"minecraft:photo_item"}, false)) {
        return offhand.isInstance(::HashedString{"minecraft:filled_map"}, false)
            || offhand.isInstance(::HashedString{"minecraft:photo_item"}, false)
            || offhand.isInstance(::HashedString{"minecraft:shield"}, false);
    }

    auto component   = player.getEntityContext().tryGetComponent<::ItemInUseComponent>();
    bool mainInUse   = component != nullptr && component->mDuration > 0;

    if (mainhand.isInstance(::HashedString{"minecraft:bow"}, false)) {
        return !mainInUse;
    }
    if (mainhand.isInstance(::HashedString{"minecraft:crossbow"}, false)) {
        // 参考模组还检查 hasChargedItem，LL 头未导出该方法，略过。
        return !mainInUse;
    }
    return true;
}

// 副手自绘期间读取的使用时长：组件被 renderFirstPerson 置零时用缓存值。
int itemUseDuration(Player const& player) {
    if (hidingOffhandUse) {
        return offhandUseDuration;
    }
    auto component = const_cast<::Player&>(player).getEntityContext().tryGetComponent<::ItemInUseComponent>();
    return component != nullptr ? component->mDuration : 0;
}

// 复刻 ItemInHandRenderer::_applyUseAnimation（去掉了需要渲染尺寸的分屏进食高度修正）。
void applyUseAnimation(Matrix& pose, ItemStack const& item, int useDuration, float frameAlpha) {
    Item const*        type      = item.mItem.get();
    UseAnimation const animation = type->mUseAnim;

    switch (animation) {
    case UseAnimation::Eat:
    case UseAnimation::Drink:
    case UseAnimation::GlowStick:
    case UseAnimation::Sparkler: {
        float const maxDuration = static_cast<float>(type->getMaxUseDuration(&item));
        float const t           = static_cast<float>(useDuration) - frameAlpha + 1.0f;
        float const progress    = 1.0f - t / maxDuration;

        float settle = 1.0f - progress;
        settle       = settle * settle * settle;
        settle       = settle * settle * settle;
        settle       = settle * settle * settle;
        float const raise = 1.0f - settle;

        float const magnitude = std::abs(std::cos(t / 4.0f * PI) * 0.1f) * (progress > 0.2f ? 1.0f : 0.0f);
        if (animation == UseAnimation::GlowStick || animation == UseAnimation::Sparkler) {
            pose.translate(magnitude, 0.0f, 0.0f);
        } else {
            pose.translate(0.0f, magnitude, 0.0f);
        }

        pose.translate(raise * 0.55f, -raise * 0.5f, 0.0f);
        pose.rotate(raise * 90.0f, 0.0f, 1.0f, 0.0f);
        pose.rotate(raise * 10.0f, 1.0f, 0.0f, 0.0f);
        pose.rotate(raise * 30.0f, 0.0f, 0.0f, 1.0f);
        break;
    }
    case UseAnimation::Camera: {
        float const timeHeld =
            (static_cast<float>(type->getMaxUseDuration(&item)) - (static_cast<float>(useDuration) - frameAlpha + 1.0f))
            / 20.0f;
        float const power = std::min(1.0f, (timeHeld * timeHeld + timeHeld * 2.0f) / 3.0f);

        float settle = 1.0f - power;
        settle       = settle * settle * settle;
        settle       = settle * settle * settle;
        settle       = settle * settle * settle;
        float const raise = 1.0f - settle;

        pose.rotate(raise * 45.0f, 0.0f, 1.0f, 0.0f);
        pose.translate(-1.5f, 0.0f, 0.0f);
        break;
    }
    case UseAnimation::GoatHorn:
        pose.rotate(25.0f, 0.0f, 1.0f, 0.0f);
        pose.rotate(32.742f, 1.0f, 0.0f, 0.0f);
        pose.rotate(1.2f, 0.0f, 0.0f, 1.0f);
        pose.translate(-0.405f, 0.023f, 0.344f);
        break;
    case UseAnimation::Brush: {
        float const sinceSwingStart = static_cast<float>(useDuration % kBrushSwingDuration) - frameAlpha + 1.0f;
        float const swipe           = 1.0f - sinceSwingStart / static_cast<float>(kBrushSwingDuration);

        pose.translate(0.28f, 0.196f, -0.184f);
        pose.rotate(-80.0f, 1.0f, 0.0f, 0.0f);
        pose.rotate(90.0f, 0.0f, 1.0f, 0.0f);
        pose.rotate(-15.0f + 75.0f * std::cos(swipe * 4.0f * PI), 1.0f, 0.0f, 0.0f);
        pose.translate(0.0f, 0.452f, 0.608f);
        break;
    }
    default:
        break;
    }
}

// 复刻参考模组 computeHeldItemPose 的无挥动版本（挥动动画未移植，swing 恒 0，
// 对应项全部退化消失）。姿态 = 使用动画 + 持握偏移 + 换手高度 + 45° 转向 +
// 非方块 0.4 缩放 + 方块显示变换 + 镜像贴图 180°。
Matrix computeHeldItemPose(
    ItemInHandRenderer& renderer,
    Player&             player,
    ItemStack const&    item,
    ::Block const*      block,
    bool                useBlockTransforms,
    float               frameAlpha
) {
    int const useDuration = offhands::isUsingOffhandItem(player) ? itemUseDuration(player) : 0;

    Matrix pose = Matrix::IDENTITY();
    if (useDuration > 0) {
        applyUseAnimation(pose, item, useDuration, frameAlpha);
    }

    pose.translate(0.56f, -0.52f, -0.72f);

    float const height =
        renderer.mOldHeightOffHand.get() + (renderer.mHeightOffHand.get() - renderer.mOldHeightOffHand.get()) * frameAlpha;
    pose.translate(0.0f, -(1.0f - height) * 0.6f, 0.0f);

    pose.rotate(45.0f, 0.0f, 1.0f, 0.0f);

    if (!useBlockTransforms) {
        pose.scale(0.4f, 0.4f, 0.4f);
    }

    if (useBlockTransforms && block != nullptr) {
        multInPlace(pose, ::ClientBlockPipeline::ItemTransforms::getFixSpaceTransformMatrix(ItemTransformType::FirstpersonRighthand));
        multInPlace(pose, getItemDisplayTransform(*block, ItemTransformType::FirstpersonRighthand));
    }

    Item const* type = item.mItem.get();
    if (type != nullptr && type->mIsMirroredArt.get() && !useBlockTransforms) {
        pose.rotate(180.0f, 0.0f, 1.0f, 0.0f);
    }

    return pose;
}

LL_TYPE_INSTANCE_HOOK(
    OffhandItemRenderHook,
    HookPriority::Normal,
    ItemInHandRenderer,
    &ItemInHandRenderer::renderOffhandItem,
    void,
    ::BaseActorRenderContext& renderContext,
    ::Player&                 player,
    ::ItemContextFlags        itemFlags
) {
    // 复制一份：后续 HandSwapScope / attachable 作用域可能改动槽位引用。
    ItemStack const item = mOffHandItem.get();

    // 空手、地图/照片与数据驱动 JSON 矩阵物品交由原版处理。
    if (!offhands::hasItem(item) || item.isInstance(::HashedString{"minecraft:filled_map"}, false)
        || item.isInstance(::HashedString{"minecraft:photo_item"}, false) || setsMatrixFromJson(item)) {
        origin(renderContext, player, itemFlags);
        return;
    }

    if (!shouldRenderOffhandItem(*this, player)) {
        return;
    }

    ::Block const* block               = item.getBlockForRendering();
    bool const     useBlockTransforms  = block != nullptr && usesNewTessellation(*block)
        && ::BlockTessellator::canRender(::BlockGraphics::getForBlock(*block)->mBlockShape.get());

    float const  frameAlpha = renderContext.getFrameAlpha(player);
    Matrix const pose       = computeHeldItemPose(*this, player, item, block, useBlockTransforms, frameAlpha);

    // push 返回的 MatrixStackRef 析构时自动出栈。
    auto matrixRef            = renderContext.mScreenContext.camera.worldMatrixStack.get().push(false);
    matrixRef.mat->_m.get()   = matrixRef.mat->_m.get() * mirrored(pose)._m.get();

    // 弓/矛等物品的蓄力帧从使用组件读时长；renderFirstPerson 把它置零了，
    // 自绘期间恢复成真实时长，画完再置零（由 renderFirstPerson 最终恢复）。
    auto component = player.getEntityContext().tryGetComponent<::ItemInUseComponent>();
    if (hidingOffhandUse && component != nullptr) {
        component->mDuration = offhandUseDuration;
    }

    mirroringItemTransforms = true;
    // 平面精灵启发式：非方块、且不是盾/弓/弩/矛这类模型物品。
    mirroringFlatItem = !useBlockTransforms && !isModelUseAnimation(item.mItem.get());
    renderItem(
        renderContext, player, item, false,
        firstPersonItemFlags != ::ItemContextFlags::None ? firstPersonItemFlags : itemFlags,
        useBlockTransforms, true
    );
    mirroringItemTransforms = false;

    if (hidingOffhandUse && component != nullptr) {
        component->mDuration = 0;
    }
}

// 物品自身的显示变换（手持 JSON 变换）在镜像姿态内若不跟着镜像，
// 物品的位置/朝向会错。保存父矩阵 → 置单位矩阵让原版累加变换 →
// 取出结果镜像后乘回父矩阵。
LL_TYPE_INSTANCE_HOOK(
    OffhandDefaultTransformsHook,
    HookPriority::Normal,
    ItemInHandRenderer,
    &ItemInHandRenderer::_applyDefaultItemTransforms,
    void,
    ::MatrixStack::MatrixStackRef& worldMatrix,
    ::ItemStack const&             item,
    bool                           isInHandItem,
    ::BlockType const*             blockType,
    ::BlockShape                   blockShape,
    ::ItemRenderCall const*        renderObjectCall,
    float                          heldItemScale,
    bool                           posAndRotSetByJSON
) {
    if (!mirroringItemTransforms) {
        origin(worldMatrix, item, isInHandItem, blockType, blockShape, renderObjectCall, heldItemScale,
               posAndRotSetByJSON);
        return;
    }

    // MatrixStackRef 的 mat/stack 是 TypedStorage 退化的裸指针，直接用。
    Matrix& top    = *worldMatrix.mat;
    Matrix  parent = top;
    top            = Matrix::IDENTITY();

    origin(worldMatrix, item, isInHandItem, blockType, blockShape, renderObjectCall, heldItemScale,
           posAndRotSetByJSON);

    Matrix const transforms = top;
    top                     = parent;
    Matrix flipped          = mirrored(transforms);
    top._m.get()            = top._m.get() * flipped._m.get();

    // 平面精灵镜像后从背面显示且对角反向，翻回（等价参考模组的 mIsFlat 分支）。
    if (renderObjectCall != nullptr && mirroringFlatItem) {
        top.scale(-1.0f, -1.0f, 1.0f);
    }

    worldMatrix.stack->_isDirty = true;
    mTransform.get()            = top;
}

// minecraft:bow 的 attachable 没有槽位绑定，恒挂在持有者右臂 rightitem 骨骼上；
// 弩/盾/三叉戟绑定各自槽位骨骼，原版即可正确显示副手。
bool attachesToRightItem(ItemStack const& item) { return item.isInstance(::HashedString{"minecraft:bow"}, false); }

// 第一人称里副手弓只会被它的 attachable 画在右臂上（renderFirstPerson 不会为
// 有 attachable 的物品调 renderOffhandItem），需要镜像到左臂。
bool mirrorsOffhandAttachable(ItemStack const& item, ::AttachableSlotIndex slot, ::Actor& actor) {
    return slot == ::AttachableSlotIndex::OffhandItem && firstPersonPlayer != nullptr
        && &actor == static_cast<::Actor*>(firstPersonPlayer) && attachesToRightItem(item);
}

// 弓 attachable 的拉弓动画只读主手（query.main_hand_item_use_duration 与
// query.get_animation_frame 取选中物品）：作用域内交换双手并把使用时长
// 恢复成副手真实值。
class OffhandAttachableScope {
public:
    explicit OffhandAttachableScope(Player& player)
        : mComponent(player.getEntityContext().tryGetComponent<::ItemInUseComponent>())
        , mDuration(mComponent != nullptr ? mComponent->mDuration : 0)
        , mHands(player) {
        if (mComponent != nullptr) {
            mComponent->mDuration = hidingOffhandUse ? offhandUseDuration : 0;
        }
    }

    ~OffhandAttachableScope() {
        if (mComponent != nullptr) {
            mComponent->mDuration = mDuration;
        }
    }

    OffhandAttachableScope(OffhandAttachableScope const&)            = delete;
    OffhandAttachableScope& operator=(OffhandAttachableScope const&) = delete;

private:
    // optional_ref 默认构造为空，可直接与 nullptr 比较。
    decltype(std::declval<Player&>().getEntityContext().tryGetComponent<::ItemInUseComponent>()) mComponent;
    int                      mDuration;
    offhands::HandSwapScope  mHands;
};

LL_TYPE_INSTANCE_HOOK(
    OffhandSetupAttachableHook,
    HookPriority::Normal,
    DataDrivenModel,
    &DataDrivenModel::setupAttachable,
    void,
    ::ItemStack const&           itemInstance,
    ::AttachableSlotIndex const& attachableSlotIndex,
    ::RenderParams&              renderParams,
    ::Actor&                     actor
) {
    if (!mirrorsOffhandAttachable(itemInstance, attachableSlotIndex, actor)) {
        origin(itemInstance, attachableSlotIndex, renderParams, actor);
        return;
    }

    // itemInstance 引用副手槽，交换双手会被覆盖，先复制。
    ItemStack const        item = itemInstance;
    OffhandAttachableScope scope(*firstPersonPlayer);
    origin(item, attachableSlotIndex, renderParams, actor);
}

LL_TYPE_INSTANCE_HOOK(
    OffhandRenderAttachableHook,
    HookPriority::Normal,
    DataDrivenModel,
    &DataDrivenModel::renderAttachable,
    void,
    ::ItemStack const&           itemInstance,
    ::AttachableSlotIndex const& attachableSlotIndex,
    ::RenderParams&              renderParams,
    ::Actor&                     actor
) {
    if (!mirrorsOffhandAttachable(itemInstance, attachableSlotIndex, actor)) {
        origin(itemInstance, attachableSlotIndex, renderParams, actor);
        return;
    }

    ItemStack const item = itemInstance;

    // attachable 绑在右臂骨骼空间，x 镜像后就是左臂，与其他物品的自绘镜像同空间。
    auto& worldMatrix = renderParams.mBaseActorRenderContext->mScreenContext.camera.worldMatrixStack.get();
    {
        auto matrixRef = worldMatrix.push(false);
        matrixRef.mat->scale(-1.0f, 1.0f, 1.0f);
        worldMatrix._isDirty = true;

        OffhandAttachableScope scope(*firstPersonPlayer);
        origin(item, attachableSlotIndex, renderParams, actor);
    }
}

// 原版 renderFirstPerson 假定「使用中物品」一定是主手：副手盾格挡/进食时它会把
// 使用动画套在主手上。把使用组件时长临时置零（主手按常态绘制），画完恢复；
// 同时捕获完整 itemFlags 供 renderOffhandItem 自绘使用。
LL_TYPE_INSTANCE_HOOK(
    MainhandFirstPersonHook,
    HookPriority::Normal,
    ItemInHandRenderer,
    &ItemInHandRenderer::renderFirstPerson,
    void,
    ::BaseActorRenderContext& renderContext,
    ::Matrix const&           prevProj,
    ::ItemContextFlags        itemFlags
) {
    firstPersonItemFlags = itemFlags;

    LocalPlayer* player = mClient.getLocalPlayer();
    if (player == nullptr) {
        origin(renderContext, prevProj, itemFlags);
        firstPersonItemFlags = ::ItemContextFlags::None;
        return;
    }

    auto component = offhands::isUsingOffhandItem(*player)
        ? player->getEntityContext().tryGetComponent<::ItemInUseComponent>()
        : decltype(player->getEntityContext().tryGetComponent<::ItemInUseComponent>()){};
    if (component != nullptr) {
        offhandUseDuration   = component->mDuration;
        hidingOffhandUse     = true;
        component->mDuration = 0;
    }

    firstPersonPlayer = player;
    origin(renderContext, prevProj, itemFlags);
    firstPersonPlayer = nullptr;

    if (component != nullptr) {
        component->mDuration = offhandUseDuration;
        hidingOffhandUse     = false;
    }
    firstPersonItemFlags = ::ItemContextFlags::None;
}

} // namespace

void install() {
    OffhandDefaultTransformsHook::hook();
    OffhandItemRenderHook::hook();
    MainhandFirstPersonHook::hook();
    OffhandSetupAttachableHook::hook();
    OffhandRenderAttachableHook::hook();
}

void uninstall() {
    OffhandRenderAttachableHook::unhook();
    OffhandSetupAttachableHook::unhook();
    MainhandFirstPersonHook::unhook();
    OffhandItemRenderHook::unhook();
    OffhandDefaultTransformsHook::unhook();
}

} // namespace bedrock_edition_deputy::offhand_render

#endif // LL_PLAT_C
