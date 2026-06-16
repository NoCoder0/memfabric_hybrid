#ifndef ZBAL_TEST_MOCK_DL_CANN_API_H
#define ZBAL_TEST_MOCK_DL_CANN_API_H

// Include the real dl_cann_api.h with private members exposed for mocking
#define private public
#include "../../../src/csrc/under_api/cann/dl_cann_api.h"
#undef private

// Helper to set mock function pointers
namespace zbal {
namespace underapi {

inline void SetMockAclrtGetDevice(aclrtGetDeviceFunc fn)
{
    DlCannApi::pAclrtGetDevice = fn;
}

inline void SetMockAclrtReserveMemAddress(aclrtReserveMemAddressFunc fn)
{
    DlCannApi::pAclrtReserveMemAddress = fn;
}

} // namespace underapi
} // namespace zbal

#endif // ZBAL_TEST_MOCK_DL_CANN_API_H