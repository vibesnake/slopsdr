// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "FlowgraphLifecycle.hpp"

#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace sdr::dsp::detail {
namespace {

void appendFailure(std::vector<std::string>& failures, const char* operation)
{
    try {
        throw;
    } catch (const std::exception& error) {
        failures.emplace_back(std::string(operation) + ": " + error.what());
    } catch (...) {
        failures.emplace_back(std::string(operation) + ": unknown error");
    }
}

std::string joinFailures(const std::vector<std::string>& failures)
{
    std::string message;
    for (const auto& failure : failures) {
        if (!message.empty()) {
            message += "; ";
        }
        message += failure;
    }
    return message;
}

}  // namespace

FlowgraphLifecycle::FlowgraphLifecycle(FlowgraphLifecycleActions actions)
    : m_actions(std::move(actions))
{
    if (!m_actions.startScheduler || !m_actions.stopScheduler ||
        !m_actions.waitScheduler) {
        throw std::invalid_argument(
            "Flowgraph lifecycle requires scheduler start, stop, and wait actions");
    }
}

FlowgraphLifecycle::~FlowgraphLifecycle()
{
    try {
        stopAndWait();
    } catch (const std::exception& error) {
        std::fprintf(
            stderr, "GNU Radio flowgraph cleanup failed: %s\n", error.what());
    } catch (...) {
        std::fputs(
            "GNU Radio flowgraph cleanup failed with an unknown error\n", stderr);
    }
}

void FlowgraphLifecycle::start(
    const std::function<void()>& afterSchedulerStart)
{
    if (m_running) {
        return;
    }

    if (m_actions.startSource) {
        m_actions.startSource();
        m_sourceStarted = true;
    }

    try {
        m_schedulerStartAttempted = true;
        m_actions.startScheduler();
        if (afterSchedulerStart) {
            afterSchedulerStart();
        }
        m_running = true;
    } catch (const std::exception& startError) {
        const std::string startMessage = startError.what();
        try {
            stopAndWait();
        } catch (const std::exception& cleanupError) {
            throw std::runtime_error(
                startMessage + "; startup cleanup failed: " + cleanupError.what());
        }
        throw;
    } catch (...) {
        try {
            stopAndWait();
        } catch (const std::exception& cleanupError) {
            throw std::runtime_error(
                std::string("Unknown flowgraph startup error; startup cleanup failed: ") +
                cleanupError.what());
        }
        throw;
    }
}

void FlowgraphLifecycle::stopAndWait()
{
    std::vector<std::string> failures;
    const bool schedulerNeedsCleanup = m_schedulerStartAttempted;
    const bool sourceNeedsCleanup = m_sourceStarted;
    m_schedulerStartAttempted = false;
    m_sourceStarted = false;
    m_running = false;

    if (schedulerNeedsCleanup) {
        try {
            m_actions.stopScheduler();
        } catch (...) {
            appendFailure(failures, "scheduler stop failed");
        }
        try {
            m_actions.waitScheduler();
        } catch (...) {
            appendFailure(failures, "scheduler wait failed");
        }
    }
    if (sourceNeedsCleanup && m_actions.stopSource) {
        try {
            m_actions.stopSource();
        } catch (...) {
            appendFailure(failures, "source stop failed");
        }
    }

    if (!failures.empty()) {
        throw std::runtime_error(joinFailures(failures));
    }
}

bool FlowgraphLifecycle::running() const noexcept
{
    return m_running;
}

}  // namespace sdr::dsp::detail
