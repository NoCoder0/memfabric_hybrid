# MemFabric日志与文档规范

> **适用场景**：本规范适用于MemFabric仓所有代码中的日志打印，以及仓库内全部Markdown文档的检视。当PR修改涉及`.md`文件或日志输出时，必须执行本规范。

## 规范列表

| 规范编号 | 规范名称 | 类别 | 严重级别 |
|---------|---------|------|---------|
| 1.1 | 失败必须打印错误描述与关键信息 | 日志 | 高 |
| 1.2 | 调用非本组件接口失败须打印api名/错误码/参数 | 日志 | 中 |
| 1.3 | 外部参数校验和非本组件接口失败必须使用MemFabric CHECK宏 | 日志 | 高 |
| 1.4 | 日志内容全部使用英文 | 日志 | 高 |
| 1.5 | 内容无语法拼写错误、完整准确、避免自创缩写 | 日志 | 中 |
| 1.6 | 度量信息须添加度量单位 | 日志 | 中 |
| 1.7 | 不得以明文记录敏感信息 | 日志 | 高 |
| 1.8 | 日志不得携带个人信息 | 日志 | 高 |
| 1.9 | 性能敏感流程不得记录run日志、生产环境不持续输出DEBUG | 日志 | 中 |
| 1.10 | 避免高频循环体内打印重复错误日志 | 日志 | 中 |
| 1.11 | 单行日志长度不超过1024字符 | 日志 | 中 |
| 2.1 | 文档表格与列表项须逐项完整 | 文档写作 | 中 |

## 说明

本规范日志部分（第1章）依据 MemFabric 仓内日志实践（`SM_LOG_ERROR` / `BM_LOG_ERROR` / `LOG_ERROR` / `STORE_LOG_ERROR` 等模块级宏）和仓内既有 PR 检视检查项整理。文档写作规范（第2章）直接遵循 GitCode CANN 社区《文档写作规范》，不在本文件重复。

参与 MemFabric 代码仓开发的开发者提交日志或文档相关变更时，首先需要遵循本规范内容，其余遵循 MemFabric 通用编码规范。如果对规则有异议，建议提交 issue 并说明理由，经项目维护者评审后可接纳并修改生效。

## 适用范围

MemFabric 代码仓所有模块的日志打印、Markdown 文档、README、示例与脚本说明的检视。

---

### 日志规范

#### 规则 1.1 失败必须打印错误描述与关键信息

执行出错或失败时，日志必须进行错误描述，并打印出可能造成该失败的关键信息，不得静默返回失败。

[反例]：未打印日志及关键信息

```cpp
auto ret = ParseStoreUrl(store_url, &parsed_addr);
if (ret != SUCCESS) {
  return FAILED;
}
```

[正例]：

```cpp
auto ret = ParseStoreUrl(store_url, &parsed_addr);
if (ret != SUCCESS) {
  STORE_LOG_ERROR("Failed to parse store url, url=" << store_url << ", ret=" << ret);
  return FAILED;
}
```

#### 规则 1.2 调用非本组件接口失败须打印api名/错误码/参数

调用非本组件接口失败时，需要将调用的api名字、失败错误码以及参数打印出来，以便问题定位。非本组件接口包括 ACL、URMA/IBverbs（RDMA）、HCOM/HCOMM（主机端/设备端通信）、etcd 客户端、OpenSSL、securec、JSON 库、动态库加载接口（dlopen/dlsym），以及 socket、setsockopt、connect、poll、read、write、open 等系统接口。参考格式 `Call api:xxx failed, ret:xxx, param:xxx.`。

[反例]：未打印函数名及入参

```cpp
auto ret = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
if (ret != 0) {
  LOG_ERROR("Failed to set socket option.");
  return FAILED;
}
```

[正例]：

```cpp
auto ret = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
if (ret != 0) {
  LOG_ERROR("Call api:setsockopt failed, ret=" << ret << ", fd=" << fd
                                               << ", option=SO_RCVTIMEO, errno=" << errno);
  return FAILED;
}
```

> 对 ACL/URMA/HCOM 等已有模块级封装或统一 CHECK 宏的接口，直接使用项目约定的校验宏统一处理，参见规则 1.3。

#### 规则 1.3 外部参数校验和非本组件接口失败必须使用项目约定的 CHECK 宏

