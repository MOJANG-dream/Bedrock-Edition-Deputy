# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- 背包/容器界面中，鼠标悬停在物品上按 F 可直接将其与副手交换（光标持有物品时不触发），对应新配置 `enableInventoryOffhand`。
- Alt+F 改为独立图形设置界面（自绘开关面板，即时生效并写回 `config.json`），替代原聊天框菜单；支持 Esc/Enter 或点击「关闭」退出。
- 设置界面内可点击键绑定行进入捕获模式，按任意键修改交换键/菜单键快捷键，配置写回 `config.json`。
- 盾牌右键格挡现在同步产生举盾减速，且服务端按原版潜行格挡规则进行伤害判定。

### Fixed

- 修复长按 F 键导致物品消失：将键盘重复触发改为边沿触发，防止连续发送交换请求。
- 修复持盾右键时玩家异常蹲下：不再向本地输入注入潜行状态，改为在发送的 `PlayerAuthInputPacket` 中伪造潜行位，本地视觉不再蹲下。
- 修复举盾时移动速度异常：在 `ClientInputUpdateSystem::extractRawHIDInput` 之后统一缩放原始移动输入，减速行为与 Java 版一致。
- 修复持盾时无法正常使用主手物品/放置方块：移除上游输入注入后，右键交互恢复正常。
- 修复背包界面按 F 无法将物品放入副手：优化副手容器集合扫描逻辑，并增加 debug 日志便于排查。

### Changed

- F 键主副手交换重写为走原版客户端 legacy inventory transaction（容器 setter + 平衡事务），替代手工构造的 ItemStackRequestPacket，修复服务端不认可交换请求的问题。
- 盾牌格挡判定改为在 `PlayerAuthInputPacket` 网络包层伪造潜行位，本地输入系统不再注入潜行状态，消除视觉与速度异常。
- 配置结构升级到 version 4，新增 `swapKey` 与 `menuKey` 键位绑定字段（旧配置文件可直接沿用，新增项取默认值）。

## [0.1.1]

### Added

- 盾牌右键格挡：手持盾牌（主手或副手）时按住右键即可格挡，与 Java 版行为一致。
- 盾牌蹲下格挡禁用：手持盾牌时蹲下不再触发格挡（可在 `config.json` 中通过 `disableShieldSneakBlock` 配置，关闭后恢复原版行为）。
- 配置菜单新增盾牌相关开关显示。

## [0.1.0] - 2026-10-06

### Added

- 初始版本（客户端模组）。
- F 键交换主手与副手物品（通过物品栈交换请求实现）。
- Alt+F 打开模组配置菜单，显示当前配置状态。
- 按键绑定集成到原版 设置 → 键盘 界面，支持自定义重映射。
- 右键使用物品时优先使用主手物品（可在 `config.json` 中配置）。
