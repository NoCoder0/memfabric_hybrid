# Upstream Software Notices and License Texts

The repository root LICENSE contains Mulan PSL v2. The following notices and license text describe referenced upstream material; they do not establish the origin of similar code or change applicable license terms.

## CANN Open Software License Agreement Version 2.0

```text
CANN Open Software License Agreement Version 2.0

This CANN Open Software License Agreement Version 2.0 (hereinafter referred to as this "Agreement") is a legal agreement between you and Huawei, and it governs your use, modification, or distribution of CANN Open Software (hereinafter referred to as "Software"). Please read this Agreement carefully.

If you are entering into this Agreement on behalf of a company or other legal entity, you represent that you have the legal authority to bind that entity to this Agreement, in which case "you" will mean the entity you represent.

BY DOWNLOADING, INSTALLING, OR USING THE SOFTWARE, YOU AGREE YOU HAVE FULLY UNDERSTOOD AND ACCEPTED THE TERMS CONTAINED HEREIN. IF YOU DO NOT AGREE TO ANY OF THE TERMS OF THIS AGREEMENT, OR IF YOU DO NOT QUALIFY FOR AGREEING TO THIS AGREEMENT, YOU ARE NOT AUTHORIZED TO AND SHALL NOT DOWNLOAD, INSTALL, OR MAKE ANY USE OF THE SOFTWARE.

1. Definition

1.1   Software means the APIs, source code files, binaries, and related documents of Compute Architecture for Neural Networks("CANN") that are licensable by Huawei, and provided and licensed under this Agreement.

1.2   Huawei AI Processors mean AI chipsets (i) branded with "Ascend", "Kirin"," Yueying" or other brands owned or controlled by Huawei; or (ii) manufactured (including have manufactured), supplied (including have supplied) or designed (including have designed) by Huawei.

2.  Grant of Intellectual Property Rights
2.1 Subject to the terms and conditions of this Agreement, including your full compliance thereof, Huawei hereby grants you a limited, worldwide, royalty-free, non-transferable, non-sublicensable, and revocable license for you to (i) download, use, modify, integrate, and distribute the Software or its derivative works for the purpose of developing software solely for use in systems with Huawei AI Processors and/or Software, and (ii) distribute any software developed based upon Software and/or its derivative works solely for use in systems with Huawei AI Processors and/or Software.

3.  Restrictions
3.1 You are not authorized to, and shall not use, modify, or distribute this Software or its derivative works for any other purposes than those expressly permitted by this Agreement. You shall not make any use of the Software or its derivative works to develop or distribute any software for use in systems with processors other than Huawei AI Processors. All rights not expressly granted herein are expressly reserved by Huawei.

3.2 You are not authorized to, and shall not remove, obscure, or alter any copyright or other notices in this Software or any part of it.

3.3 Distribution Restrictions
You may distribute the Software or its derivative works in any medium, whether in source or executable forms, for the purpose stipulated in Section 2; provided that you provide recipients with a copy of this Agreement, and retain all notices in the Software.

4. Disclaimer of Warranty and Limitation of Liability
THE SOFTWARE IS PROVIDED WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED. IN NO EVENT SHALL HUAWEI OR ANY OTHER COPYRIGHT HOLDER BE LIABLE TO YOU FOR ANY DAMAGES, INCLUDING, BUT NOT LIMITED TO ANY DIRECT, OR INDIRECT, SPECIAL OR CONSEQUENTIAL DAMAGES ARISING FROM YOUR USE OR INABILITY TO USE THE SOFTWARE, IN WHOLE OR IN PART, NO MATTER HOW IT IS CAUSED OR THE LEGAL THEORY IT IS BASED ON, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGES.

5. Termination
5.1 This Agreement will continue to apply until terminated by either you or Huawei as described below:
a．You may terminate this Agreement by ceasing your use of the Software;
b.  Huawei may at any time, terminate this Agreement if: (i) you fail to comply with any term of this Agreement; or (ii) you directly or indirectly initiate any legal proceeding against any individual or entity by alleging that the Software or any part of it infringes your intellectual property rights.
5.2 By termination, all the rights granted to you under this Agreement are terminated, and you shall cease to use and delete this Software or any derivative works immediately. All rights granted to you under this Agreement shall hereby be void ab initio in the event of termination in accordance with Section 5.1. b above. Huawei reserves the right to pursue any and all legal remedies available to enforce the terms and conditions of this Agreement or to protect Huawei’s intellectual property rights for such breach or violation. All provisions shall survive the termination of this Agreement except for Section 2 and Section 3.3.

6. MISCELLANEOUS
If the application of any provision of this Agreement to any particular facts or circumstances is held to be invalid or unenforceable by a court of competent jurisdiction, then (a) the validity and enforceability of such provision as applied to any other particular facts or circumstances and the validity of other provisions of this Agreement shall not in any way be affected or impaired thereby and (b) such provision shall be enforced to the maximum extent possible so as to affect the intent of the you and Huawei and reformed without further action by you and Huawei to the extent necessary to make such provision valid and enforceable.

END OF THE TERMS AND CONDITIONS
```

