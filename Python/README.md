# Unreal MCP

Python bridge that drives the UnrealMCP **editor plugin** in Unreal Engine over the
Model Context Protocol (MCP). The plugin listens on `127.0.0.1:55557`; this server
exposes its commands as MCP tools.

The C++ editor plugin is included in this repository. See the root
[English README](../README.md) or [中文说明](../README.zh-CN.md) for plugin installation
and Claude Code, Codex, Pi, OpenCode, WorkBuddy and ZCode configuration.

## Setup

1. Make sure Python 3.10+ is installed
2. Install `uv` if you haven't already:
   ```bash
   curl -LsSf https://astral.sh/uv/install.sh | sh
   ```
3. Create and activate a virtual environment:
   ```bash
   uv venv
   source .venv/bin/activate  # On Unix/macOS
   # or
   .venv\Scripts\activate     # On Windows
   ```
4. Install dependencies:
   ```bash
   uv pip install -e .
   ```

Then point your MCP client (Claude Desktop, Cursor, Windsurf, ...) at
`unreal_mcp_server.py`. The server connects to the plugin on demand, so the editor
must be running (with the plugin loaded) by the time you call a tool. Starting the
server before the editor is fine — it will simply report a connection failure until
the editor is up.

The server speaks **stdio**, not HTTP.

## Tools

Tools are grouped by plugin command family in `tools/`:

| Module | Covers |
| --- | --- |
| `editor_tools.py` | Actors (list/find/spawn/delete/transform/properties), level duplication |
| `blueprint_tools.py` | Blueprint creation, components, physics, default-object properties |
| `node_tools.py` | Event/function/variable nodes, pin connections |
| `umg_tools.py` | Widget Blueprints, text blocks, buttons, event binding, viewport |
| `project_tools.py` | Input mappings |
| `material_tools.py` | Material graphs, persistent instances, assignment, and runtime parameters |
| `debug_tools.py` | Output log |

## Testing

```bash
.venv/Scripts/python.exe -m unittest discover -s tests -v
```

The tests call registered tool implementations, exercise the shared response
reader and socket lifecycle using fake sockets, and verify initialization, tool
discovery and tool calls through a real stdio subprocess and local fake TCP
editor. They run without Unreal and do not prove live editor behavior.

There are also manual scripts in the [scripts](./scripts) folder that talk to the
plugin over a raw socket. They are useful for driving a live editor, and do not
need the MCP server to be running.

## Console output and logs

The server writes one compact JSON record to its own console after each Unreal
command completes. The record contains the command, parameters, response, elapsed
time, and success or error status. Error records are red in supported terminals.
Windows startup disables QuickEdit to prevent mouse selection from pausing the
server. Redirected output stays plain text.

Console lines go to **stderr** so the stdio JSON channel on stdout stays clean.
Full parameters, plugin responses, per-command traffic and tracebacks go to the
log file, which is per launch (`Python/logs/`) and rotates at 5 MB keeping 3
backups per launch. Older launches are not automatically deleted, so this is
not a cap on the total directory size. If the log directory is not writable the
server warns once and keeps running with console output only.

## Troubleshooting

- Make sure the Unreal editor is loaded and running before calling an editor tool.
- Check the newest file in `Python/logs/` for detailed error information.

## API changes

`spawn_actor` keeps its existing `type` parameter. Three previously ignored
parameters have been removed from the MCP schemas:

- `add_component_to_blueprint.component_properties`: create the component, then
  call `set_component_property` for each property and check each response.
- `set_physics_properties.gravity_enabled`: use `set_component_property` with
  `property_name="bEnableGravity"`.
- `bind_widget_event.function_name`: binding creates a component-bound event node;
  connect its graph pins with the Blueprint node tools.

`set_text_block_binding` supports `binding_type="Text"` only. `focus_viewport`
and `set_pawn_properties` remain disabled in the Python tool registry.

Responses from the plugin use outer `status` and nested `result`; a missing
`result.success` does not mean failure. Dictionary tools report local failures
with `success=False`, while actor listing tools retain their existing empty-list
fallback.

## Development

Command names and parameter names must match what the plugin reads. When adding a
tool, check the plugin's dispatch table and the matching `Handle*` function — a
parameter the plugin never reads is silently ignored. Cross-check against the UE
source when behaviour matters (`FString` comparisons ignore case, for example).
