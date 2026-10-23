"""真实 stdio 子进程的初始化与工具发现检查，不调用编辑器。"""

import asyncio
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


class StdioTests(unittest.IsolatedAsyncioTestCase):
    async def test_initialize_and_list_tools(self):
        root = Path(__file__).resolve().parent.parent
        parameters = StdioServerParameters(command=sys.executable,
                                           args=[str(root / "unreal_mcp_server.py")],
                                           cwd=str(root))

        async def check():
            async with stdio_client(parameters) as (reader, writer):
                async with ClientSession(reader, writer) as session:
                    initialization = await session.initialize()
                    # 验证规范随 MCP 握手传给客户端，不依赖主动请求提示词
                    self.assertIn("Blueprint layout requirements", initialization.instructions)
                    self.assertIn("align each execution chain horizontally", initialization.instructions)
                    result = await session.list_tools()
                    tools = {tool.name: tool for tool in result.tools}
                    self.assertGreaterEqual(len(tools), 59)
                    self.assertIn("type", tools["spawn_actor"].inputSchema["properties"])
                    self.assertIn("get_output_log", tools)
                    self.assertIn("create_material", tools)
                    self.assertIn("separate unrelated", tools["connect_blueprint_nodes"].description)
        await asyncio.wait_for(check(), timeout=15)

    async def test_tool_call_roundtrip_to_fake_tcp_editor(self):
        requests = []
        async def handle(reader, writer):
            try:
                request = json.loads(await reader.read(4096))
                requests.append(request)
                wire = json.dumps({"status": "success", "result": {
                    "actors": ["Alpha"]}}, ensure_ascii=False).encode()
                writer.write(wire)
                await writer.drain()
            finally:
                writer.close()
                await writer.wait_closed()

        root = Path(__file__).resolve().parent.parent
        server = await asyncio.start_server(handle, "127.0.0.1", 0)
        port = server.sockets[0].getsockname()[1]
        parameters = StdioServerParameters(command=sys.executable, args=["-c",
            f"import unreal_mcp_server as s; s.UNREAL_PORT={port}; s.mcp.run(transport='stdio')"],
            cwd=str(root))
        errlog = tempfile.TemporaryFile(mode="w+", encoding="utf-8")
        self.addCleanup(errlog.close)

        async def check():
            async with server, stdio_client(parameters, errlog=errlog) as (reader, writer):
                async with ClientSession(reader, writer) as session:
                    await session.initialize()
                    for _ in range(2):
                        result = await session.call_tool("find_actors_by_name", {"pattern": "A"})
                        self.assertFalse(result.isError)
                        self.assertIn("Alpha", "".join(item.text for item in result.content))
        await asyncio.wait_for(check(), timeout=15)
        self.assertEqual([{"type": "find_actors_by_name", "params": {"pattern": "A"}}] * 2, requests)
        errlog.seek(0)
        stderr = errlog.read()
        events = [json.loads(line.split(" - ", 2)[-1]) for line in stderr.splitlines()
                  if ' - {"event":' in line]
        self.assertEqual(2, len(events), stderr)
        for event in events:
            self.assertEqual("command", event["event"])
            self.assertEqual("find_actors_by_name", event["command"])
            self.assertEqual({"pattern": "A"}, event["params"])
            self.assertEqual({"status": "success", "result": {"actors": ["Alpha"]}}, event["response"])
            self.assertTrue(event["success"])
        self.assertNotIn("\x1b[", stderr)
        self.assertNotIn("开始 find_actors_by_name", stderr)
        self.assertNotIn("完成 find_actors_by_name", stderr)

    async def test_complete_console_event_goes_to_stderr_not_stdout(self):
        root = Path(__file__).resolve().parent.parent
        # lifespan 负责配置日志，导入启动和直接运行脚本使用同一路径。
        script = ("import unreal_mcp_server as s; s.UNREAL_PORT=1; "
                  "s.mcp.run(transport='stdio')")
        # stdout 必须只有 JSON-RPC，完整控制台事件只能出现在 stderr。
        process = await asyncio.create_subprocess_exec(
            sys.executable, "-c", script, cwd=str(root),
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        async def cleanup():
            if process.returncode is None:
                process.kill()
            await process.wait()
        self.addAsyncCleanup(cleanup)
        stderr_task = asyncio.create_task(process.stderr.read())
        self.addCleanup(stderr_task.cancel)
        request = {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                   "params": {"name": "find_actors_by_name", "arguments": {"pattern": "A"}}}
        initialize = {"jsonrpc": "2.0", "id": 0, "method": "initialize", "params": {
            "protocolVersion": "2024-11-05", "capabilities": {},
            "clientInfo": {"name": "test", "version": "0"}}}
        process.stdin.write((json.dumps(initialize) + "\n").encode())
        await process.stdin.drain()
        initialization = json.loads(await asyncio.wait_for(process.stdout.readline(), timeout=15))
        self.assertEqual(0, initialization["id"])
        self.assertIn("result", initialization)
        for message in ({"jsonrpc": "2.0", "method": "notifications/initialized"}, request):
            process.stdin.write((json.dumps(message) + "\n").encode())
        await process.stdin.drain()
        result = json.loads(await asyncio.wait_for(process.stdout.readline(), timeout=15))
        self.assertEqual(1, result["id"])
        self.assertIn("result", result)
        process.stdin.close()
        await asyncio.wait_for(process.wait(), timeout=15)
        self.assertEqual(b"", await process.stdout.read())
        self.assertEqual(0, process.returncode)
        stderr = (await asyncio.wait_for(stderr_task, timeout=15)).decode("utf-8")
        self.assertIn("Unreal MCP Server 已启动", stderr)
        self.assertIn("stdio", stderr)
        events = [json.loads(line.split(" - ", 2)[-1]) for line in stderr.splitlines()
                  if ' - {"event":' in line]
        event, = events
        self.assertEqual("tool", event["event"])
        self.assertEqual("find_actors_by_name", event["tool"])
        self.assertEqual({"pattern": "A"}, event["params"])
        self.assertEqual([], event["response"])
        self.assertFalse(event["success"])
        self.assertTrue(event["error"])
        self.assertNotIn("\x1b[", stderr)
        self.assertNotIn("Traceback", stderr)


if __name__ == "__main__":
    unittest.main()
