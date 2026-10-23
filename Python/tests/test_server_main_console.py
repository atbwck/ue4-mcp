import asyncio
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


@unittest.skipUnless(sys.platform == "win32", "Windows console API only")
class MainConsoleTests(unittest.IsolatedAsyncioTestCase):
    async def test_client_command_and_result_share_server_console_record(self):
        root = Path(__file__).resolve().parent.parent
        requests = []

        async def editor(reader, writer):
            request = json.loads(await reader.read(4096))
            requests.append(request)
            response = ({"status": "success", "result": {"actors": ["main-success-marker"]}}
                        if request["params"]["pattern"] == "Success" else
                        {"status": "error", "error": "main-error-marker"})
            writer.write(json.dumps(response).encode())
            await writer.drain()
            writer.close()
            await writer.wait_closed()

        script = r'''
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import sys
sys.stderr = open("CONOUT$", "w", encoding="utf-8")
import unreal_mcp_server as s
s.UNREAL_PORT = int(sys.argv[1])
s.mcp.run(transport="stdio")
class Info(ctypes.Structure):
    _fields_ = [("size", wintypes._COORD), ("cursor", wintypes._COORD),
                ("attributes", wintypes.WORD), ("window", wintypes.SMALL_RECT),
                ("maximum", wintypes._COORD)]
kernel = ctypes.WinDLL("kernel32", use_last_error=True)
kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
kernel.CreateFileW.restype = wintypes.HANDLE
kernel.GetConsoleScreenBufferInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(Info)]
kernel.ReadConsoleOutputCharacterW.argtypes = [wintypes.HANDLE, wintypes.LPWSTR, wintypes.DWORD, wintypes._COORD, ctypes.POINTER(wintypes.DWORD)]
kernel.ReadConsoleOutputAttribute.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.WORD), wintypes.DWORD, wintypes._COORD, ctypes.POINTER(wintypes.DWORD)]
handle = kernel.CreateFileW("CONOUT$", 0x80000000, 3, None, 3, 0, None)
info = Info()
assert kernel.GetConsoleScreenBufferInfo(handle, ctypes.byref(info))
length = info.size.X * (info.cursor.Y + 1)
text = ctypes.create_unicode_buffer(length + 1)
count = wintypes.DWORD()
assert kernel.ReadConsoleOutputCharacterW(handle, text, length, wintypes._COORD(0, 0), ctypes.byref(count))
observed = {"screen": text.value, "foreground": info.attributes & 15, "markers": {}}
for marker in ("main-success-marker", "main-error-marker"):
    index = text.value.find(marker)
    assert index >= 0, marker
    color = wintypes.WORD()
    assert kernel.ReadConsoleOutputAttribute(handle, ctypes.byref(color), 1, wintypes._COORD(index % info.size.X, index // info.size.X), ctypes.byref(count))
    observed["markers"][marker] = color.value & 15
Path(sys.argv[2]).write_text(json.dumps(observed), encoding="utf-8")
'''
        async with await asyncio.start_server(editor, "127.0.0.1", 0) as endpoint:
            port = endpoint.sockets[0].getsockname()[1]
            with tempfile.TemporaryDirectory() as directory:
                result_path = Path(directory) / "server-console.json"
                process = await asyncio.create_subprocess_exec(
                    sys.executable, "-c", script, str(port), str(result_path), cwd=root,
                    stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                    creationflags=subprocess.CREATE_NEW_CONSOLE)

                async def cleanup():
                    if process.returncode is None:
                        process.kill()
                    await process.wait()
                self.addAsyncCleanup(cleanup)

                async def send(message):
                    process.stdin.write((json.dumps(message) + "\n").encode())
                    await process.stdin.drain()

                await send({"jsonrpc": "2.0", "id": 0, "method": "initialize", "params": {
                    "protocolVersion": "2024-11-05", "capabilities": {},
                    "clientInfo": {"name": "console-check", "version": "1"}}})
                initialized = json.loads(await asyncio.wait_for(process.stdout.readline(), 15))
                self.assertEqual(0, initialized["id"])
                await send({"jsonrpc": "2.0", "method": "notifications/initialized"})
                for index, pattern in enumerate(("Success", "Failure"), 1):
                    await send({"jsonrpc": "2.0", "id": index, "method": "tools/call", "params": {
                        "name": "find_actors_by_name", "arguments": {"pattern": pattern}}})
                    response = json.loads(await asyncio.wait_for(process.stdout.readline(), 15))
                    self.assertEqual(index, response["id"])
                process.stdin.close()
                await asyncio.wait_for(process.wait(), 15)
                self.assertEqual(0, process.returncode)
                self.assertEqual(b"", await process.stdout.read())
                observed = json.loads(result_path.read_text(encoding="utf-8"))
        self.assertEqual(["Success", "Failure"], [item["params"]["pattern"] for item in requests])
        self.assertEqual(2, observed["screen"].count('"event":"command"'), observed)
        self.assertIn('"pattern":"Success"', observed["screen"])
        self.assertIn('"pattern":"Failure"', observed["screen"])
        self.assertEqual(4, observed["markers"]["main-error-marker"], observed)
        self.assertEqual(observed["foreground"], observed["markers"]["main-success-marker"], observed)


if __name__ == "__main__":
    unittest.main()
