import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


@unittest.skipUnless(sys.platform == "win32", "Windows console API only")
class WindowsConsoleTests(unittest.TestCase):
    def test_startup_disables_quick_edit_and_preserves_other_input_flags(self):
        root = Path(__file__).resolve().parent.parent
        script = r'''
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import sys
import unreal_mcp_server as s
kernel = ctypes.WinDLL("kernel32", use_last_error=True)
kernel.GetStdHandle.argtypes = [wintypes.DWORD]
kernel.GetStdHandle.restype = wintypes.HANDLE
kernel.GetConsoleMode.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
kernel.SetConsoleMode.argtypes = [wintypes.HANDLE, wintypes.DWORD]
handle = kernel.GetStdHandle(-10 & 0xffffffff)
mode = wintypes.DWORD()
assert kernel.GetConsoleMode(handle, ctypes.byref(mode))
initial = mode.value | 0x0080 | 0x0040
assert kernel.SetConsoleMode(handle, initial)
s.configure_logging(Path(sys.argv[1]).parent)
assert kernel.GetConsoleMode(handle, ctypes.byref(mode))
Path(sys.argv[1]).write_text(json.dumps({"before": initial, "after": mode.value}), encoding="utf-8")
'''
        with tempfile.TemporaryDirectory() as directory:
            result = Path(directory) / "input-mode.json"
            child = subprocess.run([sys.executable, "-c", script, str(result)], cwd=root,
                                   creationflags=subprocess.CREATE_NEW_CONSOLE, timeout=15)
            self.assertEqual(0, child.returncode)
            observed = json.loads(result.read_text(encoding="utf-8"))
        self.assertEqual(0, observed["after"] & 0x0040, observed)
        self.assertEqual(observed["before"] & ~0x0040, observed["after"], observed)

    def test_error_characters_are_red_and_next_record_restores_color(self):
        root = Path(__file__).resolve().parent.parent
        script = r'''
import ctypes
from ctypes import wintypes
import json
import logging
from pathlib import Path
import sys
from unreal_console import make_console_handler
class Info(ctypes.Structure):
    _fields_ = [("size", wintypes._COORD), ("cursor", wintypes._COORD),
                ("attributes", wintypes.WORD), ("window", wintypes.SMALL_RECT),
                ("maximum", wintypes._COORD)]
kernel = ctypes.WinDLL("kernel32", use_last_error=True)
kernel.GetStdHandle.argtypes = [wintypes.DWORD]
kernel.GetStdHandle.restype = wintypes.HANDLE
kernel.GetConsoleScreenBufferInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(Info)]
kernel.SetConsoleCursorPosition.argtypes = [wintypes.HANDLE, wintypes._COORD]
kernel.ReadConsoleOutputAttribute.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.WORD), wintypes.DWORD, wintypes._COORD, ctypes.POINTER(wintypes.DWORD)]
handle = kernel.GetStdHandle(-12 & 0xffffffff)
before = Info()
assert kernel.GetConsoleScreenBufferInfo(handle, ctypes.byref(before))
assert kernel.SetConsoleCursorPosition(handle, wintypes._COORD(0, 0))
log = logging.getLogger("color-check")
log.setLevel(logging.INFO)
log.addHandler(make_console_handler(sys.stderr, "%(message)s"))
log.error("red-marker")
log.info("normal-marker")
attrs = []
for y in (0, 1):
    value = wintypes.WORD()
    count = wintypes.DWORD()
    assert kernel.ReadConsoleOutputAttribute(handle, ctypes.byref(value), 1, wintypes._COORD(0, y), ctypes.byref(count))
    assert count.value == 1
    attrs.append(value.value & 15)
after = Info()
assert kernel.GetConsoleScreenBufferInfo(handle, ctypes.byref(after))
Path(sys.argv[1]).write_text(json.dumps({"colors": attrs, "original": before.attributes & 15, "restored": after.attributes & 15}), encoding="utf-8")
'''
        with tempfile.TemporaryDirectory() as directory:
            result = Path(directory) / "console.json"
            environment = dict(os.environ)
            environment.pop("NO_COLOR", None)
            child = subprocess.run([sys.executable, "-c", script, str(result)], cwd=root,
                                   env=environment, creationflags=subprocess.CREATE_NEW_CONSOLE, timeout=15)
            self.assertEqual(0, child.returncode)
            observed = json.loads(result.read_text(encoding="utf-8"))
        self.assertEqual(4, observed["colors"][0], observed)
        self.assertEqual(observed["original"], observed["colors"][1], observed)
        self.assertEqual(observed["original"], observed["restored"], observed)


if __name__ == "__main__":
    unittest.main()
