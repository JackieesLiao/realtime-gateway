"""Real TCP tests. sendall boundaries are deliberately never treated as recv boundaries."""
import concurrent.futures
import json
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest

SERVER, PY_PROTO, CLIENT = sys.argv[1:4]
sys.path.insert(0, PY_PROTO)
import gateway_pb2 as pb


def frame(body):
    return struct.pack('!I', len(body)) + body


def request(rid=42, kind=pb.TELEMETRY, device='robot-1', payload=b'xyz'):
    return frame(pb.Request(request_id=rid, device_id=device,
                            message_type=kind, payload=payload).SerializeToString())


def exact(sock, size):
    chunks = bytearray()
    while len(chunks) < size:
        part = sock.recv(size - len(chunks))
        if not part:
            raise AssertionError('unexpected EOF')
        chunks.extend(part)
    return bytes(chunks)


def ack(sock, rid, result=pb.OK):
    size, = struct.unpack('!I', exact(sock, 4))
    assert 0 < size <= 1048576, size
    message = pb.Ack.FromString(exact(sock, size))
    assert (message.request_id, message.result) == (rid, result), message
    if result != pb.OK:
        assert message.error


def closed(sock):
    try:
        assert sock.recv(1) == b'', 'expected closed socket'
    except (ConnectionResetError, BrokenPipeError):
        pass


