#pragma once

namespace bedrock_edition_deputy {

struct Config {
    int  version                = 6;
    bool enableSwapKey          = true; // 游戏内按交换键互换主副手物品
    int  swapKey                = 0x46; // 交换键键码（默认 F）
    int  menuKey                = 0x46; // 菜单键键码（固定配合 Alt，默认 Alt+F）
    bool enableShieldRightClick = true; // 盾牌右键格挡（Java 版行为，无需蹲下）
    bool enableInventoryOffhand = true; // 背包界面悬停物品按交换键直接放入副手
    bool enableOffhandSlot      = true; // HUD 热键栏左侧显示副手槽位（Java 版样式）
};

} // namespace bedrock_edition_deputy
