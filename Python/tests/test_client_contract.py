"""UnrealMCP 客户端回归测试。

用假 Unreal（脚本化响应）驱动真实的 MCP 工具入口，覆盖参数形状、
响应解包与错误上报；不需要启动 UE 编辑器。

运行：.venv/Scripts/python.exe -m unittest discover -s tests -v
"""

import json
import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import unreal_mcp_server as srv


class FakeConnection:
    """记录发往 Unreal 的命令，并按脚本返回响应。"""

    def __init__(self, responder=None):
        self.calls = []
        self.responder = responder
        self.connected = True

    def send_command(self, command, params):
        self.calls.append((command, params))
        if self.responder:
            return self.responder(command, params)
        return {"status": "success", "result": {}}


def tool_fn(name):
    """取回已注册工具的实现函数（这些工具都不使用 ctx）。"""
    for t in srv.mcp._tool_manager.list_tools():
        if t.name == name:
            return t.fn
    raise AssertionError(f"tool not registered: {name}")


def call_tool(name, arguments):
    """直接调用工具实现，不经过 MCP 序列化层。"""
    return tool_fn(name)(ctx=None, **arguments)


class ToolRegistrationTests(unittest.TestCase):
    """契约：插件支持的每一类命令都必须有可达的 MCP 工具。"""

    EXPECTED = {
        # material_tools + debug_tools 必须与其它模块一起注册
        "create_material",
        "add_vector_parameter",
        "set_material_on_component",
        "get_output_log",
    }

    def test_material_and_debug_tools_are_registered(self):
        tools = {t.name for t in srv.mcp._tool_manager.list_tools()}
        missing = sorted(self.EXPECTED - tools)
        self.assertEqual([], missing, f"工具未注册: {missing}")

    def test_tool_count_does_not_regress(self):
        # 重构不得减少已暴露的工具数量
        tools = {t.name for t in srv.mcp._tool_manager.list_tools()}
        self.assertGreaterEqual(len(tools), 59)


class ResponseUnwrapTests(unittest.TestCase):
    """契约：插件把负载包在 result 内，工具必须按内层取值。"""

    def test_find_actors_by_name_reads_nested_actors(self):
        fake = FakeConnection(lambda c, p: {
            "status": "success",
            "result": {"actors": ["Alpha", "Beta"]},
        })
        with mock.patch.object(srv, "_unreal_connection", fake):
            actors = call_tool("find_actors_by_name", {"pattern": "A"})
        self.assertEqual(["Alpha", "Beta"], actors)

    def test_get_actors_in_level_reads_nested_actors(self):
        fake = FakeConnection(lambda c, p: {
            "status": "success",
            "result": {"actors": [{"name": "A"}]},
        })
        with mock.patch.object(srv, "_unreal_connection", fake):
            self.assertEqual([{"name": "A"}], call_tool("get_actors_in_level", {}))

    def test_result_payload_helper_handles_missing_and_odd_shapes(self):
        self.assertEqual({}, srv.result_payload(None))
        self.assertEqual({}, srv.result_payload({"status": "error"}))
        self.assertEqual({}, srv.result_payload({"result": "not-a-dict"}))
        self.assertEqual({"a": 1}, srv.result_payload({"result": {"a": 1}}))


