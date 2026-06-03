#!/usr/bin/env python3
"""Quiet ESP32 hardware integration test.

The full test flashes a factory image over UART, moves the host onto the ESP32
AP, performs OTA, confirms the new image, verifies mocked GNSS routing, and
then makes a best-effort check for live GNSS data.
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import queue
import re
import shlex
import shutil
import socket
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from typing import Callable, Iterable
from urllib import error, request
from urllib.parse import urlparse


PROJECT_BIN = "esp32_wifi_stream.bin"
NORMAL_BOOT_MARKER = "ALL_TASKS_STARTED"
DEFAULT_BASE_URL = "http://192.168.1.1"
DEFAULT_SERIAL_BAUD = 115200
DEFAULT_WIFI_IFACE = "wlan0"
DEFAULT_WIFI_SSID = "AgroLine-GNSS"
DEFAULT_WIFI_PASSWORD = "agroline123"
VERSION_HASH_RE = r"[0-9a-f]{8,64}"


class TestFailure(RuntimeError):
    def __init__(self, step: str, reason: str, log_path: Path | None = None) -> None:
        super().__init__(reason)
        self.step = step
        self.log_path = log_path


class StatusReporter:
    def __init__(self) -> None:
        progress = os.environ.get("ESP_TEST_PROGRESS", "auto").lower()
        if progress in ("0", "false", "no", "off"):
            self.progress_enabled = False
        elif progress in ("1", "true", "yes", "on", "always"):
            self.progress_enabled = True
        else:
            self.progress_enabled = sys.stdout.isatty()

        self._lock = threading.Lock()
        self._status_line_len = 0

    def working(self, step: str, detail: str | None = None) -> "BusyIndicator":
        return BusyIndicator(self, step, detail)

    def success(self, step: str, detail: str | None = None) -> None:
        self._line(step, "success", detail)

    def warning(self, step: str, detail: str) -> None:
        self._line(step, "warning", detail)

    def failure(self, step: str, detail: str, log_path: Path | None = None) -> None:
        self._line(step, "failed", detail)
        if log_path is not None:
            print(f"  log: {log_path}", flush=True)

    def _line(self, step: str, status: str, detail: str | None = None) -> None:
        with self._lock:
            self._clear_status_line_locked()

            if not detail:
                print(f"{step}: {status}", flush=True)
            elif detail.startswith("and "):
                print(f"{step}: {status} {detail}", flush=True)
            else:
                print(f"{step}: {status} ({detail})", flush=True)

    def _show_working(self, line: str) -> None:
        if not self.progress_enabled:
            return

        terminal_width = shutil.get_terminal_size((100, 20)).columns
        if terminal_width > 1 and len(line) >= terminal_width:
            line = line[: terminal_width - 2]

        with self._lock:
            padding = " " * max(0, self._status_line_len - len(line))
            sys.stdout.write(f"\r{line}{padding}")
            sys.stdout.flush()
            self._status_line_len = len(line)

    def _clear_status_line(self) -> None:
        with self._lock:
            self._clear_status_line_locked()

    def _clear_status_line_locked(self) -> None:
        if self.progress_enabled and self._status_line_len > 0:
            sys.stdout.write("\r" + (" " * self._status_line_len) + "\r")
            sys.stdout.flush()
            self._status_line_len = 0


class BusyIndicator:
    def __init__(self, reporter: StatusReporter, step: str, detail: str | None) -> None:
        self.reporter = reporter
        self.step = step
        self.detail = detail
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    def __enter__(self) -> "BusyIndicator":
        if self.reporter.progress_enabled:
            self._thread = threading.Thread(target=self._run, daemon=True)
            self._thread.start()
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=1.0)
        self.reporter._clear_status_line()

    def _run(self) -> None:
        frames = ("Working.", "Working..", "Working...")
        start = time.monotonic()
        index = 0

        while not self._stop.is_set():
            elapsed = int(time.monotonic() - start)
            detail = f" ({self.detail})" if self.detail else ""
            self.reporter._show_working(f"{self.step}: {frames[index]}{detail} {elapsed}s")
            index = (index + 1) % len(frames)
            self._stop.wait(0.5)

        self.reporter._clear_status_line()

@dataclass(frozen=True)
class FirmwareVariant:
    flavor: str
    build_dir: Path
    bin_path: Path
    sha256: str
    git_tag: str

    def version_regex(self, prefix: str) -> re.Pattern[str]:
        return re.compile(
            rf"{re.escape(prefix)}{re.escape(self.git_tag)}\+"
            rf"{VERSION_HASH_RE}-{re.escape(self.flavor)}"
        )


def project_root() -> Path:
    return Path(__file__).resolve().parents[1]


def command_text(command: Iterable[object]) -> str:
    return shlex.join(str(part) for part in command)


def append_log(log_path: Path, text: str) -> None:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("a", encoding="utf-8") as log_file:
        log_file.write(text)


def run_logged(
    command: list[str],
    *,
    cwd: Path,
    log_path: Path,
    step: str,
    env: dict[str, str] | None = None,
    timeout: float | None = None,
    check: bool = True,
) -> subprocess.CompletedProcess[str]:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("a", encoding="utf-8") as log_file:
        log_file.write(f"\n$ {command_text(command)}\n")
        log_file.flush()
        try:
            result = subprocess.run(
                command,
                cwd=cwd,
                env=env,
                timeout=timeout,
                text=True,
                stdout=log_file,
                stderr=subprocess.STDOUT,
                check=False,
            )
        except subprocess.TimeoutExpired as exc:
            log_file.write(f"\ncommand timed out after {timeout:.1f}s\n")
            raise TestFailure(
                step,
                f"command timed out after {timeout:.0f}s: {command_text(command)}",
                log_path,
            ) from exc

        if check and result.returncode != 0:
            raise TestFailure(
                step,
                f"command failed with exit code {result.returncode}: {command_text(command)}",
                log_path,
            )
        return result


def capture(command: list[str], *, cwd: Path, timeout: float | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=cwd,
        timeout=timeout,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git_describe(root: Path) -> str:
    result = capture(["git", "describe", "--tags", "--always", "--dirty"], cwd=root)
    value = result.stdout.strip()
    return value if result.returncode == 0 and value else "unknown"


def configure_variant(root: Path, build_dir: Path, flavor: str, log_dir: Path) -> None:
    build_dir.parent.mkdir(parents=True, exist_ok=True)
    run_logged(
        [
            "cmake",
            "-S",
            str(root),
            "-B",
            str(build_dir),
            "-G",
            "Ninja",
            f"-DSTREAM_APP_VERSION_FLAVOR={flavor}",
        ],
        cwd=root,
        log_path=log_dir / f"build-{flavor}.log",
        step="Build",
    )


def ninja_build(root: Path, build_dir: Path, flavor: str, log_dir: Path) -> None:
    run_logged(
        ["ninja", "-C", str(build_dir)],
        cwd=root,
        log_path=log_dir / f"build-{flavor}.log",
        step="Build",
    )


def build_variant(root: Path, build_root: Path, flavor: str, log_dir: Path) -> FirmwareVariant:
    build_dir = build_root / flavor
    configure_variant(root, build_dir, flavor, log_dir)
    ninja_build(root, build_dir, flavor, log_dir)

    bin_path = build_dir / PROJECT_BIN
    if not bin_path.exists():
        raise TestFailure("Build", f"expected firmware binary was not produced: {bin_path}")

    return FirmwareVariant(
        flavor=flavor,
        build_dir=build_dir,
        bin_path=bin_path,
        sha256=file_sha256(bin_path),
        git_tag=git_describe(root),
    )


def natural_key(path: str) -> list[object]:
    return [int(part) if part.isdigit() else part for part in re.split(r"(\d+)", path)]


def serial_candidates() -> list[str]:
    candidates: list[str] = []
    for prefix in ("/dev/ttyUSB", "/dev/ttyACM"):
        for index in range(100):
            path = f"{prefix}{index}"
            if os.path.exists(path):
                candidates.append(path)
    return sorted(candidates, key=natural_key)


def esptool_probe(port: str, baud: int, timeout: float, log_dir: Path) -> bool:
    command = [
        sys.executable,
        "-m",
        "esptool",
        "--chip",
        "esp32",
        "--port",
        port,
        "--baud",
        str(baud),
        "chip_id",
    ]
    result = run_logged(
        command,
        cwd=project_root(),
        log_path=log_dir / "serial-probe.log",
        step="Serial-available",
        timeout=timeout,
        check=False,
    )
    return result.returncode == 0


def discover_port(explicit_port: str | None, baud: int, probe_timeout: float, log_dir: Path) -> str:
    if explicit_port:
        if not os.path.exists(explicit_port):
            raise TestFailure("Serial-available", f"requested serial port does not exist: {explicit_port}")
        return explicit_port

    candidates = serial_candidates()
    log_path = log_dir / "serial-probe.log"
    append_log(log_path, f"serial candidates: {', '.join(candidates) or '<none>'}\n")
    if not candidates:
        raise TestFailure(
            "Serial-available",
            "no serial device found under /dev/ttyUSB0-99 or /dev/ttyACM0-99",
            log_path,
        )

    for candidate in candidates:
        if esptool_probe(candidate, baud, probe_timeout, log_dir):
            return candidate

    if len(candidates) == 1:
        return candidates[0]

    raise TestFailure(
        "Serial-available",
        "multiple serial devices exist and none probed as ESP32; rerun with PORT=/dev/ttyUSBn",
        log_path,
    )


def import_serial_module():
    try:
        import serial  # type: ignore
    except ImportError as exc:
        raise TestFailure(
            "Serial-available",
            "pyserial is required for UART log checks; run through the Makefile target",
        ) from exc
    return serial


class SerialCapture:
    def __init__(self, port: str, baud: int, log_path: Path) -> None:
        self.port = port
        self.baud = baud
        self.log_path = log_path
        self.serial = None

    def __enter__(self) -> "SerialCapture":
        serial = import_serial_module()
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        try:
            self.serial = serial.Serial(port=None, baudrate=self.baud, timeout=0.2)
            self.serial.dtr = False
            self.serial.rts = False
            self.serial.port = self.port
            self.serial.open()
            self.serial.dtr = False
            self.serial.rts = False
        except Exception as exc:
            raise TestFailure("Serial-available", f"could not open {self.port}: {exc}", self.log_path) from exc
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        if self.serial is not None:
            self.serial.close()

    def discard_input(self) -> None:
        assert self.serial is not None
        self.serial.reset_input_buffer()

    def reset_to_app(self) -> None:
        assert self.serial is not None
        self.serial.reset_input_buffer()
        self.serial.dtr = False
        self.serial.rts = True
        time.sleep(0.12)
        self.serial.rts = False
        time.sleep(0.25)

    def wait_for_boot(self, firmware: FirmwareVariant, timeout: float, step: str) -> str:
        assert self.serial is not None
        deadline = time.monotonic() + timeout
        transcript = ""
        seen_marker = False
        version: str | None = None
        version_re = firmware.version_regex("version:")

        with self.log_path.open("ab") as log_file:
            while time.monotonic() < deadline:
                chunk = self.serial.read(4096)
                if not chunk:
                    continue

                log_file.write(chunk)
                log_file.flush()

                text = chunk.decode("utf-8", errors="replace")
                transcript = (transcript + text)[-200_000:]

                match = version_re.search(transcript)
                if match:
                    version = match.group(0).removeprefix("version:")

                if NORMAL_BOOT_MARKER in transcript:
                    seen_marker = True

                if version and seen_marker:
                    return version

        missing: list[str] = []
        if version is None:
            missing.append(f"version:<git>+<elf-sha>-{firmware.flavor}")
        if not seen_marker:
            missing.append(NORMAL_BOOT_MARKER)
        raise TestFailure(step, f"UART boot markers not found before timeout: {', '.join(missing)}", self.log_path)


def flash_factory(root: Path, firmware: FirmwareVariant, port: str, baud: int | None, log_dir: Path) -> None:
    env = os.environ.copy()
    env["ESPPORT"] = port
    if baud:
        env["ESPBAUD"] = str(baud)
    run_logged(
        ["ninja", "-C", str(firmware.build_dir), "flash"],
        cwd=root,
        env=env,
        log_path=log_dir / "uart-flash.log",
        step="Upload",
    )


def http_request(
    base_url: str,
    path: str,
    *,
    method: str = "GET",
    data: bytes | None = None,
    headers: dict[str, str] | None = None,
    timeout: float = 10.0,
) -> tuple[int, str]:
    url = base_url.rstrip("/") + path
    req = request.Request(url, data=data, headers=headers or {}, method=method)
    try:
        with request.urlopen(req, timeout=timeout) as response:
            return response.status, response.read().decode("utf-8", errors="replace")
    except error.HTTPError as exc:
        return exc.code, exc.read().decode("utf-8", errors="replace")


def status_version_from_body(body: str, firmware: FirmwareVariant) -> str | None:
    match = firmware.version_regex("version=").search(body)
    if not match:
        return None
    return match.group(0).removeprefix("version=")


def wait_for_http_available(base_url: str, timeout: float, log_dir: Path) -> None:
    deadline = time.monotonic() + timeout
    log_path = log_dir / "network-status.log"
    last_error = ""

    while time.monotonic() < deadline:
        try:
            status, body = http_request(base_url, "/flash/status", timeout=5)
            append_log(log_path, f"\nHTTP {status} /flash/status\n{body}\n")
            if status == 200:
                return
            last_error = f"HTTP {status}: {body.strip()}"
        except OSError as exc:
            last_error = str(exc)
            append_log(log_path, f"\n{last_error}\n")
        time.sleep(1.0)

    raise TestFailure("Network available", f"could not reach {base_url.rstrip('/')}/flash/status: {last_error}", log_path)


def wait_for_status(
    base_url: str,
    timeout: float,
    *,
    log_dir: Path,
    step: str,
    expected_firmware: FirmwareVariant | None = None,
    expected_pending: str | None = None,
) -> str:
    deadline = time.monotonic() + timeout
    log_path = log_dir / "flash-status.log"
    last_error = ""

    while time.monotonic() < deadline:
        try:
            status, body = http_request(base_url, "/flash/status", timeout=5)
            append_log(log_path, f"\nHTTP {status} /flash/status\n{body}\n")
            if status != 200:
                last_error = f"HTTP {status}: {body.strip()}"
            elif expected_firmware and status_version_from_body(body, expected_firmware) is None:
                last_error = f"status did not report flavor {expected_firmware.flavor}: {body.strip()}"
            elif expected_pending and f"confirmation_pending={expected_pending}" not in body:
                last_error = f"status did not report confirmation_pending={expected_pending}: {body.strip()}"
            else:
                if expected_firmware:
                    version = status_version_from_body(body, expected_firmware)
                    if version:
                        return version
                return ""
        except OSError as exc:
            last_error = str(exc)
            append_log(log_path, f"\n{last_error}\n")

        time.sleep(1.0)

    raise TestFailure(step, f"could not verify /flash/status before timeout: {last_error}", log_path)


def upload_ota(base_url: str, firmware: FirmwareVariant, timeout: float, log_dir: Path) -> None:
    log_path = log_dir / "ota-upload.log"
    payload = firmware.bin_path.read_bytes()
    try:
        status, body = http_request(
            base_url,
            "/flash",
            method="POST",
            data=payload,
            headers={
                "Content-Type": "application/octet-stream",
                "X-Firmware-SHA256": firmware.sha256,
            },
            timeout=timeout,
        )
    except OSError as exc:
        append_log(log_path, f"POST /flash socket failure: {exc}\n")
        raise TestFailure("OTA flash", f"upload request failed: {exc}", log_path) from exc

    append_log(log_path, f"HTTP {status} POST /flash\n{body}\n")
    if status != 200:
        raise TestFailure("OTA flash", f"OTA upload failed with HTTP {status}: {body.strip()}", log_path)


def confirm_ota(base_url: str, timeout: float, log_dir: Path) -> None:
    log_path = log_dir / "ota-confirm.log"
    try:
        status, body = http_request(base_url, "/flash/confirm", method="POST", data=b"", timeout=timeout)
    except OSError as exc:
        raise TestFailure("Binary confirmation", f"confirmation request failed: {exc}", log_path) from exc

    append_log(log_path, f"HTTP {status} POST /flash/confirm\n{body}\n")
    if status != 200:
        raise TestFailure("Binary confirmation", f"OTA confirmation failed with HTTP {status}: {body.strip()}", log_path)


def root_command(command: list[str]) -> list[str]:
    if os.geteuid() == 0:
        return command
    return ["sudo", "-n", *command]


def require_root_access(root: Path, log_dir: Path) -> None:
    if os.geteuid() == 0:
        return
    if shutil.which("sudo") is None:
        raise TestFailure(
            "Network available",
            "Wi-Fi management requires root; rerun with sudo -E or preconnect and use --no-manage-wifi",
        )
    result = run_logged(
        ["sudo", "-n", "true"],
        cwd=root,
        log_path=log_dir / "network.log",
        step="Network available",
        check=False,
    )
    if result.returncode != 0:
        raise TestFailure(
            "Network available",
            "Wi-Fi management requires passwordless sudo/root; rerun with sudo -E or preconnect and use --no-manage-wifi",
            log_dir / "network.log",
        )


def shell_quote_wpa(value: str) -> str:
    return value.replace("\\", "\\\\").replace('"', '\\"')


def write_wpa_config(path: Path, ssid: str, password: str) -> None:
    if len(password) < 8:
        raise TestFailure("Network available", "WPA2 password must be at least 8 characters")

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        "ctrl_interface=/run/wpa_supplicant\n"
        "update_config=0\n"
        "network={\n"
        f'    ssid="{shell_quote_wpa(ssid)}"\n'
        f'    psk="{shell_quote_wpa(password)}"\n'
        "    key_mgmt=WPA-PSK\n"
        "}\n",
        encoding="utf-8",
    )
    path.chmod(0o600)


def connect_wifi(
    root: Path,
    iface: str,
    ssid: str,
    password: str,
    base_url: str,
    log_dir: Path,
    timeout: float,
) -> None:
    log_path = log_dir / "network.log"
    for required in ("ip", "wpa_supplicant"):
        if shutil.which(required) is None:
            raise TestFailure("Network available", f"required network command is missing: {required}", log_path)

    dhcp_command = "dhcpcd" if shutil.which("dhcpcd") else "dhclient" if shutil.which("dhclient") else None
    if dhcp_command is None:
        raise TestFailure("Network available", "required DHCP command is missing: dhcpcd or dhclient", log_path)

    require_root_access(root, log_dir)

    wpa_config = log_dir / "wifi-test-wpa.conf"
    wpa_pid = log_dir / "wifi-test-wpa.pid"
    wpa_log = log_dir / "wifi-test-wpa.log"
    write_wpa_config(wpa_config, ssid, password)

    if shutil.which("systemctl"):
        run_logged(root_command(["systemctl", "stop", f"wpa_supplicant@{iface}.service"]), cwd=root, log_path=log_path, step="Network available", check=False)
    if shutil.which("pkill"):
        pattern = f"wpa_supplicant.*-i ?{re.escape(iface)}"
        run_logged(root_command(["pkill", "-f", pattern]), cwd=root, log_path=log_path, step="Network available", check=False)

    if dhcp_command == "dhcpcd":
        run_logged(root_command(["dhcpcd", "-x", iface]), cwd=root, log_path=log_path, step="Network available", check=False)
    else:
        run_logged(root_command(["dhclient", "-r", iface]), cwd=root, log_path=log_path, step="Network available", check=False)

    run_logged(root_command(["ip", "link", "set", iface, "up"]), cwd=root, log_path=log_path, step="Network available")
    run_logged(root_command(["ip", "addr", "flush", "dev", iface]), cwd=root, log_path=log_path, step="Network available", check=False)
    run_logged(
        root_command([
            "wpa_supplicant",
            "-B",
            "-i",
            iface,
            "-c",
            str(wpa_config),
            "-P",
            str(wpa_pid),
            "-f",
            str(wpa_log),
        ]),
        cwd=root,
        log_path=log_path,
        step="Network available",
    )

    if dhcp_command == "dhcpcd":
        run_logged(
            root_command(["dhcpcd", "-q", "-w", "-G", iface]),
            cwd=root,
            log_path=log_path,
            step="Network available",
            timeout=timeout,
        )
    else:
        run_logged(
            root_command(["dhclient", "-v", iface]),
            cwd=root,
            log_path=log_path,
            step="Network available",
            timeout=timeout,
        )

    host, _, _ = parse_base_url(base_url)
    if re.fullmatch(r"\d{1,3}(?:\.\d{1,3}){3}", host):
        run_logged(
            root_command(["ip", "route", "replace", f"{host}/32", "dev", iface]),
            cwd=root,
            log_path=log_path,
            step="Network available",
        )


def ensure_network(args: argparse.Namespace, root: Path, log_dir: Path) -> None:
    if not args.no_manage_wifi:
        connect_wifi(
            root,
            args.wifi_interface,
            args.wifi_ssid,
            args.wifi_password,
            args.base_url,
            log_dir,
            args.network_timeout,
        )
    wait_for_http_available(args.base_url, args.http_timeout, log_dir)


def parse_base_url(base_url: str) -> tuple[str, int, str]:
    parsed = urlparse(base_url)
    if parsed.scheme != "http" or not parsed.hostname:
        raise TestFailure("Network available", f"unsupported base URL: {base_url}")
    base_path = parsed.path.rstrip("/")
    return parsed.hostname, parsed.port or 80, base_path


def http_path(base_path: str, path: str) -> str:
    joined = f"{base_path}{path}" if base_path else path
    return joined or "/"


def stream_until(
    base_url: str,
    path: str,
    predicate: Callable[[str], bool],
    timeout: float,
    log_path: Path,
    step: str,
) -> str:
    host, port, base_path = parse_base_url(base_url)
    deadline = time.monotonic() + timeout
    buffer = b""

    log_path.parent.mkdir(parents=True, exist_ok=True)
    try:
        with socket.create_connection((host, port), timeout=5) as sock, log_path.open("ab") as log_file:
            sock.settimeout(1.0)
            request_line = (
                f"GET {http_path(base_path, path)} HTTP/1.1\r\n"
                f"Host: {host}\r\n"
                "Connection: close\r\n"
                "\r\n"
            )
            sock.sendall(request_line.encode("ascii"))
            while time.monotonic() < deadline:
                try:
                    data = sock.recv(4096)
                except socket.timeout:
                    continue

                if not data:
                    break

                log_file.write(data)
                log_file.flush()
                buffer = (buffer + data)[-200_000:]
                text = buffer.decode("utf-8", errors="replace")
                if predicate(text):
                    return text
    except OSError as exc:
        raise TestFailure(step, f"stream request failed: {exc}", log_path) from exc

    raise TestFailure(step, "expected stream data was not observed before timeout", log_path)


def run_stream_thread(
    base_url: str,
    predicate: Callable[[str], bool],
    timeout: float,
    log_path: Path,
    step: str,
) -> tuple[threading.Thread, queue.Queue[object]]:
    result_queue: queue.Queue[object] = queue.Queue(maxsize=1)

    def target() -> None:
        try:
            result_queue.put(stream_until(base_url, "/stream", predicate, timeout, log_path, step))
        except TestFailure as exc:
            result_queue.put(exc)

    thread = threading.Thread(target=target, daemon=True)
    thread.start()
    return thread, result_queue


def nmea_sentence(payload: str) -> bytes:
    checksum = 0
    for byte in payload.encode("ascii"):
        checksum ^= byte
    return f"${payload}*{checksum:02X}\r\n".encode("ascii")


def make_mock_route() -> tuple[list[bytes], set[str]]:
    now = time.gmtime()
    base_seconds = (now.tm_hour * 3600 + now.tm_min * 60 + now.tm_sec) % 86400
    date = time.strftime("%d%m%y", now)
    points = [
        ("3723.2475", "12158.3416", "1.4"),
        ("3723.2480", "12158.3420", "1.0"),
        ("3723.2485", "12158.3425", "0.6"),
    ]
    chunks: list[bytes] = []
    expected: set[str] = set()

    for index, (lat, lon, hdop) in enumerate(points):
        seconds = (base_seconds + index) % 86400
        timestamp = f"{seconds // 3600:02d}{(seconds // 60) % 60:02d}{seconds % 60:02d}"
        gga = nmea_sentence(
            f"GPGGA,{timestamp},{lat},N,{lon},W,1,08,{hdop},545.4,M,46.9,M,,"
        )
        rmc = nmea_sentence(
            f"GPRMC,{timestamp},A,{lat},N,{lon},W,000.5,084.4,{date},003.1,W"
        )
        chunks.append(gga + rmc)
        expected.add(gga.decode("ascii").strip())
        expected.add(rmc.decode("ascii").strip())

    return chunks, expected


def post_paced_body(
    base_url: str,
    path: str,
    chunks: list[bytes],
    interval: float,
    timeout: float,
    log_path: Path,
) -> str:
    host, port, base_path = parse_base_url(base_url)
    body_len = sum(len(chunk) for chunk in chunks)
    deadline = time.monotonic() + timeout
    response = b""

    log_path.parent.mkdir(parents=True, exist_ok=True)
    try:
        with socket.create_connection((host, port), timeout=5) as sock, log_path.open("ab") as log_file:
            sock.settimeout(1.0)
            headers = (
                f"POST {http_path(base_path, path)} HTTP/1.1\r\n"
                f"Host: {host}\r\n"
                "Content-Type: application/octet-stream\r\n"
                f"Content-Length: {body_len}\r\n"
                "Connection: close\r\n"
                "\r\n"
            )
            sock.sendall(headers.encode("ascii"))
            for index, chunk in enumerate(chunks):
                sock.sendall(chunk)
                if interval > 0 and index < len(chunks) - 1:
                    time.sleep(interval)

            while time.monotonic() < deadline:
                try:
                    data = sock.recv(4096)
                except socket.timeout:
                    continue
                if not data:
                    break
                response += data
                log_file.write(data)
                log_file.flush()
                text = response.decode("utf-8", errors="replace")
                if "GNSS mock stream accepted" in text:
                    return text
    except OSError as exc:
        raise TestFailure("GNSS mocking", f"mock upload failed: {exc}", log_path) from exc

    text = response.decode("utf-8", errors="replace")
    if "GNSS mock stream accepted" not in text:
        raise TestFailure("GNSS mocking", "mock upload did not receive the expected HTTP response", log_path)
    return text


def run_gnss_mock(base_url: str, log_dir: Path, stream_timeout: float, upload_timeout: float) -> set[str]:
    chunks, expected = make_mock_route()

    def has_mock_sentence(text: str) -> bool:
        return any(sentence in text for sentence in expected)

    stream_thread, stream_result = run_stream_thread(
        base_url,
        has_mock_sentence,
        stream_timeout,
        log_dir / "gnss-mock-stream.log",
        "GNSS mocking",
    )
    time.sleep(0.75)
    post_paced_body(base_url, "/write", chunks, interval=0.35, timeout=upload_timeout, log_path=log_dir / "gnss-mock-upload.log")

    stream_thread.join(stream_timeout + 2.0)
    try:
        result = stream_result.get_nowait()
    except queue.Empty as exc:
        raise TestFailure("GNSS mocking", "mock stream reader did not finish before timeout", log_dir / "gnss-mock-stream.log") from exc

    if isinstance(result, TestFailure):
        raise result
    return expected


def run_real_gnss(base_url: str, log_dir: Path, ignored_mock_sentences: set[str], timeout: float) -> bool:
    nmea_re = re.compile(r"\$(?:GP|GN|GL|GA|GB)[A-Z]{3},[^\r\n]*\*[0-9A-Fa-f]{2}")

    def has_non_mock_nmea(text: str) -> bool:
        for match in nmea_re.finditer(text):
            if match.group(0) not in ignored_mock_sentences:
                return True
        return False

    stream_until(
        base_url,
        "/stream",
        has_non_mock_nmea,
        timeout,
        log_dir / "gnss-real-stream.log",
        "real GNSS",
    )
    return True


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build and run ESP32 UART/OTA/GNSS hardware tests")
    parser.add_argument("--build-root", type=Path, default=project_root() / "build" / "hardware-tests")
    parser.add_argument("--factory-flavor", default="factory-test")
    parser.add_argument("--ota-flavor", default="main")
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument("--skip-ota", action="store_true")
    parser.add_argument("--skip-gnss", action="store_true")
    parser.add_argument("--no-manage-wifi", action="store_true")
    parser.add_argument("--port", default=os.environ.get("ESP_TEST_PORT") or os.environ.get("PORT"))
    parser.add_argument("--baud", type=int, default=int(os.environ.get("ESP_TEST_BAUD", DEFAULT_SERIAL_BAUD)))
    parser.add_argument("--base-url", default=os.environ.get("ESP_TEST_BASE_URL", DEFAULT_BASE_URL))
    parser.add_argument("--wifi-interface", default=os.environ.get("ESP_TEST_WIFI_IFACE", DEFAULT_WIFI_IFACE))
    parser.add_argument("--wifi-ssid", default=os.environ.get("ESP_TEST_WIFI_SSID", DEFAULT_WIFI_SSID))
    parser.add_argument("--wifi-password", default=os.environ.get("ESP_TEST_WIFI_PASSWORD", DEFAULT_WIFI_PASSWORD))
    parser.add_argument("--boot-timeout", type=float, default=float(os.environ.get("ESP_TEST_BOOT_TIMEOUT", 45)))
    parser.add_argument("--http-timeout", type=float, default=float(os.environ.get("ESP_TEST_HTTP_TIMEOUT", 120)))
    parser.add_argument("--upload-timeout", type=float, default=float(os.environ.get("ESP_TEST_UPLOAD_TIMEOUT", 180)))
    parser.add_argument("--probe-timeout", type=float, default=float(os.environ.get("ESP_TEST_PROBE_TIMEOUT", 12)))
    parser.add_argument("--network-timeout", type=float, default=float(os.environ.get("ESP_TEST_NETWORK_TIMEOUT", 45)))
    parser.add_argument("--mock-stream-timeout", type=float, default=float(os.environ.get("ESP_TEST_MOCK_STREAM_TIMEOUT", 20)))
    parser.add_argument("--mock-upload-timeout", type=float, default=float(os.environ.get("ESP_TEST_MOCK_UPLOAD_TIMEOUT", 30)))
    parser.add_argument("--real-gnss-timeout", type=float, default=float(os.environ.get("ESP_TEST_REAL_GNSS_TIMEOUT", 20)))
    parser.add_argument("--log-dir", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = project_root()
    build_root = args.build_root.resolve()
    log_dir = (args.log_dir or build_root / "logs").resolve()
    reporter = StatusReporter()

    if "IDF_PATH" not in os.environ:
        raise TestFailure("Build", "IDF_PATH is not set; run through the Makefile target")

    with reporter.working("Build"):
        factory = build_variant(root, build_root, args.factory_flavor, log_dir)
        ota = build_variant(root, build_root, args.ota_flavor, log_dir)
    reporter.success("Build")

    if args.build_only:
        return 0

    with reporter.working("Serial-available"):
        port = discover_port(args.port, args.baud, args.probe_timeout, log_dir)
    reporter.success("Serial-available", port)

    with reporter.working("Upload", "UART flash"):
        flash_factory(root, factory, port, args.baud, log_dir)
    reporter.success("Upload")

    with reporter.working("First-boot", "UART log"):
        with SerialCapture(port, args.baud, log_dir / "factory-boot.log") as serial_log:
            serial_log.reset_to_app()
            factory_version = serial_log.wait_for_boot(factory, args.boot_timeout, "First-boot")
    reporter.success("First-boot", f"version:{factory_version}")

    if args.skip_ota:
        return 0

    with reporter.working("Network available", args.wifi_ssid if not args.no_manage_wifi else "manual Wi-Fi"):
        ensure_network(args, root, log_dir)
        wait_for_status(
            args.base_url,
            args.http_timeout,
            log_dir=log_dir,
            step="Network available",
            expected_firmware=factory,
            expected_pending="no",
        )
    reporter.success("Network available", "and connected")

    with SerialCapture(port, args.baud, log_dir / "ota-boot.log") as serial_log:
        serial_log.discard_input()
        with reporter.working("OTA flash", "/flash"):
            upload_ota(args.base_url, ota, args.upload_timeout, log_dir)
        reporter.success("OTA flash")
        with reporter.working("Second-boot", "UART log"):
            ota_version = serial_log.wait_for_boot(ota, args.boot_timeout, "Second-boot")
    reporter.success("Second-boot", f"version:{ota_version}")

    with reporter.working("Binary confirmation", "/flash/confirm"):
        wait_for_status(
            args.base_url,
            args.http_timeout,
            log_dir=log_dir,
            step="Binary confirmation",
            expected_firmware=ota,
            expected_pending="yes",
        )
        confirm_ota(args.base_url, args.http_timeout, log_dir)
        wait_for_status(
            args.base_url,
            args.http_timeout,
            log_dir=log_dir,
            step="Binary confirmation",
            expected_firmware=ota,
            expected_pending="no",
        )
    reporter.success("Binary confirmation")

    if not args.skip_gnss:
        with reporter.working("GNSS mocking", "/write + /stream"):
            mock_sentences = run_gnss_mock(
                args.base_url,
                log_dir,
                args.mock_stream_timeout,
                args.mock_upload_timeout,
            )
        reporter.success("GNSS mocking")

        try:
            with reporter.working("real GNSS", "/stream"):
                run_real_gnss(args.base_url, log_dir, mock_sentences, args.real_gnss_timeout)
        except TestFailure as exc:
            reporter.warning("real GNSS", "no GNSS data available" if "expected stream data" in str(exc) else str(exc))
        else:
            reporter.success("real GNSS")

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except TestFailure as exc:
        StatusReporter().failure(exc.step, str(exc), exc.log_path)
        raise SystemExit(1)
    except KeyboardInterrupt:
        StatusReporter().failure("Interrupted", "received Ctrl+C")
        raise SystemExit(130)
