# Upstream attribution / 上游来源

This repository contains modified C++ editor-plugin and Python MCP-server code derived from **chongdashu/unreal-mcp**:

- Upstream / 上游：https://github.com/chongdashu/unreal-mcp
- Reference commit / 参考提交：`4e5f00da5073` (2025-04-22)
- Fork maintainer / 修改版维护者：`springkai66`

本仓库同时包含该项目派生的 C++ 编辑器插件与 Python MCP 服务端；主要修改见 [English README](README.md) / [中文 README](README.zh-CN.md)。参考提交用于标明来源，不表示此仓库保留了上游完整提交历史。

## License declaration / 许可声明

At the inspected upstream commit, the repository had no root `LICENSE` file or source copyright headers. Its README declared MIT through a license badge and a “License: MIT” section. This fork distributes the derived code under MIT based on that declaration; see [LICENSE](LICENSE).

检查的上游版本没有根 `LICENSE` 文件和源码版权头，其 README 通过 MIT 徽章及 License 章节声明许可。本派生版本依据该声明采用 MIT。这里不推测上游未提供的版权年份；上游代码仍归其原作者，维护者仅主张本分支修改部分的权利。

Python dependencies retain their own licenses and are resolved through `Python/pyproject.toml` and `Python/uv.lock`; dependency source and binaries are not vendored here. Unreal Engine is not included and remains subject to Epic's license.

Python 依赖各自遵循其许可，本仓库不打包第三方依赖源码或二进制；Unreal Engine 不包含在仓库中，遵循 Epic 的许可。
