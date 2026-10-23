"""
Unreal Engine MCP Server

A simple MCP server for interacting with Unreal Engine.
"""

import logging
import logging.handlers
import os
import socket
import sys
import json
import time
from contextlib import asynccontextmanager
from contextvars import ContextVar
from datetime import datetime
from pathlib import Path
from typing import AsyncIterator, Dict, Any, Optional
from mcp.server.fastmcp import FastMCP
from unreal_transport import receive_response
from unreal_console import disable_quick_edit, make_console_handler

# Configuration
UNREAL_HOST = "127.0.0.1"
UNREAL_PORT = 55557

LOG_DIR = Path(__file__).resolve().parent / "logs"
LOG_FORMAT = '%(asctime)s - %(levelname)s - %(message)s'
FILE_FORMAT = '%(asctime)s - %(name)s - %(levelname)s - [%(filename)s:%(lineno)d] - %(message)s'
logger = logging.getLogger("UnrealMCP")
_call_scope: ContextVar[Optional[Dict[str, Any]]] = ContextVar("call_scope", default=None)


def _capture_tool_error(record: logging.LogRecord) -> bool:
    """保留工具捕获后返回空列表的失败，以及文件日志需要的异常堆栈。"""
    if not getattr(record, "console", False):
        scope = _call_scope.get()
        if scope is not None and (record.levelno >= logging.ERROR or (
                record.levelno == logging.WARNING and record.getMessage() in {
                    "Failed to connect to Unreal Engine", "No response from Unreal Engine"})):
            scope["errors"].append(record.getMessage())
        if record.levelno >= logging.ERROR and sys.exc_info()[0] is not None and not record.exc_info:
            record.exc_info = sys.exc_info()
    return True


logger.addFilter(_capture_tool_error)

def configure_logging(log_dir: Path = LOG_DIR) -> Optional[str]:
    """写到 stderr 与按启动区分的日志文件；文件不可写时只保留 stderr。

    stdout 留给 MCP 的 JSON 消息，控制台信息只能走 stderr。
    """
    disable_quick_edit()
    if hasattr(sys.stderr, "reconfigure"):
        sys.stderr.reconfigure(encoding="utf-8", errors="backslashreplace")
    for handler in logger.handlers[:]:
        if getattr(handler, "_unreal_mcp", False):
            logger.removeHandler(handler)
            handler.close()
    logger.setLevel(logging.DEBUG)
    logger.propagate = False

    console = make_console_handler(sys.stderr, LOG_FORMAT)
    console.setLevel(logging.INFO)
    console.addFilter(lambda record: getattr(record, "console", False))
    setattr(console, "_unreal_mcp", True)
    logger.addHandler(console)

    log_file = Path(log_dir).resolve() / f"unreal_mcp_{datetime.now():%Y%m%d_%H%M%S_%f}_{os.getpid()}.log"
    try:
        os.makedirs(log_dir, exist_ok=True)
        file_handler = logging.handlers.RotatingFileHandler(
            log_file, maxBytes=5 * 1024 * 1024, backupCount=3, encoding="utf-8")
    except OSError as error:
        file_path = None
        logger.warning(f"文件日志不可用（{error}），仅输出到控制台", extra={"console": True})
    else:
        file_handler.setLevel(logging.DEBUG)
        file_handler.setFormatter(logging.Formatter(FILE_FORMAT))
        setattr(file_handler, "_unreal_mcp", True)
        logger.addHandler(file_handler)
        file_path = str(log_file)

    logger.info(
        "Unreal MCP Server 已启动\n"
        f"  Python {sys.version.split()[0]}，MCP 通信 stdio，Unreal 地址 {UNREAL_HOST}:{UNREAL_PORT}\n"
        f"  日志文件 {file_path or '不可用（仅控制台）'}\n"
        "  等待 MCP 客户端请求；首次工具调用时连接 Unreal 编辑器\n"
        "  请通过 MCP 客户端使用本服务；手动启动不会提供 HTTP 接口",
        extra={"console": True})
    return file_path


def _response_error(response):
    if isinstance(response, dict):
        for payload in (response, result_payload(response)):
            if payload.get("status") == "error" or payload.get("success") is False:
                return str(payload.get("error") or payload.get("message") or "Unreal 操作失败")
    return None


