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
    LambdaHolder(T func, std::vector<std::string> columns) : m_Func(std::move(func)), m_Columns(std::move(columns)) {}

    T Get() const { return m_Func; }
    std::vector<std::string> GetColumns() const override { return m_Columns; }
    std::type_index GetType() const override { return std::type_index(typeid(T)); }

  private:
    T m_Func;
    std::vector<std::string> m_Columns;
};

class LambdaManager
{
  public:
    template <typename T> void Register(const std::string &name, T func, std::vector<std::string> columns)
    {
        if (m_Lambdas.count(name))
        {
            throw std::runtime_error("Lambda with name '" + name + "' already registered");
        }
        m_Lambdas[name] = std::make_shared<LambdaHolder<T>>(std::move(func), std::move(columns));
    }

    template <typename T> T Get(const std::string &name) const
    {
        auto it = m_Lambdas.find(name);
        if (it == m_Lambdas.end())
        {
            throw std::runtime_error("Lambda with name '" + name + "' not found");
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
        auto it = m_Lambdas.find(name);
        if (it == m_Lambdas.end())
        {
            throw std::runtime_error("Lambda with name '" + name + "' not found");
        }
        return it->second->GetColumns();
    }

    template <typename T> std::pair<T, std::vector<std::string>> UseLambda(const std::string &name) const { return {Get<T>(name), GetColumns(name)}; }

    std::vector<std::string> ListRegisteredNames() const
    {
        std::vector<std::string> names;
        for (const auto &[name, _] : m_Lambdas)
            names.push_back(name);
        return names;
    }

  private:
    std::map<std::string, std::shared_ptr<LambdaBase>> m_Lambdas;
};
