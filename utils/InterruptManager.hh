#pragma once

#include <TROOT.h>
#include <atomic>
#include <csignal>
#include <iostream>

class InterruptManager
{
  public:
    static void Init()
    {
        std::signal(SIGINT,
                    [](int)
                    {
                        std::cerr << "\n[INTERRUPT] SIGINT (^C) received. Preparing to terminate...\n";
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