#pragma once

namespace bedrock_edition_deputy {

struct Config {
    int  version                = 7;
    bool enableSwapKey          = true;  // 游戏内按交换键互换主副手物品
    int  swapKey                = 0x46;  // 交换键键码（默认 F）
    int  menuKey                = 0x46;  // 菜单键键码（固定配合 Alt，默认 Alt+F）
    bool enableShieldRightClick = true;  // 盾牌右键格挡（Java 版行为，无需蹲下）
    bool enableInventoryOffhand = true;  // 背包界面悬停物品按交换键直接放入副手
    bool enableOffhandSlot      = true;  // HUD 热键栏左侧显示副手槽位（Java 版样式）
    bool enableDebugLog         = false; // 把诊断日志写入 <模组目录>/offhand-debug.log
};

// 全局配置实例，定义在 DeputyMod.cpp。统一在这里声明，避免各翻译单元
// 各自写一份前向声明导致签名不一致却无人报错。
Config& modConfig();

} // namespace bedrock_edition_deputy
