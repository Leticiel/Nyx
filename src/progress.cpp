#include "progress.hpp"
#include <iostream>
#include <utility>

namespace csystem
{
namespace
{
thread_local JobCallbacks callbacks;
}

void set_job_callbacks(JobCallbacks value)
{
    callbacks = std::move(value);
}

void check_cancelled()
{
    if (callbacks.cancelled && callbacks.cancelled())
    {
        throw Cancelled();
    }
}

void report_progress(const std::string& message)
{
    check_cancelled();
    if (callbacks.progress)
    {
        callbacks.progress(message);
    }
    else
    {
        std::cout << message << std::endl;
    }
}
}
