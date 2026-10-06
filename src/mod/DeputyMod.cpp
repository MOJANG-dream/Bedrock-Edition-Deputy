#include "mod/DeputyMod.h"

#include "Config.h"

#include "ll/api/Config.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/input/KeyInputEvent.h"
#include "ll/api/event/input/MouseInputEvent.h"
#include "ll/api/input/KeyRegistry.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/deps/input/Keyboard.h"
#include "mc/deps/vanilla_components/ActorDataFlagComponent.h"
#include "mc/entity/components/PlayerActionComponent.h"
#include "mc/network/packet/ItemStackRequestPacket.h"
#include "mc/network/packet/ItemStackRequestPacketPayload.h"
#include "mc/network/packet/PlayerAuthInputPacketPayload.h"
#include "mc/network/packet/cerealize/types/item_stack_request_cereal/SlotInfoData.h"
#include "mc/network/packet/cerealize/types/item_stack_request_cereal/SwapActionData.h"
#include "mc/network/packet/item_stack_request_packet_data/RequestData.h"
#include "mc/world/actor/ActorFlags.h"
#include "mc/world/actor/SynchedActorDataEntityWrapper.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/containers/ContainerEnumName.h"
#include "mc/world/inventory/network/ItemStackNetIdVariant.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/ItemStackBase.h"

#include <atomic>
#include <cstdint>

#ifdef LL_PLAT_C
// 客户端 dll 中 ItemStackRequestPacketPayload 的默认构造函数只有声明、没有导出
// （头文件中被 "prevent constructor by default"）。在此提供本模块内定义，
// 使 ItemStackRequestPacket{} 可以链接；= default 会将 mRequests 正常初始化为空 vector，
// 析构仍使用 dll 导出的 MCAPI 析构函数。
ItemStackRequestPacketPayload::ItemStackRequestPacketPayload() = default;
#endif

