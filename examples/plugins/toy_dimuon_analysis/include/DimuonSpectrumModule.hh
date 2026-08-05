#pragma once

#include "IAnalysisModule.hh"

class DimuonSpectrumModule final : public IAnalysisModule
{
  public:
    DimuonSpectrumModule();
    void Description() const override;
    ModuleMetadata GetMetadata() const override;

  protected:
    void Init() override;
    void Execute() override;
    void Finalize() override;
};
