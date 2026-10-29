#include "mod/OffhandUse.h"

#include "mod/DebugLog.h"
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

// 物品没有可用的使用时长时的兜底值（Java 版 ShieldItem/TridentItem 的 72000，约 1 小时）。
constexpr int kFallbackUseDuration = 72000;

void debugLog(std::string const& line) { appendDebugLog(line); }

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

    // 注意：不能用 Actor::isClientSide() 区分端——本版头文件里它是 `return !mLevel`
    // 的占位实现，恒为 false（参见 ShieldBlock.cpp 中的同一说明），会让进食粒子与
    // 客户端 eat 预测永远不执行。这里要的正是「本地客户端」这一侧，用 isLocalPlayer()。
    bool const localClient = player.isLocalPlayer();
    auto const component   = player.getEntityContext().tryGetComponent<ItemInUseComponent>();
    Item const* item       = offhandItem.mItem.get();

    if (localClient && component != nullptr && component->mDuration < kFeedParticleDuration
        && component->mDuration % kFeedParticleInterval == 0 && hasFeedingAnimation(item->mUseAnim)) {
        player.feed(offhandItem.getIdAux());
    }

    if (component != nullptr && component->mDuration != 0) {
        return;
    }

    // 只有「真的一直用到时长数完」的物品才需要在这里补一次完成（进食/饮用/发光棒、
    // 弓/三叉戟/矛的满蓄力：原版会走到 completeUsingItem，但使用物品被 count=0 藏了
    // 起来，只能由我们代劳）。
    //
    // 时长本来就是 0 的物品是瞬发使用——原版只是顺手把「使用中」挂上（钓鱼竿抛投就是
    // 这种）。这时绝不能调 completeUsingItem：它会回调物品自己的「松开」逻辑，钓竿
    // 会当场收线、抛投变成「抛出即收回」。直接清掉旧字段，不触发任何物品回调；ECS 的
    // ItemInUseComponent 由 ItemInUseComponentRemoveSystem 异步回收。
    int const maxDuration = item == nullptr ? 0 : item->getMaxUseDuration(&offhandItem);
    if (maxDuration <= 0) {
        itemInUse.mItem.get() = ::ItemStack{};
        return;
    }

    if (localClient && item->isFood()) {
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

    ItemStack&        itemInUse = self.mItemInUse.get().mItem.get();
    Item const* const itemType  = itemInUse.mItem.get();
    uchar const       count     = itemInUse.mCount;
    itemInUse.mCount            = 0;

    // 无论 origin 之后使用状态变成什么样都必须恢复 count：count 为 0 的堆叠在
    // 原版看来就是空物品（isNull/hasItem 都看 count），一旦泄漏到「使用中」的
    // 物品上，手上的物品就会消失。只在堆叠仍是同一个物品时恢复，避免把旧数量
    // 写到 origin 换进来的另一个物品上。
    struct CountRestore {
        ItemStack&  stack;
        Item const* type;
        uchar       count;

        void restore() {
            if (type != nullptr && stack.mItem.get() == type) {
                stack.mCount = count;
            }
            type = nullptr; // 只恢复一次
        }
        ~CountRestore() { restore(); }
    } restore{itemInUse, itemType, count};

    origin();

    // 先恢复再继续：下面的完成判定/物品同步必须看到真实数量。
    restore.restore();

    if (!offhands::isUsingOffhandItem(self)) {
        return;
    }

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

// 「这一次使用用的是哪只手」只在这次使用期间有意义。使用一结束就必须清掉记录，
// 否则同一个 runtimeID 之后的主手使用会读到上一条「副手」记录：标志重启时把
// 选中槽的物品替换成副手物品重新开始使用——表现就是点一下右键，副手盾被举起来
// 之后再也放不下（客户端与服务端的使用对象已经对不上了）。
LL_TYPE_INSTANCE_HOOK(
    OffhandStopUseHook,
    HookPriority::Normal,
    Player,
    &Player::stopUsingItem,
    void
) {
    gServerOffhandUse.erase(this->getRuntimeID().rawID);
    origin();
}

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
        } else {
            // 这次使用根本没成立（时长为 0 等），不能留下指向它的记录。
            gServerOffhandUse.erase(this->getRuntimeID().rawID);
        }
        return;
    }

    // 标志重启传入的是选中槽物品本身的引用；输入发起的使用传入的是副本。
    // 交换作用域里发起的这一次是我们自己的副手轮次（instance 就是副手物品，已经被
    // 换到选中槽），不能再按「副手槽」去重启——那时副手槽里躺着的是被换出去的主手
    // 物品，重启出来的会是错的那一件。
    bool const fromFlag =
        !offhands::HandSwapScope::isActive(*this) && &instance == &this->getSelectedItem();
    auto const hand = fromFlag ? gServerOffhandUse.find(this->getRuntimeID().rawID) : gServerOffhandUse.end();
    if (hand == gServerOffhandUse.end() || !hand->second) {
        origin(instance, duration);
        return;
    }

    ItemStack const& offhandItem = offhands::getItem(*this);
    Item const*      offhandType = offhandItem.mItem.get();
    if (!offhands::hasItem(offhandItem) || offhandType == nullptr) {
        // 副手是空的：没有可以重启的使用，按原版处理。这里必须转交 origin，
        // 否则这次 startUsingItem 被整个吞掉，USINGITEM 标志会指向一个从未
        // 开始的使用状态。
        origin(instance, duration);
        return;
    }
    // 标志里带过来的是「剩余时长」，短促点击时它可能已经是 0。用 0 重启的话这次
    // 使用在客户端会立刻结束，客户端也就再不会发出松开事务，服务端一直停在
    // 「举盾中」——表现正是快速点一下右键后盾牌举起来放不下。时长缺失时退回
    // 物品自身的时长，再退到 Java 版的 72000，保证重启出来的是一个真正在进行
    // 的使用状态。
    int useDuration = duration > 0 ? duration : offhandType->getMaxUseDuration(&offhandItem);
    if (useDuration <= 0) {
        useDuration = kFallbackUseDuration;
    }
    origin(offhandItem, useDuration);
    if (offhands::isUsingItem(*this)) {
        offhands::setItemInUseSlotToOffhand(*this);
    } else {
        // 重启没成立（物品不可用等）：记录失效，别让下一次主手使用再走副手分支。
        gServerOffhandUse.erase(this->getRuntimeID().rawID);
    }
}

} // namespace

void install() {
    OffhandItemTickHook::hook();
#ifdef LL_PLAT_C
    OffhandLocalItemTickHook::hook();
#endif
    OffhandReleaseUseHook::hook();
    // 与 ShieldBlock 的 Player::stopUsingItem 钩子并存（多钩子按优先级串行），
    // 注册失败要能在日志里看到，否则「手别记录」会一直不清。
    reportHook("OffhandUse::Player::stopUsingItem", OffhandStopUseHook::hook());
    OffhandStartUseHook::hook();
}

void uninstall() {
    OffhandStartUseHook::unhook();
    OffhandStopUseHook::unhook();
    OffhandReleaseUseHook::unhook();
#ifdef LL_PLAT_C
    OffhandLocalItemTickHook::unhook();
#endif
    OffhandItemTickHook::unhook();
}

} // namespace bedrock_edition_deputy::offhand_use
