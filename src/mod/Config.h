#pragma once

namespace bedrock_edition_deputy {

struct Config {
    int  version                 = 4;
    bool enableSwapKey           = true; // 游戏内按交换键互换主副手物品
    int  swapKey                 = 0x46; // 交换键键码（默认 F）
    bool enableMenuKey           = true; // Alt+菜单键打开配置界面
    int  menuKey                 = 0x46; // 菜单键键码（默认 F，即 Alt+F）
    bool prioritizeMainHand      = true; // 右键使用物品时优先使用主手物品
    bool enableShieldRightClick  = true; // 盾牌右键格挡（Java 版行为）
    bool disableShieldSneakBlock = true; // 蹲下不再触发盾牌格挡（仅持盾）
    bool enableInventoryOffhand  = true; // 背包界面悬停物品按交换键直接放入副手
};

} // namespace bedrock_edition_deputy
