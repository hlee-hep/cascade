#pragma once

#include "IAnalysisModule.hh"

class ResonanceFitModule final : public IAnalysisModule
{
  public:
    ResonanceFitModule();
    void Description() const override;
    ModuleMetadata GetMetadata() const override;

  protected:
    void Init() override;
    void Execute() override;
    void Finalize() override;
};