外部参数校验失败、调用非本组件接口失败（包含系统接口失败）时，必须优先使用项目约定的 CHECK/VALIDATE 宏统一处理返回、上报 error msg 并打印错误日志。错误信息必须包含可能导致当前错误的关键信息，例如参数名、参数值、合法范围、API 名称、返回码、`errno`、`strerror(errno)`、设备 ID、rankId、端口、fd、超时时间等。

常用宏包括 `SM_VALIDATE_RETURN`（SMEM）、`SM_ASSERT_RETURN`、`SM_LOG_ERROR_RETURN_IT_IF_NOT_OK`、各模块自封装的 `*_ASSERT_RETURN` / `*_VALIDATE_RETURN` 等。不得只返回错误码，也不得只使用模块级 ERROR 日志宏而遗漏错误码/关键参数。

[反例]：外部参数校验失败时只打印日志，未上报 error msg

```cpp
if (client_desc == nullptr) {
  SM_LOG_ERROR("client_desc is nullptr");
  return PARAM_INVALID;
}
```

[正例]：

```cpp
SM_ASSERT_RETURN(client_desc != nullptr, PARAM_INVALID);
SM_VALIDATE_RETURN(port >= kMinPort && port <= kMaxPort,
                   "Invalid listen_port:" << port << ", valid range is [" << kMinPort << ", " << kMaxPort << "]",
                   PARAM_INVALID);
```

[反例]：系统接口失败时未使用 CHECK/VALIDATE 宏，且缺少 error msg 上报

```cpp
int32_t ret = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
if (ret != 0) {
  LOG_ERROR("setsockopt failed");
  return FAILED;
}
```

[正例]：

```cpp
int32_t ret = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
SM_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret, "Call api:setsockopt failed, ret=" << ret << ", fd=" << fd
                                                                         << ", errno=" << errno);
```

[正例]：ACL/HCOM 等已有统一错误码处理路径时直接用仓内封装

```cpp
auto ret = aclrtSetDevice(device_id);
if (ret != ACL_ERROR_NONE) {
  SM_LOG_ERROR("Call api:aclrtSetDevice failed, ret=" << ret << ", device_id=" << device_id);
  return FAILED;
}
// 或如果调用点已由 hybm 内部统一封装，直接检查封装返回值即可
auto ret = hybm_transport_connect(entity_, rank_ids.data(), rank_ids.size(), 0);
SM_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret, "hybm_transport_connect failed, rank_count=" << rank_ids.size());
```

#### 规则 1.4 日志内容全部使用英文

日志内容全部使用英文描述，不得使用拼音、中文和中文符号。

#### 规则 1.5 内容无语法拼写错误、完整准确、避免自创缩写

日志内容不能存在语法、拼写错误，表达应完整准确、言简意赅，避免自创缩写。

#### 规则 1.6 度量信息须添加度量单位

日志中涉及度量信息（如耗时、数据量、带宽）需要添加度量单位。

#### 规则 1.7 不得以明文记录敏感信息

不能以明文的形式记录敏感信息，比如密码、密钥、token等。

#### 规则 1.8 日志不得携带个人信息

日志内容中不得携带个人信息。

#### 规则 1.9 性能敏感流程不得记录run日志、生产环境不持续输出DEBUG

性能敏感流程中不能记录 run 日志（INFO 级），例如 hybm 数据面传输、SMEM BM/TRANS 搬运、RDMA/URMA 通信的 hot path；生产环境不应出现 DEBUG 级别的持续输出。

#### 规则 1.10 避免高频循环体内打印重复错误日志

避免高频循环体内打印重复错误日志，否则将导致有用的错误信息被掩盖。

#### 规则 1.11 单行日志长度不超过1024字符

单行日志输出长度不得超过1024个字符。

---

### 文档写作规范

文档（`.md`）写作规范直接遵循 GitCode CANN 社区[《文档写作规范》](https://gitcode.com/cann/community/blob/master/contributor/docs/document_writing_specs.md)。该规范涵盖文件命名、标题、字体样式、图片、代码块、列表、链接、锚点、表格、标点符号等全部文档写作要求（含"数字/单位/中英文之间不加空格、产品名称例外""中文文档使用全角标点、数字半角"等规则）。检视文档变更时先以此为准（community 规范不在此重复）。MemFabric 补充以下检查项：

#### 规则 2.1 文档表格与列表项须逐项完整

文档中的表格、列表，每一项必须有对应的说明内容，不得出现空单元格或缺失项。