# CANN upstream reference notices

These notices reproduce the headers of the referenced upstream files.
A reference identifies a similar implementation or definition; it does not establish the original source.

## driver: pkg_inc/ascend_hal_define.h

Source: https://gitcode.com/cann/driver/blob/v9.0.0-rc.1/pkg_inc/ascend_hal_define.h

```text
/**
 * Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
```

## driver: pkg_inc/hal_pkg/trs_pkg.h

Source: https://gitcode.com/cann/driver/blob/v8.5.0/pkg_inc/hal_pkg/trs_pkg.h

```text
/**
 * Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 *
 * The code snippet comes from CANN project
 *
 * Copyright (c) Huawei Technologies Co., Ltd. 2012-2019. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
```

## driver: src/ascend_hal/svm/v2/devmm/devmm_virt_interface.h

Source: https://gitcode.com/cann/driver/blob/v8.5.0/src/ascend_hal/svm/v2/devmm/devmm_virt_interface.h

```text
/**
 * Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
```

## driver: src/sdk_driver/ts_agent/inc/tsch/task_struct.h

Source: https://gitcode.com/cann/driver/blob/v8.5.0/src/sdk_driver/ts_agent/inc/tsch/task_struct.h

```text
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */
```

## runtime: src/runtime/core/inc/sqe/v200_base/memcpy_sqe_common.h

Source: https://gitcode.com/cann/runtime/blob/v9.1.0/src/runtime/core/inc/sqe/v200_base/memcpy_sqe_common.h

```text
/**
 * Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
```

## runtime: src/runtime/core/inc/sqe/v100/stars_sqe.hpp

Source: https://gitcode.com/cann/runtime/blob/v9.2.0/src/runtime/core/inc/sqe/v100/stars_sqe.hpp

```text
/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
```

## hcomm: include/hcomm_res_defs.h

Source: https://gitcode.com/cann/hcomm/blob/v9.1.0/include/hcomm_res_defs.h

```text
/**
 * Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
```

## hcomm: include/hccl/hccl_res.h

Source: https://gitcode.com/cann/hcomm/blob/v9.0.0-beta.1/include/hccl/hccl_res.h

```text
/**
 * Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
```

## hcomm: include/hccl/hccl_types.h

Source: https://gitcode.com/cann/hcomm/blob/v9.0.0/include/hccl/hccl_types.h

```text
/**
 * Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
```

## hcomm: src/legacy/ascend950/framework/topo/topo_addr_info/src/rank_info_types.h

Source: https://gitcode.com/cann/hcomm/blob/v9.1.0/src/legacy/ascend950/framework/topo/topo_addr_info/src/rank_info_types.h

```text
/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
```

## runtime: include/external/acl/acl_rt.h

Source: https://gitcode.com/cann/runtime/blob/v8.5.0/include/external/acl/acl_rt.h

```text
/**
 * Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
```

## runtime: pkg_inc/runtime/rt_external_device.h

Source: https://gitcode.com/cann/runtime/blob/v8.5.0/pkg_inc/runtime/rt_external_device.h

