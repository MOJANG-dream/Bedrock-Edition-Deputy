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
#include "mc/deps/renderer/Camera.h"
#include "mc/deps/renderer/MatrixStack.h"
#include "mc/entity/components/ItemInUseComponent.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/ItemStack.h"

// 原版 ItemInHandRenderer::renderOffhandItem 用一套与主手不同的姿态画副手物品，
// 结果就是「模型位置/朝向不对」。这里改成和 Java 版一样：副手用主手的握持姿态
// 镜像到左臂上（S * T * S，S = scale(-1, 1, 1)，保持缠绕方向）。
//
// 这是最小可用版本：只修正位置/朝向与使用抬升，暂不含挥动动画、
// 方块贴图变换（ItemTransforms）与弓/弩的 attachable 特例。

namespace bedrock_edition_deputy::offhand_render {

namespace {

// renderOffhandItem 只收到 renderFirstPerson flags 的一个子集，缺 FirstPersonPass/InHand
// 位时 renderItem 会直接不画（表现为副手模型完全消失）。在 renderFirstPerson 里捕获
// 完整 flags，供 renderOffhandItem 的自绘路径使用。
thread_local ::ItemContextFlags firstPersonItemFlags = ::ItemContextFlags::None;

// renderFirstPerson 期间隐藏副手的使用动画（原版假定「使用中」一定是主手）：
// 把使用组件时长临时置零，主手就会按常态姿态绘制。
thread_local bool hidingOffhandUse   = false;
thread_local int  offhandUseDuration = 0;

// 把姿态沿 x 轴镜像到左手。行列下标约定与 glm 一致：_m[列][行]。
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

    // 空手与地图/照片等交由原版处理，避免丢掉它们的专用绘制路径。
    if (!offhands::hasItem(item) || item.isInstance(::HashedString{"minecraft:filled_map"}, false)
        || item.isInstance(::HashedString{"minecraft:photo_item"}, false)) {
        origin(renderContext, player, itemFlags);
        return;
    }

    bool const useBlockTransforms = item.getBlockForRendering() != nullptr;

    float const frameAlpha = renderContext.getFrameAlpha(player);

    // 主手的握持姿态（不含挥动与方块贴图变换）。
    Matrix pose = Matrix::IDENTITY();
    float const height = mOldHeightOffHand + (mHeightOffHand - mOldHeightOffHand) * frameAlpha;
    pose.translate(0.0f, -(1.0f - height) * 0.6f, 0.0f);
    pose.translate(0.56f, -0.52f, -0.72f);
    pose.rotate(45.0f, 0.0f, 1.0f, 0.0f);
    if (!useBlockTransforms) {
        pose.scale(0.4f, 0.4f, 0.4f);
    }

    mirrorInPlace(pose);

    // push 的结果对象析构时自动出栈。
    auto matrixRef = renderContext.mScreenContext.camera.worldMatrixStack.get().push(false);
    matrixRef.mat->_m.get() = matrixRef.mat->_m.get() * pose._m.get();

    // 与参考模组一致：用 renderFirstPerson 的完整 flags 自绘，否则不渲染。
    ::ItemContextFlags const renderFlags = firstPersonItemFlags != ::ItemContextFlags::None
        ? firstPersonItemFlags
        : itemFlags;
    renderItem(renderContext, player, item, false, renderFlags, useBlockTransforms, true);
}

// 原版 renderFirstPerson 假定「使用中物品」一定是主手：副手盾格挡/进食时它会把
// 使用动画套在主手上。参考模组的做法是把使用组件时长临时置零（渲染主手按常态
// 姿态绘制），画完恢复；同时捕获完整 itemFlags 供 renderOffhandItem 自绘使用。
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
    if (player == nullptr || !offhands::isUsingOffhandItem(*player)) {
        origin(renderContext, prevProj, itemFlags);
        firstPersonItemFlags = ::ItemContextFlags::None;
        return;
    }

    auto component = player->getEntityContext().tryGetComponent<::ItemInUseComponent>();
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
    OffhandItemRenderHook::hook();
    MainhandFirstPersonHook::hook();
}

void uninstall() {
    MainhandFirstPersonHook::unhook();
    OffhandItemRenderHook::unhook();
}

} // namespace bedrock_edition_deputy::offhand_render

#endif // LL_PLAT_C