def _log_event(kind, name, params, response, begin, error=None):
    scope = _call_scope.get()
    if kind == "command" and scope is not None:
        scope["command_records"] += 1
    error = error if error is not None else _response_error(response)
    if kind == "tool" and scope is not None and scope["errors"]:
        error = error or scope["errors"][0]
    if kind == "command" and scope is not None and error is not None:
        scope["command_failed"] = True
    event = {"event": kind, kind: name, "params": params or {}, "response": response,
             "duration": time.monotonic() - begin, "success": error is None, "error": error}
    message = json.dumps(event, ensure_ascii=False, separators=(",", ":"), default=str)
    message = message.translate({code: f"\\u{code:04x}" for code in
                                 (*range(0x7f, 0xa0), 0x2028, 0x2029)})
    logger.log(logging.ERROR if error is not None else logging.INFO,
               "%s", message, extra={"console": True})


def _instrument_tools():
    original = mcp._tool_manager.call_tool

    async def call_tool(name, arguments, context=None):
        begin = time.monotonic()
        scope = {"command_records": 0, "command_failed": False, "errors": []}
        token = _call_scope.set(scope)
        response = None
        failure = None
        logger.debug("工具 %s 参数 %r", name, arguments)
        try:
            response = await original(name, arguments, context=context)
            logger.debug("工具 %s 响应 %r", name, response)
            return response
        except Exception as error:
            failure = str(error)
            logger.debug("工具 %s 异常", name, exc_info=True)
            raise
        finally:
            try:
                if failure is None:
                    failure = _response_error(response) or next(iter(scope["errors"]), None)
                if not scope["command_records"] or (failure is not None and not scope["command_failed"]):
                    _log_event("tool", name, arguments, response, begin, failure)
            finally:
                _call_scope.reset(token)

    mcp._tool_manager.call_tool = call_tool

