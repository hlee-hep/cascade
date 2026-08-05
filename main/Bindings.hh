#pragma once

#include <pybind11/pybind11.h>

namespace cascade::python_binding
{
void BindPlugins(pybind11::module_ &module);
void BindState(pybind11::module_ &module);
void BindWorkflow(pybind11::module_ &module);
} // namespace cascade::python_binding
