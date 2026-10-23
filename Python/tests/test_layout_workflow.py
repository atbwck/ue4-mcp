"""布局工作流通过真实工具入口与传输接口验证，不操作编辑器。"""
import unittest
from unittest import mock
import unreal_mcp_server as srv


class LayoutWorkflowTests(unittest.TestCase):
    def test_compile_formats_only_pending_nodes_of_that_blueprint(self):
        connection = srv.UnrealConnection()
        calls = []
        def reply(command, params=None):
            calls.append((command, params))
            if command == "add_blueprint_self_reference":
                return {"status": "success", "result": {"node_id": params["blueprint_name"] + "-node"}}
            return {"status": "success", "result": {}}
        with mock.patch.object(connection, "_send_command", side_effect=reply):
            connection.send_command("add_blueprint_self_reference", {"blueprint_name": "A"})
            connection.send_command("add_blueprint_self_reference", {"blueprint_name": "B"})
            connection.send_command("compile_blueprint", {"blueprint_name": "A"})
        self.assertEqual(("format_blueprint_graph", {"blueprint_name": "A", "mode": "selection",
                                                    "node_ids": ["A-node"], "defer_save": True}), calls[-2])
        self.assertEqual(["B-node"], connection.pending_layout[("B", "")])
        self.assertNotIn(("A", ""), connection.pending_layout)

    def test_layout_failure_stops_compile_and_keeps_retry_selection(self):
        connection = srv.UnrealConnection()
        connection.pending_layout[("A", "")] = ["node"]
        with mock.patch.object(connection, "_send_command", return_value={"status": "error", "error": "layout failed"}) as send:
            response = connection.send_command("compile_blueprint", {"blueprint_name": "A"})
        self.assertEqual("error", response["status"])
        self.assertEqual(["node"], connection.pending_layout[("A", "")])
        self.assertEqual(1, send.call_count)

    def test_compile_formats_each_graph_without_saving_before_compile(self):
        connection = srv.UnrealConnection()
        calls = []
        def reply(command, params=None):
            calls.append((command, params))
            if command == "add_blueprint_self_reference":
                return {"status": "success", "result": {"node_id": params.get("graph_name") or "event"}}
            return {"status": "success", "result": {}}
        with mock.patch.object(connection, "_send_command", side_effect=reply):
            for graph in ("", "Calculate", "MyMacro"):
                connection.send_command("add_blueprint_self_reference", {
                    "blueprint_name": "A", "graph_name": graph, "defer_save": True})
            connection.send_command("compile_blueprint", {"blueprint_name": "A", "defer_save": True})
        formats = [p for c, p in calls if c == "format_blueprint_graph"]
        self.assertEqual(["", "Calculate", "MyMacro"], [p.get("graph_name", "") for p in formats])
        self.assertEqual([["event"], ["Calculate"], ["MyMacro"]], [p["node_ids"] for p in formats])
        self.assertTrue(all(p["defer_save"] for p in formats))
        self.assertEqual(("compile_blueprint", {"blueprint_name": "A", "defer_save": True}), calls[-1])
        self.assertEqual({}, connection.pending_layout)

    def test_format_and_delete_only_clear_the_matching_graph(self):
        connection = srv.UnrealConnection()
        connection.pending_layout = {("A", "F"): ["one", "two"], ("A", "M"): ["three"]}
        with mock.patch.object(connection, "_send_command", return_value={"status": "success", "result": {}}):
            connection.send_command("delete_blueprint_node", {"blueprint_name": "A", "graph_name": "F", "node_id": "one"})
            self.assertEqual(["two"], connection.pending_layout[("A", "F")])
            connection.send_command("format_blueprint_graph", {"blueprint_name": "A", "graph_name": "M", "whole_graph": True})
        self.assertEqual({("A", "F"): ["two"]}, connection.pending_layout)

    def test_deleted_guids_are_removed_across_graph_name_aliases(self):
        connection = srv.UnrealConnection()
        connection.pending_layout = {("A", ""): ["event"], ("A", "Calculate"): ["function"], ("B", ""): ["other"]}
        with mock.patch.object(connection, "_send_command", return_value={"status": "success", "result": {}}) as send:
            connection.send_command("delete_blueprint_node", {"blueprint_name": "A", "graph_name": "EventGraph", "node_id": "event"})
            connection.send_command("delete_blueprint_node", {"blueprint_name": "A", "graph_name": "calculate", "node_id": "function"})
            connection.send_command("compile_blueprint", {"blueprint_name": "A"})
        self.assertEqual("compile_blueprint", send.call_args_list[-1].args[0])
        self.assertEqual(3, send.call_count)
        self.assertEqual({("B", ""): ["other"]}, connection.pending_layout)

    def test_confirmed_deletion_is_cleared_even_when_saving_fails(self):
        connection = srv.UnrealConnection()
        connection.pending_layout = {("A", "F"): ["deleted"]}
        failed_save = {"status": "error", "error": "Save failed", "result": {"deleted_node_id": "deleted"}}
        with mock.patch.object(connection, "_send_command", side_effect=[failed_save, {"status": "success", "result": {}}]) as send:
            result = connection.send_command("delete_blueprint_node", {"blueprint_name": "A", "graph_name": "f", "node_id": "deleted"})
            self.assertEqual(failed_save, result)
            connection.send_command("compile_blueprint", {"blueprint_name": "A"})
        self.assertEqual(2, send.call_count)
        self.assertEqual("compile_blueprint", send.call_args_list[-1].args[0])
        self.assertEqual({}, connection.pending_layout)
        # A refusal is not a deletion: only the editor can confirm mutation.
        connection.pending_layout = {("A", "F"): ["protected"]}
        with mock.patch.object(connection, "_send_command", return_value={"status": "error", "error": "Protected"}):
            connection.send_command("delete_blueprint_node", {"blueprint_name": "A", "graph_name": "F", "node_id": "protected"})
        self.assertEqual(["protected"], connection.pending_layout[("A", "F")])

    def test_explicit_positions_do_not_enter_automatic_selection(self):
        connection = srv.UnrealConnection()
        with mock.patch.object(connection, "_send_command", return_value={"status": "success", "result": {"node_id": "fixed"}}):
            connection.send_command("add_blueprint_self_reference", {"blueprint_name": "A", "node_position": [100, 200]})
        self.assertEqual({}, connection.pending_layout)


if __name__ == "__main__":
    unittest.main()
