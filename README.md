# Bedrock Edition Deputy

在基岩版中实现 Java 版的副手功能（LeviLamina 原生模组，客户端 + 服务端双端构建）。

## 功能

- **F 键交换主副手物品**：游戏内按 F 将当前手持物品与副手物品互换。交换键通过 LeviLamina KeyRegistry 注册，可在原版「键盘与鼠标」设置中重新绑定。
- **背包内直接放入副手**：打开背包/容器界面，鼠标悬停在任意物品上按 F，即可将该物品与副手交换（光标持有物品时不触发）。
- **副手物品使用**（Java 语义）：右键时若主手物品未消耗这次点击（空手、无法使用的物品或交互失败），自动尝试使用副手物品——进食、喝药水、抛/收钓鱼竿、拉弓、举矛等均可在副手完成；副手使用期间会吞掉后续点击，与 Java 版行为一致。
- **盾牌右键格挡**：盾牌在任意一只手时按住右键即可举盾格挡（点按即生效），减速、动画与服务端伤害判定与原版一致；不再需要潜行触发。
- **副手模型渲染**：第三人称下其他玩家/生物可见副手持物姿态（Molang `variable.offhand_*` 动画驱动），第一人称下自绘副手物品与使用动画，均镜像 Java 版表现。
- **Alt+F 图形配置界面**：游戏内按 Alt+F 打开独立设置窗口，点击开关即时切换功能并保存。

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
    "version": 5,
    "enableSwapKey": true,
    "swapKey": 70,
    "menuKey": 70,
    "enableShieldRightClick": true,
    "enableInventoryOffhand": true
}
```

- `enableSwapKey`：是否启用交换键功能。
- `swapKey`：交换主副手物品的键码（默认 `70`，即 F 键）。
- `menuKey`：配合 Alt 打开图形配置界面的键码（默认 `70`，即 F 键）。
- `enableShieldRightClick`：是否启用盾牌右键格挡。
- `enableInventoryOffhand`：是否启用背包界面悬停物品按 F 送入副手。

## 构建

1. 安装 [xmake](https://xmake.io/)、Visual Studio（含 C++ 桌面开发）、Clang for Windows。
2. 在项目根目录执行（`target_type` 可选 `client` 或 `server`）：

```powershell
xmake f -y -p windows -a x64 -m release --target_type=client
xmake
```

3. 构建产物位于 `bin/bedrock-edition-deputy/` 目录。CI 会为每个提交同时构建客户端与服务端产物。

## 安装

- **lip/tooth 安装**（推荐）：

```powershell
lip install github.com/MOJANG-dream/Bedrock-Edition-Deputy        # 客户端变体
lip install github.com/MOJANG-dream/Bedrock-Edition-Deputy@server # 服务端变体
```

- **手动安装**：从 Releases 下载对应变体的 zip（`Bedrock-Edition-Deputy-client-windows-x64.zip` 或 `-server-`），解压后将 `bedrock-edition-deputy/` 目录放入 LeviLamina 的 `plugins/` 目录（LeviLauncher 用户为版本目录下的 `mods/`）。

依赖 LeviLamina **26.51.6**。

## 致谢

- 副手交换、输入注入与自绘界面的实现方式参考了开源客户端模组 [Lamium](https://github.com/amatouhake/Lamium)（LGPL-3.0）。
- 副手使用、盾牌格挡与副手模型渲染的方案移植自 Amethyst 模组 [FrederoxDev/Offhand](https://github.com/FrederoxDev/Offhand)。

## 贡献

欢迎提交 Issue 与 Pull Request。

## 许可证

MIT © MOJANG-dream，详见 [LICENSE](LICENSE) 文件。
