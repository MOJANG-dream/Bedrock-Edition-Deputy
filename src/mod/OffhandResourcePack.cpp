#include "mod/OffhandResourcePack.h"

#ifdef LL_PLAT_C

#include "ll/api/memory/Hook.h"
#include "ll/api/mod/NativeMod.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/resource/PackIdVersion.h"
#include "mc/deps/core/resource/PackType.h"
#include "mc/resources/PackInstance.h"
#include "mc/resources/ResourcePackManager.h"
#include "mc/resources/ResourcePackRepository.h"
#include "mc/resources/ResourcePackStack.h"

// 把模组自带的资源包激活到客户端资源包栈顶：
// addCustomResourcePackPath 让仓库扫描到 <模组目录>/packs 下的包；
// _composeFullStack 钩子在每次组栈完成后把我们的 PackInstance 追加到栈顶，
// 从而覆盖同 ID 的动画/动画控制器（基岩版按 ID 合并，栈顶优先）。

namespace bedrock_edition_deputy::offhand_resource_pack {

namespace {

// data/packs/RP/manifest.json 的 header uuid 与版本。
constexpr char kPackIdVersion[] = "3f8c7a2e-5b4d-4e1a-9c6f-2d8a7b1e5f30_1.0.0";

LL_TYPE_INSTANCE_HOOK(
    ComposeFullStackHook,
    HookPriority::Normal,
    ResourcePackManager,
    &ResourcePackManager::_composeFullStack,
    void
) {
    origin();

    auto repo = ll::service::bedrock::getResourcePackRepository(true);
    if (!repo) {
        return;
    }

    // TypedStorage<8,8,unique_ptr<T>> 直接退化为 unique_ptr<T>，get() 按值返回裸指针。
    ResourcePackStack* fullStack = mFullStack.get();
    if (!fullStack) {
        return;
    }

    auto const pack = repo->getResourcePackSatisfiesPackId(PackIdVersion::fromString(kPackIdVersion), true);
    if (!pack) {
        return;
    }

    fullStack->add(PackInstance{pack, 0, false, nullptr}, *repo, false);
}

} // namespace

void install() {
    if (auto repo = ll::service::bedrock::getResourcePackRepository(true)) {
        if (auto mod = ll::mod::NativeMod::current()) {
            repo->addCustomResourcePackPath(mod->getModDir() / "packs", PackType::Resources);
        }
    }
    ComposeFullStackHook::hook();
}

void uninstall() { ComposeFullStackHook::unhook(); }

} // namespace bedrock_edition_deputy::offhand_resource_pack

#else // LL_PLAT_C

namespace bedrock_edition_deputy::offhand_resource_pack {

void install() {}
void uninstall() {}

} // namespace bedrock_edition_deputy::offhand_resource_pack

#endif
