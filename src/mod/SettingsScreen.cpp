#include "mod/SettingsScreen.h"

#include "mod/Config.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/input/KeyInputEvent.h"
#include "ll/api/event/input/MouseInputEvent.h"
#include "ll/api/event/render/UIRenderEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/IClientInstance.h"
#include "mc/client/game/IMinecraftGame.h"
#include "mc/client/gui/Font.h"
#include "mc/client/gui/CaretMeasureData.h"
#include "mc/client/gui/FontHandle.h"
#include "mc/client/gui/FontRepository.h"
#include "mc/client/gui/GuiData.h"
#include "mc/client/gui/ScreenView.h"
#include "mc/client/gui/TextAlignment.h"
#include "mc/client/gui/TextMeasureData.h"
#include "mc/client/gui/screens/AbstractScene.h"
#include "mc/client/gui/screens/SceneFactory.h"
#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/client/gui/screens/UIScene.h"
#include "mc/client/gui/screens/interfaces/ISceneStack.h"
#include "mc/client/renderer/screen/MinecraftUIRenderContext.h"
#include "mc/deps/core/math/Color.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/input/MouseAction.h"
#include "mc/deps/input/RectangleArea.h"
#include "mc/deps/minecraft_renderer/objects/FrameRenderObject.h"

#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <mutex>
#include <optional>
#include <utility>

// 界面实现方式参考开源客户端模组 Lamium（LGPL-3.0，amatouhake/Lamium）：
// 借用原版 CommonDialogInfoScreen 取得焦点与鼠标独占（无 form id / 网络包 /
// 服务端回调），所有内容在 BeforeUIRenderEvent 中用原生 2D 原语自绘，
// 鼠标点击入队后在渲染帧消费，避免跨线程操作容器/界面。

namespace bedrock_edition_deputy {
Config& modConfig();
}