class UnrealConnection:
    """Connection to an Unreal Engine instance."""

    def __init__(self):
        """Initialize the connection."""
        self.socket = None
        self.connected = False
        self.pending_layout: Dict[tuple[str, str], list[str]] = {}
        # 编辑批次：{(blueprint, graph_name): 批次开始时的全部节点 ID}。
        # 只有能确定「本次新增」时才敢删节点，误删旧节点是不可接受的。
        self.baseline_nodes: Dict[tuple[str, str], set[str]] = {}

    def begin_edit_batch(self, blueprint: str, graph_name: str = "") -> Dict[str, Any]:
        """Snapshot the graph so a later finish call can tell new nodes from old ones."""
        scope = (blueprint, graph_name)
        params: Dict[str, Any] = {"blueprint_name": blueprint}
        if graph_name:
            params["graph_name"] = graph_name
        response = self._send_command("get_blueprint_graph", params)
        payload = result_payload(response)
        nodes = payload.get("nodes")
        if not isinstance(nodes, list):
            return response or {"status": "error", "error": "No response while snapshotting graph"}
        self.baseline_nodes[scope] = {n["node_id"] for n in nodes if isinstance(n, dict) and "node_id" in n}
        return {"status": "success", "result": {"baseline_count": len(self.baseline_nodes[scope])}}

    def diff_new_nodes(self, blueprint: str, graph_name: str = "") -> Dict[str, Any]:
        """Return the node IDs created since begin_edit_batch for this graph."""
        scope = (blueprint, graph_name)
        if scope not in self.baseline_nodes:
            return {"status": "error", "error": f"No edit batch open for {scope}"}
        params: Dict[str, Any] = {"blueprint_name": blueprint}
        if graph_name:
            params["graph_name"] = graph_name
        response = self._send_command("get_blueprint_graph", params)
        nodes = result_payload(response).get("nodes")
        if not isinstance(nodes, list):
            return response or {"status": "error", "error": "No response while reading graph"}
        current = {n["node_id"] for n in nodes if isinstance(n, dict) and "node_id" in n}
        return {"status": "success", "result": {"new_node_ids": sorted(current - self.baseline_nodes[scope])}}

    def connect(self) -> bool:
        """Connect to the Unreal Engine instance."""
        try:
            # Close any existing socket
            if self.socket:
                try:
                    self.socket.close()
                except:
                    pass
                self.socket = None

            logger.debug(f"Connecting to Unreal at {UNREAL_HOST}:{UNREAL_PORT}...")
            self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.socket.settimeout(30)  # 30s: some UE ops (compile/asset load) exceed 5s

            # Set socket options for better stability
            self.socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)

            # Set larger buffer sizes
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536)
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 65536)

            self.socket.connect((UNREAL_HOST, UNREAL_PORT))
            self.connected = True
            logger.debug("Connected to Unreal Engine")
            return True

        except Exception as e:
            logger.error(f"连接 Unreal 失败：{e} 请确认编辑器已启动并启用了 UnrealMCP 插件")
            self.disconnect()
            return False

    def disconnect(self):
        """Disconnect from the Unreal Engine instance."""
        if self.socket:
            try:
                self.socket.close()
            except:
                pass
        self.socket = None
        self.connected = False

    def receive_full_response(self, sock, buffer_size=4096) -> bytes:
        """接收完整响应，使用服务和脚本共用的字节接收逻辑。"""
        return receive_response(sock, buffer_size)

    def send_command(self, command: str, params: Optional[Dict[str, Any]] = None) -> Optional[Dict[str, Any]]:
        """生成结束编译前，仅整理本轮未显式定位的节点。"""
        parameters = params or {}
        blueprint = parameters.get("blueprint_name", "")
        scope = (blueprint, parameters.get("graph_name", ""))
        if command == "compile_blueprint":
            for pending_scope in list(self.pending_layout):
                if pending_scope[0] != blueprint:
                    continue
                layout_params = {
                    "blueprint_name": blueprint,
                    "mode": "selection",
                    "node_ids": list(self.pending_layout[pending_scope]),
                    # Formatting is preparation; only the final successful compile may save.
                    "defer_save": True,
                }
                if pending_scope[1]:
                    layout_params["graph_name"] = pending_scope[1]
                layout = self._send_command("format_blueprint_graph", layout_params)
                if not layout or layout.get("status") != "success":
                    return layout or {"status": "error", "error": "No response while formatting graph"}
                del self.pending_layout[pending_scope]
        response = self._send_command(command, parameters)
        if command == "delete_blueprint_node" and response:
            # A GUID identifies a node independently of graph-name aliases. A save
            # failure can still confirm deletion in memory via deleted_node_id.
            deleted = result_payload(response).get("deleted_node_id")
            if response.get("status") == "success":
                deleted = deleted or parameters.get("node_id")
            if isinstance(deleted, str):
                for pending_scope in list(self.pending_layout):
                    if pending_scope[0] == blueprint:
                        remaining = [node for node in self.pending_layout[pending_scope] if node != deleted]
                        if remaining:
                            self.pending_layout[pending_scope] = remaining
                        else:
                            del self.pending_layout[pending_scope]
        if response and response.get("status") == "success":
            node_commands = {"add_blueprint_event_node", "add_blueprint_function_node",
                             "add_blueprint_input_action_node", "add_blueprint_self_reference",
                             "add_blueprint_get_self_component_reference", "add_blueprint_cast_node",
                             "add_blueprint_external_member_get_node"}
            if command in node_commands and "node_position" not in parameters and not result_payload(response).get("already_exists"):
                node_id = result_payload(response).get("node_id")
                if isinstance(node_id, str) and blueprint:
                    ids = self.pending_layout.setdefault(scope, [])
                    if node_id not in ids:
                        ids.append(node_id)
            if command == "format_blueprint_graph":
                ids = parameters.get("node_ids", [])
                if parameters.get("whole_graph"):
                    self.pending_layout.pop(scope, None)
                elif scope in self.pending_layout:
                    remaining = [node for node in self.pending_layout[scope] if node not in ids]
                    if remaining:
                        self.pending_layout[scope] = remaining
                    else:
                        del self.pending_layout[scope]
        return response

    def _send_command(self, command: str, params: Optional[Dict[str, Any]] = None) -> Optional[Dict[str, Any]]:
        """发送一次命令；失败后不重发可能已经执行的编辑器操作。"""
        begin = time.monotonic()
        raw_response = None
        failure = None
        token = None
        if _call_scope.get() is None:
            token = _call_scope.set({"command_records": 0, "command_failed": False, "errors": []})
        try:
            if not self.connected and not self.connect():
                errors = _call_scope.get()["errors"]
                failure = errors[0] if errors else "Failed to connect to Unreal Engine"
                return None
            sock = self.socket
            if sock is None:
                raise ConnectionError("Unreal socket is not connected")
            command_json = json.dumps({"type": command, "params": params or {}})
            logger.debug(f"Unreal 命令 {command} 参数 {json.dumps(params or {}, ensure_ascii=False)}")
            sock.sendall(command_json.encode("utf-8"))
            raw_response = json.loads(self.receive_full_response(sock))
            logger.debug(f"Unreal 命令 {command} 响应 {raw_response}")
            response = raw_response.copy() if isinstance(raw_response, dict) else raw_response
            if response.get("status") == "error":
                response.setdefault("error", response.get("message", "Unknown Unreal error"))
            elif response.get("success") is False:
                response = {"status": "error", "error": response.get("error") or
                            response.get("message", "Unknown Unreal error")}
            return response
        except Exception as error:
            failure = str(error)
            logger.error(f"Unreal 命令 {command} 失败：{error}")
            return {"status": "error", "error": str(error)}
        finally:
            try:
                self.disconnect()
                _log_event("command", command, params, raw_response, begin, failure)
            finally:
                if token is not None:
                    _call_scope.reset(token)

