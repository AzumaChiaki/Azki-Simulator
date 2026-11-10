"""Desktop controls for the same modeled hardware inputs as the macOS app."""
import json
import math


def integer_value(value, minimum, maximum):
    number = int(value)
    if not minimum <= number <= maximum:
        raise ValueError(f"数值必须在 {minimum} 至 {maximum} 之间")
    return number


def pose_value(roll, pitch):
    values = {"rollDegrees": float(roll), "pitchDegrees": float(pitch)}
    if not all(math.isfinite(v) and -180 <= v <= 180 for v in values.values()):
        raise ValueError("角度必须是 −180 至 180 之间的有限数值")
    return values


class SensorsPanel:
    def __init__(self, parent, runtime_call):
        import tkinter as tk
        from tkinter import ttk
        self.tk, self.ttk, self.runtime_call = tk, ttk, runtime_call
        self.closed = False
        self.window = tk.Toplevel(parent)
        self.window.title("Azki Simulator · 模拟传感器输入")
        self.window.geometry("700x660")
        self.window.minsize(620, 560)
        self.window.protocol("WM_DELETE_WINDOW", self.close)
        frame = ttk.Frame(self.window, padding=16)
        frame.pack(fill="both", expand=True)
        ttk.Button(frame, text="读取当前输入状态", command=lambda: self.invoke("live.get", callback=self.update_live)).pack(anchor="w", pady=(0, 12))
        health = ttk.LabelFrame(frame, text="健康与步行", padding=10)
        health.pack(fill="x", pady=5)
        self.heart_rate, self.cadence, self.steps = (tk.StringVar(value=v) for v in ("75", "100", "100"))
        self.worn = tk.BooleanVar(value=True)
        for column, text, value, minimum, maximum in ((0, "模拟心率", self.heart_rate, 0, 255),
                                                      (2, "步频 / 分钟", self.cadence, 40, 200)):
            ttk.Label(health, text=text).grid(row=0, column=column, padx=5, sticky="w")
            ttk.Spinbox(health, textvariable=value, from_=minimum, to=maximum, width=9).grid(row=0, column=column + 1, padx=5)
        ttk.Checkbutton(health, text="已佩戴", variable=self.worn).grid(row=1, column=0, padx=5, pady=8)
        ttk.Button(health, text="应用输入", command=self.apply_health).grid(row=1, column=1, padx=5)
        ttk.Spinbox(health, textvariable=self.steps, from_=1, to=100000, width=10).grid(row=1, column=2, padx=5)
        ttk.Button(health, text="追加模拟步行", command=self.walk).grid(row=1, column=3, padx=5)
        self.walked = tk.StringVar(value="尚未读取")
        ttk.Label(health, textvariable=self.walked).grid(row=2, column=0, columnspan=4, sticky="w", padx=5)
        pose = ttk.LabelFrame(frame, text="机身姿态", padding=10)
        pose.pack(fill="x", pady=5)
        self.roll, self.pitch = tk.StringVar(value="0"), tk.StringVar(value="0")
        for title, value in (("横滚角", self.roll), ("俯仰角", self.pitch)):
            ttk.Label(pose, text=title).pack(side="left", padx=4)
            ttk.Spinbox(pose, textvariable=value, from_=-180, to=180, width=8).pack(side="left", padx=4)
        ttk.Button(pose, text="保持此姿态", command=self.apply_pose).pack(side="left", padx=7)
        ttk.Button(pose, text="自动", command=lambda: self.invoke("motion.orientation.set", {"value": None})).pack(side="left")
        charging = ttk.Frame(frame, padding=(0, 10))
        charging.pack(fill="x")
        ttk.Label(charging, text="充电器").pack(side="left", padx=(0, 8))
        self.charger = ttk.Combobox(charging, state="readonly", values=("未连接", "充电中", "已充满"), width=14)
        self.charger.current(0)
        self.charger.pack(side="left")
        ttk.Button(charging, text="应用充电状态", command=self.apply_charger).pack(side="left", padx=8)
        adc = ttk.LabelFrame(frame, text="原始 ADC 通道", padding=10)
        adc.pack(fill="x", pady=5)
        self.adc = [tk.StringVar(value="") for _ in range(8)]
        self.adc_enabled = [tk.BooleanVar(value=False) for _ in range(8)]
        for index in range(8):
            row, column = index // 4, (index % 4) * 2
            ttk.Checkbutton(adc, text=str(index), variable=self.adc_enabled[index]).grid(row=row, column=column, pady=4)
            ttk.Entry(adc, textvariable=self.adc[index], width=9).grid(row=row, column=column + 1, padx=(0, 12))
        ttk.Button(adc, text="读取 ADC", command=lambda: self.invoke("pmu.adcRaw.get", callback=self.update_adc)).grid(row=2, column=0, columnspan=2, pady=8)
        ttk.Button(adc, text="应用勾选通道", command=self.apply_adc).grid(row=2, column=2, columnspan=3, pady=8)
        self.adc_info = tk.StringVar(value="只修改勾选通道；其余通道保留当前值。")
        ttk.Label(adc, textvariable=self.adc_info).grid(row=3, column=0, columnspan=8, sticky="w")
        self.output = tk.Text(frame, height=8, state="disabled", wrap="word", font=("Consolas", 9))
        self.output.pack(fill="both", expand=True, pady=(10, 0))
        self.groups = {"health": health, "orientation": pose, "charger": charging, "adcRaw": adc}
        self.invoke("capabilities", callback=self.update_capabilities)
        self.invoke("live.get", callback=self.update_live)

    def alive(self):
        return not self.closed and bool(self.window.winfo_exists())

    def log(self, value):
        if not self.alive():
            return
        text = value if isinstance(value, str) else json.dumps(value, ensure_ascii=False, indent=2)
        self.output.configure(state="normal")
        self.output.insert("end", text[:12000] + "\n")
        if int(self.output.index("end-1c").split(".")[0]) > 300:
            self.output.delete("1.0", "150.0")
        self.output.see("end")
        self.output.configure(state="disabled")

    def invoke(self, command, fields=None, callback=None):
        def received(result):
            if not self.alive():
                return
            self.log(result)
            if callback:
                callback(result)
        self.runtime_call("input", dict(fields or {}, command=command), callback=received,
                          error_callback=lambda error: self.log(str(error)), report=True)

    def update_health(self, state):
        if isinstance(state, dict):
            for key, variable in (("heartRate", self.heart_rate), ("cadence", self.cadence)):
                if type(state.get(key)) is int:
                    variable.set(str(state[key]))
            if type(state.get("worn")) is bool:
                self.worn.set(state["worn"])
            if type(state.get("walked")) is int:
                self.walked.set(f"当前模拟步行已产生 {state['walked']} 步；固件按其自身算法计步。")

    def update_capabilities(self, result):
        if not isinstance(result, dict):
            return
        def enable(widget, supported):
            if hasattr(widget, "state"):
                widget.state(["!disabled" if supported else "disabled"])
            for child in widget.winfo_children():
                enable(child, supported)
        for name, group in self.groups.items():
            if type(result.get(name)) is bool:
                enable(group, result[name])

    def update_live(self, result):
        if not isinstance(result, dict):
            return
        self.update_health(result.get("health"))
        modes = ("unplugged", "charging", "full")
        if result.get("charger") in modes:
            self.charger.current(modes.index(result["charger"]))
        if isinstance(result.get("adc"), list):
            self.update_adc({"values": result["adc"]})
        if result.get("batteryChannel") is not None:
            self.adc_info.set(f"电池通道：{result['batteryChannel']}；NTC 通道：{result.get('ntcChannels', [])}")
        pose = result.get("orientation")
        if isinstance(pose, dict) and pose.get("mode") == "held":
            self.roll.set(str(pose["rollDegrees"]))
            self.pitch.set(str(pose["pitchDegrees"]))

    def update_adc(self, result):
        values = result.get("values") if isinstance(result, dict) else None
        if isinstance(values, list) and len(values) == 8 and all(type(v) is int for v in values):
            for variable, value in zip(self.adc, values):
                variable.set(str(value))

    def apply_health(self):
        try:
            value = {"heartRate": integer_value(self.heart_rate.get(), 0, 255),
                     "cadence": integer_value(self.cadence.get(), 40, 200), "worn": self.worn.get()}
            self.invoke("health.set", {"value": value}, callback=self.update_health)
        except ValueError as error:
            self.log(str(error))

    def walk(self):
        try:
            self.invoke("health.walk", {"steps": integer_value(self.steps.get(), 1, 100000)}, callback=self.update_health)
        except ValueError as error:
            self.log(str(error))

    def apply_pose(self):
        try:
            self.invoke("motion.orientation.set", {"value": pose_value(self.roll.get(), self.pitch.get())})
        except ValueError as error:
            self.log(str(error))

    def apply_charger(self):
        self.invoke("charger.set", {"value": ("unplugged", "charging", "full")[self.charger.current()]})

    def apply_adc(self):
        try:
            channels = {str(i): integer_value(value.get(), 0, 65535) for i, value in enumerate(self.adc)
                        if self.adc_enabled[i].get()}
            if not channels:
                raise ValueError("请勾选要修改的 ADC 通道")
            self.invoke("pmu.adcChannels.set", {"channels": channels}, callback=self.update_live)
        except ValueError as error:
            self.log(str(error))

    def close(self):
        self.closed = True
        self.window.destroy()
