// 仅客户端：副手槽位 HUD 依赖客户端 UI 渲染。服务端构建时本文件编译为空。
#ifdef LL_PLAT_C

#include "mod/OffhandHud.h"

#include "mod/Config.h"
#include "mod/Offhands.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/render/UIRenderEvent.h"

#include "mc/client/game/IClientInstance.h"
#include "mc/client/game/IMinecraftGame.h"
#include "mc/client/gui/CaretMeasureData.h"
#include "mc/client/gui/Font.h"
#include "mc/client/gui/FontRepository.h"
#include "mc/client/gui/TextAlignment.h"
#include "mc/client/gui/TextMeasureData.h"
#include "mc/client/gui/controls/UIControl.h"
#include "mc/client/gui/controls/VisualTree.h"
#include "mc/client/gui/screens/ScreenView.h"
#include "mc/client/options/IOptionRegistry.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/actor/ItemRenderer.h"
#include "mc/client/renderer/screen/MinecraftUIRenderContext.h"
#include "mc/deps/core/file/PathView.h"
#include "mc/deps/core/math/Color.h"
#include "mc/deps/core/resource/ResourceLocation.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/input/RectangleArea.h"
#include "mc/deps/minecraft_renderer/renderer/BedrockTextureData.h"
#include "mc/deps/minecraft_renderer/renderer/TexturePtr.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/ItemStack.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>

// 副手槽位 HUD，实现方式参考开源客户端模组 Lamium（LGPL-3.0，amatouhake/Lamium）
// 的 OffhandSlot/InfoHud：
// 1. 游戏 HUD 一帧渲染多个视图，只在根控件名以 .hud_screen 结尾的视图上画；
// 2. 槽位位置跟随本帧热键栏的实际布局（desktop_hotbar / pocket_hotbar /
//    hotbar_panel 控件），放在热键栏左侧、同高同底线；布局不可用或会画出
//    屏幕时不显示；
// 3. 槽位底纹复用热键栏自身的三段贴图（hotbar_start_cap / hotbar_0 /
//    hotbar_end_cap）；
// 4. 物品图标用 ItemRenderer::renderGuiItemNew 画（与背包内图标同一管线），
//    另画耐久条与堆叠数；指南针/时钟按物品动画帧取值。

namespace bedrock_edition_deputy {
Config& modConfig();
}