class CommandShapeTests(unittest.TestCase):
    """契约：发往插件的命令名与参数名必须与插件读取的字段一致。"""

    def capture(self, tool, arguments, responder=None):
        fake = FakeConnection(responder)
        with mock.patch.object(srv, "_unreal_connection", fake):
            call_tool(tool, arguments)
        return fake.calls

    def test_spawn_actor_preserves_public_type_parameter(self):
        """保留既有工具参数名，并使用插件接受的字段。"""
        calls = self.capture("spawn_actor", {"name": "N", "type": "StaticMeshActor"})
        command, params = calls[0]
        self.assertEqual("spawn_actor", command)
        self.assertEqual("StaticMeshActor", params["type"])
        self.assertNotIn("actor_type", params)

    def test_create_input_mapping_sends_input_type(self):
        calls = self.capture("create_input_mapping",
                             {"action_name": "A", "key": "F", "input_type": "Axis"})
        self.assertEqual("create_input_mapping", calls[0][0])
        self.assertEqual("Axis", calls[0][1]["input_type"])

    def test_input_mapping_rejects_unknown_type_before_sending(self):
        fake = FakeConnection()
        with mock.patch.object(srv, "_unreal_connection", fake):
            response = call_tool("create_input_mapping", {"action_name": "A", "key": "F", "input_type": "Axix"})
        self.assertFalse(response["success"])
        self.assertEqual([], fake.calls)
        calls = self.capture("create_input_mapping", {"action_name": "A", "key": "F", "input_type": "axis"})
        self.assertEqual("axis", calls[0][1]["input_type"])

    def test_bind_widget_event_does_not_send_function_name(self):
        """插件不读 function_name；继续发送会造成已文档化但被忽略的参数。"""
        calls = self.capture("bind_widget_event", {
            "widget_name": "W", "widget_component_name": "B", "event_name": "OnClicked",
        })
        _, params = calls[0]
        self.assertNotIn("function_name", params)

    def test_set_physics_properties_does_not_send_gravity_enabled(self):
        """插件不读 gravity_enabled；该参数是文档承诺但实现不接受的输入。"""
        calls = self.capture("set_physics_properties",
                            {"blueprint_name": "B", "component_name": "C"})
        _, params = calls[0]
        self.assertNotIn("gravity_enabled", params)

    def test_add_component_to_blueprint_does_not_send_component_properties(self):
        """插件不读 component_properties；静默忽略的参数不应继续宣传。"""
        calls = self.capture("add_component_to_blueprint",
                            {"blueprint_name": "B", "component_type": "StaticMeshComponent",
                             "component_name": "C"})
        _, params = calls[0]
        self.assertNotIn("component_properties", params)


