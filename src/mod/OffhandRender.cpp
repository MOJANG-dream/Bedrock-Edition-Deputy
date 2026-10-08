#include "mod/OffhandRender.h"

#include "mod/Offhands.h"

#include "ll/api/memory/Hook.h"

#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/game/ItemInHandRenderer.h"
#include "mc/deps/core/math/Matrix.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/minecraft_renderer/game/ItemContextFlags.h"
#include "mc/deps/renderer/Camera.h"
#include "mc/deps/renderer/MatrixStack.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/ItemStack.h"

namespace bedrock_edition_deputy::offhand_render {

namespace {

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

    if (!offhands::hasItem(item) || item.isInstance(::HashedString{"minecraft:filled_map"}, false)
        || item.isInstance(::HashedString{"minecraft:photo_item"}, false)) {
        origin(renderContext, player, itemFlags);
        return;
    }

    bool const useBlockTransforms = item.getBlockForRendering() != nullptr;

    float const frameAlpha = renderContext.getFrameAlpha(player);

    Matrix pose = Matrix::IDENTITY();
    float const height = mOldHeightOffHand + (mHeightOffHand - mOldHeightOffHand) * frameAlpha;
    pose.translate(0.0f, -(1.0f - height) * 0.6f, 0.0f);
    pose.translate(0.56f, -0.52f, -0.72f);
    pose.rotate(45.0f, 0.0f, 1.0f, 0.0f);
    if (!useBlockTransforms) {
        pose.scale(0.4f, 0.4f, 0.4f);
    }

    mirrorInPlace(pose);

    auto matrixRef = renderContext.mScreenContext.camera.worldMatrixStack.get().push(false);
    matrixRef.mat->_m.get() = matrixRef.mat->_m.get() * pose._m.get();

    renderItem(renderContext, player, item, false, itemFlags, useBlockTransforms, true);
}

} // namespace

void install() { OffhandItemRenderHook::hook(); }

void uninstall() { OffhandItemRenderHook::unhook(); }

} // namespace bedrock_edition_deputy::offhand_render