namespace bedrock_edition_deputy::offhand_hud {

namespace {

constexpr mce::Color kWhite{1.f, 1.f, 1.f, 1.f};

// 热键栏槽位贴图的几何（单位：热键栏高的 1/22）：
// 槽位占 22 单位高、22 单位宽，左右各 1 单位端帽，中间 20 单位槽体；
// 16x16 图标放在槽内偏移 (3,3) 处；副手槽与热键栏间隔 6 单位。
constexpr float kSlotUnits = 22.f;
constexpr float kGapUnits  = 6.f;

struct Box {
    float x, y, w, h;
};

ll::event::ListenerPtr gListener;

// 游戏内 HUD 视图：根控件名以 .hud_screen 结尾。
bool isHudView(::ScreenView const& view) {
    auto const& tree = view.mVisualTree;
    return tree && std::string_view(tree->mRootControlName.get()).ends_with(".hud_screen");
}

// 本帧热键栏的布局盒；控件还没布局完（位置脏标记）时视为不可用。
std::optional<Box> hotbarBox(::ScreenView const& view) {
    auto const& tree = view.mVisualTree;
    if (!tree) {
        return std::nullopt;
    }
    for (char const* name : {"desktop_hotbar", "pocket_hotbar", "hotbar_panel"}) {
        auto control = tree->getControlByName(name, true);
        if (!control) {
            continue;
        }
        if (control->mCachedPositionDirty) {
            return std::nullopt;
        }
        glm::vec2 const position = control->mCachedPosition;
        glm::vec2 const size     = control->mSize;
        return Box{position.x, position.y, size.x, size.y};
    }
    return std::nullopt;
}

// 副手槽：热键栏左侧 6 单位间隔，同高同底线；会画出屏幕时不显示。
std::optional<Box> slotBox(Box hotbar, glm::vec2 screen) {
    if (hotbar.w <= 0 || hotbar.h <= 0 || screen.x <= 0 || screen.y <= 0) {
        return std::nullopt;
    }
    float const unit = hotbar.h / kSlotUnits;
    Box const   slot{hotbar.x - (kGapUnits + kSlotUnits) * unit, hotbar.y, kSlotUnits * unit, kSlotUnits * unit};
    if (slot.x < 0 || slot.y < 0 || slot.y + slot.h > screen.y + 1 || hotbar.x + hotbar.w > screen.x + 1) {
        return std::nullopt;
    }
    return slot;
}

// 16x16 图标的位置：槽内偏移 (3,3)。
Box iconBox(Box slot) {
    float const unit = slot.h / kSlotUnits;
    return {slot.x + 3 * unit, slot.y + 3 * unit, 16 * unit, 16 * unit};
}

void drawTexture(
    ::MinecraftUIRenderContext& ctx,
    char const*                 texture,
    float                       x,
    float                       y,
    float                       w,
    float                       h,
    float                       opacity
) {
    ::mce::TexturePtr const ptr = ctx.getTexture(::ResourceLocation(::Core::PathView(std::string_view(texture))), false);
    std::shared_ptr<::BedrockTextureData const> const& data = ptr.mClientTexture;
    if (!data) {
        return;
    }
    ctx.drawImage(data->mClientTexture.get(), glm::vec2{x, y}, glm::vec2{w, h}, glm::vec2{0, 0}, glm::vec2{1, 1}, false);
    ctx.flushImages(kWhite, std::clamp(opacity, 0.f, 1.f), ::HashedString{"ui_textured_and_glcolor"});
}

void fillRect(::MinecraftUIRenderContext& ctx, float x, float y, float w, float h, mce::Color const& color) {
    if (w <= 0 || h <= 0) {
        return;
    }
    ctx.fillRectangle(::RectangleArea{x, x + w, y, y + h}, color, 1.f);
    ctx.flushImages(kWhite, 1.f, ::HashedString{"ui_fillColor"});
}

// 堆叠数：与热键栏同款——右对齐贴在 18x18 单元格右下角、下移 1 单位，
// 阴影是偏移 1 单位的深色副本（drawText 不读 renderShadow）。
void drawCount(::MinecraftUIRenderContext& ctx, Box icon, float unit, int count) {
    ::Font& font = ctx.mClient.getMinecraftGame_DEPRECATED().getFontRepository()->getFontFromFontType("default").getFont();
    std::string const text = std::to_string(count);
    ::TextMeasureData const measure{unit, 0.f, false, false, false, ::ui::TextAlignment::Right};
    ::CaretMeasureData const caret{-1, false};
    float const right  = icon.x + 17 * unit;
    float const bottom = icon.y + 18 * unit;
    float const width  = 30 * unit; // 足够宽，右对齐贴边即可
    auto        draw   = [&](float offset, mce::Color const& color) {
        ctx.drawText(
            font,
            ::RectangleArea{right - width + offset, right + offset, bottom - 9 * unit + offset, bottom + offset},
            text,
            color,
            1.f,
            ::ui::TextAlignment::Right,
            measure,
            caret
        );
    };
    draw(unit, mce::Color{0.25f, 0.25f, 0.25f, 1.f});
    draw(0.f, kWhite);
    ctx.flushText(0.f, std::nullopt);
}

// 耐久条：原版几何为图标内偏移 (2,13)、宽 13、高 2，颜色绿→红。
void drawDurability(::MinecraftUIRenderContext& ctx, Box icon, float unit, ::ItemStack const& stack) {
    if (!stack.isDamageableItem()) {
        return;
    }
    short const maxDamage = stack.mItem->getMaxDamage();
    short const damage    = stack.getDamageValue();
    if (maxDamage <= 0 || damage <= 0) {
        return;
    }
    float const ratio = 1.f - static_cast<float>(damage) / static_cast<float>(maxDamage);
    float const bx    = icon.x + 2 * unit;
    float const by    = icon.y + 13 * unit;
    fillRect(ctx, bx, by, 13 * unit, 2 * unit, mce::Color{0.f, 0.f, 0.f, 1.f});
    mce::Color const bar{std::min(1.f, 2.f * (1.f - ratio)), std::min(1.f, 2.f * ratio), 0.f, 1.f};
    fillRect(ctx, bx, by, 13 * unit * ratio, 1 * unit, bar);
}

void drawSlot(::MinecraftUIRenderContext& ctx, ::Player& player, Box slot) {
    float const unit = slot.h / kSlotUnits;

    // 槽位底纹：热键栏同款三段贴图（端帽半透明与热键栏两端一致）。
    drawTexture(ctx, "textures/ui/hotbar_start_cap", slot.x, slot.y, unit, slot.h, 0.65f);
    drawTexture(ctx, "textures/ui/hotbar_0", slot.x + unit, slot.y, 20 * unit, slot.h, 1.f);
    drawTexture(ctx, "textures/ui/hotbar_end_cap", slot.x + 21 * unit, slot.y, unit, slot.h, 0.65f);

    ::ItemStack const& source = offhands::getItem(player);
    if (!offhands::hasItem(source)) {
        return;
    }

    // 复制一份本帧专用：渲染器不能重放入手动画。
    ::ItemStack stack     = source;
    stack.mShowPickUp  = false;
    stack.mWasPickedUp = false;

    Box const icon = iconBox(slot);
    if (auto* renderer = ctx.mClient.getItemRenderer()) {
        ::BaseActorRenderContext renderContext(
            ctx.mScreenContext,
            ctx.mClient,
            ctx.mClient.getMinecraftGame_DEPRECATED()
        );
        float const x = std::round(icon.x);
        float const y = std::round(icon.y);
        // 指南针/时钟等与背包槽内一样取动画帧。
        int const frame = stack.mItem->getAnimationFrameFor(&player, false, &stack, true);
        renderer->renderGuiItemNew(renderContext, stack, frame, x, y, false, 1.f, 1.f, unit, 17);
        // 附魔光效与容器预览同参数。
        if (stack.mItem->isGlint(stack)) {
            renderer->renderGuiItemNew(renderContext, stack, frame, x, y, true, 1.35f, 1.f, unit, 17);
        }
    }

    drawDurability(ctx, icon, unit, stack);
    if (stack.mCount > 1) {
        drawCount(ctx, icon, unit, stack.mCount);
    }
}

void onRenderHud(::ll::event::AfterUIRenderEvent& event) {
    if (!modConfig().enableOffhandSlot) {
        return;
    }
    auto&              ctx    = event.uiRenderContext();
    ::ScreenView&      view   = event.screenView();
    ::IClientInstance& client = ctx.mClient;
    // 只在游戏 HUD 顶屏时画（背包/暂停等界面下不画）。
    if (!std::string_view(client.getScreenName()).starts_with("hud_screen") || !isHudView(view)) {
        return;
    }
    if (client.getOptions().getHideHud()) {
        return;
    }
    auto* player = client.getLocalPlayer();
    if (player == nullptr) {
        return;
    }
    auto const hotbar = hotbarBox(view);
    if (!hotbar) {
        return;
    }
    auto const slot = slotBox(*hotbar, glm::vec2(view.mSize));
    if (!slot) {
        return;
    }
    drawSlot(ctx, *player, *slot);
}

} // namespace

void install() {
    auto& bus = ll::event::EventBus::getInstance();
    gListener = bus.emplaceListener<ll::event::AfterUIRenderEvent>(onRenderHud);
}

void uninstall() {
    if (gListener) {
        ll::event::EventBus::getInstance().removeListener(gListener);
        gListener = nullptr;
    }
}

} // namespace bedrock_edition_deputy::offhand_hud

#endif // LL_PLAT_C
