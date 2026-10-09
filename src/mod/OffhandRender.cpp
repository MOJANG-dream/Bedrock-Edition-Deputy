// 仅客户端：副手第一人称渲染依赖客户端渲染器。服务端构建时本文件编译为空。
#ifdef LL_PLAT_C

#include "mod/OffhandRender.h"

#include "mod/Offhands.h"

#include "ll/api/memory/Hook.h"

#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/client/game/IClientInstance.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/game/ItemInHandRenderer.h"
#include "mc/deps/core/math/Matrix.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/minecraft_renderer/game/ItemContextFlags.h"
#include "mc/deps/minecraft_renderer/game/ItemRenderCall.h"
#include "mc/deps/renderer/Camera.h"
#include "mc/deps/renderer/MatrixStack.h"
#include "mc/entity/components/ItemInUseComponent.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/components/ComponentItem.h"
#include "mc/world/level/block/BlockType.h"

#include <algorithm>
#include <cmath>
#include <numbers>

// 第一人称副手渲染，移植自参考模组 FrederoxDev/Offhand 的 OffhandRendering.cpp：
// 1. renderFirstPerson 捕获完整 ItemContextFlags（renderOffhandItem 只收到子集，
//    缺 FirstPersonPass/InHand 位时 renderItem 不画）；
// 2. 副手使用期间把 ItemInUseComponent.mDuration 置零，避免使用动画套在主手上，
//    自绘副手时临时恢复真实时长（进食/喝药水抬升姿态与弓/矛蓄力帧都读它）；
// 3. 自绘副手用镜像主手姿态，并钩住 _applyDefaultItemTransforms 把物品自身的
//    显示变换一起镜像，否则位置/朝向错误；
// 4. 平面精灵在镜像后会从背面显示，需要再 scale(-1,-1,1) 翻回。
//    ItemRenderCall::mIsFlat 在本版 LeviLamina 头文件中未导出（空结构），
//    改用「非方块且非模型类使用动画（盾/弓/弩/矛）」启发式判定平面物品。

namespace bedrock_edition_deputy::offhand_render {

namespace {

using UseAnimation = ::SharedTypes::Legacy::UseAnimation;

constexpr float PI = std::numbers::pi_v<float>;

// renderOffhandItem 只收到 renderFirstPerson flags 的一个子集。
thread_local ::ItemContextFlags firstPersonItemFlags = ::ItemContextFlags::None;

// renderFirstPerson 期间隐藏副手使用时置位。
thread_local bool hidingOffhandUse   = false;
thread_local int  offhandUseDuration = 0;

// _applyDefaultItemTransforms 镜像期间置位。
thread_local bool mirroringItemTransforms = false;
// 本次镜像的副手物品是否是平面精灵（启发式，见文件头说明）。
thread_local bool mirroringFlatItem = false;

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

// 复刻 ItemInHandRenderer::_shouldRenderOffhandItem：主手弓/弩正在使用时
// 不画副手（拉弓动画占据整个第一人称视野）。
bool shouldRenderOffhandItem(ItemInHandRenderer& renderer, Player& player) {
    ItemStack const& main = renderer.mItem.get();
    if (main.isInstance(::HashedString{"minecraft:bow"}, false)
        || main.isInstance(::HashedString{"minecraft:crossbow"}, false)) {
        auto component = player.getEntityContext().tryGetComponent<::ItemInUseComponent>();
        return !(component != nullptr && component->mDuration > 0);
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

// 复刻 ItemInHandRenderer::_applyUseAnimation 的进食/喝药水抬升部分
// （Block 等无姿态变换的动画走默认，盾的举起由 attachable/标志驱动）。
void applyUseAnimation(Matrix& pose, ItemStack const& item, int useDuration, float frameAlpha) {
    Item const*     type = item.mItem.get();
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
        settle        = settle * settle * settle;
        settle        = settle * settle * settle;
        settle        = settle * settle * settle;
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
    default:
        break;
    }
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
    ItemStack const& item = mOffHandItem.get();

    // 空手、地图/照片与数据驱动 JSON 矩阵物品交由原版处理。
    if (!offhands::hasItem(item) || item.isInstance(::HashedString{"minecraft:filled_map"}, false)
        || item.isInstance(::HashedString{"minecraft:photo_item"}, false) || setsMatrixFromJson(item)) {
        origin(renderContext, player, itemFlags);
        return;
    }

    if (!shouldRenderOffhandItem(*this, player)) {
        return;
    }

    bool const useBlockTransforms = item.getBlockForRendering() != nullptr;
    float const frameAlpha         = renderContext.getFrameAlpha(player);

    // 主手握持姿态 + 副手使用时的抬升动画。
    Matrix pose = Matrix::IDENTITY();
    int const useDuration = offhands::isUsingOffhandItem(player) ? itemUseDuration(player) : 0;
    if (useDuration > 0) {
        applyUseAnimation(pose, item, useDuration, frameAlpha);
    }

    float const height = mOldHeightOffHand + (mHeightOffHand - mOldHeightOffHand) * frameAlpha;
    pose.translate(0.56f, -0.52f, -0.72f);
    pose.translate(0.0f, -(1.0f - height) * 0.6f, 0.0f);
    pose.rotate(45.0f, 0.0f, 1.0f, 0.0f);
    if (!useBlockTransforms) {
        pose.scale(0.4f, 0.4f, 0.4f);
    }

    // push 返回的 MatrixStackRef 析构时自动出栈。
    auto matrixRef = renderContext.mScreenContext.camera.worldMatrixStack.get().push(false);
    matrixRef.mat->_m.get() = matrixRef.mat->_m.get() * mirrored(pose)._m.get();

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
    ::ItemStack const&              item,
    bool                            isInHandItem,
    ::BlockType const*              blockType,
    ::BlockShape                    blockShape,
    ::ItemRenderCall const*         renderObjectCall,
    float                           heldItemScale,
    bool                            posAndRotSetByJSON
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

    origin(renderContext, prevProj, itemFlags);

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
}

void uninstall() {
    MainhandFirstPersonHook::unhook();
    OffhandItemRenderHook::unhook();
    OffhandDefaultTransformsHook::unhook();
}

} // namespace bedrock_edition_deputy::offhand_render

#endif // LL_PLAT_C
