
# yaml 配置文件格式说明
[简体中文](README_CN.md) [English](README.md)

该配置文件定义了运行 gem5 仿真所需的环境和设置。它由多个部分组成，用于控制环境路径、执行行为、工作负载定义、架构配置、服务器分配以及可选的优化设置。

## 顶层结构

```yaml
environment:         # 【必需】gem5 环境配置
running:             # 【必需】运行控制参数
workloads:           # 【必需】工作负载列表及路径
archs:               # 【必需】架构配置
servers:             # 【必需】用于并行执行的服务器池
optimization:        # 【可选】参数优化空间（仅在 bayesianOpt.py 中必需）
```

---

## 1. `environment` 部分【必需】

定义运行 gem5 仿真所需的路径和共享对象。

| 字段          | 类型  | 描述             |
| ----------- | --- | -------------- |
| `gem5_home` | 字符串 | gem5 根目录的路径    |
| `bin_home`  | 字符串 | 包含仿真可执行文件的目录路径 |
| `gem5_data_proc_home` | 字符串 | [gem5_data_proc](https://github.com/shinezyy/gem5_data_proc) 仓库目录 |
| `restorer`  | 字典  | 检查点恢复工具的配置     |
| `ref_so`    | 字典  | 参考共享对象的配置      |

### 示例

```yaml
environment:
  gem5_home: "/nfs/home/$(whoami)/repos/gem5"
  bin_home:  "/nfs/home/$(whoami)/repos/gem5/build/RISCV"
  gem5_data_proc_home: "/nfs/home/$(whoami)/repos/gem5_data_proc"
  restorer:
    type: "GCB_RESTORER"
    path: ""  # 如果检查点已包含恢复器，则留空
  ref_so:
    type: "GCBV_REF_SO"
    path: "/nfs/home/share/gem5_ci/ref/normal/riscv64-nemu-interpreter-so"
```

查看 `restorer` 和 `ref_so` 类型详情：
👉 [香山Gem5 README.md](https://github.com/OpenXiangShan/GEM5/blob/xs-dev/README.md#environment-variables)

---

## 2. `running` 部分【必需】

控制如何启动和管理 gem5 仿真。

| 字段                    | 类型  | 描述                             |
| --------------------- | --- | ------------------------------ |
| `gem5_bin`            | 字符串 | gem5 可执行文件名（相对于 `bin_home` 路径） |
| `output_base_dir`     | 字符串 | 仿真输出文件的基础目录                    |
| `resume`              | 布尔值 | 是否从现有检查点恢复                     |
| `max_proc_per_server` | 整数  | 每台服务器的最大进程数                    |

### 示例

```yaml
running:
  gem5_bin: "gem5.opt"
  output_base_dir: "/nfs/home/$(whoami)/gem5_output"
  resume: false
  max_proc_per_server: 4
```

### 注

本示例中 `gem5_bin` 的真实路径为：

```bash
/nfs/home/$(whoami)/repos/gem5/build/RISCV/gem5.opt
```

---

## 3. `workloads` 部分【必需】

定义基准测试列表以及包含检查点文件的目录。

| 字段               | 类型        | 描述            |
| ---------------- | --------- | ------------- |
| `workloads_path` | 字符串       | 检查点目录的绝对路径    |
| `run_weight`     | 浮点数 (0-1) | 要运行的最小检查点权重之和 |
| `workload_list`  | 字符串列表     | 要仿真的基准测试名称列表  |

### 示例

```yaml
workloads:
  workloads_path: "/nfs/home/$(whoami)/gem5_checkpoints"
  run_weight: 0.5
  workload_list:
    - "workload1"
    - "workload2"
    - "workload3"
```

### 注

检查点可以位于 `workloads_path` 目录下的任意子目录中，脚本将递归搜索。其文件名应符合以下格式：`.*_\d+_\d\.\d+.zstd`，其中：
* 第一部分无特殊要求（例如 `foo`、`bar`，甚至为空）。
* 第二部分为整数（如 `100000000`、`200000000`）。
* 第三部分为浮点数，表示其权重（如 `0.25`、`0.5` 等）。

检查点目录结构应如下所示：

```bash
/nfs/home/$(whoami)/gem5_checkpoints
├── workload1
│   ├── checkpoint1
│   │   ├── foo_100000000_0.25.zstd
│   ├── checkpoint2
│   │   └── bar_200000000_0.5.zstd
│   ...
├── workload2
│   ├── checkpoint1
│   │   └── _300000000_0.75.zstd
│   ...
...
```

---

## 4. `archs` 部分【必需】

指定仿真时使用的不同架构配置。

| 字段              | 类型    | 描述                            |
| --------------- | ----- | ----------------------------- |
| `name`          | 字符串   | 架构的唯一标识名                      |
| `script_file`   | 字符串   | gem5 配置脚本的路径（相对于 `gem5_home`） |
| `script_params` | 字符串列表 | 传递给脚本的命令行参数                   |

### 示例

```yaml
archs:
  - name: "example-config-kmh_v3"
    script_file: "configs/example/xiangshan.py"
    script_params:
      - "--ideal-kmhv3"

  - name: "example-config-kmh_v2"
    script_file: "configs/example/kmh.py"
    script_params:
      - "--kmh-align"
```

### 注

`script_file` 是相对于 `gem5_home` 的路径。本示例中`scripts_file`的真实路径是：

```bash
/nfs/home/$(whoami)/repos/gem5/configs/example/xiangshan.py
```

`script_params` 是直接传递给 gem5 配置脚本的命令行参数，支持该脚本允许的任何参数。

---

## 5. `servers` 部分【必需】

定义可用于并行运行仿真的服务器列表。

| 字段        | 类型    | 描述                          |
| --------- | ----- | --------------------------- |
| `servers` | 字符串列表 | 可用服务器主机名的列表。可以通过注释临时禁用某台服务器 |

### 示例

```yaml
servers:
  servers:
    - "localhost"
    - "server1"
    - "192.168.1.123"
    # - "server2" # 暂时禁用此服务器
    - "server3"
```

---

## 6. `optimization` 部分【可选】

指定参数调优的搜索空间。此部分**仅在使用 `bayesianOpt.py` 时需要**。

| 字段                | 类型    | 描述             |
| ----------------- | ----- | -------------- |
| `constant_params` | 字符串列表 | 所有优化运行中保持不变的参数 |
| `param_space`     | 列表      | 可调参数的定义        |

### 支持的参数类型

| 类型            | 必需字段                     | 描述              |
| ------------- | ------------------------ | --------------- |
| `categorical` | `values`                 | 离散的字符串取值集合      |
| `pow2`        | `min_exp`, `max_exp`     | 指数范围，生成值为 2 的幂  |
| `float`       | `min_float`, `max_float` | 连续浮点数范围         |
| `integer`     | `min_int`, `max_int`     | 整数范围            |
| `boolean`     | 无额外字段                    | 布尔值（True/False） |

### 示例

```yaml
optimization:
  constant_params:
    - "--ideal-kmhv3"
    - "--l1d-enable-pht"
  param_space:
    - name: "--example-categorical"
      type: "categorical"
      values: ["value1", "value2"]
    - name: "--example-pow2"
      type: "pow2"
      min_exp: 2
      max_exp: 6
    - name: "--example-continuous-float"
      type: "float"
      min_float: 1.0
      max_float: 10.0
    - name: "--example-continuous-integer"
      type: "integer"
      min_int: 1
      max_int: 10
    - name: "--example-boolean"
      type: "boolean"
```

### 注

* `constant_params` 中的参数会直接传给 gem5 脚本。
* `param_space` 定义了可被优化的参数。
* `name` 是传给 gem5 脚本的命令行参数名。

本示例中，在进行贝叶斯优化时，传递给脚本的参数格式如下：

```bash
--ideal-kmhv3 --l1d-enable-pht --example-categorical=value1 --example-pow2=4 --example-continuous-float=5.0 --example-continuous-integer=5 --example-boolean=True
```

---

## 提示与最佳实践

* 文件路径和目录尽量使用**绝对路径**。
* `workload_list`、`archs`、`servers` 等列表都是可扩展、可自定义的。
* 使用 `run_weight` 可以从大型工作负载中采样一部分进行仿真。
* 如果不使用 `bayesianOpt.py`，可以省略 `optimization` 部分。
* 确保 `script_file` 路径在运行脚本的机器上是可访问的。
* 在大规模仿真中，可利用服务器池分发仿真任务以提升效率。