class GraphScopeTests(unittest.TestCase):
    """契约：节点命令必须能把图作用域传给插件，否则函数图/宏图无法寻址。"""

    def capture(self, tool, arguments):
        fake = FakeConnection()
        with mock.patch.object(srv, "_unreal_connection", fake):
            call_tool(tool, arguments)
        return fake.calls[0]

    def test_graph_name_is_forwarded(self):
        for tool, args in [
            ("add_blueprint_event_node", {"blueprint_name": "B", "event_name": "E"}),
            ("add_blueprint_function_node", {"blueprint_name": "B", "target": "T", "function_name": "F"}),
            ("find_blueprint_nodes", {"blueprint_name": "B"}),
            ("get_blueprint_graph", {"blueprint_name": "B"}),
        ]:
            with self.subTest(tool=tool):
                _, params = self.capture(tool, dict(args, graph_name="MyFunction"))
                self.assertEqual("MyFunction", params["graph_name"])

    def test_deferred_graph_writes_forward_save_policy(self):
        for tool, args in [
            ("create_blueprint_graph", {"blueprint_name": "B", "graph_name": "F", "graph_type": "function"}),
            ("disconnect_blueprint_nodes", {"blueprint_name": "B", "source_node_id": "S", "source_pin": "then", "target_node_id": "T", "target_pin": "execute"}),
            ("set_blueprint_pin_default", {"blueprint_name": "B", "node_id": "N", "pin_name": "P", "default_value": "false"}),
            ("compile_blueprint", {"blueprint_name": "B"}),
        ]:
            with self.subTest(tool=tool):
                command, params = self.capture(tool, dict(args, defer_save=True))
                self.assertEqual(tool, command)
                self.assertTrue(params["defer_save"])

    def test_invalid_graph_kind_does_not_send(self):
        fake = FakeConnection()
        with mock.patch.object(srv, "_unreal_connection", fake):
            result = call_tool("create_blueprint_graph", {"blueprint_name": "B", "graph_name": "F", "graph_type": "unknown"})
        self.assertFalse(result["success"])
        self.assertEqual([], fake.calls)

    def test_compile_diagnostics_are_returned_on_failure(self):
        response = {"status": "error", "error": "Bad pin", "result": {
            "diagnostics": {"num_errors": 1, "errors": [{"message": "Bad pin"}]}}}
        fake = FakeConnection(lambda c, p: response)
        with mock.patch.object(srv, "_unreal_connection", fake):
            self.assertEqual(response, call_tool("compile_blueprint", {"blueprint_name": "B"}))

    def test_format_scopes_are_mutually_exclusive(self):
        fake = FakeConnection()
        with mock.patch.object(srv, "_unreal_connection", fake):
            bad = [
                {},                                                          # nothing chosen
                {"node_ids": ["a"], "whole_graph": True},                    # both
                {"mode": "unplaced", "node_ids": ["a"]},                    # unplaced + selection
                {"mode": "unplaced", "whole_graph": True},
                {"whole_graph": True, "mode": "nonsense"},
            ]
            for extra in bad:
                with self.subTest(extra=extra):
                    result = call_tool("format_blueprint_graph", dict({"blueprint_name": "B"}, **extra))
                    self.assertFalse(result["success"])
        self.assertEqual([], fake.calls)

    def test_unplaced_mode_sends_no_selection_and_dry_run_is_forwarded(self):
        fake = FakeConnection()
        with mock.patch.object(srv, "_unreal_connection", fake):
            call_tool("format_blueprint_graph", {"blueprint_name": "B", "mode": "unplaced", "dry_run": True})
            call_tool("format_blueprint_graph", {"blueprint_name": "B", "whole_graph": True})
        command, unplaced = fake.calls[0]
        self.assertEqual("format_blueprint_graph", command)
        self.assertEqual("unplaced", unplaced["mode"])
        self.assertTrue(unplaced["dry_run"])
        self.assertNotIn("node_ids", unplaced)
        self.assertNotIn("whole_graph", unplaced)
        _, whole = fake.calls[1]
        self.assertTrue(whole["whole_graph"])
        self.assertNotIn("dry_run", whole)

    def test_graph_name_omitted_keeps_default_event_graph_behaviour(self):
        """不传 graph_name 时不得发送该字段，插件据此回落到第一个 Event Graph。"""
        _, params = self.capture("add_blueprint_event_node",
                                 {"blueprint_name": "B", "event_name": "E"})
        self.assertNotIn("graph_name", params)


class MaterialParameterTests(unittest.TestCase):
    """契约：材质参数只能写动态实例，客户端先拒绝歧义输入。"""

    def test_scalar_and_vector_are_mutually_exclusive(self):
        fake = FakeConnection()
        with mock.patch.object(srv, "_unreal_connection", fake),              mock.patch.object(srv.UnrealConnection, "connect", return_value=True):
            both = call_tool("set_material_parameter", {
                "actor_name": "A", "component_name": "C", "param_name": "P",
                "scalar": 1.0, "vector": [1, 1, 1]})
            neither = call_tool("set_material_parameter", {
                "actor_name": "A", "component_name": "C", "param_name": "P"})
        self.assertFalse(both["success"])
        self.assertFalse(neither["success"])
        self.assertEqual([], fake.calls)

    def test_invalid_vector_does_not_send(self):
        fake = FakeConnection()
        with mock.patch.object(srv, "_unreal_connection", fake):
            for vector in ([1, 2], [1, 2, float("nan")], [True, 0, 0]):
                with self.subTest(vector=vector):
                    result = call_tool("set_material_parameter", {
                        "actor_name": "A", "component_name": "C", "param_name": "P", "vector": vector})
                    self.assertFalse(result["success"])
        self.assertEqual([], fake.calls)

    def test_scalar_is_sent_as_number_and_pie_flag_survives(self):
        fake = FakeConnection()
        with mock.patch.object(srv, "_unreal_connection", fake):
            call_tool("set_material_parameter", {
                "actor_name": "A", "component_name": "C", "param_name": "P",
                "scalar": 0.5, "pie": True})
        _, params = fake.calls[0]
        self.assertEqual(0.5, params["scalar"])
        self.assertTrue(params["pie"])
        self.assertNotIn("vector", params)


