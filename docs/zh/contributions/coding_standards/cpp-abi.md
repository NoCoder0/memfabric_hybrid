# MemFabric C++ ABI 兼容性编码规范

> **适用场景**：适用于`src/smem/include/host/`、`src/hybm/include/`、`src/acc_links/include/`目录下全部公开头文件的接口设计，以及`libmf_smem.so`、`libmf_hybm_core.so`导出符号的演进管理。当PR涉及公开头文件或公开接口实现变更时，必须执行本规范。

## 规范列表

| 规范编号 | 规范名称 | 类别 | 严重级别 |
|------|---------|------|---------|
| 1.1  | 理解ABI变更的构成 | 总体原则 | —— |
| 1.2  | ABI变更必须显式评审 | 总体原则 | 高 |
| 2.1  | 禁止含双ABI类型的模板实例化 | STL类型准入 | 高 |
| 2.2  | 字符串参数使用C风格或受控字符串类型 | STL类型准入 | 中 |
| 2.3  | 公开头文件保持自包含 | STL类型准入 | 中 |
| 3.1  | 新增字段必须追加在结构体末尾 | 数据布局 | 高 |
| 3.2  | 新增字段优先复用reserved空间 | 数据布局 | 高 |
| 3.3  | 新增对外结构体必须预留reserved字段 | 数据布局 | 中 |
| 3.4  | 禁止修改已有字段类型和顺序 | 数据布局 | 高 |
| 3.5  | 枚举值显式赋值且仅允许末尾追加 | 数据布局 | 高 |
| 4.1  | 公开符号只增不改 | 符号演进 | 高 |
| 4.2  | 公开constexpr常量值禁止变更 | 符号演进 | 高 |
| 4.3  | 默认参数值变更视同签名变更 | 符号演进 | 高 |
| 5.1  | 公开类新增虚函数只能加在末尾 | 类布局 | 高 |
| 5.2  | 公开类优先使用pimpl惯用法 | 类布局 | 中 |
| 6.1  | _GLIBCXX_USE_CXX11_ABI设置保持统一 | 构建约定 | 中 |

## 说明

本规范基于GCC libstdc++双ABI行为（`_GLIBCXX_USE_CXX11_ABI`）、MemFabric构建设置以及公开接口现状整理：本文件是ABI兼容性主题的权威规则来源。如果对规则有异议，建议提交issue并说明理由，经MemFabric团队评审后可接纳并修改生效。

## 适用范围

MemFabric代码仓的公开头文件设计、公开接口（C++类接口与C接口）演进、导出符号管理。

---

### 1. 总体原则

#### 规则 1.1 理解ABI变更的构成

API兼容是源码级兼容（老代码重新编译仍可用）；ABI兼容是二进制级兼容（老编译产物不重编，直接替换新版`.so`仍正确链接、正确运行）。以下任一变化都构成ABI变更，修改公开头文件或公开接口实现前应逐项自查：

- 导出符号的删除、重命名（C++ mangled name编码完整签名，改签名即改符号）
- 对外结构体大小、字段偏移、对齐的变化
- 枚举取值的变化
- 公开`constexpr`常量值的变化（编译期烧入调用方）
- 类的vtable布局变化（虚函数增删、顺序调整）
- 函数默认参数值的变化（在调用方展开）

#### 规则 1.2 ABI变更必须显式评审

非必要不变更公开ABI。确需变更时，PR描述中必须显式说明ABI影响、兼容性结论与版本演进方案，经评审确认后才能合入。判断依据是规则1.1的构成清单，而非"是否改了头文件"——部分ABI变更（如导出符号增删）不触碰公开头文件。

---

### 2. 公开头文件STL类型准入

#### 规则 2.1 禁止含双ABI类型的模板实例化

禁用类型的毒性会沿模板实例化链传导：`std::vector<std::string>`、`std::map<std::string, std::string>`等模板参数含`std::string`/`std::list`的组合，其元素布局与整条实例化链的符号名同样随双ABI变化，等同直接引入禁用类型。

[反例]：

```cpp
// 禁止：模板参数含 std::string，整条实例化链的符号名携带 __cxx11
int32_t smem_bm_init_with_options(const std::map<std::string, std::string> &options);
```

[正例]（使用C风格字符串或受控类型替代）：

```cpp
// 正例1：用 C 风格字符串数组 + 长度替代
int32_t smem_bm_init_with_options(const char *const *keys, const char *const *values, uint32_t count);

// 正例2：用固定长度字符数组（MemFabric现有做法）
#define SMEM_TLS_PATH_SIZE 256
struct smem_tls_config {
  bool tlsEnable;
  char caPath[SMEM_TLS_PATH_SIZE];
  char certPath[SMEM_TLS_PATH_SIZE];
  // ...
};
```

