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
#ifndef MF_GENETTY_FUNCTIONS_H
#define MF_GENETTY_FUNCTIONS_H

#include <string>

#include "genetty_define.h"
#include "mf_monotonic_time.h"

namespace ock {
namespace openDCC {
using MonotonicTime = mf::MonotonicTime;

class Func {
public:
    /**
     * @brief Remove the leading and trailing blanks of the given src, blank means space, tab, cr and lf
     *
     * @param src          [in] src to be trimmed
     * @return trimmed src
     */
    static inline std::string Trim(const std::string &src);
};

ALWAYS_INLINE std::string Func::Trim(const std::string &src)
{
    constexpr const char *BLANK_CHARS = " \t\r\n";

    auto begin = src.find_first_not_of(BLANK_CHARS);
    if (begin == std::string::npos) {
        return "";
    }

    auto end = src.find_last_not_of(BLANK_CHARS);
    return src.substr(begin, end - begin + 1);
}
} // namespace openDCC
} // namespace ock

#endif // MF_GENETTY_FUNCTIONS_H