```text
/**
 * Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
```

## runtime: src/dfx/adump/adump/exception/hccl_mc2_define.h

Source: https://gitcode.com/cann/runtime/blob/v9.0.0/src/dfx/adump/adump/exception/hccl_mc2_define.h

```text
/**
 * Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
```

## hcomm: src/platform/hccp/inc/network/hccp_common.h

Source: https://gitcode.com/cann/hcomm/blob/v8.5.0/src/platform/hccp/inc/network/hccp_common.h

```text
/**
 * @file hccp_common.h
 * @brief This module provides common structure about sockets and rdma for HCCL
 * @version Copyright (c) Huawei Technologies Co., Ltd. 2019-2025. All rights reserved.
 * @author
 * @date 2019-03-25
 * @defgroup libsocket socket interface
 * @defgroup librdma rdma interface
 * @defgroup libinit init interface
 */
```

## hcomm: src/platform/hccp/external_depends/rdma-core/include/infiniband/verbs.h

Source: https://gitcode.com/cann/hcomm/blob/v8.5.0/src/platform/hccp/external_depends/rdma-core/include/infiniband/verbs.h

```text
/**
 * Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 *
 * The code snippet comes from linux-rdma project
 *
 * Copyright (c) 2004, 2005 Topspin Communications.  All rights reserved.
 * Copyright (c) 2004, 2011-2012 Intel Corporation.  All rights reserved.
 * Copyright (c) 2005, 2006, 2007 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2005 PathScale, Inc.  All rights reserved.
 *
 *           OpenIB.org BSD license (MIT variant)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   - Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 *   - Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
```

## hcomm: src/legacy/ascend950/framework/topo/topo_addr_info/src/topo.c

Source: https://gitcode.com/cann/hcomm/blob/v9.1.0/src/legacy/ascend950/framework/topo/topo_addr_info/src/topo.c

```text
/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
```

## runtime: src/runtime/core/src/task/task_info/cmo/cmo_task_v100.cc

Source: https://gitcode.com/cann/runtime/blob/v9.1.0/src/runtime/core/src/task/task_info/cmo/cmo_task_v100.cc

```text
/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
```

## runtime: src/runtime/core/src/task/task_info/dma/rdma_task_v100.cc

Source: https://gitcode.com/cann/runtime/blob/v9.1.0/src/runtime/core/src/task/task_info/dma/rdma_task_v100.cc

```text
/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
```

## runtime: example/2_advanced_features/kernel/file_ops.cpp

Source: https://gitcode.com/cann/runtime/blob/v9.1.0/example/2_advanced_features/kernel/file_ops.cpp

```text
/**
 * Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
```

