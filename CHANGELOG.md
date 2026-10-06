# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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
