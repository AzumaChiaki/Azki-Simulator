"""Bounded JSONL connection to the owned Windows runtime process."""
from __future__ import annotations

import json
import os
import queue
import subprocess
import threading

MAX_LINE = 8 * 1024 * 1024


class RuntimeErrorResponse(RuntimeError):
    def __init__(self, code, message):
        self.code = code
        super().__init__(message)


class RuntimeClient:
    def __init__(self, command, *, cwd, log_file, windows_files=None):
        options = {"creationflags": subprocess.CREATE_NO_WINDOW} if hasattr(subprocess, "CREATE_NO_WINDOW") else {}
        environment = dict(os.environ, PYTHONUTF8="1", PYTHONIOENCODING="utf-8")
        self.job = None
        self.close_report = {}
        self.process = None
        if windows_files is not None:
            from src.windows_runtime_files import OwnedWindowsJob
            self.job = OwnedWindowsJob(windows_files)
        try:
            self.process = subprocess.Popen(command, cwd=cwd, stdin=subprocess.PIPE,
                                            stdout=subprocess.PIPE, stderr=log_file, env=environment, **options)
            if self.job is not None:
                self.job.attach(self.process)
        except BaseException:
            if self.job is not None:
                self.job.close()
            if self.process is not None and self.process.poll() is None:
                self.process.terminate()
                self.process.wait(timeout=5)
            raise
        self.responses = queue.Queue(maxsize=8)
        self.lock = threading.Lock()
        self.next_id = 0
        self.broken = False
        self.closed = False
        self.closing = False
        self.reader = threading.Thread(target=self._read, daemon=True, name="azki-runtime-reader")
        self.reader.start()

    def _read(self):
        try:
            while True:
                line = self.process.stdout.readline(MAX_LINE + 1)
                if not line:
                    raise RuntimeErrorResponse("runtime_closed", "模拟器运行服务已退出")
                if len(line) > MAX_LINE or not line.endswith(b"\n"):
                    raise RuntimeErrorResponse("invalid_response", "运行服务响应超出限制")
                value = json.loads(line)
                if not isinstance(value, dict):
                    raise RuntimeErrorResponse("invalid_response", "运行服务返回了无效响应")
                self.responses.put(value, timeout=5)
        except BaseException as error:
            try:
                self.responses.put(error, timeout=1)
            except queue.Full:
                pass

    def request(self, command, args=None, *, timeout=30):
        with self.lock:
            if self.closed or self.broken or (self.closing and command != "stop"):
                raise RuntimeErrorResponse("runtime_closed", "模拟器运行服务不可用")
            self.next_id += 1
            request_id = self.next_id
            payload = json.dumps({"id": request_id, "command": command, "args": args or {}},
                                 ensure_ascii=True, allow_nan=False).encode() + b"\n"
            if len(payload) > 65536:
                raise ValueError("请求数据超出限制")
            try:
                self.process.stdin.write(payload)
                self.process.stdin.flush()
                response = self.responses.get(timeout=timeout)
                if isinstance(response, BaseException):
                    raise response
                if (type(response.get("id")) is not int or response["id"] != request_id or
                        type(response.get("ok")) is not bool):
                    raise RuntimeErrorResponse("invalid_response", "运行服务响应与请求不一致")
            except Exception as error:
                self.broken = True
                if isinstance(error, queue.Empty):
                    raise RuntimeErrorResponse("runtime_timeout", "等待模拟器响应超时") from error
                raise
            if not response["ok"]:
                error = response.get("error", {})
                if not isinstance(error, dict):
                    error = {}
                raise RuntimeErrorResponse(str(error.get("code", "runtime_error")),
                                           str(error.get("message", "模拟器操作失败")))
            return response.get("result")

    def close(self):
        if self.closed:
            return
        self.closing = True
        self.close_report = {"stopAcknowledged": False, "forced": False}
        # Preparation may be copying a large image. Let the in-flight request
        # finish before stopping; closing rejects any subsequent start request.
        # This runs on the UI's shutdown thread, so the window stays responsive.
        available = self.lock.acquire(timeout=125)
        self.close_report["activeRequestCompleted"] = available
        if available:
            self.lock.release()
        if available and not self.broken and self.process.poll() is None:
            try:
                result = self.request("stop", timeout=45)
                self.close_report.update(stopAcknowledged=True, stopResult=result)
            except Exception as error:
                self.close_report["stopError"] = str(error)
        self.closed = True
        try:
            self.process.stdin.close()
        except (OSError, ValueError):
            pass
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.close_report["forced"] = True
            # The outer job also owns helpers launched during preparation,
            # before the runtime creates its inner QEMU job.
            if self.job is not None:
                self.job.close()
            else:
                self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=3)
        finally:
            if self.job is not None:
                self.job.close()
        self.close_report["ownedRuntimeClosed"] = self.process.poll() is not None
        self.process.stdout.close()