The full agreement is available in the
[CANN Open Software License Agreement Version 2.0](#cann-open-software-license-agreement-version-20) section above.

## HCOMM signing and packaging references

These references identify related implementations; the original source is not established.

## hcomm: scripts/sign/add_header_sign.py

Source: https://gitcode.com/cann/hcomm/blob/v9.0.0/scripts/sign/add_header_sign.py

```text
#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
#
#
#**************************************************************
# 文件名    ：add_sign_header_cann.py
# 版本号    ：初稿
# 生成日期  ：2025年11月25日
# 功能描述  ：根据bios_check_cfg.xml配置文件，根据其中配置属性，对各文件进行制作cms签名并绑定
# 使用方法  ：python add_bios_header.py $(DEVICE_RELEASE_DIR) $(DAVINCI_TOPDIR) $(PRODUCT_NAME) $(CHIP_NAME) $(signature_tag)
# 输入参数  ：DEVICE_RELEASE_DIR：待签名文件的根目录
#            DAVINCI_TOPDIR：工程根路径
#            PRODUCT_NAME：待扫描的产品名
#            CHIP_NAME：芯片名称
#            signature_tag：是否需要数字签名
#            蓝区签名步骤 1、加esbc头; 2、生成ini文件; 3、进行签名（参数控制，暂不启动）; 4、签名结果写入文件头
# 返回值    ：0:成功，-1:失败
# 修改历史  ：
# 日期    ：2025年11月25日
# 修改内容  ：创建文件
#
# Copyright (c) 2025 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
#
"""
```

## hcomm: scripts/sign/common_log.py

Source: https://gitcode.com/cann/hcomm/blob/v9.0.0/scripts/sign/common_log.py

```text
#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#**************************************************************
#
#          版权所有 (C), 2001-2011,华为技术有限公司
#
#**************************************************************
#文件名    : ci_log.py
#版本号    : 初稿
#生成日期  : 2012年7月25日
#功能描述  : 持续集成LOG模块
#使用方法  : import ci_log as cilog
#            cilog.method
#输入参数  : 参数1:41位change-id
#          : 参数2: 组件名称
#返回值    : 0:提交都通过；1:提交未都通过
#修改历史  :
#1.日期    : 2012年7月25日
#  修改内容: 创建文件
#
# Copyright (c) 2025 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
#
```

## hcomm: scripts/sign/community_sign_build.py

Source: https://gitcode.com/cann/hcomm/blob/v9.0.0/scripts/sign/community_sign_build.py

```text
#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# 签名命令：./client --config ./client.toml add --file-type p7s --key-type x509 --key-name SignCert --detached infile
# 传入签名文件
# 解析参数
# 验证参数
# 执行签名命令
# 判断.p7s文件是否生成，没有生成则签名失败
# 返回执行结果
# Purpose:
# Copyright Huawei Technologies Co., Ltd. 2010-2023. All rights reserved.
# ----------------------------------------------------------------------------
```

## hcomm: scripts/signtool/esbc_header/esbc_header.py

Source: https://gitcode.com/cann/hcomm/blob/v9.0.0/scripts/signtool/esbc_header/esbc_header.py

```text
#!/usr/bin/env python
#-*- coding: utf-8 -*-
#----------------------------------------------------------------------------
# Purpose:
# Copyright Huawei Technologies Co., Ltd. 2010-2025. All rights reserved.
#----------------------------------------------------------------------------
```

## hcomm: scripts/signtool/image_pack/hi_platform/__init__.py

Source: https://gitcode.com/cann/hcomm/blob/v9.0.0/scripts/signtool/image_pack/hi_platform/__init__.py

The upstream file is empty and contains no copyright header.

## hcomm: scripts/signtool/image_pack/hi_platform/platform.py

Source: https://gitcode.com/cann/hcomm/blob/v9.0.0/scripts/signtool/image_pack/hi_platform/platform.py

```text
#!/usr/bin/env python
#-*- coding: utf-8 -*-
#----------------------------------------------------------------------------
# Purpose:
# Copyright Huawei Technologies Co., Ltd. 2010-2025. All rights reserved.
#----------------------------------------------------------------------------
```

## hcomm: scripts/signtool/image_pack/image_pack.py

Source: https://gitcode.com/cann/hcomm/blob/v9.0.0/scripts/signtool/image_pack/image_pack.py

```text
#!/usr/bin/env python
#-*- coding: utf-8 -*-
#----------------------------------------------------------------------------
# Purpose:
# Copyright Huawei Technologies Co., Ltd. 2010-2025. All rights reserved.
#----------------------------------------------------------------------------
```

## hcomm: scripts/signtool/image_pack/ini_gen.py

Source: https://gitcode.com/cann/hcomm/blob/v9.0.0/scripts/signtool/image_pack/ini_gen.py

```text
#!/usr/bin/python
# -*- coding: UTF-8 -*-
#----------------------------------------------------------------------------
# Purpose:
# Copyright Huawei Technologies Co., Ltd. 2010-2022. All rights reserved.
# Author: fanwenyue
#----------------------------------------------------------------------------
```

## hcomm: scripts/signtool/image_pack/tools.py

Source: https://gitcode.com/cann/hcomm/blob/v9.0.0/scripts/signtool/image_pack/tools.py

```text
#!/usr/bin/env python
#-*- coding: utf-8 -*-
#----------------------------------------------------------------------------
# Purpose:
# Copyright Huawei Technologies Co., Ltd. 2010-2025. All rights reserved.
#----------------------------------------------------------------------------
```