#### 规则 2.2 字符串参数使用C风格或受控字符串类型

MemFabric公开接口主要采用C风格，字符串参数统一使用`const char *`配合显式长度或`NUL`结束约定（参见`smem_bm_init`、`smem_trans_config_t::nic`等）。C++层（acc_links/ock::acc命名空间）允许使用`std::vector`、`std::map`、`std::pair`、`std::unique_ptr`（pimpl惯用法），这些类型不带`__cxx11`标签、布局稳定；但`std::string`、`std::list`在公开头文件中禁止使用（见规则2.1）。

#### 规则 2.3 公开头文件保持自包含

公开头文件只允许依赖C++标准库、POSIX头（`<stdint.h>`、`<stdbool.h>`）以及MemFabric内部同模块公开头（如`smem_trans_def.h`可包含`smem_bm_def.h`），不得包含仓库内部实现头或`src/`下的非公开头，保证下游可以独立包含公开头进行编译。

---

### 3. 数据布局

#### 规则 3.1 新增字段必须追加在结构体末尾

在已有字段之前或之间插入新字段会使后续所有字段偏移变化，破坏已按旧布局编译的调用方。新增字段只能追加在最后一个有效字段之后（实践中末尾通常是`flags`字段或`reserved`区域，见规则3.2）。

[反例]：

```cpp
struct smem_trans_config_t {
  uint32_t newFeature;        // 禁止：中部插入，deviceId/flags/dataOpType偏移全部改变
  smem_trans_role_t role;
  uint32_t initTimeout;
  uint32_t deviceId;
  uint32_t flags;
  smem_bm_data_op_type dataOpType;
  char nic[64];
};
```

[正例]：

```cpp
struct smem_trans_config_t {
  smem_trans_role_t role;
  uint32_t initTimeout;
  uint32_t deviceId;
  uint32_t flags;              // 建议：优先复用 flags 的 bit 位承载布尔/枚举选项
  smem_bm_data_op_type dataOpType;
  char nic[64];
  // 新增字段只能在这里追加，或从保留区扩展，见规则3.2
};
```

#### 规则 3.2 新增字段优先复用reserved空间

对外结构体应预留`reserved`字段为兼容性扩展预留空间。新增字段应置于`reserved`之前（即有效字段末尾），连同其引入的对齐填充一起从`reserved`划出空间，保持结构体总大小不变。总大小不变意味着调用方按旧大小分配的栈对象、数组、通信缓冲都不会越界。不建议依赖隐式对齐填充：优先选择不引入填充的字段类型与位置（如1字节字段紧邻其他1字节字段）；填充不可避免时应显式演算并在注释中说明。

**实践建议**：MemFabric现有部分公开结构体（如`smem_bm_config_t`、`smem_trans_config_t`）尚未预留显式`reserved`字段，新增字段时应先为其补充`reserved`空间：在结构体末尾追加`uint8_t reserved[N]`，然后再从`reserved`中划出空间承载新字段。

[正例]：假设为 MemFabric 新增一个传输端点描述结构体，初始设计预留 reserved 空间：

```cpp
// 初始版本：预留给未来扩展，总大小128字节
struct smem_trans_endpoint_t {
  char hostIp[64];
  uint16_t port;
  uint16_t reserved0;      // 对齐填充预留（hostIp 64字节后，port占2字节，需2字节填充对齐到4字节边界）
  uint32_t flags;
  uint8_t reserved[56];    // 预留给未来扩展
};

// 扩展版本：在 flags 之后追加新字段 priority，从 reserved 划出等量空间
struct smem_trans_endpoint_t {
  char hostIp[64];
  uint16_t port;
  uint16_t reserved0;
  uint32_t flags;
  uint8_t priority;         // 新增字段：1字节，紧邻flags之后（偏移72）
  uint8_t reserved[55];    // 56 - 1(字段)，总大小仍为128字节
};
```

#### 规则 3.3 新增对外结构体必须预留reserved字段

新增对外结构体必须预留`reserved`字段，预留量建议64~128字节（跟随`HYBM_TLS_PATH_SIZE`=256、`SMEM_TLS_PATH_SIZE`=256等现有公开常量的惯例），为未来不破坏布局的扩展保留空间。

#### 规则 3.4 禁止修改已有字段类型和顺序

修改已有字段的类型、顺序或删除字段都会改变结构体大小与字段偏移，属于ABI破坏。确需演进时按规则3.1、3.2新增字段并以新字段承载新语义，旧字段保持不变。

