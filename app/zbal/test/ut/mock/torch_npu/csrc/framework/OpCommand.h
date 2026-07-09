#ifndef TORCH_NPU_CSRC_FRAMEWORK_OP_COMMAND_H_
#define TORCH_NPU_CSRC_FRAMEWORK_OP_COMMAND_H_

#include <functional>
#include <string>

namespace at_npu {
namespace native {

class OpCommand {
public:
    static void RunOpApiV2(const std::string &opName, std::function<int()> fn)
    {
        (void)opName;
        fn();
    }
};

} // namespace native
} // namespace at_npu

#endif // TORCH_NPU_CSRC_FRAMEWORK_OP_COMMAND_H_
