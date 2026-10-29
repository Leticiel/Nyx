#pragma once

#include <functional>
#include <stdexcept>
#include <string>

namespace csystem
{
class Cancelled : public std::runtime_error
{
  public:
    Cancelled() : std::runtime_error("Operation cancelled")
    {
    }
};

struct JobCallbacks
{
    std::function<void(const std::string&)> progress;
    std::function<bool()> cancelled;
};

void set_job_callbacks(JobCallbacks callbacks);
void report_progress(const std::string& message);
void check_cancelled();
}
