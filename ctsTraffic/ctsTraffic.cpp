/*

Copyright (c) Microsoft Corporation
All rights reserved.

Licensed under the Apache License, Version 2.0 (the ""License""); you may not use this file except in compliance with the License. You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

THIS CODE IS PROVIDED ON AN  *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.

See the Apache Version 2.0 License for specific language governing permissions and limitations under the License.

*/

// ReSharper disable once CppUnusedIncludeDirective
#include "targetver.h"

// CRT headers
#include <cstdio>
#include <exception>
// os headers
#include <Windows.h>
// ctl headers
#include <ctThreadPoolTimer.hpp>
// wil headers
#include <wil/stl.h>
#include <wil/resource.h>
// local headers
#include "ctsConfig.h"
#include "ctsSocketBroker.h"

using namespace ctsTraffic;
using namespace ctl;
using namespace std;


// global ptr for easing debugging
ctsSocketBroker* g_SocketBroker = nullptr;

BOOL WINAPI CtrlBreakHandlerRoutine(DWORD) noexcept
{
    // handle all exit types - notify config that it's time to shutdown
    ctsConfig::Shutdown();
    return TRUE;
}

int _cdecl wmain(int argc, _In_reads_z_(argc) const wchar_t** argv)
{
    WSADATA wsadata{};
    const int wsError = WSAStartup(WINSOCK_VERSION, &wsadata);
    if (wsError != 0)
    {
        wprintf(L"ctsTraffic failed at WSAStartup [%d]\n", wsError);
        return wsError;
    }

    DWORD err = ERROR_SUCCESS;
    try
    {
        if (!ctsConfig::Startup(argc, argv))
        {
            ctsConfig::Shutdown();
            err = ERROR_INVALID_DATA;
        }
    }
    catch (const ctsSafeIntException& e)
    {
        ctsConfig::PrintErrorInfoOverride(wil::str_printf<std::wstring>(L"Invalid parameters : %ws", ctsPrintSafeIntException(e)).c_str());
        ctsConfig::Shutdown();
        err = ERROR_INVALID_DATA;
    }
    catch (const invalid_argument& e)
    {
        ctsConfig::PrintErrorInfoOverride(wil::str_printf<std::wstring>(L"Invalid argument specified: %hs", e.what()).c_str());
        ctsConfig::Shutdown();
        err = ERROR_INVALID_DATA;
    }
    catch (const exception& e)
    {
        ctsConfig::PrintExceptionOverride(e.what());
        ctsConfig::Shutdown();
        err = ERROR_INVALID_DATA;
    }

    if (err == ERROR_INVALID_DATA)
    {
        wprintf(
            L"\n\n"
            L"For more information on command line options, specify -Help\n"
            L"ctsTraffic.exe -Help:[tcp] [udp] [logging] [advanced]\n"
            L"\t- <default> == prints this usage statement\n"
            L"\t- tcp : prints usage for TCP-specific options\n"
            L"\t- udp : prints usage for UDP-specific options\n"
            L"\t- logging : prints usage for logging options\n"
            L"\t- advanced : prints the usage for advanced and experimental options\n"
            L"\n\n");
        return err;
    }

    try
    {
        if (!SetConsoleCtrlHandler(CtrlBreakHandlerRoutine, TRUE))
        {
            THROW_WIN32_MSG(GetLastError(), "SetConsoleCtrlHandler");
        }

        ctsConfig::PrintSettings();
        ctsConfig::PrintLegend();

        // set the start timer as close as possible to the start of the engine
        ctsConfig::Settings->StartTimeMilliseconds = ctTimer::ctSnapQpcInMillis();
        std::shared_ptr<ctsSocketBroker> broker(std::make_shared<ctsSocketBroker>());
        g_SocketBroker = broker.get();
        broker->start();

        ctThreadpoolTimer status_timer;
        status_timer.schedule_reoccuring(ctsConfig::PrintStatusUpdate, 0LL, ctsConfig::Settings->StatusUpdateFrequencyMilliseconds);
        if (!broker->wait(ctsConfig::Settings->TimeLimit > 0 ? ctsConfig::Settings->TimeLimit : INFINITE))
        {
            ctsConfig::PrintSummary(L"\n ** Timelimit of %lu reached **\n", static_cast<unsigned long>(ctsConfig::Settings->TimeLimit));
        }
    }
    catch (const ctsSafeIntException& e)
    {
        ctsConfig::PrintErrorInfoOverride(wil::str_printf<std::wstring>(L"ctsTraffic failed when converting integers : %ws", ctsPrintSafeIntException(e)).c_str());
        ctsConfig::Shutdown();
        return ERROR_INVALID_DATA;
    }
    catch (const wil::ResultException& e)
    {
        ctsConfig::PrintExceptionOverride(e.what());
        ctsConfig::Shutdown();
        return e.GetErrorCode();
    }
    catch (const bad_alloc&)
    {
        ctsConfig::PrintErrorInfoOverride(L"ctsTraffic failed: Out of Memory");
        ctsConfig::Shutdown();
        return ERROR_OUTOFMEMORY;
    }
    catch (const exception& e)
    {
        ctsConfig::PrintErrorInfoOverride(wil::str_printf<std::wstring>(L"ctsTraffic failed: %hs", e.what()).c_str());
        ctsConfig::Shutdown();
        return ERROR_CANCELLED;
    }

    const auto total_time_run = ctTimer::ctSnapQpcInMillis() - ctsConfig::Settings->StartTimeMilliseconds;

    // write out the final status update
    ctsConfig::PrintStatusUpdate();

    ctsConfig::Shutdown();

    ctsConfig::PrintSummary(
        L"\n\n"
        L"  Historic Connection Statistics (all connections over the complete lifetime)  \n"
        L"-------------------------------------------------------------------------------\n"
        L"  SuccessfulConnections [%lld]   NetworkErrors [%lld]   ProtocolErrors [%lld]\n",
        ctsConfig::Settings->ConnectionStatusDetails.successful_completion_count.get(),
        ctsConfig::Settings->ConnectionStatusDetails.connection_error_count.get(),
        ctsConfig::Settings->ConnectionStatusDetails.protocol_error_count.get());

    if (ctsConfig::Settings->Protocol == ctsConfig::ProtocolType::TCP)
    {
        ctsConfig::PrintSummary(
            L"\n"
            L"  Total Bytes Recv : %lld\n"
            L"  Total Bytes Sent : %lld\n",
            ctsConfig::Settings->TcpStatusDetails.bytes_recv.get(),
            ctsConfig::Settings->TcpStatusDetails.bytes_sent.get());
    }
    else
    {
        // currently don't track UDP server stats
        if (!ctsConfig::IsListening())
        {
            const auto successfulFrames = ctsConfig::Settings->UdpStatusDetails.successful_frames.get();
            const auto droppedFrames = ctsConfig::Settings->UdpStatusDetails.dropped_frames.get();
            const auto duplicateFrames = ctsConfig::Settings->UdpStatusDetails.duplicate_frames.get();
            const auto errorFrames = ctsConfig::Settings->UdpStatusDetails.error_frames.get();

            const auto totalFrames =
                successfulFrames +
                droppedFrames +
                duplicateFrames +
                errorFrames;
            ctsConfig::PrintSummary(
                L"\n"
                L"  Total Bytes Recv : %lld\n"
                L"  Total Successful Frames : %lld (%f)\n"
                L"  Total Dropped Frames : %lld (%f)\n"
                L"  Total Duplicate Frames : %lld (%f)\n"
                L"  Total Error Frames : %lld (%f)\n",
                ctsConfig::Settings->UdpStatusDetails.bits_received.get() / 8LL,
                successfulFrames,
                totalFrames > 0 ? static_cast<double>(successfulFrames) / totalFrames * 100.0 : 0.0,
                droppedFrames,
                totalFrames > 0 ? static_cast<double>(droppedFrames) / totalFrames * 100.0 : 0.0,
                duplicateFrames,
                totalFrames > 0 ? static_cast<double>(duplicateFrames) / totalFrames * 100.0 : 0.0,
                errorFrames,
                totalFrames > 0 ? static_cast<double>(errorFrames) / totalFrames * 100.0 : 0.0);
        }
    }
    ctsConfig::PrintSummary(
        L"  Total Time : %lld ms.\n",
        static_cast<long long>(total_time_run));

    long long error_count =
        ctsConfig::Settings->ConnectionStatusDetails.connection_error_count.get() +
        ctsConfig::Settings->ConnectionStatusDetails.protocol_error_count.get();
    if (error_count > MAXINT)
    {
        error_count = MAXINT;
    }
    return static_cast<int>(error_count);
}
