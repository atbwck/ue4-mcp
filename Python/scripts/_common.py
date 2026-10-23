"""脚本共用的 Unreal 连接与命令发送工具。

这些脚本是手动驱动编辑器的辅助工具，不属于 MCP 服务路径；
此前每个脚本各自复制了一份 send_command，这里统一到一处。
"""

import json
import logging
import socket
import sys
from pathlib import Path
from typing import Any, Dict, Optional
# 手动脚本从任意目录运行时仍能导入仓库中的传输实现
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from unreal_transport import receive_response

logger = logging.getLogger("UnrealMCP")

UNREAL_HOST = "127.0.0.1"
UNREAL_PORT = 55557


def connect() -> socket.socket:
    """新建到编辑器的连接。"""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        sock.settimeout(30)
        sock.connect((UNREAL_HOST, UNREAL_PORT))
        return sock
    except Exception:
        sock.close()
        raise


def _receive(sock: socket.socket) -> bytes:
    """读到能解析成一个完整 JSON 对象为止。"""
    return receive_response(sock)


def send_command_on(sock: socket.socket, command: str, params: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    """在既有连接上发送命令并返回响应。"""
    try:
        command_json = json.dumps({"type": command, "params": params})
        logger.info(f"Sending command: {command_json}")
        sock.sendall(command_json.encode("utf-8"))
        response = json.loads(_receive(sock).decode("utf-8"))
        logger.info(f"Received response: {response}")
        return response
    except Exception as e:
        logger.error(f"Error sending command: {e}")
        return None


def send_command(command: str, params: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    """每条命令使用新连接发送。"""
    try:
        sock = connect()
        try:
            return send_command_on(sock, command, params)
        finally:
            sock.close()
    except Exception as e:
        logger.error(f"Error sending command: {e}")
        return None
