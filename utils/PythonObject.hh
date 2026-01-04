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
    PythonObject(const std::string &moduleName, const std::string &className, const std::vector<py::object> &args = {})
    {
        EnsureInterpreterInitialized_();
        py::module_ mod = py::module_::import(moduleName.c_str());
        py::object cls = mod.attr(className.c_str());
        m_Instance = cls(*args);
    }

    template <typename... Args> py::object Call(const std::string &method, Args &&...args)
    {
        py::gil_scoped_acquire acquire;
        return m_Instance.attr(method.c_str())(std::forward<Args>(args)...);
    }

    py::object GetInstance() const { return m_Instance; }

  private:
    py::object m_Instance;

    static void EnsureInterpreterInitialized_()
    {
        static bool initialized = false;
        if (!initialized)
        {
            py::initialize_interpreter();
            initialized = true;
        }
    }
};
