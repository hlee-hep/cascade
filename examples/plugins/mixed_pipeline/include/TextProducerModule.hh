#pragma once

#include "IAnalysisModule.hh"

class TextProducerModule final : public IAnalysisModule
{
  public:
    TextProducerModule();
    void Description() const override;

  protected:
    void Init() override;
    void Execute() override;
    void Finalize() override;
};
