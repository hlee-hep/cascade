#pragma once

#include <TROOT.h>
#include "Logger.hh"
#include <atomic>
#include <csignal>

class InterruptManager
{
  public:
    static void Init()
    {
        std::signal(SIGINT,
                    [](int)
                    {
                        LOG_WARN("InterruptManager", "SIGINT (^C) received. Preparing to terminate...");
                        m_Interrupted.store(true);
                    });
    }

    static bool IsInterrupted() { return m_Interrupted.load() || gROOT->IsInterrupted(); }

    static void SetInterrupted() { m_Interrupted.store(true); }
    static void Reset() { m_Interrupted.store(false); }

  private:
    static std::atomic<bool> m_Interrupted;
};

inline std::atomic<bool> InterruptManager::m_Interrupted = false;