# Global connection state
_unreal_connection: Optional[UnrealConnection] = None

def get_unreal_connection() -> Optional[UnrealConnection]:
    """Get the connection to Unreal Engine."""
    global _unreal_connection
    try:
        if _unreal_connection is None:
            _unreal_connection = UnrealConnection()
        if not _unreal_connection.connected:
            if not _unreal_connection.connect():
                return None

        return _unreal_connection
    except Exception as e:
        logger.error(f"Error getting Unreal connection: {e}")
        return None

def no_connection() -> Dict[str, Any]:
    """Log and return the standard "could not connect" result.

    Kept separate from the "no response" result, because callers distinguish
    "the editor is not reachable" from "the command returned nothing".
    """
    logger.error("Failed to connect to Unreal Engine")
    return {"success": False, "message": "Failed to connect to Unreal Engine"}

def result_payload(response: Optional[Dict[str, Any]]) -> Dict[str, Any]:
    """Return the inner payload of a bridged response.

    The plugin wraps every reply as {"status": ..., "result": {...}}, so a
    tool that needs a field from the payload must unwrap it first.
    """
    if not response:
        return {}
    payload = response.get("result")
    return payload if isinstance(payload, dict) else {}

@asynccontextmanager
async def server_lifespan(server: FastMCP) -> AsyncIterator[Dict[str, Any]]:
    """Handle server startup and shutdown."""
    global _unreal_connection
    configure_logging()
    # 首个工具调用时再建立连接，避免启动阶段留下空闲或失效连接
    try:
        yield {}
    finally:
        if _unreal_connection:
            _unreal_connection.disconnect()
            _unreal_connection = None
        logger.info("Unreal MCP Server 已关闭，连接已释放", extra={"console": True})

# 连接初始化和 info 提示词共用同一份蓝图整洁规范
BLUEPRINT_LAYOUT_RULES = """Blueprint layout requirements:
- Plan positions before creating nodes. Use node_position where supported; arrange execution left to right, align each execution chain horizontally, and keep consistent spacing on the editor grid.
- Place each event or function flow in its own area, with related data nodes close to their consumers. Avoid overlapping nodes, crossing wires, backward wires and long wires between separate flows.
- Do not funnel unrelated event/function execution chains into the same operation node merely to reuse it. Use separate call nodes for separate flows; when logic genuinely belongs together, prefer a shared function with a separate call node in each flow if supported.
- Preserve valid branch convergence within a related flow, shared data nodes and execution semantics. Place branches on separate rows; never split or duplicate stateful nodes or introduce redundant logic solely for layout.
- compile_blueprint automatically formats nodes created without explicit positions in this server session. For existing graphs, inspect get_blueprint_graph and use format_blueprint_graph with explicit node_ids or whole_graph=true. Report measured/estimated geometry separately from visual inspection.
UMG design workflow:
- Before creating a complete interface, ask for a sketch/screenshot and target resolution. A sketch is preferred, not mandatory: if unavailable, confirm a textual layout plan before creating widgets. Do not repeatedly ask for a sketch for a single widget or a small edit.
- Honor user-specified widget types first. Otherwise inspect list_widget_blueprints and get_widget_tree, recommend project controls based on observed usage and ask the user when candidates are ambiguous. Never infer popularity from names alone.
- Interpret images in the AI client and send explicit layout data to the plugin. Use appropriate Canvas, HorizontalBox/VerticalBox, Overlay, Border or SizeBox containers and anchors instead of stacking all widgets at absolute coordinates.
"""