class MaterialGraphContractTests(unittest.TestCase):
    """契约：材质图与实例命令的参数名必须与插件读取的字段一致。"""

    def capture(self, tool, arguments):
        fake = FakeConnection()
        with mock.patch.object(srv, "_unreal_connection", fake):
            call_tool(tool, arguments)
        return fake.calls[0]

    def test_expression_creation_sends_class_and_omits_empty_locators(self):
        command, params = self.capture("add_material_expression", {
            "material_path": "/Game/M.M", "expression_class": "ScalarParameter"})
        self.assertEqual("add_material_expression", command)
        self.assertEqual("ScalarParameter", params["expression_class"])
        for omitted in ("parameter_name", "properties", "x", "y"):
            self.assertNotIn(omitted, params)

    def test_expression_creation_forwards_properties_and_position(self):
        _, params = self.capture("add_material_expression", {
            "material_path": "/Game/M.M", "expression_class": "VectorParameter",
            "parameter_name": "Tint", "properties": {"DefaultValue": [1, 0, 0]}, "x": 64, "y": 128})
        self.assertEqual("Tint", params["parameter_name"])
        self.assertEqual({"DefaultValue": [1, 0, 0]}, params["properties"])
        self.assertEqual((64, 128), (params["x"], params["y"]))

    def test_connect_requires_a_target_pin_and_keeps_named_pin(self):
        fake = FakeConnection()
        with mock.patch.object(srv, "_unreal_connection", fake):
            rejected = call_tool("connect_material_expressions", {
                "material_path": "/Game/M.M", "from_node_id": "F", "to_node_id": "T"})
        self.assertFalse(rejected["success"])
        self.assertEqual([], fake.calls)
        _, params = self.capture("connect_material_expressions", {
            "material_path": "/Game/M.M", "from_node_id": "F", "to_node_id": "T",
            "from_output": "R", "to_input": "A"})
        self.assertEqual("R", params["from_output"])
        self.assertEqual("A", params["to_input"])
        self.assertNotIn("to_input_index", params)

    def test_disconnect_without_from_node_clears_the_whole_input(self):
        _, params = self.capture("disconnect_material_expressions", {
            "material_path": "/Game/M.M", "to_node_id": "T", "to_input_index": 1})
        self.assertEqual(1, params["to_input_index"])
        self.assertNotIn("from_node_id", params)

    def test_material_settings_reject_an_out_of_range_clip_value_before_sending(self):
        fake = FakeConnection()
        with mock.patch.object(srv, "_unreal_connection", fake):
            result = call_tool("set_material_settings", {
                "material_path": "/Game/M.M", "opacity_mask_clip_value": 1.5})
        self.assertFalse(result["success"])
        self.assertEqual([], fake.calls)

    def test_comment_colour_is_validated(self):
        fake = FakeConnection()
        with mock.patch.object(srv, "_unreal_connection", fake):
            bad = call_tool("add_material_comment", {
                "material_path": "/Game/M.M", "text": "t", "color": [1, 2]})
        self.assertFalse(bad["success"])
        self.assertEqual([], fake.calls)
        _, params = self.capture("add_material_comment", {
            "material_path": "/Game/M.M", "text": "t", "color": [0.2, 0.4, 0.6]})
        self.assertEqual([0.2, 0.4, 0.6], params["color"])

    def test_instance_parameter_accepts_exactly_one_kind(self):
        fake = FakeConnection()
        with mock.patch.object(srv, "_unreal_connection", fake):
            both = call_tool("set_material_instance_parameter", {
                "instance_path": "/Game/I.I", "param_name": "P", "scalar": 1.0, "static_switch": True})
            neither = call_tool("set_material_instance_parameter", {
                "instance_path": "/Game/I.I", "param_name": "P"})
        self.assertFalse(both["success"])
        self.assertFalse(neither["success"])
        self.assertEqual([], fake.calls)
        _, params = self.capture("set_material_instance_parameter", {
            "instance_path": "/Game/I.I", "param_name": "P", "static_switch": False})
        self.assertFalse(params["static_switch"])
        self.assertNotIn("scalar", params)

    def test_instance_vector_accepts_three_or_four_channels(self):
        _, params = self.capture("set_material_instance_parameter", {
            "instance_path": "/Game/I.I", "param_name": "P", "vector": [0.1, 0.2, 0.3]})
        self.assertEqual([0.1, 0.2, 0.3], params["vector"])


