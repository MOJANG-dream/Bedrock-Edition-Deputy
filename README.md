# Bedrock Edition Deputy

在基岩版中实现 Java 版的副手功能（客户端模组）。

## 功能

- **F 键交换主副手物品**：游戏内按 F 将当前手持物品与副手物品互换。移动走原版客户端库存事务（legacy inventory transaction），由游戏自身完成容器写入与网络同步。
- **背包内直接放入副手**：打开背包/容器界面，鼠标悬停在任意物品上按 F，即可将该物品与副手交换（光标持有物品时不触发）。
- **Alt+F 图形配置界面**：游戏内按 Alt+F 打开独立设置窗口，点击开关即可即时切换功能并保存，不再使用聊天框菜单。
- **盾牌右键格挡**：手持盾牌（主手或副手）时按住右键即可举盾格挡，减速、动画与服务端伤害判定均与原版潜行格挡一致；仅副手持盾且主手拿着食物/弓/方块等可右键使用的物品时，优先使用主手物品。
- **蹲下不格挡**：手持盾牌时单纯蹲下不再触发格挡，与 Java 版行为一致（可配置）。
- **右键优先使用主手物品**（可配置）。

## 使用指南

### 按键

| 按键 | 说明 |
|------|------|
| `F` | 游戏内交换主手与副手；背包界面悬停物品时将其送入副手 |
| `Alt+F` | 打开/关闭图形配置界面 |

配置界面也可按 `Esc` 或 `Enter`、点击「关闭」按钮关闭。界面内点击键位绑定行可进入捕获模式，此时按任意键即可修改快捷键（纯修饰键 Esc 取消捕获）。

### 配置文件

首次加载模组后，将在模组配置目录下生成 `config.json`：

```json
{
    "version": 4,
    "enableSwapKey": true,
    "swapKey": 70,
    "enableMenuKey": true,
    "menuKey": 70,
    "prioritizeMainHand": true,
    "enableShieldRightClick": true,
    "disableShieldSneakBlock": true,
    "enableInventoryOffhand": true
}
```

- `enableSwapKey`：是否启用交换键功能。
- `swapKey`：交换主副手物品的键码（默认 `70`，即 F 键）。
- `enableMenuKey`：是否启用图形配置界面。
- `menuKey`：打开图形配置界面的键码（默认 `70`，即 F 键，需配合 Alt 使用）。
- `prioritizeMainHand`：右键使用物品时是否优先使用主手物品。
- `enableShieldRightClick`：是否启用盾牌右键格挡。
- `disableShieldSneakBlock`：是否禁用蹲下格挡（仅持盾时生效，关闭后恢复原版蹲下格挡）。
- `enableInventoryOffhand`：是否启用背包界面悬停物品按 F 送入副手。

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

## 致谢

物品移动、输入注入与自绘界面的实现方式参考了开源客户端模组 [Lamium](https://github.com/amatouhake/Lamium)（LGPL-3.0）。

## 贡献

欢迎提交 Issue 与 Pull Request。

## 许可证

MIT © MOJANG-dream，详见 [LICENSE](LICENSE) 文件。
