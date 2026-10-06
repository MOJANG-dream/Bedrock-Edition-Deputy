#pragma once

namespace bedrock_edition_deputy::shield_block {

// 鼠标右键按住状态（由 MouseInputEvent 更新，跨线程，原子存储）。
void setRightHeld(bool held);

void install();
void uninstall();

} // namespace bedrock_edition_deputy::shield_block
