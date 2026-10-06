# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- 背包/容器界面中，鼠标悬停在物品上按 F 可直接将其与副手交换（光标持有物品时不触发），对应新配置 `enableInventoryOffhand`。
- Alt+F 改为独立图形设置界面（自绘开关面板，即时生效并写回 `config.json`），替代原聊天框菜单；支持 Esc/Enter 或点击「关闭」退出。
- 盾牌右键格挡现在同步产生举盾减速，且服务端按原版潜行格挡规则进行伤害判定。

### Changed

- F 键主副手交换重写为走原版客户端 legacy inventory transaction（容器 setter + 平衡事务），替代手工构造的 ItemStackRequestPacket，修复服务端不认可交换请求的问题。
- 盾牌输入注入点上移到 `ClientInputUpdateSystem::extractRawHIDInput`，使客户端移动预测与发送的 PlayerAuthInput 一致，修复「有动画但格挡无效」。
- 配置结构升级到 version 3（旧配置文件可直接沿用，新增项取默认值）。

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
