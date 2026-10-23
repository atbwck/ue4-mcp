"""传输回归：使用假 socket，不接触真实编辑器。"""

import json
import socket
import unittest
from unittest import mock

import unreal_mcp_server as srv
from unreal_transport import receive_response


class ScriptedSocket:
    def __init__(self, chunks):
        self.chunks = iter(chunks)
        self.timeout = None
        self.closed = False
        self.sent = []

    def gettimeout(self):
        return self.timeout

    def settimeout(self, timeout):
        self.timeout = timeout

    def recv(self, size):
        chunk = next(self.chunks)
        if isinstance(chunk, Exception):
            raise chunk
        return chunk

    def sendall(self, data):
        self.sent.append(data)

    def close(self):
        self.closed = True

    def setsockopt(self, *args):
        pass

    def connect(self, address):
        pass


class TransportTests(unittest.TestCase):
    def test_utf8_response_at_every_split_returns_without_eof(self):
        expected = {"status": "success", "result": {"text": "中文 🐾"}}
        wire = json.dumps(expected, ensure_ascii=False).encode("utf-8")
        for index in range(1, len(wire)):
            with self.subTest(split=index):
                sock = ScriptedSocket([wire[:index], wire[index:]])
                self.assertEqual(expected, json.loads(receive_response(sock)))
                self.assertIsNone(sock.timeout)
                self.assertFalse(sock.closed)

    def test_partial_response_eof_and_timeout_fail(self):
        for tail, error in [(b"", ConnectionError), (socket.timeout(), TimeoutError)]:
            sock = ScriptedSocket([b'{"status":', tail])
            with self.assertRaises(error):
                receive_response(sock)
            self.assertIsNone(sock.timeout)

    def test_invalid_utf8_and_non_object_fail(self):
        for wire, error in [(b'{"x":"\xff"}', UnicodeDecodeError), (b'[]', ValueError)]:
            with self.assertRaises(error):
                receive_response(ScriptedSocket([wire]))

    def test_receive_deadline_and_size_limit(self):
        sock = ScriptedSocket([b'{"status":'])
        with mock.patch("unreal_transport.time.monotonic", side_effect=[0, 1, 31]):
            with self.assertRaises(TimeoutError):
                receive_response(sock)
        self.assertIsNone(sock.timeout)
        sock = ScriptedSocket([b' ' * (16 * 1024 * 1024 + 1)])
        with self.assertRaisesRegex(ValueError, "16 MiB"):
            receive_response(sock)

    def test_failed_connect_closes_socket(self):
        sock = ScriptedSocket([])
        sock.connect = mock.Mock(side_effect=ConnectionRefusedError("refused"))
        with mock.patch.object(srv.socket, "socket", return_value=sock):
            connection = srv.UnrealConnection()
            self.assertFalse(connection.connect())
        self.assertTrue(sock.closed)
        self.assertIsNone(connection.socket)
        self.assertFalse(connection.connected)

    def test_cached_connection_connects_once_per_command(self):
        sockets = [ScriptedSocket([b'{"status":"success","result":{}}']) for _ in range(2)]
        with mock.patch.object(srv, "_unreal_connection", None), \
             mock.patch.object(srv.socket, "socket", side_effect=sockets) as factory:
            for index in range(2):
                connection = srv.get_unreal_connection()
                self.assertEqual("success", connection.send_command("ping")["status"])
                self.assertEqual(index + 1, factory.call_count)
        self.assertTrue(all(sock.closed for sock in sockets))

    def test_failure_after_send_never_retries(self):
        sock = ScriptedSocket([b''])
        with mock.patch.object(srv.socket, "socket", return_value=sock) as factory:
            response = srv.UnrealConnection().send_command("create_blueprint", {"name": "B"})
        self.assertEqual("error", response["status"])
        self.assertEqual(1, factory.call_count)
        self.assertEqual(1, len(sock.sent))
        self.assertTrue(sock.closed)


if __name__ == "__main__":
    unittest.main()
