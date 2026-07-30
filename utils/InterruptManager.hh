#pragma once

#include <TROOT.h>
#include <csignal>

class InterruptManager
{
  public:
    static void Init()
    {
        std::signal(SIGINT,
                    [](int)
                    {
                        m_Interrupted = 1;
                    });
    }

    static bool IsInterrupted() { return m_Interrupted != 0 || gROOT->IsInterrupted(); }

    static void SetInterrupted() { m_Interrupted = 1; }
    static void Reset()
    {
        m_Interrupted = 0;
        if (gROOT) gROOT->SetInterrupt(false);
    }

  private:
    static volatile std::sig_atomic_t m_Interrupted;
};

inline volatile std::sig_atomic_t InterruptManager::m_Interrupted = 0;
