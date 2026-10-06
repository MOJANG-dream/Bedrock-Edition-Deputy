# Bedrock Edition Deputy

在基岩版中实现 Java 版的副手功能（客户端模组）。

## 功能

- **F 键交换主副手物品**：按下 F 键将当前主手物品与副手物品互换（通过向服务端发送物品栈交换请求实现）。
- **Alt+F 打开配置菜单**：在游戏中显示当前模组配置状态。
- **按键绑定自定义**：模组按键已集成到原版 设置 → 键盘 界面，可自由重映射。
- **右键优先使用主手物品**：右键使用物品时优先使用主手物品（可配置）。
- **盾牌右键格挡**：手持盾牌（主手或副手）时按住右键即可格挡，如同 Java 版（可配置）。
- **蹲下不格挡**：手持盾牌时蹲下不再触发格挡，与 Java 版行为一致（可配置）。

## 使用指南

### 按键

| 按键 | 说明 |
|------|------|
| `F` | 交换主手与副手物品 |
| `Alt+F` | 打开模组配置菜单 |

按键可在 设置 → 键盘 中重新绑定（找到 `Bedrock Edition Deputy` 相关条目）。

### 配置文件

首次加载模组后，将在模组配置目录下生成 `config.json`：

```json
{
    "version": 2,
    "enableSwapKey": true,
    "enableMenuKey": true,
    "prioritizeMainHand": true,
    "enableShieldRightClick": true,
    "disableShieldSneakBlock": true
}
```

- `enableSwapKey`：是否启用 F 键交换主副手物品。
- `enableMenuKey`：是否启用 Alt+F 打开配置菜单。
- `prioritizeMainHand`：右键使用物品时是否优先使用主手物品。
- `enableShieldRightClick`：是否启用盾牌右键格挡。
- `disableShieldSneakBlock`：是否禁用蹲下格挡（仅持盾时生效，关闭后恢复原版蹲下格挡）。

## 构建

1. 安装 [xmake](https://xmake.io/)、Visual Studio（含 C++ 桌面开发）、Clang for Windows。
2. 在项目根目录执行：

```powershell
xmake f -y -p windows -a x64 -m release --target_type=client
xmake
```

3. 构建产物位于 `bin/bedrock-edition-deputy/` 目录。

## 安装

将 `bin/bedrock-edition-deputy/` 目录复制到 LeviLamina 客户端的 `plugins/` 目录下，然后启动客户端即可。

## 贡献

欢迎提交 Issue 与 Pull Request。

## 许可证

MIT © MOJANG-dream，详见 [LICENSE](LICENSE) 文件。
