#pragma once
#include "IAnalysisModule.hh"

class ExampleModule : public IAnalysisModule
{
  public:
    ExampleModule();
    void Description() const override;

  protected:
    void Init() override;
    void Execute() override;
    void Finalize() override;
};
