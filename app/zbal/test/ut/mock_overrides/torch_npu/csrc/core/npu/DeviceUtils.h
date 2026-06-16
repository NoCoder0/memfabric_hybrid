#ifndef TORCH_NPU_CSRC_CORE_NPU_DEVICE_UTILS_H_
#define TORCH_NPU_CSRC_CORE_NPU_DEVICE_UTILS_H_

#include <ATen/Tensor.h>

namespace torch_npu {
namespace utils {

inline bool is_npu(const at::Tensor &tensor)
{
    return true;
}

} // namespace utils
} // namespace torch_npu

#endif // TORCH_NPU_CSRC_CORE_NPU_DEVICE_UTILS_H_
