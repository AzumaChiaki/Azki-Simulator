"""Manage only the current user's saved simulator instances."""
import json


class InstancesPanel:
    def __init__(self, parent, runtime_call, changed):
        import tkinter as tk
        from tkinter import ttk
        self.runtime_call, self.changed, self.closed = runtime_call, changed, False
        self.window = tk.Toplevel(parent)
        self.window.title("Azki Simulator · 已保存的实例")
        self.window.geometry("840x560")
        self.window.protocol("WM_DELETE_WINDOW", self.close)
        frame = ttk.Frame(self.window, padding=14)
        frame.pack(fill="both", expand=True)
        self.view = ttk.Treeview(frame, columns=("profile", "state", "revision"), height=10)
        for key, title in (("#0", "实例"), ("profile", "设备固件"), ("state", "状态"), ("revision", "保存版本")):
            self.view.heading(key, text=title)
            self.view.column(key, width=300 if key == "profile" else 140)
        self.view.pack(fill="both", expand=True)
        self.view.bind("<<TreeviewSelect>>", lambda _: self.selected())
        bar = ttk.Frame(frame, padding=(0, 10))
        bar.pack(fill="x")
        ttk.Button(bar, text="刷新", command=self.refresh).pack(side="left")
        ttk.Button(bar, text="移到实例回收站", command=self.trash).pack(side="left", padx=8)
        ttk.Button(bar, text="恢复到最后保存版本", command=self.recover).pack(side="left")
        self.details = tk.Text(frame, height=10, wrap="word", state="disabled", font=("Consolas", 9))
        self.details.pack(fill="both", expand=True)
        self.rows = {}
        self.refresh()

    def alive(self):
        return not self.closed and bool(self.window.winfo_exists())

    def show(self, value):
        if not self.alive():
            return
        text = value if isinstance(value, str) else json.dumps(value, ensure_ascii=False, indent=2)
        self.details.configure(state="normal")
        self.details.delete("1.0", "end")
        self.details.insert("end", text[:20000])
        self.details.configure(state="disabled")

    def request(self, action, fields=None, callback=None):
        def received(result):
            if not self.alive():
                return
            if callback:
                callback(result)
            else:
                self.show(result)
                self.refresh()
                self.changed()
        self.runtime_call("instances", dict(fields or {}, action=action), callback=received,
                          report=True, error_callback=lambda error: self.show(str(error)), timeout=60)

    def refresh(self):
        def listed(result):
            previous = self.selection()
            self.view.delete(*self.view.get_children())
            self.rows.clear()
            for value in result.get("instances", []):
                identifier = value.get("instanceID")
                if not isinstance(identifier, str):
                    continue
                item = self.view.insert("", "end", text=value.get("name", identifier),
                                        values=(value.get("profileID", ""), value.get("status", ""), value.get("headRevision", "")))
                self.rows[item] = value
                if previous and previous.get("instanceID") == identifier:
                    self.view.selection_set(item)
            self.selected()
        self.request("list", callback=listed)

    def selection(self):
        selected = self.view.selection()
        return self.rows.get(selected[0]) if selected else None

    def selected(self):
        value = self.selection()
        self.show(value or "选择一个实例查看其存储与保存状态。")

    def trash(self):
        from tkinter import messagebox
        row = self.selection()
        if row and messagebox.askyesno("移到回收站", f"将“{row.get('name', row['instanceID'])}”移到实例回收站？正在运行的实例不会被删除。", parent=self.window):
            self.request("delete", {"instanceID": row["instanceID"]})

    def recover(self):
        from tkinter import messagebox
        row = self.selection()
        if row and messagebox.askyesno("恢复保存版本", "保留最后成功保存的版本，并放弃未发布工作副本作为当前启动状态？工作副本会保留供诊断，不会强行提交。", parent=self.window):
            self.request("recover", {"instanceID": row["instanceID"], "operation": "discardUnpublished"})

    def close(self):
        self.closed = True
        self.window.destroy()