# Initialize server
mcp = FastMCP(
    "UnrealMCP",
    instructions="Unreal Engine integration via Model Context Protocol.\n" + BLUEPRINT_LAYOUT_RULES,
    lifespan=server_lifespan
)

# 以脚本启动时让工具的延迟导入复用同一模块及连接状态
if __name__ == "__main__":
    sys.modules["unreal_mcp_server"] = sys.modules[__name__]

# Import and register tools
from tools.editor_tools import register_editor_tools
from tools.blueprint_tools import register_blueprint_tools
from tools.node_tools import register_blueprint_node_tools
from tools.project_tools import register_project_tools
from tools.umg_tools import register_umg_tools
from tools.material_tools import register_material_tools
from tools.debug_tools import register_debug_tools
from tools.layout_tools import register_layout_tools

# Register tools
register_editor_tools(mcp)
register_blueprint_tools(mcp)
register_blueprint_node_tools(mcp)
register_project_tools(mcp)
register_umg_tools(mcp)
register_material_tools(mcp)
register_debug_tools(mcp)
register_layout_tools(mcp)
_instrument_tools()

@mcp.prompt()
def info():
    """Information about available Unreal MCP tools and best practices."""
    return BLUEPRINT_LAYOUT_RULES + """
    # Unreal MCP Server Tools and Best Practices

    ## UMG (Widget Blueprint) Tools
    - `create_umg_widget_blueprint(widget_name, parent_class="UserWidget", path="/Game/UI")`
      Create a new UMG Widget Blueprint
    - `add_text_block_to_widget(widget_name, text_block_name, text="", position=[0,0], size=[200,50], font_size=12, color=[1,1,1,1])`
      Add a Text Block widget with customizable properties
    - `add_button_to_widget(widget_name, button_name, text="", position=[0,0], size=[200,50], font_size=12, color=[1,1,1,1], background_color=[0.1,0.1,0.1,1])`
      Add a Button widget with text and styling
    - `bind_widget_event(widget_name, widget_component_name, event_name)`
      Bind a widget event (e.g. OnClicked) to a component-bound event node
    - `add_widget_to_viewport(widget_name, z_order=0)`
      Add widget instance to game viewport
    - `set_text_block_binding(widget_name, text_block_name, binding_property, binding_type="Text")`
      Set up dynamic text binding (only Text bindings are supported)

    - `list_widget_blueprints(path="/Game", recursive=true)` - List project Widget Blueprints
    - `get_widget_tree(widget_name)` - Read a widget hierarchy and slot layouts
    - `add_widget_to_widget(widget_name, child_name, widget_class, parent_name="", layout={}, properties={})` - Add native or project widgets to any panel

    ## Editor Tools
    ### Actor Management
    - `get_actors_in_level()` - List all actors in current level
    - `find_actors_by_name(pattern)` - Find actor names by pattern
    - `spawn_actor(name, type, location=[0,0,0], rotation=[0,0,0], scale=[1,1,1], mesh_path="")` - Create actors
    - `delete_actor(name)` - Remove actors
    - `set_actor_transform(name, location, rotation, scale)` - Modify actor transform
    - `get_actor_properties(name)` - Get actor properties
    - `set_actor_property(name, property_name, property_value)` - Set an actor property
    - `spawn_blueprint_actor(blueprint_name, actor_name)` - Spawn a Blueprint actor
    - `duplicate_level(source_path, destination_path)` - Duplicate a level
    - `take_screenshot(filepath)` - Capture the active viewport to PNG (rendering verification)
    - `save_level(path="")` - Save the editor map after deferred material changes

    ### Play In Editor
    - `start_pie()` - Queue PIE start; poll get_pie_state for readiness
    - `stop_pie()` - Queue PIE stop; poll get_pie_state until playing=false
    - `get_pie_state()` - Report whether PIE is running and ready

    Runtime-only work (dynamic material instances, gameplay events) needs an active
    session; pass `pie=true` to the material tools to address the play world.

    ## Blueprint Management
    - `create_blueprint(name, parent_class)` - Create new Blueprint classes
    - `add_component_to_blueprint(blueprint_name, component_type, component_name)` - Add components
    - `set_static_mesh_properties(blueprint_name, component_name, static_mesh)` - Configure meshes
    - `set_component_property(blueprint_name, component_name, property_name, property_value)` - Set a component property
    - `set_physics_properties(blueprint_name, component_name, simulate_physics, mass, linear_damping, angular_damping)` - Configure physics
    - `compile_blueprint(blueprint_name)` - Compile Blueprint changes and return compiler errors/warnings
    - `set_blueprint_property(blueprint_name, property_name, property_value)` - Set properties

    ## Blueprint Node Management
    - `list_blueprint_graphs(blueprint_name)` - List Event, function and macro graphs
    - `create_blueprint_graph(blueprint_name, graph_name, graph_type="function", defer_save=false)` - Create a function or macro graph
    - `set_blueprint_pin_default(blueprint_name, node_id, pin_name, default_value, graph_name="", defer_save=false)` - Set an unconnected input pin literal
    - `get_blueprint_graph(blueprint_name, graph_name="")` - Read node geometry and links
    - `format_blueprint_graph(blueprint_name, node_ids, whole_graph=false, graph_name="")` - Arrange selected nodes or the whole graph
    - `add_blueprint_event_node(blueprint_name, event_name)` - Add event nodes
    - `add_blueprint_input_action_node(blueprint_name, action_name)` - Add input nodes
    - `add_blueprint_function_node(blueprint_name, target, function_name)` - Add function nodes
    - `connect_blueprint_nodes(blueprint_name, source_node_id, source_pin, target_node_id, target_pin)` - Connect nodes
    - `add_blueprint_variable(blueprint_name, variable_name, variable_type)` - Add variables
    - `add_blueprint_get_self_component_reference(blueprint_name, component_name)` - Add component refs
    - `add_blueprint_self_reference(blueprint_name)` - Add self references
    - `find_blueprint_nodes(blueprint_name, node_type, event_type, graph_name="")` - Find nodes
    - `disconnect_blueprint_nodes(blueprint_name, source_node_id, source_pin, target_node_id, target_pin, graph_name="")` - Break one pin link
    - `set_blueprint_node_property(blueprint_name, node_id, property_name, property_value, graph_name="")` - Edit a node property
    - `get_blueprint_node_details(blueprint_name, node_id, graph_name="")` - Read pins, defaults and links
    - `delete_blueprint_node(blueprint_name, node_id, graph_name="")` - Delete a node

    Every node tool accepts `graph_name` to target a function or macro graph instead of
    the first Event Graph. Omit it to keep the default Event Graph behaviour.

    ## Project Tools
    - `create_input_mapping(action_name, key, input_type="Action")` - Create input mappings (Action or Axis)
    - `refresh_project_reflection(blueprint_name="")` - Rebuild graph nodes against current C++ reflection
    - `compile_project()` - Trigger a Live Coding compile (asynchronous)
    - `save_assets(asset_paths)` - Save specific assets after `defer_save=True` edits

    ## Material Tools
    - `create_material(material_name, save_path="/Game/Materials")` - Create a Material asset
    - `create_material_instance(instance_name, parent_material_path, save_path="/Game/Materials")` - Create a persistent material instance
    - `get_material_graph(material_path)` - Read expressions, links, property inputs and settings
    - `list_material_parameters(material_path)` - List scalar, vector, texture and static switch parameters
    - `get_material_instance_parameters(instance_path)` - Read a material instance's parent and overrides
    - `add_material_expression(material_path, expression_class, parameter_name="", properties=None, x=None, y=None)` - Add an expression node
    - `set_material_expression_property(material_path, node_id, properties)` - Write node properties
    - `delete_material_expression(material_path, node_id)` - Delete a node and report broken links
    - `connect_material_expressions(material_path, from_node_id, to_node_id, from_output="", to_input="")` - Link two nodes
    - `disconnect_material_expressions(material_path, to_node_id, from_node_id="", to_input="")` - Break one link
    - `connect_material_property(material_path, from_node_id, property, from_output="")` - Feed a material property
    - `disconnect_material_property(material_path, property)` - Clear a material property input
    - `set_material_settings(material_path, blend_mode="", shading_model="", material_domain="", two_sided=None, opacity_mask_clip_value=None)` - Change and recompile material settings
    - `add_material_comment(material_path, text, x=None, y=None, size_x=None, size_y=None, color=None)` - Add a comment box
    - `format_material_graph(material_path, dry_run=false)` - Lay out nodes by data depth
    - `set_material_instance_parameter(instance_path, param_name, scalar=None, vector=None, texture=None, static_switch=None)` - Override one persistent parameter
    - `add_vector_parameter(material_name, param_name, default_color=[r,g,b,a])` - Add a vector parameter and connect it to Base Color
    - `set_material_on_component(actor_name, component_name, material_path, material_index=0)` - Assign a material to a mesh component
    - `create_dynamic_material_instance(actor_name, component_name, material_index=0, pie=true)` - Create/assign a DMI in PIE
    - `set_material_parameter(actor_name, component_name, param_name, scalar=None, vector=None, material_index=0, pie=true)` - Set a DMI parameter
    - `get_component_materials(actor_name, component_name, pie=false)` - Read every slot's material, DMI flag, parent and parameters

    Material graph nodes are addressed by node_id from `get_material_graph`. Material
    properties accept BaseColor, Roughness, Normal and the MP_ forms. Persistent values
    go on a material instance; dynamic material instances only exist during a play session.

    ## Debug Tools
    - `get_output_log(filter="", max_lines=50)` - Read the editor output log

    ## Best Practices

    ### UMG Widget Development
    - Create widgets with descriptive names that reflect their purpose
    - Use consistent naming conventions for widget components
    - Organize widget hierarchy logically
    - Set appropriate anchors and alignment for responsive layouts
    - Use property bindings for dynamic updates instead of direct setting
    - Handle widget events appropriately with meaningful function names
    - Clean up widgets when no longer needed
    - Test widget layouts at different resolutions

    ### Editor and Actor Management
    - Use unique names for actors to avoid conflicts
    - Clean up temporary actors
    - Validate transforms before applying
    - Check actor existence before modifications
    - Take regular viewport screenshots during development
    - Keep the viewport focused on relevant actors during operations

    ### Batch Edits and Saving
    - Node creation, connection, deletion, property/default edits, graph creation/layout,
      compile_blueprint and set_material_on_component accept defer_save=True.
      Deferred edits remain in memory until save_assets is called (save_level for maps).
    - Graph edits capture an editor undo transaction; Ctrl+Z can undo each command.
    - Deferred saving is not rollback. Failed commands can leave changes in memory.
      save_assets writes assets individually, can partially succeed, and reports failures.
      It does not provide cross-asset atomic saving.

    ### C++ Reflection Changes
    - Adding a BlueprintCallable function requires building the module; the editor keeps
      metadata changes can require closing the editor, rebuilding its target, and restarting it.
    - After building and loading native code, call refresh_project_reflection so existing graph nodes rebind.
      Graph refresh cannot regenerate or reload native reflection and does not save assets.
    - `compile_project` only covers Live Coding function-body patches; do not rely on it
      to publish new reflected members.

    ### Verifying Runtime Results
    - Use `get_component_materials` to confirm a material or DMI actually landed on a slot.
    - Use `take_screenshot` to confirm what is actually rendered rather than inferring it.
    - Dynamic material instances exist only in a play session; start it with `start_pie`.

    ### Blueprint Development
    - Compile Blueprints after changes
    - Use meaningful names for variables and functions
    - Organize nodes logically
    - Test functionality in isolation
    - Consider performance implications
    - Document complex setups

    ### Error Handling
    - Check command responses for success
    - Handle errors gracefully
    - Log important operations
    - Validate parameters
    - Clean up resources on errors
    """

# Run the server
if __name__ == "__main__":
    mcp.run(transport='stdio')