#### 规则 3.5 枚举值显式赋值且仅允许末尾追加

对外枚举必须显式赋值：隐式取值在插入或重排成员时会发生整体漂移。已有枚举值禁止修改（编译期烧入调用方，且无符号痕迹可查）；新增枚举值只能在末尾追加并显式赋值。MemFabric现有枚举（`smem_bm_copy_type`、`smem_bm_mem_type`、`smem_trans_role_t`等）已遵循此规则。

[反例]：

```cpp
typedef enum {
  SMEM_TRANS_NONE = 0,
  SMEM_TRANS_BOTH,              // 禁止：插入到 SENDER/RECEIVER 之前，迫使后续枚举值整体漂移
  SMEM_TRANS_SENDER,
  SMEM_TRANS_RECEIVER,
  SMEM_TRANS_BUTT,
} smem_trans_role_t;
```

[正例]：

```cpp
typedef enum {
  SMEM_TRANS_NONE = 0,
  SMEM_TRANS_SENDER = 1,
  SMEM_TRANS_RECEIVER = 2,
  SMEM_TRANS_BOTH = 3,
  SMEM_TRANS_BUTT = 4,           // 仅末尾追加，显式赋值，保留 BUTT 作为结束标记
} smem_trans_role_t;
```

---

### 4. 符号与接口演进

#### 规则 4.1 公开符号只增不改

公开接口的实现符号禁止删除、重命名或修改签名（参数类型、顺序、个数）。C++ mangled name编码完整签名，实现侧重命名或改签名即使头文件同步修改，也会导致下游已编译产物链接失败。MemFabric C公开接口（`smem_bm_*`、`smem_trans_*`、`smem_shm_*`、`hybm_*`系列）的符号名本身就是ABI，不可变更。新能力以新增接口承载（如`smem_bm_create2`作为`smem_bm_create`的扩展版本）；旧接口废弃走保留加标记的deprecation流程（参见`smem_bm_set_group_event_handler`、`smem_bm_group_event_t`的废弃模式），不直接移除。

#### 规则 4.2 公开constexpr常量值禁止变更

公开头文件中的`constexpr`常量（状态码、特性值等）在调用方编译期直接烧入指令，变更后新旧二进制对同一常量的判断不一致。此类变更不改变任何符号与布局，是ABI变更中最隐蔽的一类，必须等同接口签名变更对待。

[反例]：

```cpp
// src/acc_links/include/acc_def.h
constexpr uint32_t UNO_512 = 512;  // 禁止：后续版本改为其他值，已编译调用方仍持旧值
```

#### 规则 4.3 默认参数值变更视同签名变更

默认参数在调用点展开，修改默认参数值会改变所有省略该参数的已编译调用方的行为，等同修改函数签名。`timeout = 1000`变更为其他取值必须按ABI变更评审。

---

### 5. 类布局

#### 规则 5.1 公开类新增虚函数只能加在末尾

虚函数在vtable中的槽位偏移是ABI的一部分，在中间插入虚函数会使后续虚函数槽位整体偏移，已编译的虚调用全部错位。公开类新增虚函数只能追加在现有虚函数之后。MemFabric的主要公开接口均为C风格（`smem_bm_*`、`hybm_*`等函数集合），不存在vtable约束；C++层acc_links（`ock::acc::AccTcpServer`、`ock::acc::AccHttpServer`、`ock::acc::AccTcpLink`等类）若需扩展，需遵守此规则。

#### 规则 5.2 公开类优先使用pimpl惯用法

公开类仅持有`std::unique_ptr<Impl> impl_`成员（8字节、布局稳定），实现细节全部封装在`Impl`中：实现侧任意演进都不影响公开类大小与布局。析构函数必须在实现文件中定义（`Impl`完整类型要求）。acc_links C++层类（如`ock::acc::AccTcpServer`）内部实现建议采用该模式，新增公开类必须沿用。

---

### 6. 构建约定

#### 规则 6.1 _GLIBCXX_USE_CXX11_ABI设置保持统一

MemFabric构建脚本（`build.sh`）默认设置`BUILD_OPEN_ABI=OFF`，即注入`-D_GLIBCXX_USE_CXX11_ABI=0`；根CMakeLists.txt在`BUILD_OPEN_ABI != ON`时也设置相同取值。下游examples、benchmarks、python_wrapper均显式固定为`_GLIBCXX_USE_CXX11_ABI=0`。任何构建侧对该宏取值的变更都可能引起`__cxx11`标签符号差异导致混链失败，必须按ABI变更评审。结合规则2.1，公开头文件不引入`std::string`/`std::list`时，该宏取值对公开接口的符号与布局无影响。
