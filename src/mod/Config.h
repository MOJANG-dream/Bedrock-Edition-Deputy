#pragma once

namespace bedrock_edition_deputy {

struct Config {
    int  version                   = 3;
    bool enableSwapKey             = true;  // 游戏内 F 键交换主副手物品
    bool enableMenuKey             = true;  // Alt+F 打开配置界面
    bool prioritizeMainHand        = true;  // 右键使用物品时优先使用主手物品
    bool enableShieldRightClick    = true;  // 盾牌右键格挡（Java 版行为）
    bool disableShieldSneakBlock   = true;  // 蹲下不再触发盾牌格挡（仅持盾）
    bool enableInventoryOffhand    = true;  // 背包界面悬停物品按 F 直接放入副手
};

} // namespace bedrock_edition_deputy
