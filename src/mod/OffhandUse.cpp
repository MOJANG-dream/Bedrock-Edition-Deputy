#include "mod/OffhandUse.h"

#include "mod/Offhands.h"

#include "ll/api/memory/Hook.h"
#include "ll/api/mod/NativeMod.h"

#include "mc/deps/shared_types/legacy/item/UseAnimation.h"
#include "mc/entity/components/ItemInUseComponent.h"
#include "mc/legacy/ActorRuntimeID.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/actor/player/PlayerItemInUse.h"
#include "mc/world/gamemode/GameMode.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/ItemStack.h"

#ifdef LL_PLAT_C
#include "mc/client/player/LocalPlayer.h"
#endif

#include <fstream>
#include <string>
#include <unordered_map>

// 副手物品的「使用中」维持逻辑，移植自参考模组的 OffhandItemUse.cpp：
// Player::normalTick 的物品使用块会停掉「物品不在选中槽」的使用。把使用物品的
// count 置 0 只对该检查隐藏物品——isUsingItem/getItemInUse 走 isNull，不看 count，
// 因此使用时长的递减照常进行。origin 之后再按副手槽重放使用块的语义
// （同步物品、进食粒子、完成使用）。

namespace bedrock_edition_deputy::offhand_use {

namespace {

using SharedTypes::Legacy::UseAnimation;

// Player::normalTick 发送进食粒子的剩余时长窗口与间隔。
constexpr int kFeedParticleDuration = 26;
constexpr int kFeedParticleInterval = 4;

void debugLog(std::string const& line) {
    auto mod = ll::mod::NativeMod::current();
    if (!mod) {
        return;
    }
    std::ofstream out(mod->getModDir() / "offhand-debug.log", std::ios::app);
    if (out) {
        out << line << '\n';
    }
}

bool hasFeedingAnimation(UseAnimation animation) {
    return animation == UseAnimation::Eat || animation == UseAnimation::Drink
        || animation == UseAnimation::GlowStick || animation == UseAnimation::Sparkler;
}

// Player::normalTick 物品使用块的副手版本。
void tickOffhandItemInUse(Player& player) {
    PlayerItemInUse& itemInUse   = player.mItemInUse.get();
    ItemStack const& offhandItem = offhands::getItem(player);

    if (!offhands::hasItem(offhandItem)
        || !offhandItem.sameItem(itemInUse.mItem.get(), ItemStackBase::COMPARISONOPTIONS_RELEVANTUSERDATA())) {
        player.stopUsingItem();
        return;
    }

    if (offhandItem.mCount != itemInUse.mItem.get().mCount
        || !offhandItem.matchesItem(itemInUse.mItem.get())) {
        itemInUse.mItem.get() = offhandItem;
    }

    bool const clientSide = player.isClientSide();
    auto const component  = player.getEntityContext().tryGetComponent<ItemInUseComponent>();
    Item const* item      = offhandItem.mItem.get();

    if (clientSide && component != nullptr && component->mDuration < kFeedParticleDuration
        && component->mDuration % kFeedParticleInterval == 0 && hasFeedingAnimation(item->mUseAnim)) {
        player.feed(offhandItem.getIdAux());
    }

    if (component != nullptr && component->mDuration != 0) {
        return;
    }

    if (clientSide && item->isFood()) {
        player.eat(offhandItem);
    }

    offhands::HandSwapScope scope(player);
    player.completeUsingItem();
}

// normalTick 钩子的共同实现：count 置零 → origin → 恢复 → 重放副手使用块语义。
// LocalPlayer 与（经由基类 thunk 链到达的）ServerPlayer 各挂一个钩子调用本函数。
//
// gNormalTickPlayer 防止重复处理：LocalPlayer::normalTick 若内部链调基类
// Player::normalTick，同一玩家的保护逻辑不能跑两遍（进食粒子/完成判定会加倍）。
thread_local void* gNormalTickPlayer = nullptr;

struct NormalTickGuard {
    void* key;
    explicit NormalTickGuard(void* p) : key(p) { gNormalTickPlayer = p; }
    ~NormalTickGuard() { gNormalTickPlayer = nullptr; }
};

template <class PlayerT, class OriginFn>
void runNormalTick(PlayerT& self, OriginFn&& origin) {
    if (!offhands::isUsingOffhandItem(self) || offhands::HandSwapScope::isActive(self)) {
        origin();
        return;
    }

    ItemStack&  itemInUse = self.mItemInUse.get().mItem.get();
    uchar const count     = itemInUse.mCount;
    itemInUse.mCount      = 0;

    origin();

    if (!offhands::isUsingOffhandItem(self)) {
        return;
    }

    itemInUse.mCount = count;
    tickOffhandItemInUse(self);
}

LL_TYPE_INSTANCE_HOOK(
    OffhandItemTickHook,
    HookPriority::Normal,
    Player,
    &Player::$normalTick,
    void
) {
    // 已被外层 LocalPlayer 钩子覆盖（基类链调用）时只透传。
    if (gNormalTickPlayer == static_cast<void const*>(this)) {
        origin();
        return;
    }
    runNormalTick(*this, [&] { origin(); });
}

#ifdef LL_PLAT_C
// 关键：LocalPlayer 覆写了 normalTick（虚函数），挂基类 Player::$normalTick 的 thunk
// 对 LocalPlayer 不触发——客户端的「使用物品不在选中槽」保护此前完全没运行，
// 表现为副手使用被客户端每刻掐掉（矛蓄力几秒即取消、盾牌持续点击穿透）。
LL_TYPE_INSTANCE_HOOK(
    OffhandLocalItemTickHook,
    HookPriority::Normal,
    LocalPlayer,
    &LocalPlayer::$normalTick,
    void
) {
    NormalTickGuard guard(this);
    runNormalTick(*this, [&] { origin(); });
}
#endif

LL_TYPE_INSTANCE_HOOK(
    OffhandReleaseUseHook,
    HookPriority::Normal,
    GameMode,
    &GameMode::$releaseUsingItem,
    void
) {
    Player& player = mPlayer;
    if (offhands::HandSwapScope::isActive(player) || !offhands::isUsingOffhandItem(player)) {
        origin();
        return;
    }

    // 诊断：记录松手时的蓄力时长，区分「玩家主动松手」与「使用被异常取消」。
    auto        component = player.getEntityContext().tryGetComponent<ItemInUseComponent>();
    int const   duration  = component ? component->mDuration : -1;
    Item const* item      = player.mItemInUse.get().mItem.get().mItem.get();
    debugLog(
        std::string("[use] releaseUsingItem local=") + (player.isLocalPlayer() ? "1" : "0")
        + " ecsDur=" + std::to_string(duration)
        + " item=" + (item != nullptr ? item->mFullName->getString() : std::string("-"))
    );
    offhands::HandSwapScope scope(player);
    origin();
}

} // namespace

// 移植参考模组的 OffhandUseSync：服务端每次开始使用物品时记录用的是哪只手；
// 客户端的使用状态被 USINGITEM 标志重启时（标志同步只带「在使用」不带手，
// 原版会从选中槽=主手重启，把手搞错）按记录的手从副手重启。
// 本地整合服与客户端同进程，共用这张表即可，不需要参考模组的自定义数据包。
namespace {

std::unordered_map<uint64_t, bool> gServerOffhandUse;

LL_TYPE_INSTANCE_HOOK(
    OffhandStartUseHook,
    HookPriority::Normal,
    Player,
    &Player::startUsingItem,
    void,
    ::ItemStack const& instance,
    int                duration
) {
    if (!this->isLocalPlayer()) {
        // 服务端侧（含本地整合服的 ServerPlayer 与客户端的 RemotePlayer）。
        origin(instance, duration);
        if (offhands::isUsingItem(*this)) {
            gServerOffhandUse[this->getRuntimeID().rawID] = offhands::HandSwapScope::isActive(*this);
        }
        return;
    }

    // 标志重启传入的是选中槽物品本身的引用；输入发起的使用传入的是副本。
    bool const fromFlag = &instance == &this->getSelectedItem();
    auto const hand     = fromFlag ? gServerOffhandUse.find(this->getRuntimeID().rawID) : gServerOffhandUse.end();
    if (hand == gServerOffhandUse.end() || !hand->second) {
        origin(instance, duration);
        return;
    }

    ItemStack const& offhandItem = offhands::getItem(*this);
    Item const*      offhandType = offhandItem.mItem.get();
    if (!offhands::hasItem(offhandItem) || offhandType == nullptr) {
        return;
    }
    origin(offhandItem, offhandType->getMaxUseDuration(&offhandItem));
    if (offhands::isUsingItem(*this)) {
        offhands::setItemInUseSlotToOffhand(*this);
    }
}

} // namespace

void install() {
    OffhandItemTickHook::hook();
#ifdef LL_PLAT_C
    OffhandLocalItemTickHook::hook();
#endif
    OffhandReleaseUseHook::hook();
    OffhandStartUseHook::hook();
}

void uninstall() {
    OffhandStartUseHook::unhook();
    OffhandReleaseUseHook::unhook();
#ifdef LL_PLAT_C
    OffhandLocalItemTickHook::unhook();
#endif
    OffhandItemTickHook::unhook();
}

} // namespace bedrock_edition_deputy::offhand_use
