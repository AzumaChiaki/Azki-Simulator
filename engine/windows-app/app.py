#!/usr/bin/env python3
"""Windows desktop frontend for all catalogued Azki Simulator profiles."""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import queue
import sys
import threading
import time
import uuid

from frames import decode_frame, fit_panel, panel_point
from rpc import RuntimeClient


def default_layout(app_path):
    container = Path(app_path).resolve().parent.parent
    portable = (container / "python").is_dir() or (container / "AzkiSimulator.pyw").is_file()
    core = container / "qemu-system-arm.exe" if portable else container.parent / "dist/windows/qemu-system-arm.exe"
    return container / "project", core


def initialize_workspace(project_root, workspace):
    if os.name == "nt":
        backend = str(project_root / "backend")
        if backend not in sys.path:
            sys.path.insert(0, backend)
        from src.windows_runtime_files import WindowsFiles
        files = WindowsFiles()
        files.initialize_private_creator()
        files.private_directory(workspace, exist_ok=True)
        return files
    else:
        workspace.mkdir(mode=0o700, parents=True, exist_ok=True)


def launch_arguments(profile_id, fields, skip_oobe, mode, device_path="", *, instance_id=None, initialized=False):
    if mode not in ("reject", "winrt", "usb", "serial"):
        raise ValueError("请选择蓝牙通道")
    if mode == "serial":
        raise ValueError("当前版本尚未完成串口 HCI 通道，请选择已验证的蓝牙通道")
    args = {"bluetooth": mode}
    if instance_id:
        args["instanceID"] = instance_id
    if not initialized:
        args.update(profileID=profile_id, skipOOBE=skip_oobe)
    if mode == "usb":
        if not device_path.strip():
            raise ValueError("请选择或填写专供模拟器使用的 WinUSB 蓝牙接口")
        args["hciDevicePath"] = device_path.strip()
    for key in (() if initialized else ("firmwarePath", "resourcePath", "audioPath", "sensorPath")):
        if fields.get(key, "").strip():
            args[key] = fields[key].strip()
    return args


def completion_status(value, *, persistent=False, natural=False):
    status = "模拟器已退出" if natural else "已停止"
    result = value.get("stopResult", value) if isinstance(value, dict) else {}
    persistence = result.get("persistence") if isinstance(result, dict) else None
    if isinstance(persistence, dict) and persistence.get("committed") is True:
        return status + " · 设备数据已保存"
    if persistent:
        return status + " · 本次设备数据未保存，详情见运行记录"
    return status


