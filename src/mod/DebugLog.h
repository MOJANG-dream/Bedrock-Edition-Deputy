#pragma once#pragma once

#include <string>
#include <string_view>

// 诊断日志：默认关闭，只有 config.json 里 enableDebugLog 为 true（或模组目录下
// 存在 debug.on 文件）时才写 <模组目录>/offhand-debug.log。诊断语句分散在
// 输入/格挡/交换等热路径上（每刻、每次点击都会走到），因此关闭时必须完全不
// 触碰磁盘。
namespace bedrock_edition_deputy {

bool debugLogEnabled();

// 未启用时立即返回，不做任何 I/O。
void appendDebugLog(std::string_view line);

// 会话开始时清空日志；未启用时不创建文件。
void resetDebugLog();

// 报告一次 Hook 注册结果。参数是 ll::memory::hookEx 的返回码（0 = 成功，
// 非 0 = 失败），不能直接当 bool 用：0 会被当成 false，成功反而记成失败。
// 失败时必须走模组日志（诊断日志默认关闭，只写在那里的话，一版游戏更新后
// 就会表现成「功能静默失效」）。
bool reportHook(std::string_view name, int hookResult);

} // namespace bedrock_edi#pragma once

#include <string>
#include <string_view>

// 诊断日志：默认关闭，只有 config.json 里 enableDebugLog 为 true（或模组目录下
// 存在 debug.on 文件）时才写 <模组目录>/offhand-debug.log。诊断语句分散在
// 输入/格挡/交换等热路径上（每刻、每次点击都会走到），因此关闭时必须完全不
// 触碰磁盘。
namespace bedrock_edition_deputy {

bool debugLogEnabled();

// 未启用时立即返回，不做任何 I/O。
void appendDebugLog(std::string_view line);

// 会话开始时清空日志；未启用时不创建文件。
void resetDebugLog();

// 报告一次 Hook 注册结果。参数是 ll::memory::hookEx 的返回码（0 = 成功，
// 非 0 = 失败），不能直接当 bool 用：0 会被当成 false，成功反而记成失败。
// 失败时必须走模组日志（诊断日志默认关闭，只写在那里的话，一版游戏更新后
// 就会表现成「功能静默失效」）。
bool reportHook(std::string_view name, int hookResult);

} // namespace bedrock_edition_deputy
#pragma oncetion_deputy
#pragma once

#include <string>
#include <string_view>

// 诊断日志：默认关闭，只有 config.json 里 enableDebugLog 为 true 时才写
// <模组目录>/offhand-debug.log。诊断语句分散在输入/格挡/交换等热路径上
// （每刻、每次点击都会走到），因此关闭时必须完全不触碰磁盘。
namespace bedrock_edition_deputy {

bool debugLogEnabled();

// 未启用时立即返回，不做任何 I/O。
void appendDebugLog(std::string_view line);

// 会话开始时清空日志；未启用时不创建文件。
void resetDebugLog();

// 报告一次 Hook 注册结果：失败时必须走模组日志（诊断日志默认关闭，
// 只写在那里的话，一版游戏更新后就会表现成「功能静默失效」）。
bool reportHook(std::string_view name, bool installed);

} // namespace bedrock_edition_deputy
