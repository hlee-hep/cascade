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
                        interrupted_.store(true);
                    });
    }

    static bool IsInterrupted() { return interrupted_.load() || gROOT->IsInterrupted(); }

    static void SetInterrupted() { interrupted_.store(true); }
    static void Reset() { interrupted_.store(false); }

  private:
    static std::atomic<bool> interrupted_;
};

inline std::atomic<bool> InterruptManager::interrupted_ = false;