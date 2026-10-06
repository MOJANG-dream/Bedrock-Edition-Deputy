#pragma once

namespace bedrock_edition_deputy::settings_screen {

// 以下函数均只能在客户端线程调用（输入回调请先投递到 ClientThreadExecutor）。
void open();
void close();
bool isOpen();

// 开关切换后的持久化回调（由模组入口注册，负责写 config.json）。
void setSaveCallback(void (*callback)());

void install();
void uninstall();

} // namespace bedrock_edition_deputy::settings_screen
