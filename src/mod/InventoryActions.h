#pragma once

namespace bedrock_edition_deputy::inventory_actions {

// HUD 界面下：交换当前选中快捷栏槽与副手（走原版 legacy inventory transaction）。
void swapHotbarOffhand();

// 容器/背包屏幕打开时：将鼠标悬停的玩家物品槽与副手交换。必须在客户端线程调用。
void swapHoveredToOffhand();

// 输入线程查询：当前是否悬停在可送往副手的玩家物品槽上。
bool hasHoveredPlayerSlot();

void install();
void uninstall();

} // namespace bedrock_edition_deputy::inventory_actions