class SimulatorApp:
    def __init__(self, root, options):
        import tkinter as tk
        from tkinter import ttk
        self.tk, self.ttk = tk, ttk
        self.root, self.options = root, options
        self.closing = self.running = self.frame_pending = False
        self.starting = False
        self.last_frame = self.last_sequence = self.photo = None
        self.panel_rectangle = (0, 0, 1, 1)
        self.pressed = False
        self.last_point = (0, 0)
        self.last_move = 0.0
        self.generation = 0
        self.status_pending = False
        self.bluetooth_panel = None
        self.sensors_panel = None
        self.instances_panel = None
        self.profiles = []
        self.instances = []
        self.form_widgets = []
        self.temporary_fields = {}
        self.selected_instance_id = None
        self.executor = ThreadPoolExecutor(max_workers=2, thread_name_prefix="azki-ui")
        self.completed = queue.SimpleQueue()
        self.windows_files = initialize_workspace(options.project_root, options.workspace)
        self.service_log = (options.workspace / ("ui-" + uuid.uuid4().hex + ".log")).open("xb")
        command = [sys.executable, "-u", "-m", "src.windows_runtime",
                   "--project-root", str(options.project_root), "--qemu", str(options.qemu),
                   "--workspace", str(options.workspace)]
        self.client = RuntimeClient(command, cwd=options.project_root / "backend", log_file=self.service_log,
                                    windows_files=self.windows_files)

        root.title("Azki Simulator · Windows")
        root.geometry("1120x820")
        root.minsize(880, 660)
        root.protocol("WM_DELETE_WINDOW", self.close)
        root.columnconfigure(1, weight=1)
        root.rowconfigure(0, weight=1)
        sidebar_shell = ttk.Frame(root)
        sidebar_shell.grid(row=0, column=0, sticky="ns")
        sidebar_canvas = tk.Canvas(sidebar_shell, width=410, highlightthickness=0)
        sidebar_canvas.pack(side="left", fill="both", expand=True)
        scrollbar = ttk.Scrollbar(sidebar_shell, orient="vertical", command=sidebar_canvas.yview)
        scrollbar.pack(side="right", fill="y")
        sidebar_canvas.configure(yscrollcommand=scrollbar.set)
        sidebar = ttk.Frame(sidebar_canvas, padding=18)
        sidebar_window = sidebar_canvas.create_window(0, 0, window=sidebar, anchor="nw")
        sidebar.bind("<Configure>", lambda _: sidebar_canvas.configure(scrollregion=sidebar_canvas.bbox("all")))
        sidebar_canvas.bind("<Configure>", lambda event: sidebar_canvas.itemconfigure(sidebar_window, width=event.width))
        def scroll_sidebar(event):
            left = sidebar_shell.winfo_rootx()
            if left <= event.x_root < left + sidebar_shell.winfo_width() and event.delta:
                sidebar_canvas.yview_scroll(-1 if event.delta > 0 else 1, "units")
        root.bind("<MouseWheel>", scroll_sidebar, add="+")
        sidebar.columnconfigure(0, weight=1)
        ttk.Label(sidebar, text="Azki Simulator", font=("Segoe UI", 19, "bold")).grid(sticky="w", pady=(0, 4))
        ttk.Label(sidebar, text="Windows", foreground="#666666").grid(sticky="w", pady=(0, 20))
        ttk.Label(sidebar, text="设备与固件版本").grid(sticky="w")
        self.profile = ttk.Combobox(sidebar, state="readonly", width=42)
        self.profile.grid(sticky="ew", pady=(5, 14))
        ttk.Label(sidebar, text="实例").grid(sticky="w")
        self.instance = ttk.Combobox(sidebar, state="readonly", values=("临时会话（不保存）",), width=42)
        self.instance.current(0)
        self.instance.bind("<<ComboboxSelected>>", lambda _: self.choose_instance())
        self.instance.grid(sticky="ew", pady=(4, 6))
        instance_actions = ttk.Frame(sidebar)
        instance_actions.grid(sticky="ew", pady=(0, 6))
        ttk.Button(instance_actions, text="新建实例", command=self.create_instance).pack(side="left")
        ttk.Button(instance_actions, text="刷新", command=self.refresh_instances).pack(side="left", padx=5)
        ttk.Button(instance_actions, text="管理", command=self.manage_instances).pack(side="left")
        self.instance_info = tk.StringVar(value="临时会话结束后不保留设备数据。")
        ttk.Label(sidebar, textvariable=self.instance_info, wraplength=360).grid(sticky="w", pady=(0, 10))
        self.fields = {}
        for key, title in (("firmwarePath", "固件（完整 OTA 或 AP）"),
                           ("resourcePath", "资源镜像（可选）"),
                           ("audioPath", "音频镜像（可选）"),
                           ("sensorPath", "传感器镜像（可选）")):
            ttk.Label(sidebar, text=title).grid(sticky="w")
            row = ttk.Frame(sidebar)
            row.grid(sticky="ew", pady=(4, 10))
            value = tk.StringVar()
            self.fields[key] = value
            entry = ttk.Entry(row, textvariable=value, width=34)
            entry.pack(side="left", fill="x", expand=True)
            browse = ttk.Button(row, text="…", width=3, command=lambda k=key: self.browse(k))
            browse.pack(side="left", padx=(4, 0))
            self.form_widgets.extend((entry, browse))
        self.skip_oobe = tk.BooleanVar(value=True)
        self.skip_oobe_control = ttk.Checkbutton(sidebar, text="新实例跳过首次引导", variable=self.skip_oobe)
        self.skip_oobe_control.grid(sticky="w", pady=(0, 12))
        ttk.Label(sidebar, text="蓝牙通道").grid(sticky="w")
        self.bluetooth_mode = ttk.Combobox(sidebar, state="readonly", width=40,
                                         values=("离线", "Windows 蓝牙桥接", "独占 USB HCI", "串口 HCI"))
        self.bluetooth_mode.current(0)
        self.bluetooth_mode.bind("<<ComboboxSelected>>", lambda _: self.update_bluetooth_mode())
        self.bluetooth_mode.grid(sticky="ew", pady=(4, 12))
        self.usb_path = tk.StringVar()
        self.usb_frame = ttk.Frame(sidebar)
        self.usb_frame.grid(sticky="ew", pady=(0, 10))
        ttk.Label(self.usb_frame, text="专用 WinUSB 控制器接口").pack(anchor="w")
        self.usb_selector = ttk.Combobox(self.usb_frame, textvariable=self.usb_path, width=37)
        self.usb_selector.pack(side="left", fill="x", expand=True, pady=(4, 0))
        ttk.Button(self.usb_frame, text="刷新", command=self.refresh_usb).pack(side="left", padx=(4, 0))
        self.usb_frame.grid_remove()
        actions = ttk.Frame(sidebar)
        actions.grid(sticky="ew", pady=(8, 14))
        self.start_button = ttk.Button(actions, text="启动", command=self.start)
        self.start_button.pack(side="left", expand=True, fill="x")
        self.stop_button = ttk.Button(actions, text="停止", command=self.stop, state="disabled")
        self.stop_button.pack(side="left", expand=True, fill="x", padx=(8, 0))
        ttk.Button(sidebar, text="保存截图", command=self.screenshot).grid(sticky="ew", pady=4)
        ttk.Button(sidebar, text="蓝牙设备与服务", command=self.bluetooth).grid(sticky="ew", pady=4)
        ttk.Button(sidebar, text="模拟传感器输入", command=self.sensors).grid(sticky="ew", pady=4)
        ttk.Button(sidebar, text="运行状态", command=self.inspect_status).grid(sticky="ew", pady=4)
        ttk.Label(sidebar, text="运行记录", foreground="#666666").grid(sticky="w", pady=(18, 3))
        self.log = tk.Text(sidebar, width=43, height=9, wrap="word", state="disabled", font=("Consolas", 9))
        self.log.grid(sticky="nsew")
        sidebar.rowconfigure(sidebar.grid_size()[1] - 1, weight=1)

        display = ttk.Frame(root, padding=(6, 16, 16, 16))
        display.grid(row=0, column=1, sticky="nsew")
        display.columnconfigure(0, weight=1)
        display.rowconfigure(0, weight=1)
        self.canvas = tk.Canvas(display, background="#15171a", highlightthickness=0)
        self.canvas.grid(row=0, column=0, sticky="nsew")
        self.canvas.bind("<Configure>", lambda _: self.redraw())
        self.canvas.bind("<ButtonPress-1>", lambda event: self.pointer(event, True))
        self.canvas.bind("<B1-Motion>", self.move)
        self.canvas.bind("<ButtonRelease-1>", lambda event: self.pointer(event, False))
        self.canvas.bind("<MouseWheel>", self.wheel)
        self.canvas.bind("<FocusOut>", self.release)
        root.bind("<Unmap>", self.release)
        buttons = ttk.Frame(display, padding=(0, 12, 0, 0))
        buttons.grid(row=1, column=0, sticky="ew")
        for title, command, data in (("表冠", "button", {"button": "crown", "clicks": 1}),
                                     ("侧键", "button", {"button": "lower", "clicks": 1}),
                                     ("侧键双击", "button", {"button": "lower", "clicks": 2}),
                                     ("表冠 −", "crown", {"delta": -1}),
                                     ("表冠 +", "crown", {"delta": 1})):
            ttk.Button(buttons, text=title, command=lambda c=command, a=data: self.input(c, a)).pack(side="left", padx=3)
        self.status = tk.StringVar(value="正在读取设备目录…")
        ttk.Label(root, textvariable=self.status, padding=(16, 8)).grid(row=1, column=0, columnspan=2, sticky="ew")
        self._call("profiles", callback=self.loaded_profiles, report=True)
        root.after(15, self.pump)
        root.after(50, self.poll_frame)
        root.after(1000, self.poll_status)

    def record(self, value):
        text = value if isinstance(value, str) else json.dumps(value, ensure_ascii=False, indent=2)
        self.log.configure(state="normal")
        self.log.insert("end", text + "\n")
        if int(self.log.index("end-1c").split(".")[0]) > 250:
            self.log.delete("1.0", "100.0")
        self.log.see("end")
        self.log.configure(state="disabled")

    def _submit(self, work, callback=None, *, report=False, finished=None, error_callback=None):
        if self.closing:
            return
        future = self.executor.submit(work)
        future.add_done_callback(lambda result: self.completed.put((result, callback, report, finished, error_callback)))

    def _call(self, command, args=None, callback=None, *, report=False, finished=None, timeout=30, error_callback=None):
        self._submit(lambda: self.client.request(command, args, timeout=timeout), callback,
                     report=report, finished=finished, error_callback=error_callback)

    def pump(self):
        for _ in range(100):
            try:
                future, callback, report, finished, error_callback = self.completed.get_nowait()
            except queue.Empty:
                break
            if self.closing:
                continue
            try:
                result = future.result()
                if callback:
                    callback(result)
            except Exception as error:
                if error_callback:
                    error_callback(error)
                if report:
                    self.status.set(str(error))
                    self.record(str(error))
            finally:
                if finished:
                    finished()
        if not self.closing:
            self.root.after(15, self.pump)

    def loaded_profiles(self, result):
        profiles = result.get("profiles", []) if isinstance(result, dict) else result
        if not isinstance(profiles, list) or not all(isinstance(p, dict) and isinstance(p.get("id"), str) for p in profiles):
            raise ValueError("设备目录格式无效")
        self.profiles = profiles
        self.profile["values"] = [p.get("displayName") or p.get("name") or p["id"] for p in profiles]
        if profiles:
            self.profile.current(0)
        self.status.set(f"已加载 {len(profiles)} 个设备固件配置")
        self.refresh_instances()

    def current_instance(self):
        index = self.instance.current() - 1
        return self.instances[index] if 0 <= index < len(self.instances) else None

    def refresh_instances(self, select_id=None):
        previous = select_id or (self.current_instance() or {}).get("instanceID")
        def received(result):
            values = result.get("instances", []) if isinstance(result, dict) else []
            if not isinstance(values, list):
                raise ValueError("实例列表格式无效")
            self.instances = [value for value in values if isinstance(value, dict) and isinstance(value.get("instanceID"), str)]
            self.instance["values"] = ["临时会话（不保存）"] + [value.get("name", value["instanceID"]) for value in self.instances]
            index = next((i + 1 for i, value in enumerate(self.instances) if value["instanceID"] == previous), 0)
            self.instance.current(index)
            self.choose_instance()
        self._call("instances", {"action": "list"}, callback=received,
                   error_callback=lambda error: self.instance_info.set(str(error)))

    def choose_instance(self):
        row = self.current_instance()
        if self.selected_instance_id is None and row is not None:
            self.temporary_fields = {key: value.get() for key, value in self.fields.items()}
        if row is None and self.selected_instance_id is not None:
            for key, value in self.fields.items():
                value.set(self.temporary_fields.get(key, ""))
        self.selected_instance_id = row.get("instanceID") if row else None
        initialized = bool(row and row.get("initialized"))
        if row:
            index = next((i for i, value in enumerate(self.profiles) if value["id"] == row.get("profileID")), None)
            if index is not None:
                self.profile.current(index)
            if initialized:
                for key, value in self.fields.items():
                    value.set("使用已保存镜像" if key == "firmwarePath" else "")
            self.instance_info.set(f"{row.get('name', '')} · {row.get('status', '已保存')} · 保存版本 {row.get('headRevision', '尚未初始化')}")
        else:
            self.instance_info.set("临时会话结束后不保留设备数据。")
        busy = self.running or self.starting
        self.profile.configure(state="disabled" if busy or row else "readonly")
        self.instance.configure(state="disabled" if busy else "readonly")
        for widget in self.form_widgets:
            widget.configure(state="disabled" if busy or initialized else "normal")
        self.skip_oobe_control.configure(state="disabled" if busy or initialized else "normal")

    def create_instance(self):
        from tkinter import simpledialog
        if self.running or self.starting or self.profile.current() < 0:
            self.status.set("请先停止当前实例")
            return
        profile = self.profiles[self.profile.current()]
        name = simpledialog.askstring("新建实例", "实例名称", initialvalue=profile.get("name", profile["id"]), parent=self.root)
        if name is None or not name.strip():
            return
        def created(result):
            self.record(result)
            self.refresh_instances(result.get("instanceID"))
        self._call("instances", {"action": "create", "profileID": profile["id"], "name": name.strip()}, callback=created, report=True)

    def manage_instances(self):
        from instances_panel import InstancesPanel
        if self.instances_panel is not None and self.instances_panel.alive():
            self.instances_panel.window.lift()
        else:
            self.instances_panel = InstancesPanel(self.root, self._call, self.refresh_instances)

    def browse(self, key):
        from tkinter import filedialog
        selected = filedialog.askopenfilename(parent=self.root, title="选择镜像", filetypes=(("固件镜像", "*.bin *.img *.zip"), ("所有文件", "*")))
        if selected:
            self.fields[key].set(selected)

    def start(self):
        if self.running or self.starting or self.profile.current() < 0:
            return
        instance = self.current_instance()
        initialized = bool(instance and instance.get("initialized"))
        firmware = self.fields["firmwarePath"].get().strip()
        if not initialized and (not firmware or not Path(firmware).is_file()):
            self.status.set("请选择要运行的固件镜像")
            return
        modes = ("reject", "winrt", "usb", "serial")
        try:
            args = launch_arguments(self.profiles[self.profile.current()]["id"],
                                    {key: value.get() for key, value in self.fields.items()},
                                    self.skip_oobe.get(), modes[self.bluetooth_mode.current()], self.usb_path.get(),
                                    instance_id=instance.get("instanceID") if instance else None, initialized=initialized)
        except ValueError as error:
            self.status.set(str(error))
            return
        self.start_button.configure(state="disabled")
        self.starting = True
        self.choose_instance()
        self.generation += 1
        self.status.set("正在准备并启动固件…")
        self.last_frame = self.last_sequence = None
        def launch():
            prepared = self.client.request("prepare", args, timeout=120)
            started = self.client.request("start", args, timeout=90)
            return {"prepared": prepared, "started": started}
        def ready(value):
            started = value["started"]
            self.running = not isinstance(started, dict) or started.get("running", True)
            self.stop_button.configure(state="normal" if self.running else "disabled")
            self.status.set("模拟器运行中，等待固件显示…" if self.running else "固件未开始运行")
            self.record(value)
            self.choose_instance()
        self._submit(launch, ready, report=True,
                     finished=self.start_finished)

    def start_finished(self):
        self.starting = False
        self.start_button.configure(state="disabled" if self.running else "normal")
        self.choose_instance()
        if not self.running:
            # Preparation may have committed the initial revision before a
            # launch failure. Read it back instead of resubmitting import
            # fields as though the instance were still uninitialized.
            self.refresh_instances()

    def update_bluetooth_mode(self):
        if self.bluetooth_mode.current() == 2:
            self.usb_frame.grid()
            self.refresh_usb()
        else:
            self.usb_frame.grid_remove()

    def refresh_usb(self):
        def received(result):
            devices = result.get("devices", []) if isinstance(result, dict) else []
            paths = [item["devicePath"] for item in devices
                     if isinstance(item, dict) and isinstance(item.get("devicePath"), str)]
            self.usb_selector["values"] = paths
            if len(paths) == 1 and not self.usb_path.get().strip():
                self.usb_path.set(paths[0])
            self.status.set(f"发现 {len(paths)} 个专用 WinUSB 接口" if paths else
                            "未发现可用的专用接口；可填写已配置的完整接口路径")
            self.record(result)
        self._call("bluetooth", {"action": "hci.usb.list"}, callback=received, report=True)

    def stop(self):
        self.release()
        self.generation += 1
        self.stop_button.configure(state="disabled")
        self.status.set("正在停止当前实例…")
        def stopped(value):
            if not isinstance(value, dict) or value.get("running") is not False:
                raise RuntimeError("尚未确认模拟器已停止，详情见运行记录")
            self.session_stopped(value)
            self.record(value)
        self._call("stop", callback=stopped, report=True, timeout=45,
                   finished=lambda: self.stop_button.configure(state="normal" if self.running else "disabled"))

    def session_stopped(self, value, *, natural=False):
        self.running = False
        self.generation += 1
        self.start_button.configure(state="normal")
        self.stop_button.configure(state="disabled")
        self.status.set(completion_status(value, persistent=self.current_instance() is not None, natural=natural))
        self.choose_instance()
        self.refresh_instances()

    def inspect_status(self, *, quiet=False):
        def received(value):
            if not quiet:
                self.record(value)
            if isinstance(value, dict) and value.get("running") is False and self.running:
                if quiet:
                    self.record(value)
                self.session_stopped(value, natural=True)
        self.status_pending = True
        self._call("status", callback=received, report=not quiet,
                   finished=lambda: setattr(self, "status_pending", False), timeout=45)

    def poll_status(self):
        if self.closing:
            return
        if self.running and not self.status_pending:
            self.inspect_status(quiet=True)
        self.root.after(1000, self.poll_status)

    def runtime_failed(self, error):
        if self.client.broken and self.running:
            self.running = False
            self.start_button.configure(state="disabled")
            self.stop_button.configure(state="disabled")
            self.status.set("运行服务已退出，请重新打开模拟器。")
            self.record(str(error))

    def poll_frame(self):
        if self.closing:
            return
        if self.running and not self.frame_pending:
            self.frame_pending = True
            generation = self.generation
            self._call("frame", callback=lambda value: self.receive_frame(value) if generation == self.generation else None,
                       finished=lambda: setattr(self, "frame_pending", False), timeout=5,
                       error_callback=self.runtime_failed)
        self.root.after(33, self.poll_frame)

    def receive_frame(self, result):
        if not self.running:
            return
        if isinstance(result, dict) and result.get("running") is False:
            if not self.status_pending:
                self.inspect_status()
            return
        if result is None or (isinstance(result, dict) and result.get("ready") is False):
            return
        frame = decode_frame(result)
        identity = (frame.sequence, frame.width, frame.height, frame.pixel_format)
        if identity == self.last_sequence:
            return
        self.last_sequence, self.last_frame = identity, frame
        self.status.set(f"运行中 · {frame.width} × {frame.height}")
        self.redraw()

    def redraw(self):
        self.canvas.delete("all")
        width, height = max(1, self.canvas.winfo_width()), max(1, self.canvas.winfo_height())
        if self.last_frame is None:
            self.canvas.create_text(width / 2, height / 2, text="选择设备和固件后启动", fill="#a9afb8", font=("Segoe UI", 14))
            return
        from PIL import Image, ImageTk
        frame = self.last_frame
        self.panel_rectangle = fit_panel(frame.width, frame.height, width, height)
        left, top, panel_width, panel_height = self.panel_rectangle
        image = frame.image().resize((panel_width, panel_height), Image.Resampling.NEAREST)
        self.photo = ImageTk.PhotoImage(image)
        self.canvas.create_image(left, top, image=self.photo, anchor="nw")

    def pointer(self, event, down):
        if not self.running or self.last_frame is None:
            return
        point = panel_point(event.x, event.y, self.panel_rectangle,
                            self.last_frame.width, self.last_frame.height, clamp=self.pressed)
        if point is None or (not down and not self.pressed):
            return
        self.canvas.focus_set()
        self.pressed, self.last_point = down, point
        self.input("touch", {"x": point[0], "y": point[1], "down": down})

    def move(self, event):
        now = time.monotonic()
        if self.pressed and now - self.last_move >= 1 / 30:
            self.last_move = now
            self.pointer(event, True)

    def release(self, event=None):
        if self.pressed:
            self.pressed = False
            self.input("touch", {"x": self.last_point[0], "y": self.last_point[1], "down": False})

    def wheel(self, event):
        if event.delta:
            self.input("crown", {"delta": 1 if event.delta > 0 else -1})

    def input(self, command, args):
        if self.running:
            self._call(command, args, report=True)

    def screenshot(self):
        from tkinter import filedialog
        if self.last_frame is None:
            self.status.set("当前还没有固件画面")
            return
        path = filedialog.asksaveasfilename(parent=self.root, title="保存固件截图", defaultextension=".png", filetypes=(("PNG", "*.png"),))
        if path:
            try:
                self.last_frame.image().save(path)
                self.status.set("截图已保存")
            except OSError as error:
                self.status.set(str(error))

    def bluetooth(self):
        from bluetooth_panel import BluetoothPanel
        if self.bluetooth_panel is not None and self.bluetooth_panel.alive():
            self.bluetooth_panel.window.lift()
        else:
            self.bluetooth_panel = BluetoothPanel(self.root, self._call)

    def sensors(self):
        if not self.running:
            self.status.set("请先启动一个模拟器实例")
            return
        from sensors_panel import SensorsPanel
        if self.sensors_panel is not None and self.sensors_panel.alive():
            self.sensors_panel.window.lift()
        else:
            self.sensors_panel = SensorsPanel(self.root, self._call)

    def close(self):
        if self.closing:
            return
        self.release()
        needs_persistence = (self.running or self.starting) and self.current_instance() is not None
        self.closing = True
        self.status.set("正在停止实例并保存已支持的设备数据…")
        self.start_button.configure(state="disabled")
        self.stop_button.configure(state="disabled")
        self.executor.shutdown(wait=False, cancel_futures=True)
        done = threading.Event()
        def finish():
            try:
                self.client.close()
            finally:
                self.service_log.close()
                done.set()
        threading.Thread(target=finish, daemon=True, name="azki-ui-close").start()
        def poll():
            if done.is_set():
                report = self.client.close_report
                receipt = self.service_log.name[:-4] + ".close.json"
                try:
                    with open(receipt, "x", encoding="utf-8") as stream:
                        json.dump(report, stream, ensure_ascii=False, indent=2)
                        stream.write("\n")
                except OSError:
                    receipt = str(self.options.workspace)
                result = report.get("stopResult")
                persistence = result.get("persistence", {}) if isinstance(result, dict) else {}
                if needs_persistence and persistence.get("committed") is not True:
                    from tkinter import messagebox
                    messagebox.showwarning("Azki Simulator", "本次设备数据未确认保存。请重新打开实例管理检查状态。\n关闭记录：" + receipt,
                                           parent=self.root)
                self.root.destroy()
            else:
                self.root.after(30, poll)
        poll()


def main():
    project_root, core = default_layout(__file__)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project-root", type=Path, default=project_root)
    parser.add_argument("--qemu", type=Path, default=core)
    local = Path(os.environ.get("LOCALAPPDATA", str(Path.home())))
    parser.add_argument("--workspace", type=Path, default=local / "AzkiSimulator/Windows")
    args = parser.parse_args()
    args.project_root = args.project_root.resolve()
    args.qemu = args.qemu.resolve()
    args.workspace = args.workspace.resolve()
    import tkinter as tk
    from tkinter import messagebox
    root = tk.Tk()
    try:
        SimulatorApp(root, args)
    except Exception as error:
        messagebox.showerror("Azki Simulator", str(error), parent=root)
        root.destroy()
        return 1
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
