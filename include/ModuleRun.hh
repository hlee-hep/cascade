#pragma once

#include <exception>
#include <string>

enum class ModuleStatus
{
    Pending,
    Initializing,
    Running,
    Finalizing,
    Done,
    Skipped,
    Interrupted,
    Failed
};

enum class ModulePhase
{
    None,
    Init,
    Check,
    Execute,
    Finalize,
    Commit
};

inline const char *ToString(ModuleStatus status)
{
    switch (status)
    {
    case ModuleStatus::Pending:
        return "Pending";
    case ModuleStatus::Initializing:
        return "Initializing";
    case ModuleStatus::Running:
        return "Running";
    case ModuleStatus::Finalizing:
        return "Finalizing";
    case ModuleStatus::Done:
        return "Done";
    case ModuleStatus::Skipped:
        return "Skipped";
    case ModuleStatus::Interrupted:
        return "Interrupted";
    case ModuleStatus::Failed:
        return "Failed";
    }
    return "Unknown";
}

inline const char *ToString(ModulePhase phase)
{
    switch (phase)
    {
    case ModulePhase::None:
        return "None";
    case ModulePhase::Init:
        return "Init";
    case ModulePhase::Check:
        return "Check";
    case ModulePhase::Execute:
        return "Execute";
    case ModulePhase::Finalize:
        return "Finalize";
    case ModulePhase::Commit:
        return "Commit";
    }
    return "Unknown";
}

struct RunResult
{
    ModuleStatus Status = ModuleStatus::Pending;
    ModulePhase Phase = ModulePhase::None;
    std::string Message;
    std::exception_ptr Exception;
    std::string CacheDecision = "not_checked";
    std::string CacheReason;

    bool Succeeded() const { return Status == ModuleStatus::Done; }
    bool Failed() const { return Status == ModuleStatus::Failed; }
    bool IsTerminal() const
    {
        return Status == ModuleStatus::Done || Status == ModuleStatus::Skipped || Status == ModuleStatus::Interrupted ||
               Status == ModuleStatus::Failed;
    }
    bool AllowsDependents() const { return Status == ModuleStatus::Done || Status == ModuleStatus::Skipped; }
    bool HasException() const { return Exception != nullptr; }
};
