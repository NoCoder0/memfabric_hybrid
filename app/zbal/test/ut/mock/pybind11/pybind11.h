/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * ZBAL is licensed under Mulan PSL v2.
 */

// Minimal mock for pybind11/pybind11.h for UT compilation only.
// The real zbal_deepep_config.h does not use any pybind11 types in its
// declarations; this mock exists solely to satisfy the #include directive.

#ifndef PYBIND11_MOCK_H_
#define PYBIND11_MOCK_H_

#include <cstdint>
#include <string>
#include <stdexcept>

namespace pybind11 {

class object {
public:
    object() = default;
};

class dict : public object {};
class list : public object {};
class module_ : public object {};

class error_already_set : public std::runtime_error {
public:
    error_already_set() : std::runtime_error("pybind11 error") {}
};

} // namespace pybind11

#define PYBIND11_OVERRIDE(name, ...)

#endif // PYBIND11_MOCK_H_