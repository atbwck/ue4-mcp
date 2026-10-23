# UnrealMCP — UE4.26 修改版

中文 | [English](README.md)

基于 [chongdashu/unreal-mcp](https://github.com/chongdashu/unreal-mcp)（上游提交 `4e5f00da5073`）修改，属于独立派生版本。上游主要面向 UE5，本仓库面向 UE4.26，由 [springkai66](https://github.com/springkai66) 维护，包含 **C++ 编辑器插件与 Python MCP 服务端**。

```text
AI 客户端 → MCP stdio → Python 服务 → TCP 127.0.0.1:55557 → Unreal 编辑器插件
```

## 核心功能与主要命令

本仓库为 **C++ 编辑器插件 + MCP 服务端**，让 AI 客户端驱动 UE4.26：关卡与 Actor 操作、蓝图资产与图表编排、UMG 控件组合、材质、工程设置以及资产属性写入。反射属性写入支持 Vector、Rotator、FIntPoint、数组和 TSet；资产保存带直存回退，失败时返回具体原因而不是静默成功。

- **编辑器与 Actor** — `get_actors_in_level`、`find_actors_by_name`、`spawn_actor`、`spawn_blueprint_actor`、`delete_actor`、`set_actor_transform`、`get_actor_properties`、`set_actor_property`、`focus_viewport`、`duplicate_level`
- **蓝图资产** — `create_blueprint`、`add_component_to_blueprint`、`set_component_property`、`set_static_mesh_properties`、`set_physics_properties`、`set_blueprint_property`、`set_pawn_properties`、`compile_blueprint`
- **蓝图图表** — `add_blueprint_event_node`、`add_blueprint_input_action_node`、`add_blueprint_function_node`、`add_blueprint_variable`、`add_blueprint_get_self_component_reference`、`add_blueprint_self_reference`、`connect_blueprint_nodes`、`find_blueprint_nodes`、`get_blueprint_node_details`、`delete_blueprint_node`
- **图表排版** — `get_blueprint_graph`、`format_blueprint_graph`
- **UMG 控件** — `create_umg_widget_blueprint`、`add_text_block_to_widget`、`add_button_to_widget`、`add_image_to_widget`、`list_widget_blueprints`、`get_widget_tree`、`add_widget_to_widget`、`bind_widget_event`、`set_text_block_binding`、`add_widget_to_viewport`
- **材质** — `create_material`、`create_material_instance`、`get_material_graph`、`list_material_parameters`、`add_material_expression`、`set_material_expression_property`、`delete_material_expression`、`connect_material_expressions`、`disconnect_material_expressions`、`connect_material_property`、`disconnect_material_property`、`set_material_settings`、`add_material_comment`、`format_material_graph`、`set_material_instance_parameter`、`get_material_instance_parameters`、`add_vector_parameter`、`set_material_on_component`
- **工程与资产** — `create_input_mapping`；插件端还实现了 DataTable 行读写（`create_data_table`、`add_row`、`read_row`）与任意资产属性写入（`set_asset_property`），供直接使用 TCP 协议的客户端调用。
- **调试** — `get_output_log`

新增图表与运行态接口包括 `list_blueprint_graphs`、`create_blueprint_graph`、`disconnect_blueprint_nodes`、`set_blueprint_node_property`、`set_blueprint_pin_default`、`start_pie` / `stop_pie` / `get_pie_state`、`take_screenshot`、`create_dynamic_material_instance`、`set_material_parameter`、`get_component_materials`、`refresh_project_reflection`、`compile_project`、`save_assets` 与 `save_level`。节点工具可通过 `graph_name` 操作函数图和宏图。使用方式、保存语义、引擎验证状态及边界见 [蓝图编辑与运行态验证](docs/blueprint-runtime-tools.md)。材质图的读写、实例参数编辑与蓝图排版约定见 [材质编辑与蓝图排版约定](docs/material-editing.md)。

## 快速开始

前置要求：**Windows + UE4.26.2**、对应版本的 C++ 编译工具链，以及 [uv](https://docs.astral.sh/uv/getting-started/installation/)。无需手动安装 Python 依赖：客户端首次启动服务时，uv 会按 `uv.lock` 自动创建环境。

### 第 1 步：安装插件并编译

1. 将整个仓库复制到 `<你的工程>/Plugins/UnrealMCP`，确保 `UnrealMCP.uplugin` 直接位于该目录。纯蓝图工程先添加一个 C++ 类，使工程可以编译插件。
2. 生成工程文件，编译工程的 Editor 目标。
3. 在 **编辑 → 插件** 中启用 **Editor Scripting Utilities** 和 **UnrealMCP**，重启编辑器。
4. 输出日志出现 `UnrealMCPBridge: Server started on 127.0.0.1:55557` 即成功。调用工具时保持编辑器开启；同一时刻只能有一个编辑器占用该端口。

### 第 2 步：接入 MCP 客户端

无需手动启动服务：客户端通过 stdio 自动拉起 Python 服务（`55557` 是插件 TCP 端口，不能填成 HTTP/SSE 地址；本服务不需要 API Key）。

把下面的服务条目合并进客户端配置，`C:/path/to/unreal-mcp/Python` 换成 `Python/` 目录的绝对路径：

```json
{
  "mcpServers": {
    "unreal-mcp": {
      "command": "uv",
      "args": ["--directory", "C:/path/to/unreal-mcp/Python", "run", "--frozen", "unreal_mcp_server.py"]
    }
  }
}
```

写入位置：

| 客户端 | 配置位置 |
| --- | --- |
| **Claude Code** | 一条命令即可：`claude mcp add --scope user unreal-mcp -- uv --directory "C:/path/to/unreal-mcp/Python" run --frozen unreal_mcp_server.py`，用 `/mcp` 查看状态 |
| **Pi** | `~/.pi/agent/mcp.json`（用户级）或项目 `.pi/mcp.json` |
| **Codex** | `~/.codex/config.toml`，TOML 格式见下 |
| **OpenCode** | `opencode.json`，结构见下 |
| **WorkBuddy**（腾讯桌面版） | 侧边栏 **插件 → MCP 服务器 → 配置 MCP**，即 `~/.workbuddy/mcp.json` 或项目 `.workbuddy/mcp.json` |
| **ZCode**（Z.ai 桌面版） | **Settings → MCP Servers → New MCP Server → Full configuration**，粘贴上面的 JSON |

只有两个客户端格式不同：

Codex（`~/.codex/config.toml`）：

```toml
[mcp_servers.unreal-mcp]
command = "uv"
args = ["--directory", "C:/path/to/unreal-mcp/Python", "run", "--frozen", "unreal_mcp_server.py"]
```

OpenCode（`opencode.json`）：

```json
{
  "mcp": {
    "unreal-mcp": {
      "type": "local",
      "command": ["uv", "--directory", "C:/path/to/unreal-mcp/Python", "run", "--frozen", "unreal_mcp_server.py"],
      "enabled": true
    }
  }
}
```

`~` 表示用户主目录（Windows 通常是 `%USERPROFILE%`）。

### 第 3 步：验证

对客户端说：**“使用 unreal-mcp 列出当前关卡中的 Actor。”** 编辑器未开启时也能发现工具，实际操作需要插件已监听。日常顺序：先开编辑器，再从客户端调用。

Python 服务为每条 Unreal 命令输出一条日志，包含完整参数、结果和耗时，失败记录在终端中显示为红色。参数校验失败等未发送请求的情况记录为工具日志。日志仅写 stderr，不占用 MCP 的 stdout。`Python/logs/` 按服务进程记录完整日志（每份 5 MB、保留 3 份历史），文件不含颜色转义。日志目录不可写时只保留该进程的 stderr 输出。

命令及结果直接显示在处理该请求的 Python 服务器主控制台。每条日志在收到执行结果后输出，自动排版等内部命令也分别记录。更新后重启 Python 服务或重新连接 MCP 客户端以加载新代码。

## 蓝图整洁规范

排版由插件实现：未指定坐标的节点自动落位，`compile_blueprint` 会在编译前整理本次会话生成的节点；`get_blueprint_graph` 可读取节点几何，`format_blueprint_graph`（显式 `node_ids` 或 `whole_graph=true`）用于整理已有图表。排版只移动节点，保留连接与行为；循环和共享节点保留并给出警告，不承诺零交叉。

UMG 界面建议先提供示意图或确认文字布局。插件提供 `list_widget_blueprints`、`get_widget_tree` 和 `add_widget_to_widget`，支持原生与项目控件，并校验 Canvas/Box/Overlay/Border/SizeBox 的槽位布局。AI 指导仍要求对齐与分区，机械部分已由插件执行。

## 开发与测试

运行 Python 测试：

```sh
uv --directory "C:/path/to/unreal-mcp/Python" run --frozen python -m unittest discover -s tests
```

Unreal 控制台：`Automation RunTests UnrealMCP.`。`Python/scripts/` 中的手动脚本会创建或修改编辑器内容，应按需执行。Python 接口迁移说明见 [Python/README.md](Python/README.md)。

## 许可

MIT；上游署名及许可声明依据见 [LICENSE](LICENSE) 和 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