namespace bedrock_edition_deputy {

namespace {
Config config;
bool   altHeld   = false;
bool   rightHeld = false; // 右键是否按住（MouseAction::ActionRight = 2）
bool   wantBlock = false; // 上一 tick 的目标格挡状态

bool isShield(ItemStack const& stack) {
    return !stack.isNull() && stack.getTypeName() == "minecraft:shield";
}

bool isHoldingShield(Player& player) {
    return isShield(player.getSelectedItem()) || isShield(player.getOffhandSlot());
}

// 直接读写本地玩家的 ActorDataFlagComponent（brstd::bitset<131, uint64>，
// container_ 位于组件对象起始处，每 64 位一个存储字）
void setLocalBlockingFlag(Player& player, bool on) {
    auto& flagComp = *player.mEntityData->mFlagData; // ActorDataFlagComponent&

    constexpr size_t   kBlockIdx   = static_cast<size_t>(ActorFlags::Blocking) / 64;
    constexpr uint64_t kBlockBit   = 1ull << (static_cast<size_t>(ActorFlags::Blocking) % 64);
    constexpr size_t   kTransIdx   = static_cast<size_t>(ActorFlags::TransitionBlocking) / 64;
    constexpr uint64_t kTransBit   = 1ull << (static_cast<size_t>(ActorFlags::TransitionBlocking) % 64);

    auto* words = reinterpret_cast<std::uint64_t*>(&flagComp);
    if (on) {
        words[kBlockIdx] |= kBlockBit;
    } else {
        words[kBlockIdx] &= ~kBlockBit;
        words[kTransIdx] &= ~kTransBit;
    }
}

// 在客户端构建 PlayerAuthInputPacket 时修正输入：
// 1. 右键格挡：伪造 Sneaking 输入，使服务端按原版逻辑判定格挡；
// 2. 蹲下不格挡：持盾时清除 Sneaking 相关输入位，服务端不再推导格挡。
void updateShieldBlocking(PlayerAuthInputPacketPayload& payload) {
    using InputData = PlayerAuthInputPacketPayload::InputData;

    auto clientInstance = ll::service::bedrock::getClientInstance();
    if (!clientInstance) {
        wantBlock = false;
        return;
    }
    auto* player = clientInstance->getLocalPlayer();
    if (!player) {
        wantBlock = false;
        return;
    }

    if (!isHoldingShield(*player)) {
        wantBlock = false;
        return; // 未持盾：完全不干预原版输入
    }

    bool newWantBlock =
        config.enableShieldRightClick && rightHeld && clientInstance->isInGameInputEnabled();

    auto& container   = payload.mInputData.get().mContainer; // brstd::bitset<66>
    bool  suppressSneak = config.disableShieldSneakBlock && !newWantBlock;

    // 潜行边沿模拟：服务端按当前 Sneaking 位 + 边沿事件推导格挡
    if (newWantBlock && !wantBlock) {
        container.set(static_cast<size_t>(InputData::StartSneaking), true);
    } else if (!newWantBlock && wantBlock) {
        container.set(static_cast<size_t>(InputData::StopSneaking), true);
    } else if (suppressSneak) {
        // 蹲下不格挡：清除全部潜行输入位
        container.set(static_cast<size_t>(InputData::SneakDown), false);
        container.set(static_cast<size_t>(InputData::SneakToggleDown), false);
        container.set(static_cast<size_t>(InputData::PersistSneak), false);
        container.set(static_cast<size_t>(InputData::StartSneaking), false);
        container.set(static_cast<size_t>(InputData::StopSneaking), false);
        container.set(static_cast<size_t>(InputData::SneakPressedRaw), false);
        container.set(static_cast<size_t>(InputData::SneakCurrentRaw), false);
        container.set(static_cast<size_t>(InputData::SneakReleasedRaw), false);
    }

    if (newWantBlock || suppressSneak) {
        container.set(static_cast<size_t>(InputData::Sneaking), newWantBlock);
    }

    wantBlock = newWantBlock;

    // 客户端本地格挡标志（立即举盾视觉）
    setLocalBlockingFlag(*player, newWantBlock);
}
} // namespace

// auth input 构建钩子：每次客户端构建玩家输入包时触发
LL_TYPE_INSTANCE_HOOK(
    AuthInputShieldHook,
    HookPriority::Normal,
    PlayerAuthInputPacketPayload,
    &PlayerAuthInputPacketPayload::setFromComponent,
    void,
    ::PlayerActionComponent& input
) {
    origin(input);
    updateShieldBlocking(*this);
}

DeputyMod& DeputyMod::getInstance() {
    static DeputyMod instance;
    return instance;
}

bool DeputyMod::load() {
    auto& logger = getSelf().getLogger();
    logger.debug("Loading...");

    const auto& configFilePath = getSelf().getConfigDir() / "config.json";
    if (!ll::config::loadConfig(config, configFilePath)) {
        logger.warn("Cannot load configurations from {}", configFilePath);
        if (!ll::config::saveConfig(config, configFilePath)) {
            logger.error("Cannot save default configurations to {}", configFilePath);
        }
    }

    return true;
}

void sendSwapPacket() {
    auto clientInstance = ll::service::bedrock::getClientInstance();
    if (!clientInstance) {
        return;
    }

    auto* player = clientInstance->getLocalPlayer();
    if (!player) {
        return;
    }

    int   selectedSlot = player->getSelectedItemSlot();
    auto& mainHandItem = player->getSelectedItem();
    auto& offHandItem  = player->getOffhandSlot();

    // 双手都为空时无需交换
    if (mainHandItem.isNull() && offHandItem.isNull()) {
        return;
    }

    // 构造交换包（payload 默认构造由本文件顶部提供的 = default 定义满足链接）
    ItemStackRequestPacket packet{};
    packet.mSerializationMode = SerializationMode::CerealOnly;

    // 创建请求数据
    ItemStackRequestPacketData::RequestData requestData{};
    // TypedClientNetId::sNextRawId 未从客户端 dll 导出（LNK2019），
    // 使用独立高段自增 id，避免与原版客户端计数器分配的 id 冲突。
    static std::atomic<int> customRequestId{0x40000000};
    requestData.mClientRequestId->mRawId = customRequestId.fetch_add(1, std::memory_order_relaxed);

    // 创建交换动作
    ItemStackRequestCereal::SwapActionData swapAction{};

    // 源：快捷栏槽位
    auto& sourceInfo                      = swapAction.mSource.get();
    sourceInfo.mFullContainerName->mName = ContainerEnumName::HotbarContainer;
    sourceInfo.mSlot                     = static_cast<uchar>(selectedSlot);
    sourceInfo.mNetIdVariant             = mainHandItem.mNetIdVariant.get();

    // 目标：副手槽位
    auto& destInfo                      = swapAction.mDestination.get();
    destInfo.mFullContainerName->mName = ContainerEnumName::OffhandContainer;
    destInfo.mSlot                     = 0;
    destInfo.mNetIdVariant             = offHandItem.mNetIdVariant.get();

    // 添加动作到请求
    requestData.mActions->emplace_back(std::move(swapAction));

    // 添加请求到负载
    packet.mRequests->emplace_back(std::move(requestData));

    // 发送包
    player->sendNetworkPacket(packet);
}

void showConfigMenu() {
    auto clientInstance = ll::service::bedrock::getClientInstance();
    if (!clientInstance) {
        return;
    }

    auto* player = clientInstance->getLocalPlayer();
    if (!player) {
        return;
    }

    player->displayClientMessage("§e=== Bedrock Edition Deputy 配置 ===", std::nullopt);
    player->displayClientMessage(
        std::string("§a交换按键 (F): ") + (config.enableSwapKey ? "§a启用" : "§c禁用"),
        std::nullopt
    );
    player->displayClientMessage(
        std::string("§a菜单按键 (Alt+F): ") + (config.enableMenuKey ? "§a启用" : "§c禁用"),
        std::nullopt
    );
    player->displayClientMessage(
        std::string("§a右键优先主手: ") + (config.prioritizeMainHand ? "§a启用" : "§c禁用"),
        std::nullopt
    );
    player->displayClientMessage(
        std::string("§a盾牌右键格挡: ") + (config.enableShieldRightClick ? "§a启用" : "§c禁用"),
        std::nullopt
    );
    player->displayClientMessage(
        std::string("§a盾牌蹲下格挡: ") + (config.disableShieldSneakBlock ? "§c已禁用" : "§a原版行为"),
        std::nullopt
    );
    player->displayClientMessage("§7在 设置 → 键盘 中可更改按键绑定", std::nullopt);
}

bool DeputyMod::enable() {
    auto& logger = getSelf().getLogger();
    logger.debug("Enabling...");

    // 注册交换按键 (F)
    if (config.enableSwapKey) {
        auto& registry = ll::input::KeyRegistry::getInstance();
        auto& swapKey  = registry.getOrCreateKey("deputy.swap", {Keyboard::F}, true);
        swapKey.registerButtonDownHandler([](FocusImpact, IClientInstance& client) {
            if (!client.isInGameInputEnabled()) {
                return; // 聊天框或其他界面打开时不触发
            }
            sendSwapPacket();
        });
    }

    // 监听 Alt+F 组合键
    if (config.enableMenuKey) {
        auto& bus = ll::event::EventBus::getInstance();
        bus.emplaceListener<ll::event::input::KeyInputEvent>([](ll::event::input::KeyInputEvent& event) {
            if (event.keyCode() == Keyboard::Menu) { // Alt
                altHeld = event.isDown();
            }
            if (event.keyCode() == Keyboard::F && event.isDown()) { // F 按下
                if (altHeld) {
                    auto clientInstance = ll::service::bedrock::getClientInstance();
                    if (clientInstance && clientInstance->isInGameInputEnabled()) {
                        showConfigMenu();
                        event.cancel(); // 阻止 F 触发交换
                    }
                }
            }
        });
    }

    // 鼠标输入：跟踪右键状态 + 右键优先使用主手物品
    {
        auto& bus = ll::event::EventBus::getInstance();
        bus.emplaceListener<ll::event::input::MouseInputEvent>([](ll::event::input::MouseInputEvent& event) {
            // MouseAction::ActionRight = 2, DataDown = 1, DataUp = 0
            if (event.actionButtonId() == 2) {
                rightHeld = (event.buttonData() == 1);
            }

            if (!config.prioritizeMainHand) {
                return;
            }

            // 基岩版客户端右键默认只使用主手选中物品，天然满足"优先使用主手"，
            // 此处仅确保主手持有可用物品时不做额外干预，让原版流程继续使用主手物品。
            if (event.actionButtonId() == 2 && event.buttonData() == 1) {
                auto clientInstance = ll::service::bedrock::getClientInstance();
                if (clientInstance && clientInstance->isInGameInputEnabled()) {
                    auto* player = clientInstance->getLocalPlayer();
                    if (player) {
                        (void)player->getSelectedItem();
                    }
                }
            }
        });
    }

    // 盾牌格挡行为（右键格挡 / 蹲下不格挡）
    if (config.enableShieldRightClick || config.disableShieldSneakBlock) {
        AuthInputShieldHook::hook();
    }

    return true;
}

bool DeputyMod::disable() {
    auto& logger = getSelf().getLogger();
    logger.debug("Disabling...");

    if (config.enableShieldRightClick || config.disableShieldSneakBlock) {
        AuthInputShieldHook::unhook();
    }

    // 复位状态并清除本地格挡标志，避免残留举盾状态
    rightHeld = false;
    if (wantBlock) {
        wantBlock = false;
        auto clientInstance = ll::service::bedrock::getClientInstance();
        if (clientInstance) {
            auto* player = clientInstance->getLocalPlayer();
            if (player) {
                setLocalBlockingFlag(*player, false);
            }
        }
    }

    return true;
}

} // namespace bedrock_edition_deputy

LL_REGISTER_MOD(bedrock_edition_deputy::DeputyMod, bedrock_edition_deputy::DeputyMod::getInstance());
