#pragma once

class Mob;

// 副手玩家模型（第三人称手臂姿态），移植自参考模组 FrederoxDev/Offhand 的
// OffhandPlayerModel.cpp 与 OffhandSwing.cpp。整个模块仅客户端有意义：
// Molang 变量的读写（MolangVariableMap::get/setMolangVariable）只有客户端导出。
// 服务端构建时 install/uninstall 为空操作。
namespace bedrock_edition_deputy::offhand_player_model {

// 标记 mob 当前这次挥臂来自副手（1.26 原版 Mob::swing 已带 HandSlot，
// 一般情况下无需手动调用；保留给不走 Mob::swing 的副手攻击路径）。
void markOffhandSwing(Mob& mob);

void install();
void uninstall();

} // namespace bedrock_edition_deputy::offhand_player_model
