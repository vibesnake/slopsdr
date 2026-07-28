// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "GnuRadioReceiverBackend.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>

#include <array>
#include <cstdlib>
#include <cstdio>

namespace {

bool runCycle(
    sdr::dsp::GnuRadioReceiverBackend& receiver,
    sdr::radio::DemodulationMode mode,
    bool requireSpectrum = true)
{
    const auto modeResult = receiver.setDemodulationMode(mode);
    if (!modeResult.succeeded()) {
        std::fprintf(stderr, "%s\n", modeResult.message.c_str());
        return false;
    }
    const auto startResult = receiver.startReception();
    if (!startResult.succeeded()) {
        std::fprintf(stderr, "%s\n", startResult.message.c_str());
        return false;
    }

    if (requireSpectrum) {
        bool receivedSpectrum = false;
        QElapsedTimer timer;
        timer.start();
        while (!receivedSpectrum && timer.elapsed() < 1'000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(5);
            if (const auto runtimeError = receiver.takeRuntimeError()) {
                std::fprintf(stderr, "%s\n", runtimeError->message.c_str());
                return false;
            }
            receivedSpectrum = receiver.takeLatestSpectrumFrame().has_value();
        }
        if (!receivedSpectrum) {
            std::fprintf(
                stderr,
                "Synthetic GNU Radio smoke test produced no spectrum frame in mode %d\n",
                static_cast<int>(mode));
            return false;
        }
    } else {
        QThread::msleep(50);
    }

    const auto stopResult = receiver.stopReception();
    if (!stopResult.succeeded() || receiver.state().running) {
        std::fprintf(stderr, "%s\n", stopResult.message.c_str());
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    constexpr std::array modes{
        sdr::radio::DemodulationMode::Am,
        sdr::radio::DemodulationMode::Nfm,
        sdr::radio::DemodulationMode::Wfm,
        sdr::radio::DemodulationMode::Usb,
        sdr::radio::DemodulationMode::Lsb,
    };

    for (const auto mode : modes) {
        sdr::dsp::GnuRadioReceiverBackend receiver;
        if (!runCycle(receiver, mode)) {
            return EXIT_FAILURE;
        }
    }

    sdr::dsp::GnuRadioReceiverBackend repeatedReceiver;
    for (int cycle = 0; cycle < 3; ++cycle) {
        if (!runCycle(
                repeatedReceiver, sdr::radio::DemodulationMode::Am, false)) {
            return EXIT_FAILURE;
        }
    }

    std::puts("Synthetic GNU Radio start/stop smoke test passed");
    return EXIT_SUCCESS;
}
