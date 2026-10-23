# UnrealMCP — UE4.26 fork

[中文](README.zh-CN.md) | English

A modified version of [chongdashu/unreal-mcp](https://github.com/chongdashu/unreal-mcp), based on upstream commit `4e5f00da5073`. This independent fork includes the C++ editor plugin and its Python MCP server in one repository. Maintained by [springkai66](https://github.com/springkai66); upstream originally targets UE5.

```text
AI client → MCP stdio → Python server → TCP 127.0.0.1:55557 → Unreal editor plugin
```

## Core features and main commands

An editor plugin plus MCP server for driving UE4.26 from an AI client: level and actor manipulation, Blueprint asset and graph authoring, UMG widget composition, materials, project settings, and asset property writes. Reflection-based property setting covers Vectors, Rotators, FIntPoint, arrays and TSet; asset saves fall back to a direct package save and report the underlying error instead of silently succeeding.

- **Editor and actors** — `get_actors_in_level`, `find_actors_by_name`, `spawn_actor`, `spawn_blueprint_actor`, `delete_actor`, `set_actor_transform`, `get_actor_properties`, `set_actor_property`, `focus_viewport`, `duplicate_level`
- **Blueprint assets** — `create_blueprint`, `add_component_to_blueprint`, `set_component_property`, `set_static_mesh_properties`, `set_physics_properties`, `set_blueprint_property`, `set_pawn_properties`, `compile_blueprint`
- **Blueprint graph** — `add_blueprint_event_node`, `add_blueprint_input_action_node`, `add_blueprint_function_node`, `add_blueprint_variable`, `add_blueprint_get_self_component_reference`, `add_blueprint_self_reference`, `connect_blueprint_nodes`, `find_blueprint_nodes`, `get_blueprint_node_details`, `delete_blueprint_node`
- **Graph layout** — `get_blueprint_graph`, `format_blueprint_graph`
- **UMG widgets** — `create_umg_widget_blueprint`, `add_text_block_to_widget`, `add_button_to_widget`, `add_image_to_widget`, `list_widget_blueprints`, `get_widget_tree`, `add_widget_to_widget`, `bind_widget_event`, `set_text_block_binding`, `add_widget_to_viewport`
- **Materials** — asset creation and instances (`create_material`, `create_material_instance`), graph read and edit (`get_material_graph`, `list_material_parameters`, `add_material_expression`, `set_material_expression_property`, `delete_material_expression`, `connect_material_expressions`, `disconnect_material_expressions`, `connect_material_property`, `disconnect_material_property`, `set_material_settings`, `add_material_comment`, `format_material_graph`), persistent overrides (`set_material_instance_parameter`, `get_material_instance_parameters`), and component assignment (`add_vector_parameter`, `set_material_on_component`)
- **Project and assets** — `create_input_mapping`; the bridge also implements DataTable row read/write (`create_data_table`, `add_row`, `read_row`) and arbitrary asset property writes (`set_asset_property`) for clients speaking the TCP protocol directly.
- **Debug** — `get_output_log`

Additional tools cover function/macro graph creation and inspection (`list_blueprint_graphs`, `create_blueprint_graph`, optional `graph_name` on node tools), single-wire removal, editable node properties, pin defaults, compiler diagnostics, deferred saves, PIE start/stop/state, viewport screenshots, dynamic material instances and slot/parameter readback. `compile_project` only triggers optional Live Coding; `refresh_project_reflection` refreshes nodes against already loaded native reflection. Deferred saves do not provide rollback or atomic multi-asset commits. See the [capability and validation notes (Chinese)](docs/blueprint-runtime-tools.md) for usage and current engine validation limits, and [material editing and layout conventions (Chinese)](docs/material-editing.md) for the material graph and node layout rules.

## Quick start

Requirements: **UE4.26.2 on Windows**, a C++ toolchain for that engine, and [uv](https://docs.astral.sh/uv/getting-started/installation/). No manual Python setup: uv creates the environment from `uv.lock` the first time the client launches the server.

### Step 1: Install and build the plugin

1. Copy this repository into `<YourProject>/Plugins/UnrealMCP`, so `UnrealMCP.uplugin` is directly inside that folder. For a Blueprint-only project, add a C++ class first to enable project compilation.
2. Generate project files and build your project's Editor target.
3. In **Edit → Plugins**, enable **Editor Scripting Utilities** and **UnrealMCP**, then restart the editor.
4. Success means Output Log shows `UnrealMCPBridge: Server started on 127.0.0.1:55557`. Keep the editor open when calling tools; only one editor can hold the port at a time.

### Step 2: Connect an MCP client

No manual server launch: the client starts the Python server over stdio (`55557` is the plugin's TCP port, not an HTTP/SSE URL; no API key needed).

Merge this entry into your client's config, replacing the path with the absolute path to `Python/`:

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

Where to put it:

| Client | Location |
| --- | --- |
| **Claude Code** | One command: `claude mcp add --scope user unreal-mcp -- uv --directory "C:/path/to/unreal-mcp/Python" run --frozen unreal_mcp_server.py`; check with `/mcp` |
| **Pi** | `~/.pi/agent/mcp.json` (user) or project `.pi/mcp.json` |
| **Codex** | `~/.codex/config.toml`, TOML shape below |
| **OpenCode** | `opencode.json`, shape below |
| **WorkBuddy** (Tencent desktop) | Sidebar **Plugins → MCP Servers → Configure MCP** (Chinese: 插件 → MCP 服务器 → 配置 MCP), i.e. `~/.workbuddy/mcp.json` or project `.workbuddy/mcp.json` |
| **ZCode** (Z.ai desktop) | **Settings → MCP Servers → New MCP Server → Full configuration**, paste the JSON above |

Only two clients use a different shape:

Codex (`~/.codex/config.toml`):

```toml
[mcp_servers.unreal-mcp]
command = "uv"
args = ["--directory", "C:/path/to/unreal-mcp/Python", "run", "--frozen", "unreal_mcp_server.py"]
```

OpenCode (`opencode.json`):

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

`~` is your home directory (usually `%USERPROFILE%` on Windows).

### Step 3: Verify

Ask the client: **"Use unreal-mcp to list actors in the current level."** Tool discovery works without the editor; actual calls need the plugin listener. Daily flow: start the editor first, then call tools from the client.

The Python server logs each Unreal command with its full parameters, result, and duration in one record. Failed records appear red in a terminal. Tool failures before a request is sent get a tool record instead. Console output uses stderr, leaving stdout for MCP. Per-process files under `Python/logs/` contain full logs without color codes (5 MB per file, 3 backups). If that directory is not writable, only that process's stderr remains available.

Commands and results appear directly in the main console of the Python server handling the request. Each record appears after the execution result arrives. Internal commands, including automatic formatting, get their own records. Restart the Python server or reconnect the MCP client after an update to load the new code.

## Blueprint layout rule

The plugin arranges graph layout itself: nodes created without an explicit position are placed automatically, and `compile_blueprint` formats this session's generated nodes before compiling. Use `get_blueprint_graph` to read geometry and `format_blueprint_graph` (explicit `node_ids` or `whole_graph=true`) to tidy existing graphs. Layout moves nodes only; connections and behavior are preserved, cycles and shared nodes are kept with warnings, and no zero-crossing guarantee is made.

For UMG, send the AI a sketch or confirm a textual layout first. The plugin exposes `list_widget_blueprints`, `get_widget_tree` and `add_widget_to_widget` for native and project widgets with validated Canvas/Box/Overlay/Border/SizeBox slot layouts. AI instructions still ask for aligned, separated flows; the plugin now enforces the mechanical parts.

## Development and testing

Run the Python tests:

```sh
uv --directory "C:/path/to/unreal-mcp/Python" run --frozen python -m unittest discover -s tests
```

In Unreal's console: `Automation RunTests UnrealMCP.`. Manual scripts under `Python/scripts/` create or modify editor content; run them deliberately. Python-specific API migration notes are in [Python/README.md](Python/README.md).

## License

MIT; attribution and upstream license declaration are documented in [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
