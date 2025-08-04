#include "ParamManager.hh"
#include "Logger.hh"
#include <iostream>
std::unordered_map<std::type_index, std::function<void(ParamManager *, const std::string &, const std::any &)>> ParamManager::casters;

void ParamManager::RegisterCommon()
{
    Register<bool>("dry_run", false, "simulate execution only");
    Register<bool>("force_run", false, "force execution");
}

void ParamManager::RegisterCommonLambdas(LambdaManager *lm) const { std::cout << "Not implemented yet" << std::endl; }

void ParamManager::SetParamFromAny(const std::string &key, const std::any &value)
{
    LOG_DEBUG("ParamManager", "[CALL] SetParamFromAny: this = " << this << ", key = " << key);
    auto it = casters.find(value.type());
    if (it != casters.end())
    {
        it->second(this, key, value);
    }
    else if (value.type() == typeid(std::string))
    {
        rawValues_[key] = value;
        values_[key] = std::any_cast<std::string>(value);
    }
    else
    {
        values_[key] = value.type().name();
        rawValues_[key] = value;
    }
}

std::any ParamManager::ConvertFromPy(const py::object &obj) const
{
    if (py::isinstance<py::bool_>(obj))
    {
        return std::any(obj.cast<bool>());
    }
    else if (py::isinstance<py::int_>(obj))
    {
        return std::any(obj.cast<int>());
    }
    else if (py::isinstance<py::float_>(obj))
    {
        return std::any(obj.cast<double>());
    }
    else if (py::isinstance<py::str>(obj))
    {
        return std::any(obj.cast<std::string>());
    }
    else if (py::isinstance<py::list>(obj))
    {
        if (py::len(obj) == 0) return std::any(std::vector<std::string>{});

        py::object first = obj[0];
        if (py::isinstance<py::str>(first))
        {
            return std::any(obj.cast<std::vector<std::string>>());
        }
        else if (py::isinstance<py::int_>(first))
        {
            return std::any(obj.cast<std::vector<int>>());
        }
        else if (py::isinstance<py::float_>(first))
        {
            return std::any(obj.cast<std::vector<double>>());
        }
        else
        {
            throw std::runtime_error("Unsupported list element type in ConvertFromPy().");
        }
    }
    else
    {
        throw std::runtime_error("Unsupported py::object type for conversion to std::any.");
    }
}
