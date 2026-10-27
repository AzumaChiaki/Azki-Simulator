# 实例存储、快照与空间回收

更新时间：2026-10-27

## 问题

模拟器的 NAND 用户区在每个实例里是一份精确大小的原始镜像：

```
<library>/instances/<uuid>/storage/revisions/r-<uuid>/
    nand-data.bin    384 MiB（按固件几何）
    nand-spare.bin    12 MiB
    nor-config.bin   可选
```

旧实现有三个完整复制点：

1. 每次启动把 HEAD 复制到新 revision；
2. 启动时再把 revision 复制到 `/private/tmp` 的会话工作盘；
3. 停止提交时把会话工作盘逐字节写回 revision。

每个实例还会保留最多 8 个 revision。一次“什么都没写”的启动也会产生
384 MiB 空白镜像；多实例、多轮验证后，工作区里就出现了数千份内容相同的
完整镜像。清理审计看到的 2 560 份 `nand-data.bin`、735 GB 总量、去重后
只有 691 份唯一内容，根因就在这里。

## 现在的行为

### 1. APFS 写时复制

在 APFS 卷上，以下复制改为 `clonefile(2)`：

- 新 revision 继承 HEAD；
- 会话工作盘从 revision 复制；
- 停止提交时把会话工作盘复制回 revision；
- 合成 UI seed、虚拟身份状态、NOR 配置等小文件同样适用。

克隆文件仍是普通、可读写的精确大小文件；第一次写入某个块时，APFS 自动
分配私有块。因此未修改的数据只占一份空间。文件系统不支持克隆（例如
exFAT、跨卷复制）时自动退回原来的逐字节复制，行为正确，只是更慢、更大。

### 2. 共享空白镜像

每个尺寸只保留一份真正的 `0xFF` 模板：

```
<library>/.storage/erased/<bytes>.bin
```

新建空白 NAND、OOB 或 NOR 时从模板克隆。模板只写一次；之后所有空白
快照共享它的数据块。模板会抽样校验首、中、尾块，损坏时重建。

首次创建某个尺寸的模板仍会真实写入一次（例如 384 MiB），这是之后所有
空白快照共享的底本。

### 3. 启动时自动整理

应用启动时，在清理临时会话后会执行一次 `storage-maintain --auto`：

- 字节完全相同的 revision 镜像替换为 APFS 克隆；
- 空白镜像替换为共享模板克隆；
- 删除已确认停止的临时会话。

自动整理**不会删除已提交的快照**。裁剪旧快照是“存储”设置里的显式操作。

### 4. 实例删除

删除实例时现在会：

- 移除 `instances/<uuid>`（包含所有 NAND、OOB、NOR、设置和观察记录）；
- 移除仍属于这个实例、且已确认停止的 `/private/tmp/azki-b-*`、
  `azki-sim-*` 会话工作盘；
- 运行中的会话保留，并报告保留原因。

此前 helper 的 `instance-delete` 没有对应的沙盒授权，删除记录后数据清理会
失败；本次补齐了该命令的授权和会话清理。

### 5. 存储管理界面

打开工具栏的 **存储管理** 按钮，或菜单 **存储 → 存储管理…**，也可以使用 **设置 → 存储**：

- 显示应用库总占用、卷可用空间、实例/固件库/临时会话/共享模板分布；
- 按实例列出快照数量、数据量、可去重量、空白镜像量和运行状态；
- 可对单个实例“整理”，或对整个库“优化存储”；
- “深度清理”把每个实例裁剪到最近 2 份快照并去重；
- 可单独清理已停止的临时会话；
- 未登记但残留的实例目录会单独显示，可删除。

“可优化（估算）”是逻辑值，包含可裁剪的旧快照、字节完全相同的镜像和已
停止会话；实际释放量以卷可用空间为准。APFS 克隆共享的块不会被 `du`
按文件重复计算，因此界面显示的是逻辑数据量，而不是唯一物理块数。

同一个“逻辑重复”不一定能再释放物理空间：如果两份副本本来就是用
`cp -c`/Finder 克隆复制出来的，它们已经共享数据块，替换成克隆不会让
`df` 变大。命令行 `dedupe --apply` 会报告 `beforeFreeBytes`、
`afterFreeBytes` 和 `freedBytes`，用实际卷变化判断收益；`duplicateBytes`
只是内容重复的逻辑量。

## 保留策略

| 操作 | 保留快照 | 会删数据？ |
| --- | --- | --- |
| 启动自动整理 | 不改动 | 只去重、共享空白模板、清理已停止会话 |
| “优化存储” | 界面选择 2 / 4 / 8 | 删除 HEAD 链之外更旧的已提交快照 |
| “深度清理” | 2 | 同上，更激进 |
| 删除实例 | 全部移除 | 是，包含会话工作盘 |

HEAD 和它前面一份快照始终优先保留。运行中的实例会被跳过。

## 研究工作区与证据

应用的存储界面只管理应用自己的库。Azki Music 的研究工作区里有大量独立
`library-*/instances` 和证据副本，可用仓库脚本处理：

```sh
# 只读统计（--hash 会读取候选镜像以估算完全重复量）
python3.12 scripts/simulator-storage.py report \
  --root "/Volumes/StudioData/Azki Music/workspaces" --hash

# 查看每个库裁剪到 2 份能回收多少（默认 dry-run）
python3.12 scripts/simulator-storage.py maintain \
  --root "/Volumes/StudioData/Azki Music/workspaces" --keep 2

# 只对完全相同的镜像做 APFS 克隆替换（默认 dry-run）
python3.12 scripts/simulator-storage.py dedupe \
  --root "/Volumes/StudioData/Azki Music/workspaces"

# 确认后执行；日志逐条记录保留/替换路径
python3.12 scripts/simulator-storage.py dedupe \
  --root "/Volumes/StudioData/Azki Music/workspaces" \
  --apply --log /private/tmp/azki-storage-dedupe.jsonl
```

脚本只处理同一用户的普通文件，不跟随链接；扫描和替换都跳过 `session/`、
`instance-data/`、`prepared/` 下的活动工作盘（因此报告不包含这些临时目录）；
默认跳过 30 分钟内修改过的文件。完全重复判定使用完整 SHA-256，不做抽样。

## 本机实测（2026-10-27）

- `workspaces/` 中 4 359 个镜像、459.0 GB：revision 3 162 个/338.1 GB，evidence 1 197 个/120.9 GB，共 75 个库根。
- `am-virtual-radio`：1 204 个文件、113.6 GB 逻辑重复，已用 `dedupe --apply` 替换为克隆。
- `v`：413 个文件、37.3 GB 逻辑重复，已替换为克隆；实际 `freedBytes` 只有 26.9 MB，说明这些副本原本大多已共享数据块。
- `boot-fast`：3 个文件、415.5 MB 逻辑重复，已替换为克隆。
- 抽样用 `F_LOG2PHYS_EXT` 检查：去重后两份不同 inode 的 `nand-data.bin` 返回相同 device offset，确认物理块共享。

## 限制

- APFS 克隆只在同一卷生效；跨卷或非 APFS 会退回完整复制。
- 克隆只消除**完全相同**的文件。两个内容略有差异的旧 revision 仍是两份
  数据；要减少它们只能裁剪更旧的快照。
- 归档区里已经复制出去的证据不会自动处理；请用上面的 `dedupe` 工具或在
  归档流程中改为保存 `zstd` 压缩副本。
- 任何清理都不能替代备份。删除旧快照会永久丢失更早的恢复点。
