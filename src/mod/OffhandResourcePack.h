#pragma once

// 模组自带资源包的注册/激活，仅客户端有意义（服务端构建为空操作）。
namespace bedrock_edition_deputy::offhand_resource_pack {

void install();
void uninstall();

} // namespace bedrock_edition_deputy::offhand_resource_pack
