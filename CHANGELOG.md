# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [1.0.0] - 2026-10-10

### Added

- 完整副手系统（Java 版语义）：F 键交换主副手物品；副手可直接使用物品（吃喝、弓/弩/三叉戟/矛蓄力、望远镜、钓竿收杆等）。
- 盾牌右键格挡（主手/副手均可），Java 版行为：无需潜行、格挡时减速、主手攻击命中时放下盾、盾牌被禁用后进入冷却。
- 副手槽位 HUD：热键栏左侧显示副手物品图标、耐久条与堆叠数，副手为空时不显示（可在设置中关闭）。
- 第一人称副手物品渲染与使用动画（含弩/三叉戟/矛/望远镜等模型的 JSON display 姿态）；第三人称副手模型与挥臂动画（Molang 变量 + 资源包注入）。
- 背包/容器界面中鼠标悬停物品按 F 直接放入副手（光标持有物品时不触发）。
- Alt+F 图形设置界面：副手交换/盾牌格挡/背包放入副手/副手槽位开关，交换键可重绑定，配置即时写回 `config.json`。
- 双端支持：同时提供 client 与 server 构建（LeviLamina 26.51.6）。

### Fixed

- 主手持盾右键点按秒收、无法格挡：使用槽位被错误改写到空的副手槽位，导致原版每刻收盾。
- 长按 F 键导致物品消失：键盘重复触发改为边沿触发。
- 副手使用钓竿无法收杆、副手使用中点击穿透等问题。

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
