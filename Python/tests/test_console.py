"""Complete command events, tool fallbacks and terminal/file separation."""

import asyncio
import io
import json
import logging
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from mcp.server.fastmcp import Context
from mcp.server.fastmcp.exceptions import ToolError

import unreal_console
import unreal_mcp_server as srv


class ConsoleTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.previous_handlers = srv.logger.handlers[:]
        self.previous_level = srv.logger.level
        self.previous_propagate = srv.logger.propagate
        srv.logger.handlers = []
        self.addCleanup(self.restore_logging)
        self.stderr = io.StringIO()
        patcher = mock.patch.object(sys, "stderr", self.stderr)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.path = srv.configure_logging(self.directory.name)

    def restore_logging(self):
        for handler in srv.logger.handlers:
            handler.close()
        srv.logger.handlers = self.previous_handlers
        srv.logger.setLevel(self.previous_level)
        srv.logger.propagate = self.previous_propagate

    def details(self):
        return Path(self.path).read_text(encoding="utf-8")

    def events(self):
        events = []
        for line in self.stderr.getvalue().splitlines():
            message = line.partition(" - {\"event\":")[2]
            if message:
                events.append(json.loads('{"event":' + message.removesuffix("\x1b[0m")))
        return events

    def send_response(self, response, command="test_command", params=None):
        with mock.patch.object(srv.socket, "socket"), mock.patch.object(
                srv.UnrealConnection, "receive_full_response", return_value=json.dumps(response).encode()):
            return srv.UnrealConnection().send_command(command, params)

    def register(self, fn):
        srv.mcp.tool()(fn)
        self.addCleanup(srv.mcp._tool_manager._tools.pop, fn.__name__, None)

    def test_wire_command_and_result_share_one_console_record(self):
        response = {"status": "success", "result": {"text": "完整结果\n第二行", "value": 42}}
        self.send_response(response, "read_marker", {"path": "/Game/Test", "text": "input\nvalue"})
        lines = [line for line in self.stderr.getvalue().splitlines() if "read_marker" in line]
        self.assertEqual(1, len(lines), self.stderr.getvalue())
        self.assertIn('"path":"/Game/Test"', lines[0])
        self.assertIn('"value":42', lines[0])
        self.assertIn(r"完整结果\n第二行", lines[0])
        event, = self.events()
        self.assertEqual(response, event["response"])
        self.assertTrue(event["success"])
        self.assertGreaterEqual(event["duration"], 0)
        self.assertNotIn("\x1b[", self.details())

    def test_failed_command_is_red_in_terminal_but_not_file(self):
        self.stderr.isatty = lambda: True
        with mock.patch.object(sys, "platform", "linux"):
            self.path = srv.configure_logging(self.directory.name)
        self.send_response({"status": "success", "result": {
            "success": False, "error": "nested-red-marker"}}, "failed_marker")
        lines = [line for line in self.stderr.getvalue().splitlines() if "failed_marker" in line]
        self.assertEqual(1, len(lines), self.stderr.getvalue())
        self.assertTrue(lines[0].startswith("\x1b[31m"), repr(lines[0]))
        self.assertTrue(lines[0].endswith("\x1b[0m"), repr(lines[0]))
        self.assertIn("nested-red-marker", lines[0])
        self.assertFalse(self.events()[0]["success"])
        self.assertNotIn("\x1b[", self.details())

    def test_normalization_does_not_replace_raw_response(self):
        for response, expected in [
            ({"success": False, "message": "top-level", "extra": [1, 2]},
             {"status": "error", "error": "top-level"}),
            ({"status": "error", "message": "missing-error", "extra": 3},
             {"status": "error", "message": "missing-error", "extra": 3, "error": "missing-error"}),
            ({"status": "success", "result": {"success": False, "error": "inner"}}, None),
        ]:
            with self.subTest(response=response):
                self.assertEqual(expected or response, self.send_response(response))
                self.assertEqual(response, self.events()[-1]["response"])
                self.assertFalse(self.events()[-1]["success"])
        self.assertEqual(3, len(self.events()))

    def test_control_characters_are_escaped_without_truncating_unicode(self):
        text = "完整" + "x" * 10000 + "\x1b[31m\r\n\x00\x7f\x85\x9b\u2028\u2029"
        self.send_response({"status": "success", "result": {"text": text}}, params={"text": text})
        event, = self.events()
        self.assertEqual(text, event["params"]["text"])
        self.assertEqual(text, event["response"]["result"]["text"])
        line = next(line for line in self.stderr.getvalue().splitlines() if '"event"' in line)
        for code in (0, 0x1b, 0x7f, 0x85, 0x9b, 0x2028, 0x2029):
            self.assertNotIn(chr(code), line)
        self.assertIn("完整", line)

    def test_socket_exception_logs_once_without_retry_and_keeps_traceback_in_file(self):
        with mock.patch.object(srv.socket, "socket") as factory:
            factory.return_value.sendall.side_effect = OSError("send-marker\nsecond line")
            result = srv.UnrealConnection().send_command("send_failure", {"value": 5})
            factory.return_value.sendall.assert_called_once()
            factory.return_value.connect.assert_called_once()
        self.assertEqual({"status": "error", "error": "send-marker\nsecond line"}, result)
        event, = self.events()
        self.assertFalse(event["success"])
        self.assertIsNone(event["response"])
        self.assertEqual("send-marker\nsecond line", event["error"])
        self.assertNotIn("Traceback", self.stderr.getvalue())
        self.assertIn("Traceback", self.details())

    def test_connection_failure_attempt_is_visible_and_still_returns_none(self):
        with mock.patch.object(srv.socket, "socket") as factory:
            factory.return_value.connect.side_effect = ConnectionRefusedError("direct-refused")
            self.assertIsNone(srv.UnrealConnection().send_command("not_sent", {"p": 1}))
            factory.return_value.connect.assert_called_once()
            factory.return_value.sendall.assert_not_called()
        event, = self.events()
        self.assertEqual("not_sent", event["command"])
        self.assertFalse(event["success"])
        self.assertIn("direct-refused", event["error"])
        self.assertIn("Traceback", self.details())

    async def test_fallback_shows_full_tool_result_without_wire(self):
        connection = mock.Mock()
        connection.send_command.return_value = {"status": "success", "result": {
            "actors": [{"name": "full-payload-marker"}]}}
        with mock.patch.object(srv, "get_unreal_connection", return_value=connection):
            await srv.mcp.call_tool("get_actors_in_level", {})
        event, = self.events()
        self.assertEqual("tool", event["event"])
        self.assertEqual([{"name": "full-payload-marker"}], event["response"])
        self.assertTrue(event["success"])
        console = self.stderr.getvalue()
        self.assertIn("full-payload-marker", console)
        self.assertIn("full-payload-marker", self.details())
        self.assertEqual(1, console.count("Unreal MCP Server 已启动"))
        self.assertIn("首次工具调用时连接 Unreal", console)
        self.assertIn(str(Path(self.path).resolve()), console)
        self.assertNotIn("开始 get_actors_in_level", console)
        self.assertNotIn("完成 get_actors_in_level", console)

    async def test_returned_failure_and_arguments_are_logged(self):
        await srv.mcp.call_tool("create_blueprint_graph", {
            "blueprint_name": "BP_Test", "graph_name": "Graph", "graph_type": "invalid-marker"})
        event, = self.events()
        self.assertFalse(event["success"])
        self.assertEqual("invalid-marker", event["params"]["graph_type"])
        self.assertEqual("graph_type must be function or macro", event["error"])
        self.assertFalse(event["response"]["success"])

    async def test_empty_list_after_connection_failure_is_not_success(self):
        with mock.patch.object(srv.socket, "socket") as factory:
            factory.return_value.connect.side_effect = ConnectionRefusedError("refused-marker")
            with mock.patch.object(srv, "_unreal_connection", None):
                await srv.mcp.call_tool("find_actors_by_name", {"pattern": "A"})
        event, = self.events()
        self.assertFalse(event["success"])
        self.assertEqual([], event["response"])
        self.assertIn("refused-marker", event["error"])
        self.assertIn("Traceback", self.details())

    async def test_caught_exception_returning_empty_list_is_failure(self):
        with mock.patch.object(srv, "get_unreal_connection", side_effect=ValueError("caught-marker")):
            await srv.mcp.call_tool("find_actors_by_name", {"pattern": "A"})
        event, = self.events()
        self.assertEqual([], event["response"])
        self.assertFalse(event["success"])
        self.assertIn("caught-marker", event["error"])
        self.assertIn("Traceback", self.details())
        self.assertNotIn("Traceback", self.stderr.getvalue())

    async def test_exception_traceback_only_in_file(self):
        def explode(ctx: Context) -> str:
            raise ValueError("exception-marker\nnot-truncated")

        self.register(explode)
        with self.assertRaises(ToolError):
            await srv.mcp.call_tool("explode", {})
        event, = self.events()
        self.assertFalse(event["success"])
        self.assertIn("exception-marker\nnot-truncated", event["error"])
        self.assertNotIn("Traceback", self.stderr.getvalue())
        self.assertIn("Traceback", self.details())

    async def test_sdk_validation_failure_also_has_one_complete_event(self):
        with self.assertRaises(ToolError):
            await srv.mcp.call_tool("find_actors_by_name", {})
        event, = self.events()
        self.assertEqual("find_actors_by_name", event["tool"])
        self.assertEqual({}, event["params"])
        self.assertFalse(event["success"])
        self.assertIn("pattern", event["error"])

    async def test_nested_failure_is_logged(self):
        connection = mock.Mock()
        response = {"status": "success", "result": {"success": False, "error": "nested-marker"}}
        connection.send_command.return_value = response
        with mock.patch.object(srv, "get_unreal_connection", return_value=connection):
            await srv.mcp.call_tool("compile_blueprint", {"blueprint_name": "BP_Test"})
        event, = self.events()
        self.assertFalse(event["success"])
        self.assertEqual("nested-marker", event["error"])
        self.assertEqual(response, event["response"])

    async def test_real_tool_to_wire_has_no_duplicate_and_includes_automatic_format(self):
        connection = srv.UnrealConnection()
        connection.pending_layout[("BP_Test", "")] = ["node-id"]
        response = {"status": "success", "result": {"compiled": True}}
        with mock.patch.object(srv, "get_unreal_connection", return_value=connection), \
                mock.patch.object(srv.socket, "socket") as factory, \
                mock.patch.object(connection, "receive_full_response", return_value=json.dumps(response).encode()):
            await srv.mcp.call_tool("compile_blueprint", {"blueprint_name": "BP_Test"})
        events = self.events()
        self.assertEqual(["format_blueprint_graph", "compile_blueprint"], [e["command"] for e in events])
        self.assertTrue(all(e["event"] == "command" for e in events))
        self.assertEqual({"blueprint_name": "BP_Test", "mode": "selection", "node_ids": ["node-id"],
                          "defer_save": True}, events[0]["params"])
        self.assertEqual(2, factory.return_value.sendall.call_count)

    async def test_wire_failure_does_not_get_duplicate_tool_error(self):
        connection = srv.UnrealConnection()
        with mock.patch.object(srv, "get_unreal_connection", return_value=connection), \
                mock.patch.object(srv.socket, "socket"), \
                mock.patch.object(connection, "receive_full_response", side_effect=OSError("wire-marker")):
            await srv.mcp.call_tool("compile_blueprint", {"blueprint_name": "BP_Test"})
        event, = self.events()
        self.assertEqual("command", event["event"])
        self.assertFalse(event["success"])
        self.assertEqual("wire-marker", event["error"])

    async def test_successful_wire_then_local_exception_is_visible(self):
        def after_wire(ctx: Context) -> dict:
            self.send_response({"status": "success", "result": {"sent": True}})
            raise ValueError("post-wire-marker")

        self.register(after_wire)
        with self.assertRaises(ToolError):
            await srv.mcp.call_tool("after_wire", {})
        command, tool = self.events()
        self.assertEqual("command", command["event"])
        self.assertTrue(command["success"])
        self.assertEqual("tool", tool["event"])
        self.assertFalse(tool["success"])
        self.assertIn("post-wire-marker", tool["error"])
        self.assertNotIn("Traceback", self.stderr.getvalue())
        self.assertIn("Traceback", self.details())

    async def test_successful_wire_then_caught_parser_error_is_visible(self):
        connection = srv.UnrealConnection()
        with mock.patch.object(srv, "get_unreal_connection", return_value=connection), \
                mock.patch.object(srv.socket, "socket"), \
                mock.patch.object(connection, "receive_full_response", return_value=b'{"status":"success","result":{"actors":null}}'):
            await srv.mcp.call_tool("get_actors_in_level", {})
        events = self.events()
        self.assertEqual(2, len(events), self.stderr.getvalue())
        self.assertTrue(events[0]["success"])
        self.assertFalse(events[1]["success"])
        self.assertEqual([], events[1]["response"])

    async def test_successful_wire_then_returned_failure_is_visible(self):
        def returned_failure(ctx: Context) -> dict:
            self.send_response({"status": "success", "result": {}})
            return {"success": False, "error": "postprocessing-marker"}

        self.register(returned_failure)
        await srv.mcp.call_tool("returned_failure", {})
        events = self.events()
        self.assertEqual(2, len(events), self.stderr.getvalue())
        self.assertFalse(events[-1]["success"])
        self.assertEqual("postprocessing-marker", events[-1]["error"])

    async def test_ordinary_warning_does_not_fail_tool(self):
        def warning_only(ctx: Context) -> list:
            srv.logger.warning("optional setting ignored")
            return []

        self.register(warning_only)
        await srv.mcp.call_tool("warning_only", {})
        event, = self.events()
        self.assertTrue(event["success"])
        self.assertIsNone(event["error"])
        self.assertEqual([], event["response"])

    def test_invalid_wire_json_logs_one_error_without_retry(self):
        with mock.patch.object(srv.socket, "socket") as factory, \
                mock.patch.object(srv.UnrealConnection, "receive_full_response", return_value=b"bad json"):
            response = srv.UnrealConnection().send_command("invalid_json")
        self.assertEqual("error", response["status"])
        event, = self.events()
        self.assertFalse(event["success"])
        self.assertIsNone(event["response"])
        self.assertIn("Expecting value", event["error"])
        factory.return_value.sendall.assert_called_once()
        self.assertIn("Traceback", self.details())

    async def test_concurrent_calls_keep_independent_mutable_scopes(self):
        async def scoped(ctx: Context, mode: str) -> dict:
            if mode == "wire":
                await asyncio.to_thread(self.send_response, {"status": "success", "result": {}})
                await asyncio.sleep(0)
                return {"success": True}
            await asyncio.sleep(0)
            srv.logger.error("local-marker")
            return {"success": False, "error": "local-marker"}

        self.register(scoped)
        await asyncio.gather(srv.mcp.call_tool("scoped", {"mode": "wire"}),
                             srv.mcp.call_tool("scoped", {"mode": "local"}))
        events = self.events()
        command, = [event for event in events if event["event"] == "command"]
        tool, = [event for event in events if event["event"] == "tool"]
        self.assertTrue(command["success"])
        self.assertFalse(tool["success"])
        self.assertEqual({"mode": "local"}, tool["params"])
        self.assertIsNone(srv._call_scope.get())

    async def test_failure_falls_back_to_console_and_still_serves_tools(self):
        with mock.patch.object(srv.os, "makedirs", side_effect=PermissionError("denied")):
            self.assertIsNone(srv.configure_logging(self.directory.name))
        with mock.patch.object(srv, "get_unreal_connection", return_value=None):
            await srv.mcp.call_tool("find_actors_by_name", {"pattern": "A"})
        self.assertIn("文件日志不可用", self.stderr.getvalue())
        self.assertIn("不可用（仅控制台）", self.stderr.getvalue())
        self.assertFalse(self.events()[0]["success"])

    def test_rotation_and_unique_launch_files(self):
        handler = next(h for h in srv.logger.handlers if isinstance(h, logging.FileHandler))
        self.assertEqual(5 * 1024 * 1024, handler.maxBytes)
        self.assertEqual(3, handler.backupCount)
        handler.maxBytes = 1024
        for _ in range(10):
            srv.logger.debug("detail-marker %s", "x" * 600)
        self.assertTrue(Path(self.path + ".3").exists())
        self.assertFalse(Path(self.path + ".4").exists())
        self.assertNotEqual(self.path, srv.configure_logging(self.directory.name))

    def test_file_preserves_header_and_complete_command(self):
        srv.logger.debug("debug payload marker")
        self.send_response({"status": "success", "result": {"value": 42}}, "header-marker")
        lines = self.details().splitlines()
        visible = [line.split(" - ", 4) for line in lines if '"event":"command"' in line]
        self.assertEqual(1, len(visible))
        timestamp, name, level, source, message = visible[0]
        self.assertTrue(timestamp)
        self.assertEqual("UnrealMCP", name)
        self.assertEqual("INFO", level)
        self.assertTrue(source.startswith("[unreal_mcp_server.py:"), source)
        self.assertTrue(message.startswith('{"event":"command"'), message)
        debug = next(line.split(" - ", 4)[-1] for line in lines if "debug payload" in line)
        self.assertEqual("debug payload marker", debug)


