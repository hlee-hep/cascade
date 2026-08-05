#pragma once

#include "IAnalysisModule.hh"

class ToyDimuonSourceModule final : public IAnalysisModule
{
  public:
    ToyDimuonSourceModule();
    void Description() const override;
    ModuleMetadata GetMetadata() const override;

  protected:
    void Init() override;
    void Execute() override;
    void Finalize() override;
};
