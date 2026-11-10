"""Real Windows radio controls, using the shared runtime Bluetooth contract."""
from __future__ import annotations

import json
import re


class BluetoothPanel:
    def __init__(self, parent, runtime_call):
        import tkinter as tk
        from tkinter import ttk
        self.tk, self.ttk, self.runtime_call = tk, ttk, runtime_call
        self.window = tk.Toplevel(parent)
        self.window.title("Azki Simulator · 蓝牙设备与服务")
        self.window.geometry("960x780")
        self.window.minsize(760, 640)
        self.closed = self.poll_pending = self.host_ready = self.scanning = False
        self.device_id = None
        self.devices = {}
        self.characteristics = {}
        self.window.protocol("WM_DELETE_WINDOW", self.close)
        frame = ttk.Frame(self.window, padding=14)
        frame.pack(fill="both", expand=True)
        bar = ttk.Frame(frame)
        bar.pack(fill="x")
        ttk.Button(bar, text="适配器能力", command=self.capabilities).pack(side="left")
        ttk.Button(bar, text="开始扫描", command=self.scan).pack(side="left", padx=6)
        ttk.Button(bar, text="停止扫描", command=self.stop_scan).pack(side="left")
        self.scan_filter = tk.StringVar()
        ttk.Label(bar, text="服务 UUID 过滤").pack(side="left", padx=(16, 5))
        ttk.Entry(bar, textvariable=self.scan_filter).pack(side="left", fill="x", expand=True)
        self.devices_view = ttk.Treeview(frame, columns=("name", "address", "rssi"), show="headings", height=8)
        for key, title, width in (("name", "设备", 320), ("address", "蓝牙地址", 200), ("rssi", "信号", 70)):
            self.devices_view.heading(key, text=title)
            self.devices_view.column(key, width=width)
        self.devices_view.pack(fill="both", expand=True, pady=10)
        connect = ttk.Frame(frame)
        connect.pack(fill="x")
        ttk.Button(connect, text="连接选中设备", command=self.connect).pack(side="left")
        ttk.Button(connect, text="读取服务", command=self.discover).pack(side="left", padx=6)
        ttk.Button(connect, text="断开连接", command=self.disconnect).pack(side="left")
        self.connection = tk.StringVar(value="未连接")
        ttk.Label(connect, textvariable=self.connection).pack(side="left", padx=12)
        self.services = ttk.Treeview(frame, columns=("properties",), show="tree headings", height=7)
        self.services.heading("#0", text="服务 / 特征 UUID")
        self.services.heading("properties", text="能力")
        self.services.column("#0", width=550)
        self.services.column("properties", width=180)
        self.services.pack(fill="both", expand=True, pady=10)
        values = ttk.Frame(frame)
        values.pack(fill="x")
        ttk.Label(values, text="十六进制数据").pack(side="left")
        self.value = tk.StringVar()
        ttk.Entry(values, textvariable=self.value).pack(side="left", fill="x", expand=True, padx=8)
        ttk.Button(values, text="读取", command=self.read).pack(side="left")
        ttk.Button(values, text="写入", command=self.write).pack(side="left", padx=5)
        ttk.Button(values, text="订阅", command=lambda: self.subscribe(True)).pack(side="left")
        ttk.Button(values, text="取消订阅", command=lambda: self.subscribe(False)).pack(side="left", padx=5)
        advertise = ttk.LabelFrame(frame, text="广播与固件服务", padding=10)
        advertise.pack(fill="x", pady=12)
        self.company = tk.StringVar(value="FFFF")
        self.advertisement = tk.StringVar(value="415A4B492D54455354")
        ttk.Label(advertise, text="厂商 ID").pack(side="left")
        ttk.Entry(advertise, textvariable=self.company, width=6).pack(side="left", padx=6)
        ttk.Entry(advertise, textvariable=self.advertisement, width=23).pack(side="left", fill="x", expand=True)
        ttk.Button(advertise, text="发送广播", command=self.advertise).pack(side="left", padx=6)
        ttk.Button(advertise, text="停止广播", command=lambda: self.invoke("advertise.stop")).pack(side="left")
        ttk.Button(advertise, text="公布固件服务", command=lambda: self.invoke("peripheral.start", {"allowServiceOmissions": False})).pack(side="left", padx=6)
        ttk.Button(advertise, text="撤下服务", command=lambda: self.invoke("peripheral.stop")).pack(side="left")
        self.output = tk.Text(frame, height=8, wrap="word", state="disabled", font=("Consolas", 9))
        self.output.pack(fill="both", expand=True)
        self.capabilities()
        self.window.after(250, self.poll)

    def alive(self):
        return not self.closed and bool(self.window.winfo_exists())

    def log(self, result):
        if not self.alive():
            return
        text = result if isinstance(result, str) else json.dumps(result, ensure_ascii=False, indent=2)
        self.output.configure(state="normal")
        self.output.insert("end", text[:20000] + "\n")
        if int(self.output.index("end-1c").split(".")[0]) > 400:
            self.output.delete("1.0", "200.0")
        self.output.see("end")
        self.output.configure(state="disabled")

    def invoke(self, action, fields=None, callback=None, *, quiet=False, finished=None):
        args = dict(fields or {}, action=action)
        def received(result):
            if not self.alive():
                return
            if not quiet:
                self.log(result)
            if callback:
                callback(result)
        def failed(error):
            if self.alive():
                self.log(str(error))
            if quiet:
                self.host_ready = False
        self.runtime_call("bluetooth", args, callback=received, report=not quiet,
                          finished=finished, timeout=30, error_callback=failed)

    def capabilities(self):
        self.invoke("capabilities", callback=lambda _: setattr(self, "host_ready", True))

    def scan(self):
        uuids = [value.strip() for value in self.scan_filter.get().split(",") if value.strip()]
        self.invoke("scan.start", {"active": True, "extended": False, "serviceUUIDs": uuids},
                    callback=lambda _: setattr(self, "scanning", True))

    def stop_scan(self):
        self.invoke("scan.stop", callback=lambda _: setattr(self, "scanning", False))

    def poll(self):
        if not self.alive():
            return
        if self.host_ready and not self.poll_pending:
            self.poll_pending = True
            self.invoke("events", {"limit": 128}, callback=self.events, quiet=True,
                        finished=lambda: setattr(self, "poll_pending", False))
        self.window.after(250, self.poll)

    def events(self, result):
        if not isinstance(result, dict):
            return
        for event in result.get("events", []):
            if not isinstance(event, dict):
                continue
            data = event.get("data", {})
            if event.get("event") == "scan.received" and isinstance(data, dict):
                address = data.get("address")
                if not isinstance(address, str) or len(address) > 32:
                    continue
                key = (address, str(data.get("addressType", "unspecified")))
                item = self.devices.get(key)
                values = (str(data.get("name") or "未命名设备")[:160], address, data.get("rssi", ""))
                if item is None:
                    if len(self.devices) >= 1024:
                        continue
                    item = self.devices_view.insert("", "end", values=values)
                    self.devices[key] = item
                else:
                    self.devices_view.item(item, values=values)
            else:
                self.log(event)

    def connect(self):
        selected = self.devices_view.selection()
        if not selected:
            self.log("请先选择扫描到的设备")
            return
        key = next((key for key, item in self.devices.items() if item == selected[0]), None)
        if key is None:
            return
        def connected(result):
            if not isinstance(result, dict) or not isinstance(result.get("deviceID"), str):
                self.log("连接响应缺少设备标识")
                return
            self.device_id = result["deviceID"]
            self.connection.set("已连接 " + key[0])
            self.discover()
        if self.device_id:
            previous = self.device_id
            self.invoke("central.disconnect", {"deviceID": previous})
        self.invoke("central.connect", {"address": key[0], "addressType": key[1]}, callback=connected)

    def disconnect(self):
        if self.device_id:
            device = self.device_id
            def disconnected(_):
                if self.device_id == device:
                    self.device_id = None
                    self.connection.set("未连接")
                    self.services.delete(*self.services.get_children())
                    self.characteristics.clear()
            self.invoke("central.disconnect", {"deviceID": device}, callback=disconnected)

    def discover(self):
        if not self.device_id:
            self.log("请先连接设备")
            return
        def discovered(result):
            self.services.delete(*self.services.get_children())
            self.characteristics.clear()
            for service in result.get("services", []):
                row = self.services.insert("", "end", text=str(service.get("uuid", "")), open=True)
                for characteristic in service.get("characteristics", []):
                    key = characteristic.get("key")
                    if not isinstance(key, str):
                        continue
                    properties = characteristic.get("properties", "")
                    if isinstance(properties, list):
                        properties = ", ".join(map(str, properties))
                    item = self.services.insert(row, "end", text=str(characteristic.get("uuid", "")), values=(properties,))
                    self.characteristics[item] = key
        self.invoke("central.discover", {"deviceID": self.device_id}, callback=discovered)

    def selection(self):
        selected = self.services.selection()
        key = self.characteristics.get(selected[0]) if selected else None
        if not self.device_id or not key:
            self.log("请选择一个蓝牙特征")
            return None
        return {"deviceID": self.device_id, "key": key}

    def read(self):
        fields = self.selection()
        if fields:
            def received(result):
                if isinstance(result, dict) and isinstance(result.get("value"), str):
                    self.value.set(result["value"])
            self.invoke("central.read", fields, callback=received)

    @staticmethod
    def hex_value(value, maximum=4096):
        cleaned = re.sub(r"\s+", "", value)
        if len(cleaned) > maximum * 2 or len(cleaned) % 2 or re.fullmatch(r"[0-9a-fA-F]*", cleaned) is None:
            raise ValueError("请输入有效的十六进制字节")
        return cleaned.lower()

    def write(self):
        fields = self.selection()
        if fields:
            try:
                fields.update(value=self.hex_value(self.value.get()), withResponse=True)
                self.invoke("central.write", fields)
            except ValueError as error:
                self.log(str(error))

    def subscribe(self, enabled):
        fields = self.selection()
        if fields:
            fields.update(enabled=enabled, indicate=False)
            self.invoke("central.subscribe", fields)

    def advertise(self):
        try:
            company = int(self.company.get(), 16)
            if not 0 <= company <= 65535:
                raise ValueError("厂商 ID 必须在0000至FFFF之间")
            data = self.hex_value(self.advertisement.get(), maximum=24)
            self.invoke("advertise.start", {"manufacturer": [{"companyID": company, "value": data}],
                                             "sections": [], "extended": False})
        except ValueError as error:
            self.log(str(error))

    def close(self):
        if self.closed:
            return
        if self.scanning:
            self.runtime_call("bluetooth", {"action": "scan.stop"})
        self.closed = True
        self.window.destroy()
