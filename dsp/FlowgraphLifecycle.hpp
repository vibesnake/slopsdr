// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include <functional>

namespace sdr::dsp::detail {

struct FlowgraphLifecycleActions {
    std::function<void()> startScheduler;
    std::function<void()> stopScheduler;
    std::function<void()> waitScheduler;
    std::function<void()> startSource;
    std::function<void()> stopSource;
};

class FlowgraphLifecycle final
{
public:
    explicit FlowgraphLifecycle(FlowgraphLifecycleActions actions);
    ~FlowgraphLifecycle();

    FlowgraphLifecycle(const FlowgraphLifecycle&) = delete;
    FlowgraphLifecycle& operator=(const FlowgraphLifecycle&) = delete;

    void start(const std::function<void()>& afterSchedulerStart = {});
    void stopAndWait();

    [[nodiscard]] bool running() const noexcept;

private:
    FlowgraphLifecycleActions m_actions;
    bool m_schedulerStartAttempted = false;
    bool m_sourceStarted = false;
    bool m_running = false;
};

}  // namespace sdr::dsp::detail
