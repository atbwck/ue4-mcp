"""MCP 服务与手动脚本共用的 JSON 响应接收。"""

import json
import socket
import time


def receive_response(sock: socket.socket, buffer_size: int = 4096) -> bytes:
    """接收一个 JSON 对象；允许 UTF-8 和 JSON 在任意字节处分片。"""
    data = bytearray()
    deadline = time.monotonic() + 30
    previous_timeout = sock.gettimeout()
    try:
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("Timeout receiving Unreal response")
            sock.settimeout(remaining)
            chunk = sock.recv(buffer_size)
            if not chunk:
                raise ConnectionError("Connection closed before a complete Unreal response")
            data.extend(chunk)
            # 限制真实网络输入的大小，避免异常服务无限占用内存
            if len(data) > 16 * 1024 * 1024:
                raise ValueError("Unreal response exceeds 16 MiB")
            try:
                response = json.loads(data)
            except UnicodeDecodeError as error:
                if error.reason == "unexpected end of data":
                    continue
                raise
            except json.JSONDecodeError:
                continue
            if not isinstance(response, dict):
                raise ValueError("Unreal response must be a JSON object")
            return bytes(data)
    finally:
        sock.settimeout(previous_timeout)
