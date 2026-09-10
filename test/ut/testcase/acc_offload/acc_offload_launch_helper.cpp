/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

// 先包含完整标准库链，再用 private->public hack 暴露私有静态方法，避免宏污染标准头。
#include <string>

#include "acc_offload_define.h"
#include "acc_offload_logger.h"
#include "mf_file_util.h"

#define private public
#include "acc_offload_launch.h"
#undef private

// 仅供 UT 使用：将私有静态方法转发为 C 符号，使 dladdr 在 helper so 内部解析，
// 验证 GetSelfLibDir 在共享库载体下的真实行为（生产场景）。
// 返回 0 表示成功，dirBuf 填入库目录；非 0 表示缓冲区不足。
extern "C" int AccOffloadLaunchUtGetSelfLibDir(char *dirBuf, unsigned int bufLen)
{
    if (dirBuf == nullptr) {
        return -1;
    }

    std::string dir = ock::offload::AccOffloadLaunchApi::GetSelfLibDir();
    if (dir.size() + 1 > bufLen) {
        return -1;
    }

    dir.copy(dirBuf, bufLen - 1);
    dirBuf[dir.size()] = '\0';
    return 0;
}
