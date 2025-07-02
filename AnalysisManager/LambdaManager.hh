#pragma once
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <typeindex>
#include <typeinfo>
#include <vector>

class LambdaBase
{
  public:
    virtual ~LambdaBase() = default;
    virtual std::vector<std::string> GetColumns() const = 0;
    virtual std::type_index GetType() const = 0;
};

template <typename T> class LambdaHolder : public LambdaBase
{
  public:
    LambdaHolder(T func, std::vector<std::string> columns)
        : func_(std::move(func)), columns_(std::move(columns))
    {
    }

    T Get() const { return func_; }
    std::vector<std::string> GetColumns() const override { return columns_; }
    std::type_index GetType() const override
    {
        return std::type_index(typeid(T));
    }

  private:
    T func_;
    std::vector<std::string> columns_;
};

class LambdaManager
{
  public:
    template <typename T>
    void Register(const std::string &name, T func,
                  std::vector<std::string> columns)
    {
        if (lambdas.count(name))
        {
            throw std::runtime_error("Lambda with name '" + name +
                                     "' already registered");
        }
        lambdas[name] = std::make_shared<LambdaHolder<T>>(std::move(func),
                                                          std::move(columns));
    }

    template <typename T> T Get(const std::string &name) const
    {
        auto it = lambdas.find(name);
        if (it == lambdas.end())
        {
            throw std::runtime_error("Lambda with name '" + name +
                                     "' not found");
        }

        auto base = it->second;
        if (base->GetType() != std::type_index(typeid(T)))
        {
            throw std::runtime_error("Lambda type mismatch for '" + name + "'");
        }

        auto derived = std::static_pointer_cast<LambdaHolder<T>>(base);
        return derived->Get();
    }

    std::vector<std::string> GetColumns(const std::string &name) const
    {
        auto it = lambdas.find(name);
        if (it == lambdas.end())
        {
            throw std::runtime_error("Lambda with name '" + name +
                                     "' not found");
        }
        return it->second->GetColumns();
    }

    template <typename T>
    std::pair<T, std::vector<std::string>>
    UseLambda(const std::string &name) const
    {
        return {Get<T>(name), GetColumns(name)};
    }

    std::vector<std::string> ListRegisteredNames() const
    {
        std::vector<std::string> names;
        for (const auto &[name, _] : lambdas)
            names.push_back(name);
        return names;
    }

  private:
    std::map<std::string, std::shared_ptr<LambdaBase>> lambdas;
};