namespace bedrock_edition_deputy::settings_screen {

namespace {

using namespace ll::event;

constexpr mce::Color kWhite{1.f, 1.f, 1.f, 1.f};

struct Rgb {
    float r, g, b;
};
constexpr Rgb kPanel{0.12f, 0.13f, 0.15f};
constexpr Rgb kFrame{0.36f, 0.37f, 0.40f};
constexpr Rgb kText{0.93f, 0.94f, 0.95f};
constexpr Rgb kDim{0.60f, 0.62f, 0.65f};
constexpr Rgb kAccent{0.42f, 0.76f, 0.30f};
constexpr Rgb kAccentDeep{0.24f, 0.52f, 0.16f};
constexpr Rgb kOff{0.28f, 0.29f, 0.30f};
constexpr Rgb kClose{0.78f, 0.38f, 0.34f};
constexpr Rgb kHover{0.20f, 0.21f, 0.24f};

mce::Color color(Rgb c, float a = 1.f) { return {c.r, c.g, c.b, a}; }

struct RowDef {
    const char* label;
    bool Config::*field;
};
constexpr std::array<RowDef, 6> kRows = {{
    {"F 交换主副手", &Config::enableSwapKey},
    {"Alt+F 打开本界面", &Config::enableMenuKey},
    {"右键优先使用主手", &Config::prioritizeMainHand},
    {"盾牌右键格挡", &Config::enableShieldRightClick},
    {"蹲下时不举盾格挡", &Config::disableShieldSneakBlock},
    {"背包内 F 放入副手", &Config::enableInventoryOffhand},
}};

constexpr float kPanelW   = 210.f;
constexpr float kHeaderH  = 18.f;
constexpr float kRowH     = 15.f;
constexpr float kFooterH  = 17.f;
constexpr float kPad      = 6.f;
constexpr float kPanelH   = kPad + kHeaderH + kRows.size() * kRowH + kFooterH + kPad;
constexpr float kSwitchW  = 18.f;
constexpr float kSwitchH  = 9.f;

struct Geometry {
    float left, top;
    float rowTop(size_t i) const { return top + kPad + kHeaderH + i * kRowH; }
    float footerTop() const { return top + kPad + kHeaderH + kRows.size() * kRowH; }
    bool hitRow(size_t i, float x, float y) const {
        float rt = rowTop(i);
        return x >= left && x < left + kPanelW && y >= rt && y < rt + kRowH;
    }
    bool hitClose(float x, float y) const {
        float ft = footerTop();
        return x >= left + kPad && x < left + kPad + 60.f && y >= ft && y < ft + kFooterH;
    }
};

std::mutex                    gMutex;
IClientInstance*              gClient{nullptr};
std::shared_ptr<AbstractScene> gScene;
std::shared_ptr<AbstractScene> gRetired;
thread_local ScreenView*      tRenderView{nullptr};
bool                          gSeen{false};
bool                          gClosing{false};
std::chrono::steady_clock::time_point gOpenedAt;
std::optional<std::pair<float, float>> gPendingClick;
void (*gSaveCallback)() = nullptr;
float                         gInvScale{1.f};

// ---------------------------------------------------------------------------
// 绘制原语
// ---------------------------------------------------------------------------

Font& defaultFont(MinecraftUIRenderContext& ctx) {
    return ctx.mClient.getMinecraftGame_DEPRECATED()
        .getFontRepository()
        ->getFontFromFontType("default")
        .getFont();
}

void fillRect(MinecraftUIRenderContext& ctx, float x, float y, float w, float h, Rgb rgb, float alpha = 1.f) {
    if (w <= 0 || h <= 0) {
        return;
    }
    ctx.fillRectangle(RectangleArea{x, x + w, y, y + h}, color(rgb, 1.f), alpha);
    ctx.flushImages(kWhite, alpha, HashedString{"ui_fillColor"});
}

void frameRect(MinecraftUIRenderContext& ctx, float x, float y, float w, float h, Rgb rgb, float alpha = 1.f) {
    fillRect(ctx, x, y, w, 1, rgb, alpha);
    fillRect(ctx, x, y + h - 1, w, 1, rgb, alpha);
    fillRect(ctx, x, y + 1, 1, h - 2, rgb, alpha);
    fillRect(ctx, x + w - 1, y + 1, 1, h - 2, rgb, alpha);
}

void drawLabel(
    MinecraftUIRenderContext& ctx,
    float                     x,
    float                     y,
    float                     w,
    std::string               text,
    Rgb                       rgb,
    ::ui::TextAlignment       align = ::ui::TextAlignment::Left,
    float                     size  = 1.f
) {
    auto& font = defaultFont(ctx);
    TextMeasureData const measure{size, 0.f, false, false, false, align};
    CaretMeasureData const caret{-1, false};
    ctx.drawText(
        font,
        RectangleArea{x, x + w, y, y + 14.f * size},
        std::move(text),
        color(rgb),
        1.f,
        align,
        measure,
        caret
    );
    ctx.flushText(0.f, std::nullopt);
}

void drawSwitch(MinecraftUIRenderContext& ctx, float x, float y, bool on) {
    fillRect(ctx, x, y, kSwitchW, kSwitchH, on ? kAccentDeep : kOff);
    frameRect(ctx, x, y, kSwitchW, kSwitchH, on ? kAccent : kFrame);
    float knob = kSwitchH - 2.f;
    float knobX = on ? x + kSwitchW - 1.f - knob : x + 1.f;
    fillRect(ctx, knobX, y + 1.f, knob, knob, on ? Rgb{1, 1, 1} : Rgb{0.82f, 0.82f, 0.83f});
}

// ---------------------------------------------------------------------------
// 场景状态
// ---------------------------------------------------------------------------

bool ownsTop() {
    return gClient && gScene
        && gClient->getSceneFactory().getCurrentSceneStack()->getTopScene() == gScene.get();
}

void clearLocked() {
    gRetired.reset();
    gScene.reset();
    gClient  = nullptr;
    gSeen    = false;
    gClosing = false;
    gPendingClick.reset();
}

Geometry computeGeometry(ScreenView& view) {
    auto size = view.mSize;
    return {(size.x - kPanelW) / 2.f, (size.y - kPanelH) / 2.f};
}

void renderPanel(BeforeUIRenderEvent& event) {
    auto& ctx  = event.uiRenderContext();
    auto& view = event.screenView();
    auto geo   = computeGeometry(view);
    auto& cfg  = modConfig();

    fillRect(ctx, geo.left, geo.top, kPanelW, kPanelH, kPanel, 0.97f);
    frameRect(ctx, geo.left, geo.top, kPanelW, kPanelH, kFrame);

    drawLabel(
        ctx,
        geo.left,
        geo.top + 3.f,
        kPanelW,
        "Bedrock Edition Deputy",
        kText,
        ::ui::TextAlignment::Center
    );

    for (size_t i = 0; i < kRows.size(); ++i) {
        float rt = geo.rowTop(i);
        bool on  = cfg.*(kRows[i].field);
        drawLabel(ctx, geo.left + kPad, rt + 3.f, kPanelW - kSwitchW - kPad * 3.f, kRows[i].label, on ? kText : kDim);
        drawSwitch(ctx, geo.left + kPanelW - kPad - kSwitchW, rt + 3.f, on);
    }

    fillRect(ctx, geo.left + kPad, geo.footerTop(), 60.f, kFooterH - 3.f, kClose, 0.85f);
    frameRect(ctx, geo.left + kPad, geo.footerTop(), 60.f, kFooterH - 3.f, kFrame);
    drawLabel(
        ctx,
        geo.left + kPad,
        geo.footerTop() + 3.f,
        60.f,
        "关闭",
        kText,
        ::ui::TextAlignment::Center
    );
}

void handleClick(float x, float y) {
    if (!gClient) {
        return;
    }
    // 用上一次渲染的几何（面板始终居中，尺寸固定），重建只依赖屏幕尺寸。
    // ScreenView 无法直接取到时，用标准 HUD 设计尺寸即可保持命中一致。
    auto* view = tRenderView;
    Geometry geo{0, 0};
    if (view) {
        geo = computeGeometry(*view);
    } else {
        geo = {(640.f - kPanelW) / 2.f, (380.f - kPanelH) / 2.f};
    }

    for (size_t i = 0; i < kRows.size(); ++i) {
        if (geo.hitRow(i, x, y)) {
            modConfig().*(kRows[i].field) ^= true;
            if (gSaveCallback) {
                gSaveCallback();
            }
            return;
        }
    }
    if (geo.hitClose(x, y)) {
        if (ownsTop() && !gClosing) {
            gClient->getSceneFactory().getCurrentSceneStack()->schedulePopScreen(1);
            gClosing = true;
        }
    }
}

// ---------------------------------------------------------------------------
// Hook：标记我们借用的场景，并保留世界背景
// ---------------------------------------------------------------------------

LL_TYPE_INSTANCE_HOOK(
    SceneRenderHook,
    HookPriority::Normal,
    UIScene,
    &UIScene::$render,
    void,
    ::ScreenContext& screenContext,
    ::FrameRenderObject const& renderObj
) {
    struct Restore {
        ScreenView* previous;
        ~Restore() { tRenderView = previous; }
    } restore{tRenderView};
    {
        std::lock_guard lock(gMutex);
        tRenderView = gScene.get() == this ? mScreenView.get() : nullptr;
    }
    origin(screenContext, renderObj);
}

LL_TYPE_INSTANCE_HOOK(
    SceneExitHook,
    HookPriority::Normal,
    UIScene,
    &UIScene::$onScreenExit,
    void,
    bool                              isPopping,
    bool                              doTransitions,
    std::shared_ptr<::AbstractScene> pushedScene
) {
    bool owned;
    {
        std::lock_guard lock(gMutex);
        owned = gScene.get() == this;
    }
    origin(isPopping, owned ? false : doTransitions, std::move(pushedScene));
    if (owned && isPopping) {
        std::lock_guard lock(gMutex);
        gRetired = std::move(gScene); // 保到下一帧，栈可能仍在引用
        gClient  = nullptr;
        gSeen    = false;
        gClosing = false;
        gPendingClick.reset();
    }
}

LL_TYPE_INSTANCE_HOOK(SceneBackgroundHook, HookPriority::Normal, UIScene, &UIScene::$renderGameBehind, bool) {
    std::lock_guard lock(gMutex);
    if (gScene.get() == this) {
        return true;
    }
    return origin();
}

std::vector<ListenerPtr> gListeners;

} // namespace

void open() {
    auto clientInstance = ll::service::bedrock::getClientInstance();
    if (!clientInstance) {
        return;
    }
    std::lock_guard lock(gMutex);
    if (gScene) {
        return;
    }
    std::string screenName = clientInstance->getScreenName();
    if (screenName.rfind("hud_screen", 0) != 0) {
        return; // 仅在 HUD 下打开，避免与聊天/暂停/其他界面叠加
    }
    auto scene = clientInstance->getSceneFactory().createCommonDialogInfoScreen("Bedrock Edition Deputy", "");
    if (!scene) {
        return;
    }
    gClient   = &*clientInstance;
    gScene    = std::move(scene);
    gSeen     = false;
    gClosing  = false;
    gOpenedAt = std::chrono::steady_clock::now();
    gPendingClick.reset();
    clientInstance->getSceneFactory().getCurrentSceneStack()->pushScreen(gScene, false);
}

void close() {
    std::lock_guard lock(gMutex);
    if (ownsTop()) {
        if (!gClosing) {
            gClient->getSceneFactory().getCurrentSceneStack()->schedulePopScreen(1);
            gClosing = true;
        }
    } else {
        clearLocked();
    }
}

bool isOpen() {
    std::lock_guard lock(gMutex);
    return static_cast<bool>(gScene);
}

void setSaveCallback(void (*callback)()) { gSaveCallback = callback; }

void install() {
    SceneRenderHook::hook();
    SceneExitHook::hook();
    SceneBackgroundHook::hook();

    auto& bus = EventBus::getInstance();
    gListeners.clear();

    gListeners.emplace_back(bus.emplaceListener<BeforeUIRenderEvent>([](BeforeUIRenderEvent& event) {
        std::lock_guard lock(gMutex);
        if (!gScene || tRenderView != &event.screenView()) {
            return;
        }
        event.cancel();
        gSeen = true;
        if (gClient) {
            gInvScale = gClient->getGuiData()->mInvGuiScale;
        }
        if (auto click = std::exchange(gPendingClick, std::nullopt)) {
            handleClick(click->first, click->second);
        }
        if (gScene) {
            renderPanel(event);
        }
    }));

    gListeners.emplace_back(bus.emplaceListener<AfterUIRenderEvent>([](AfterUIRenderEvent& event) {
        std::lock_guard lock(gMutex);
        gRetired.reset();
        if (gScene && &event.uiRenderContext().mClient == gClient && !ownsTop()) {
            if (gSeen || std::chrono::steady_clock::now() - gOpenedAt > std::chrono::seconds(3)) {
                clearLocked();
            }
        }
    }));

    gListeners.emplace_back(bus.emplaceListener<input::MouseInputEvent>([](input::MouseInputEvent& event) {
        std::lock_guard lock(gMutex);
        if (!ownsTop()) {
            return;
        }
        int button = event.actionButtonId();
        if (button == MouseAction::ActionMove || button == MouseAction::ActionMoveRelative) {
            return;
        }
        if (button == MouseAction::ActionWheel) {
            event.cancel(); // 吞掉滚轮，避免误触快捷栏
            return;
        }
        if (event.buttonData() == MouseAction::DataUp) {
            return; // 抬起放行，防止打开前已按下的键卡住
        }
        event.cancel();
        if (button == MouseAction::ActionLeft) {
            float scale = (gInvScale > 0.f && std::isfinite(gInvScale)) ? gInvScale : 1.f;
            gPendingClick = std::make_pair(
                static_cast<float>(event.x()) * scale,
                static_cast<float>(event.y()) * scale
            );
        }
    }));

    gListeners.emplace_back(bus.emplaceListener<input::KeyInputEvent>([](input::KeyInputEvent& event) {
        std::lock_guard lock(gMutex);
        if (!ownsTop()) {
            return;
        }
        if (!event.isDown()) {
            return; // 键位抬起一律放行
        }
        event.cancel();
        int key = event.keyCode();
        if (key == 0x1b || key == 0x0d) { // Esc / Enter 关闭
            if (!gClosing) {
                gClient->getSceneFactory().getCurrentSceneStack()->schedulePopScreen(1);
                gClosing = true;
            }
        }
    }));

    gListeners.emplace_back(bus.emplaceListener<ClientExitLevelEvent>([](ClientExitLevelEvent&) {
        std::lock_guard lock(gMutex);
        clearLocked();
    }));
}

void uninstall() {
    auto& bus = EventBus::getInstance();
    for (auto& listener : gListeners) {
        bus.removeListener(listener);
    }
    gListeners.clear();
    SceneBackgroundHook::unhook();
    SceneExitHook::unhook();
    SceneRenderHook::unhook();
    std::lock_guard lock(gMutex);
    clearLocked();
}

} // namespace bedrock_edition_deputy::settings_screen
