#pragma once

#include <cstdint>

namespace bedrock_edition_deputy::offhand_use {

void install();
void uninstall();

// 请求服务端在下一 tick 替这名玩家结束副手物品的使用。用于两种「客户端已经
// 停止使用、服务端却停在原状态」的情况：松开事务根本没发出（客户端侧发现），
// 或者松开事务到了但原版 handle 失败（服务端侧发现）。
void scheduleServerStop(uint64_t runtimeId);

} // namespace bedrock_edition_deputy::offhand_use
