#pragma once

#include <memory>
#include <pybind11/embed.h>
#include <string>
#include <utility>
#include <vector>

namespace py = pybind11;

class PythonObject
{
  public:
    PythonObject(const std::string &module_name, const std::string &class_name,
                 const std::vector<py::object> &args = {})
    {
        EnsureInterpreterInitialized();
        py::module_ mod = py::module_::import(module_name.c_str());
        py::object cls = mod.attr(class_name.c_str());
        instance_ = cls(*args);
    }

    template <typename... Args>
    py::object Call(const std::string &method, Args &&...args)
    {
        py::gil_scoped_acquire acquire;
        return instance_.attr(method.c_str())(std::forward<Args>(args)...);
    }

    py::object GetInstance() const { return instance_; }

  private:
    py::object instance_;

    static void EnsureInterpreterInitialized()
    {
        static bool initialized = false;
        if (!initialized)
        {
            py::initialize_interpreter();
            initialized = true;
        }
    }
};