class ConsoleHandlerTests(unittest.TestCase):
    def record(self, level=logging.ERROR):
        return logging.LogRecord("UnrealMCP", level, "original.py", 42, "payload %s", ("marker",), None)

    def test_redirected_output_is_plain_and_record_is_not_mutated(self):
        stream = io.StringIO()
        handler = unreal_console.make_console_handler(stream, "%(levelname)s %(message)s")
        record = self.record()
        original = record.__dict__.copy()
        handler.handle(record)
        self.assertEqual("ERROR payload marker\n", stream.getvalue())
        self.assertEqual(original, record.__dict__)

    def test_tty_red_reset_and_success_plain(self):
        stream = io.StringIO()
        stream.isatty = lambda: True
        with mock.patch.object(sys, "platform", "linux"):
            handler = unreal_console.make_console_handler(stream, "%(message)s")
        record = self.record()
        original = record.__dict__.copy()
        handler.handle(record)
        handler.handle(self.record(logging.INFO))
        self.assertEqual("\x1b[31mpayload marker\x1b[0m\npayload marker\n", stream.getvalue())
        self.assertEqual(original, record.__dict__)

    def test_fake_windows_tty_without_fileno_falls_back_gracefully(self):
        stream = io.StringIO()
        stream.isatty = lambda: True
        with mock.patch.object(sys, "platform", "win32"):
            handler = unreal_console.make_console_handler(stream, "%(message)s")
        handler.handle(self.record())
        self.assertEqual("payload marker\n", stream.getvalue())

    def test_native_foreground_restored_even_when_write_fails(self):
        stream = io.StringIO()
        stream.isatty = lambda: True
        kernel = mock.Mock()
        with mock.patch.object(sys, "platform", "win32"), \
                mock.patch.object(unreal_console, "_windows_color", return_value=(False, (kernel, 99, 0x27))):
            handler = unreal_console.make_console_handler(stream, "%(message)s")
        with mock.patch.object(stream, "write", side_effect=OSError("broken")), \
                mock.patch.object(handler, "handleError") as handle_error:
            handler.handle(self.record())
        handle_error.assert_called_once()
        self.assertEqual([mock.call(99, 0x24), mock.call(99, 0x27)], kernel.SetConsoleTextAttribute.call_args_list)

    def test_console_traceback_does_not_leak_file_formatter_cache(self):
        stream = io.StringIO()
        record = self.record()
        try:
            raise ValueError("file-only-marker")
        except ValueError:
            record.exc_info = sys.exc_info()
        file_formatter = logging.Formatter(srv.FILE_FORMAT)
        self.assertIn("Traceback", file_formatter.format(record))
        record.exc_text = "Traceback cached-file-marker"
        handler = unreal_console.make_console_handler(stream, "%(message)s")
        original = record.__dict__.copy()
        handler.handle(record)
        self.assertEqual("payload marker\n", stream.getvalue())
        self.assertEqual(original, record.__dict__)


if __name__ == "__main__":
    unittest.main()
