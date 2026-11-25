# PCIe 完整配置空间保存与恢复

FW 按 SoC 原有步骤重新初始化；Host 脚本先读取并保存设备的完整配置空间，SoC 复位后再恢复。无需预先提供 `lspci` 输出，也无需手工填写 capability 地址。

## 快速使用

把压缩包解压到 Linux Host。`pcie_full_config.py` 和 `pcie_config_restore.py` 必须放在同一目录；后者提供共享的 sysfs 读写接口。依赖 Python 3.7+，不需要额外 Python 包。

替换下面的完整 BDF；保存文件必须是新文件：

```bash
BDF=0000:01:00.0

# 复位前：自动读取完整 256B 或 4KB 配置空间。
sudo python3 pcie_full_config.py save --bdf "$BDF" --output before-full.json

# 若要确认当前是否有驱动绑定：
readlink -f "/sys/bus/pci/devices/$BDF/driver" || true
lspci -Dnnk -s "$BDF"

# restore 前必须停止设备业务并解绑驱动；不要在驱动仍运行时写回 Command/MSI/BME。
DRIVER_PATH=$(readlink -f "/sys/bus/pci/devices/$BDF/driver" || true)
if [ -n "$DRIVER_PATH" ]; then
    DRIVER_NAME=${DRIVER_PATH##*/}
    echo "$BDF" | sudo tee "/sys/bus/pci/drivers/$DRIVER_NAME/unbind"
fi

# 再用现有方式复位 SoC，FW 按原流程初始化，等链路及配置访问就绪。

# 复位后：先查看写入计划，再实际恢复。
sudo python3 pcie_full_config.py restore --bdf "$BDF" --input before-full.json --raw-unknown --dry-run
sudo python3 pcie_full_config.py restore --bdf "$BDF" --input before-full.json --raw-unknown

# 也可以让脚本等待观察串口后再运行：
chmod +x pcie_restore_after_fw.sh
./pcie_restore_after_fw.sh "$BDF" before-full.json

# 再次读取完整配置空间进行比较。
sudo python3 pcie_full_config.py compare --bdf "$BDF" --input before-full.json
```

示例中的 `--raw-unknown` 用于把未识别的私有区域也按快照回放。对于已识别的寄存器，RO、W1C 和复位/重训触发位仍按属性处理。私有区域的属性无法仅靠读值推断；此选项不会自动识别其中的 W1C 或触发位；它适用于你们能够确认这些私有字段可按原值写回的验证环境。

若不使用 `--raw-unknown`，脚本会列出发生变化的未知 DWORD，并在任何硬件写入前报错。如果未知区域已经与快照相同，不需要写它们。

## 保存和恢复内容

- 快照的 `config_hex` 保存 sysfs 暴露的全部 256B/4096B 数据，包括标准头、标准 capability、extended capability 及私有区域。
- 自动识别完整 BDF、配置空间大小、capability 链和寄存器位置。`save` 只读，因此允许设备暂时有 Host 驱动绑定；但 `restore` 前必须停止业务并解绑驱动，Host 仍需保留原枚举结果及资源分配。
- 标准 Type 0 头恢复 BAR、ROM BAR、Cache Line Size、Latency Timer、Interrupt Line，以及原始 Command 的标准配置位。
- PM 恢复 PMCSR 配置字段，跳过 PME Status 的 W1C 位；当前要求复位前后均为 D0。
- MSI 恢复地址、数据、多消息设置、支持的向量 mask 和 Enable；MSI-X 恢复配置空间中的 Enable/Function Mask。
- PCIe v1/v2 恢复 Device Control、Link Control，以及 v2 的 Device Control 2/Link Control 2 中实现的静态字段，包括 Link Control 2 的 Preset/De-emphasis/SOS 设置。
- 只读 Device Capabilities/Link Capabilities 变化会记录到比较结果，不作为恢复阻断条件；恢复前仍会检查快照里的 MPS 不超过复位后当前 Device Capabilities。排除 FLR、Retrain Link、Emergency Power Reduction Request、进入 Compliance 等操作位。
- AER 恢复 Error Mask、Severity、ECRC 等控制，跳过错误状态和日志。还识别 DSN、Power Budgeting、LTR、ARI、ATS、PASID、Resizable BAR 等 capability。
- 其他区域由 `--raw-unknown` 控制是否原样回放发生变化的字节，不会因未知区域存在就假定是普通 RW 寄存器。

