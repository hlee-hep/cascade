#pragma once

#include "IAnalysisModule.hh"

class RootEventModule final : public IAnalysisModule
{
  public:
    RootEventModule();
    void Description() const override;

  protected:
    void Init() override;
    void Execute() override;
    void Finalize() override;
};
