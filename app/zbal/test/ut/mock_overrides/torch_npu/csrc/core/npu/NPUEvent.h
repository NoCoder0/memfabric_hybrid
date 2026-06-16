#ifndef TORCH_NPU_CSRC_CORE_NPU_NPU_EVENT_H_
#define TORCH_NPU_CSRC_CORE_NPU_NPU_EVENT_H_

#include <cstdint>

namespace c10_npu {

class NPUEvent {
public:
    explicit NPUEvent(uint32_t flag = 0) : flag_(flag) {}

    void record(const class NPUStream & /**/) {}
    void block(const class NPUStream & /**/) {}
    bool query() { return query_result_; }
    void set_query_result(bool val) { query_result_ = val; }

    operator void *() const { return reinterpret_cast<void *>(flag_); }
    void *event() const { return reinterpret_cast<void *>(flag_); }

private:
    uint32_t flag_ = 0;
    bool query_result_ = true;
};

} // namespace c10_npu

#endif // TORCH_NPU_CSRC_CORE_NPU_NPU_EVENT_H_