本版会恢复快照中的原始 Command，包括原来的 BME、MSE、I/O Enable、INTx Disable；不再像之前的 MMIO 版本那样强制 BME=0。它只处理配置空间，BAR 空间里的表和队列不在快照中。

## 写入顺序

1. 重新读取完整配置空间，核对身份、功能布局、Host 资源、上游桥窗口和可验证的只读 capability 属性。
2. 关闭 I/O/MEM/BME，屏蔽 INTx，并关闭 MSI，屏蔽/关闭 MSI-X。
3. 如有 Resizable BAR，先恢复 BAR size；再在解码关闭时恢复 BAR 地址及 ROM 地址。
4. 恢复控制字段、MSI 地址/数据、私有字段和链路静态配置。
5. 在 ROM Enable、中断使能及 Command 的使能步骤前，重新核对前面已写的配置，防止 FW 后续覆盖配置后继续使能；最后恢复原始 Command。
6. 再读完整空间，核对可恢复配置和未知区域；状态差异单独报告。

所有配置写入使用 1/2/4 字节访问，不会用一个 DWORD 同时回放 Command 和 Status。混合控制/状态寄存器会把已知的 W1C/操作触发位写为 0，不会通过保留读出的 1 来误清状态。

## 比较结果

`compare` 输出三组结果：

- `config_mismatches`：已识别可恢复配置字段不一致，返回非零退出码。
- `unknown_mismatches`：未知/私有区域不一致，返回非零退出码。
- `other_differences`：不属于恢复字段的差异，例如只读能力、错误状态、日志或操作触发位；不要求硬件状态逐字节回到复位前。

恢复成功表示本脚本管理的配置和未知回放区域通过读回核对。若硬件在恢复期间继续更新配置，或未知字段有特殊语义，会产生读回错误；脚本不会把这些错误隐藏为成功。

## 运行前提与失败处理

当前支持无 Host 驱动、单功能 Type 0 PCIe Endpoint、D0 状态。共享后端仍拒绝 VF/SR-IOV/多功能设备；这些设备需要协调其他功能的复位和恢复。PCIe capability 只支持已实现的 v1/v2 Endpoint 布局。

Host 不重启，不执行 remove/rescan，不重分配 BAR。复位前停止其他配置/BAR 访问，FW 需要保持设备身份、BAR 能力和功能布局不变。原设备对象、路径、资源或上游桥窗口变化时拒绝回放。

脚本只能协调脚本自身的多个实例，不能阻止 FW、内核错误恢复或其他工具并发修改设备。写入失败时会尽力关闭解码/BME和中断；无法访问设备时会报告清理未确认，不宣称硬件已完整回滚。

`--timeout` 默认 60 秒，用于等待配置访问就绪；单次 PCIe 配置事务本身的完成时间仍由 Host 硬件/内核决定。

如果你能通过串口确认 `FW_DONE` 和 `LTSSM=L0`，可以使用 `--ready-confirmed` 跳过配置空间就绪轮询：

```bash
sudo python3 pcie_full_config.py restore \
    --bdf "$BDF" \
    --input before-full.json \
    --raw-unknown \
    --ready-confirmed
```

这个选项只跳过等待阶段；脚本随后仍会读取当前完整配置空间、恢复字段并读回校验。只有在串口确认 FW 已完成初始化且链路已进入 L0 后才能使用。

## 与旧版本的区别及验证

`pcie_config_restore.py` 保有 CLI 仍是之前的 MMIO 版本。完整配置空间请使用本说明中的 **`pcie_full_config.py`**；两种快照格式不同，不能交叉使用。

开发环境没有实际 PCIe 设备。新旧版本共 53 项测试通过；寄存器模型涵盖完整 256B/4KB 镜像、原始 BME/中断使能、MSI 配置顺序、PM/AER 的 W1C、只读能力差异、触发位不回放、私有区域回放、FW 后续覆盖 BAR 的使能检查、绑定驱动时只读保存、串口确认跳过轮询与失败清理。还需在你们的维护平台验证实际配置访问。

```bash
python3 -m unittest discover -s . -v
python3 -m py_compile pcie_config_restore.py pcie_full_config.py
python3 pcie_full_config.py restore --help
```