class RunningServer:
    def __init__(self, **overrides):
        self.config = dict(listen_address='127.0.0.1', port=0, io_threads=3)
        self.config.update(overrides)

    def __enter__(self):
        self.temp = tempfile.TemporaryDirectory(prefix='gateway-integration-')
        root = Path(self.temp.name)
        config = root / 'config.json'
        config.write_text(json.dumps(self.config))
        self.log_path = root / 'server.log'
        self.output = self.log_path.open('w')
        self.process = subprocess.Popen([SERVER, str(config)], stdout=self.output, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 8
        try:
            while time.monotonic() < deadline:
                for line in self.log_path.read_text().splitlines():
                    if line.startswith('{'):
                        event = json.loads(line)
                        if event.get('event') == 'listening':
                            self.port = event['port']
                            return self
                if self.process.poll() is not None:
                    raise AssertionError(self.log_path.read_text())
                time.sleep(0.01)
            raise AssertionError('server readiness timeout: ' + self.log_path.read_text())
        except BaseException:
            if self.process.poll() is None:
                self.process.kill()
            self.process.wait()
            self.output.close()
            self.temp.cleanup()
            raise

    def connect(self):
        return socket.create_connection(('127.0.0.1', self.port), timeout=3)

    def wait_reason(self, reason):
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            if any(json.loads(line).get('reason') == reason
                   for line in self.log_path.read_text().splitlines() if line.startswith('{')):
                return
            time.sleep(0.01)
        raise AssertionError('missing reason ' + reason + ': ' + self.log_path.read_text())

    def __exit__(self, *exc):
        try:
            if self.process.poll() is None:
                self.process.send_signal(signal.SIGTERM)
            try:
                result = self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
                raise AssertionError('server failed to exit within 5s')
            output = self.log_path.read_text()
            self.events = [json.loads(line) for line in output.splitlines() if line.startswith('{')]
            assert result == 0, output
            assert '"event":"stopped"' in output, output
            assert 'AddressSanitizer' not in output and 'runtime error:' not in output, output
        finally:
            self.output.close()
            self.temp.cleanup()


class TcpIntegration(unittest.TestCase):
    def test_request_ack_and_client(self):
        with RunningServer() as server:
            with server.connect() as sock:
                sock.sendall(request(18446744073709551615))
                ack(sock, 18446744073709551615)
            result = subprocess.run([CLIENT, '127.0.0.1', str(server.port), '123'],
                                    capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('request_id=123 result=OK', result.stdout)

    def test_fragmented_header_and_body(self):
        with RunningServer() as server, server.connect() as sock:
            wire = request(7)
            for part in (wire[:1], wire[1:3], wire[3:4], wire[4:6], wire[6:]):
                sock.sendall(part)
            ack(sock, 7)

    def test_coalesced_and_incomplete_tail(self):
        with RunningServer() as server, server.connect() as sock:
            tail = request(3)
            sock.sendall(request(1) + request(2) + tail[:6])
            ack(sock, 1)
            ack(sock, 2)
            sock.sendall(tail[6:])
            ack(sock, 3)

    def test_oversize_header_only(self):
        with RunningServer() as server, server.connect() as sock:
            sock.sendall(struct.pack('!I', 0xffffffff))
            closed(sock)
            server.wait_reason('frame_too_large')
            with server.connect() as healthy:
                healthy.sendall(request(4))
                ack(healthy, 4)

    def test_empty_frame(self):
        with RunningServer() as server, server.connect() as sock:
            sock.sendall(struct.pack('!I', 0))
            closed(sock)
            server.wait_reason('empty_frame')

    def test_invalid_protobuf(self):
        with RunningServer() as server, server.connect() as sock:
            sock.sendall(frame(b'\x12\x05x'))
            closed(sock)
            server.wait_reason('invalid_protobuf')

    def test_unknown_type_and_business_validation(self):
        with RunningServer() as server, server.connect() as sock:
            sock.sendall(request(10, kind=999) + request(11, device='') + request(12))
            ack(sock, 10, pb.UNKNOWN_MESSAGE_TYPE)
            ack(sock, 11, pb.INVALID_REQUEST)
            ack(sock, 12)

    def test_peer_disconnects_mid_header_mid_body_and_during_reply(self):
        with RunningServer() as server:
            for wire in (b'\x00\x00', struct.pack('!I', 100) + b'partial', request(5) * 10):
                with server.connect() as sock:
                    sock.sendall(wire)
            with server.connect() as sock:
                sock.sendall(request(99))
                ack(sock, 99)

    def test_multiple_connections(self):
        with RunningServer() as server:
            def one(rid):
                with server.connect() as sock:
                    for _ in range(5):
                        sock.sendall(request(rid))
                        ack(sock, rid)
            with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
                list(pool.map(one, range(1, 25)))

    def test_receive_capacity_header_only(self):
        with RunningServer(max_body_bytes=1024, max_receive_body_bytes=32) as server:
            with server.connect() as sock:
                sock.sendall(struct.pack('!I', 33))
                closed(sock)
                server.wait_reason('receive_capacity')
            with server.connect() as sock:
                sock.sendall(request(8))
                ack(sock, 8)

    def test_frame_limit_boundary(self):
        body = pb.Request(request_id=88, device_id='r', message_type=pb.TELEMETRY,
                          payload=b'x' * 128).SerializeToString()
        with RunningServer(max_body_bytes=len(body), max_receive_body_bytes=len(body)) as server:
            with server.connect() as sock:
                sock.sendall(frame(body))
                ack(sock, 88)
            with server.connect() as sock:
                sock.sendall(struct.pack('!I', len(body) + 1))
                closed(sock)

    def test_connection_capacity(self):
        with RunningServer(max_connections=1) as server, server.connect() as first:
            first.sendall(request(1))
            ack(first, 1)  # proves the first connection has been registered
            with server.connect() as second:
                closed(second)
                server.wait_reason('connection_capacity')
            first.sendall(request(2))
            ack(first, 2)

    def test_duplicate_ids_are_processed_again(self):
        with RunningServer() as server, server.connect() as sock:
            sock.sendall(request(21) + request(21))
            ack(sock, 21)
            ack(sock, 21)
        stats = next(event for event in server.events if event['event'] == 'statistics')
        self.assertEqual(stats['processed'], 2)
        self.assertEqual(stats['payload_bytes'], 6)

    def test_send_capacity_single_ack_too_large(self):
        # Deterministic queue rejection; does not depend on kernel send-buffer fullness.
        with RunningServer(max_send_bytes=1) as server, server.connect() as sock:
            sock.sendall(request(9))
            closed(sock)
            server.wait_reason('send_capacity')

    def test_signal_exit_with_live_partial_reads(self):
        for sig in (signal.SIGINT, signal.SIGTERM):
            with RunningServer() as server, server.connect() as a, server.connect() as b:
                a.sendall(b'\x00')
                b.sendall(struct.pack('!I', 100) + b'half')
                server.process.send_signal(sig)
                self.assertEqual(server.process.wait(timeout=5), 0)
                closed(a)
                closed(b)

    def test_bad_config_fails(self):
        with tempfile.NamedTemporaryFile(mode='w', suffix='.json') as f:
            json.dump({'max_body_bytes': -1}, f)
            f.flush()
            result = subprocess.run([SERVER, f.name], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 1)
            self.assertIn('invalid config field', result.stdout)


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]], verbosity=2)