class PromptAccuracyTests(unittest.TestCase):
    """契约：info 提示词只能宣传真实存在的工具。"""

    def test_info_prompt_only_names_registered_tools(self):
        import re

        registered = {t.name for t in srv.mcp._tool_manager.list_tools()}
        prompt = srv.info()
        # 提示词里用 `tool_name(...)` 形式列举工具
        named = set(re.findall(r"`(\w+)\(", prompt))
        unknown = sorted(named - registered)
        self.assertEqual([], unknown, f"提示词宣传了不存在的工具: {unknown}")
        import inspect
        for name, parameters in re.findall(r"`(\w+)\(([^)]*)\)`", prompt):
            signature = inspect.signature(tool_fn(name))
            parameters = re.sub(r"\[[^]]*\]", "[]", parameters)
            advertised = [part.strip().split("=", 1)[0] for part in parameters.split(",") if part.strip()]
            for parameter in advertised:
                self.assertIn(parameter, signature.parameters, f"{name}: {parameter}")


class ErrorReportingTests(unittest.TestCase):
    """契约：连接不可用必须可被调用方识别，而不是伪装成空成功。"""

    def test_no_connection_result_is_recognisable(self):
        """连接失败必须返回可识别的失败结果，而不是空成功。"""
        result = srv.no_connection()
        self.assertFalse(result["success"])
        self.assertIn("connect", result["message"].lower())

    def test_get_unreal_connection_returns_none_when_connect_fails(self):
        with mock.patch.object(srv, "_unreal_connection", None), \
             mock.patch.object(srv.UnrealConnection, "connect", return_value=False):
            self.assertIsNone(srv.get_unreal_connection())

    def test_every_tool_reports_failure_when_editor_is_down(self):
        """编辑器不可用时，每个工具都必须返回失败而不是 NameError/异常。

        验证返回结果，避免异常被工具捕获后漏报。
        """
        # 各工具的必填参数（仅用于让调用走到连接检查）
        required = {
            "spawn_actor": {"name": "N", "type": "StaticMeshActor"},
            "create_blueprint": {"name": "N", "parent_class": "Actor"},
            "add_component_to_blueprint": {"blueprint_name": "B", "component_type": "SphereComponent", "component_name": "C"},
            "set_static_mesh_properties": {"blueprint_name": "B", "component_name": "C"},
            "set_component_property": {"blueprint_name": "B", "component_name": "C", "property_name": "P", "property_value": "V"},
            "set_physics_properties": {"blueprint_name": "B", "component_name": "C"},
            "compile_blueprint": {"blueprint_name": "B"},
            "set_blueprint_property": {"blueprint_name": "B", "property_name": "P", "property_value": "V"},
            "add_blueprint_event_node": {"blueprint_name": "B", "event_name": "ReceiveBeginPlay"},
            "add_blueprint_input_action_node": {"blueprint_name": "B", "action_name": "A"},
            "add_blueprint_function_node": {"blueprint_name": "B", "target": "T", "function_name": "F"},
            "connect_blueprint_nodes": {"blueprint_name": "B", "source_node_id": "S", "source_pin": "P", "target_node_id": "T", "target_pin": "Q"},
            "add_blueprint_variable": {"blueprint_name": "B", "variable_name": "V", "variable_type": "Boolean"},
            "add_blueprint_get_self_component_reference": {"blueprint_name": "B", "component_name": "C"},
            "add_blueprint_self_reference": {"blueprint_name": "B"},
            "find_blueprint_nodes": {"blueprint_name": "B", "node_type": "Event"},
            "create_input_mapping": {"action_name": "A", "key": "F"},
            "create_umg_widget_blueprint": {"widget_name": "W"},
            "add_text_block_to_widget": {"widget_name": "W", "text_block_name": "T"},
            "add_button_to_widget": {"widget_name": "W", "button_name": "B"},
            "bind_widget_event": {"widget_name": "W", "widget_component_name": "C", "event_name": "OnClicked"},
            "add_widget_to_viewport": {"widget_name": "W"},
            "get_blueprint_graph": {"blueprint_name": "B"},
            "format_blueprint_graph": {"blueprint_name": "B", "node_ids": []},
            "list_widget_blueprints": {},
            "get_widget_tree": {"widget_name": "W"},
            "add_widget_to_widget": {"widget_name": "W", "child_name": "C", "widget_class": "TextBlock"},
            "set_text_block_binding": {"widget_name": "W", "text_block_name": "T", "binding_property": "P"},
            "create_material": {"material_name": "M"},
            "add_vector_parameter": {"material_name": "M", "param_name": "P"},
            "set_material_on_component": {"actor_name": "A", "component_name": "C", "material_path": "/Game/M.M"},
            "get_output_log": {},
            "get_actors_in_level": {},
            "find_actors_by_name": {"pattern": "A"},
            "delete_actor": {"name": "A"},
            "set_actor_transform": {"name": "A"},
            "get_actor_properties": {"name": "A"},
            "set_actor_property": {"name": "A", "property_name": "P", "property_value": "V"},
            "spawn_blueprint_actor": {"blueprint_name": "B", "actor_name": "A"},
            "duplicate_level": {"source_path": "/Game/A", "destination_path": "/Game/B"},
            "take_screenshot": {"filepath": "/tmp/shot.png"},
            "save_level": {},
            "start_pie": {},
            "stop_pie": {},
            "get_pie_state": {},
            "add_image_to_widget": {"widget_name": "W", "image_name": "I"},
            "add_blueprint_cast_node": {"blueprint_name": "B"},
            "add_blueprint_external_member_get_node": {"blueprint_name": "B", "member_class": "C", "member_name": "M"},
            "get_blueprint_node_details": {"blueprint_name": "B", "node_id": "N"},
            "delete_blueprint_node": {"blueprint_name": "B", "node_id": "N"},
            "disconnect_blueprint_nodes": {"blueprint_name": "B", "source_node_id": "S", "source_pin": "P",
                                           "target_node_id": "T", "target_pin": "Q"},
            "set_blueprint_node_property": {"blueprint_name": "B", "node_id": "N",
                                           "property_name": "NodeComment", "property_value": "hi"},
            "create_dynamic_material_instance": {"actor_name": "A", "component_name": "C"},
            "set_material_parameter": {"actor_name": "A", "component_name": "C",
                                       "param_name": "P", "scalar": 1.0},
            "get_component_materials": {"actor_name": "A", "component_name": "C"},
            "get_material_graph": {"material_path": "/Game/M.M"},
            "list_material_parameters": {"material_path": "/Game/M.M"},
            "add_material_expression": {"material_path": "/Game/M.M", "expression_class": "Multiply", "defer_save": True},
            "set_material_expression_property": {"material_path": "/Game/M.M", "node_id": "N",
                                                 "properties": {"Desc": "d"}, "defer_save": True},
            "delete_material_expression": {"material_path": "/Game/M.M", "node_id": "N"},
            "connect_material_expressions": {"material_path": "/Game/M.M", "from_node_id": "F",
                                              "to_node_id": "T", "to_input_index": 0, "defer_save": True},
            "disconnect_material_expressions": {"material_path": "/Game/M.M", "to_node_id": "T",
                                                 "to_input_index": 0, "defer_save": True},
            "connect_material_property": {"material_path": "/Game/M.M", "from_node_id": "F",
                                           "property": "BaseColor", "defer_save": True},
            "disconnect_material_property": {"material_path": "/Game/M.M", "property": "BaseColor", "defer_save": True},
            "set_material_settings": {"material_path": "/Game/M.M", "blend_mode": "Masked"},
            "add_material_comment": {"material_path": "/Game/M.M", "text": "t"},
            "format_material_graph": {"material_path": "/Game/M.M"},
            "create_material_instance": {"instance_name": "I", "parent_material_path": "/Game/M.M"},
            "set_material_instance_parameter": {"instance_path": "/Game/I.I", "param_name": "P",
                                               "scalar": 0.5},
            "get_material_instance_parameters": {"instance_path": "/Game/I.I"},
            "refresh_project_reflection": {},
            "compile_project": {},
            "list_blueprint_graphs": {"blueprint_name": "B"},
            "create_blueprint_graph": {"blueprint_name": "B", "graph_name": "F"},
            "set_blueprint_pin_default": {"blueprint_name": "B", "node_id": "N", "pin_name": "P", "default_value": "true"},
            "save_assets": {"asset_paths": ["/Game/A"]},
            "set_asset_property": {"asset_path": "/Game/A", "property_name": "P", "property_value": "V"},
            "begin_blueprint_edit": {"blueprint_name": "B"},
            "finish_blueprint_edit": {"blueprint_name": "B"},
        }
        tools = {t.name: t for t in srv.mcp._tool_manager.list_tools()}
        with mock.patch.object(srv, "_unreal_connection", None), \
             mock.patch.object(srv.UnrealConnection, "connect", return_value=False):
            for name in sorted(tools):
                args = required.get(name)
                if args is None:
                    self.fail(f"工具 {name} 未在测试中覆盖，请补充必填参数")
                with self.subTest(tool=name):
                    result = tools[name].fn(ctx=None, **args)
                    if name in {"get_actors_in_level", "find_actors_by_name"}:
                        self.assertEqual([], result)
                    else:
                        self.assertEqual({"success": False,
                                          "message": "Failed to connect to Unreal Engine"}, result)

    def test_editor_dict_tools_report_no_response_and_exception(self):
        cases = {
            "delete_actor": {"name": "A"},
            "set_actor_transform": {"name": "A"},
            "get_actor_properties": {"name": "A"},
            "duplicate_level": {"source_path": "/Game/A", "destination_path": "/Game/B"},
        }
        for name, arguments in cases.items():
            with self.subTest(tool=name), mock.patch.object(srv, "_unreal_connection", FakeConnection(lambda c, p: None)):
                self.assertEqual({"success": False, "message": "No response from Unreal Engine"},
                                 call_tool(name, arguments))
            def fail(command, params):
                raise ConnectionError("test transport failure")
            with self.subTest(tool=name), mock.patch.object(srv, "_unreal_connection", FakeConnection(fail)):
                response = call_tool(name, arguments)
                self.assertFalse(response["success"])
                self.assertIn("test transport failure", response["message"])

    def test_get_unreal_connection_has_no_socket_probe(self):
        """连接探针曾以 b'\\x00' 做假 ping，会破坏协议；不得再出现。"""
        server = os.path.join(os.path.dirname(os.path.dirname(
            os.path.abspath(__file__))), "unreal_mcp_server.py")
        with open(server, encoding="utf-8") as handle:
            source = handle.read()
        self.assertNotIn("ping test", source)
        self.assertNotIn("sendall(b'\\x00')", source)


if __name__ == "__main__":
    unittest.main()
