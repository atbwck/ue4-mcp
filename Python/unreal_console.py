"""Color failed server records in its console."""

import copy
import logging
import sys


class _ConsoleFormatter(logging.Formatter):
    def format(self, record):
        record = copy.copy(record)
        record.exc_info = None
        record.exc_text = None
        record.stack_info = None
        return super().format(record)


def disable_quick_edit():
    if sys.platform != "win32":
        return
    import ctypes
    from ctypes import wintypes

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.GetStdHandle.argtypes = [wintypes.DWORD]
    kernel.GetStdHandle.restype = wintypes.HANDLE
    kernel.GetConsoleMode.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    kernel.SetConsoleMode.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    mode = wintypes.DWORD()
    handle = kernel.GetStdHandle(-10 & 0xffffffff)
    if kernel.GetConsoleMode(handle, ctypes.byref(mode)):
        # ENABLE_EXTENDED_FLAGS is required when clearing ENABLE_QUICK_EDIT_MODE.
        kernel.SetConsoleMode(handle, (mode.value | 0x0080) & ~0x0040)


def _windows_color(stream):
    """Enable VT for this stream, or retain its native foreground attributes."""
    try:
        import ctypes
        from ctypes import wintypes
        import msvcrt

        handle = wintypes.HANDLE(msvcrt.get_osfhandle(stream.fileno()))
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.GetConsoleMode.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
        kernel.SetConsoleMode.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        mode = wintypes.DWORD()
        if kernel.GetConsoleMode(handle, ctypes.byref(mode)):
            if kernel.SetConsoleMode(handle, mode.value | 0x0004):
                return True, None

        class BufferInfo(ctypes.Structure):
            _fields_ = [("size", wintypes._COORD), ("cursor", wintypes._COORD),
                        ("attributes", wintypes.WORD), ("window", wintypes.SMALL_RECT),
                        ("maximum", wintypes._COORD)]

        kernel.GetConsoleScreenBufferInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(BufferInfo)]
        kernel.SetConsoleTextAttribute.argtypes = [wintypes.HANDLE, wintypes.WORD]
        info = BufferInfo()
        if kernel.GetConsoleScreenBufferInfo(handle, ctypes.byref(info)):
            return False, (kernel, handle, info.attributes)
    except (AttributeError, ImportError, OSError, ValueError, TypeError):
        # Captured stderr and fake TTY streams may not have a console handle.
        pass
    return False, None


class _ConsoleHandler(logging.StreamHandler):
    def __init__(self, stream):
        super().__init__(stream)
        self._ansi = False
        self._native = None
        try:
            terminal = bool(stream.isatty())
        except (AttributeError, OSError, ValueError):
            terminal = False
        if terminal:
            if sys.platform == "win32":
                self._ansi, self._native = _windows_color(stream)
            else:
                self._ansi = True

    def format(self, record):
        message = super().format(record)
        if self._ansi and record.levelno >= logging.ERROR:
            return "\x1b[31m" + message + "\x1b[0m"
        return message

    def emit(self, record):
        if self._native is None or record.levelno < logging.ERROR:
            return super().emit(record)
        kernel, handle, attributes = self._native
        # Change only the foreground; restore the original console attributes.
        kernel.SetConsoleTextAttribute(handle, (attributes & ~0x000F) | 0x0004)
        try:
            super().emit(record)
        finally:
            kernel.SetConsoleTextAttribute(handle, attributes)


def make_console_handler(stream, fmt: str) -> logging.StreamHandler:
    """Create a plain redirected handler, or a terminal handler with red errors."""
    handler = _ConsoleHandler(stream)
    handler.setFormatter(_ConsoleFormatter(fmt))
    return handler
