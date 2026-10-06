#pragma once

namespace bedrock_edition_deputy {

struct Config {
    int  version                   = 2;
    bool enableSwapKey             = true;  // 启用 F 键交换主副手物品
    bool enableMenuKey             = true;  // 启用 Alt+F 打开配置菜单
    bool prioritizeMainHand        = true;  // 右键使用物品时优先使用主手物品
    bool enableShieldRightClick    = true;  // 盾牌右键格挡（Java 版行为）
    bool disableShieldSneakBlock   = true;  // 蹲下不再触发盾牌格挡（仅持盾时生效）
};

} // namespace bedrock_edition_deputy
