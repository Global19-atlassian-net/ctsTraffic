/*

Copyright (c) Microsoft Corporation
All rights reserved.

Licensed under the Apache License, Version 2.0 (the ""License""); you may not use this file except in compliance with the License. You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

THIS CODE IS PROVIDED ON AN  *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.

See the Apache Version 2.0 License for specific language governing permissions and limitations under the License.

*/
#pragma once
// ReSharper disable CppInconsistentNaming
// ReSharper disable CppClangTidyCppcoreguidelinesMacroUsage
// ReSharper disable CppClangTidyClangDiagnosticGnuZeroVariadicMacroArguments

// cpp headers
#include <stdexcept>
#include <vector>
#include <functional>
#include <memory>
// os headers
#include <Windows.h>
// ctl headers
#include <ctTimer.hpp>
#include <ctSockaddr.hpp>
//
// ** NOTE ** cannot include local project cts headers to avoid circular references
// - with the below exceptions : these do not include any cts* headers
//   -- ctsSafeInt.hpp
//   -- ctsStatistics.hpp
//
#include "ctsSafeInt.hpp"
#include "ctsStatistics.hpp"

namespace ctsTraffic
{
    //
    // Forward declaring ctsSocket project headers cannot be included due to circular references
    //
    // In the ctsTraffic namespace, typedef for all function types
    // - function pointers, functors, lambdas, etc.
    //
    class ctsSocket;
    typedef std::function<void(std::weak_ptr<ctsSocket>)> ctsSocketFunction;

    namespace ctsConfig
    {

        //
        // Declaring enum types in the ctsConfig namespace
        // - to be referenced by ctsConfig functions
        //
        enum class ProtocolType
        {
            NoProtocolSet,
            TCP,
            UDP
        };

        enum class TcpShutdownType
        {
            NoShutdownOptionSet,
            ServerSideShutdown,
            GracefulShutdown,
            HardShutdown
        };

        enum class IoPatternType
        {
            NoIoSet,
            Push,
            Pull,
            PushPull,
            Duplex,
            MediaStream
        };

        enum class StatusFormatting
        {
            NoFormattingSet,
            WttLog,
            ClearText,
            Csv,
            ConsoleOutput
        };

        // cannot be an enum class and have the below operator overloads work correctly
        enum OptionType
        {
            NoOptionSet = 0x0000,
            LoopbackFastPath = 0x0001,
            Keepalive = 0x0002,
            NonBlockingIo = 0x0004,
            HandleInlineIocp = 0x0008,
            ReuseUnicastPort = 0x0010,
            SetRecvBuf = 0x0020,
            SetSendBuf = 0x0040,
            EnableCircularQueueing = 0x0080,
            MsgWaitAll = 0x0100,
            // next enum  = 0x0200
        };

        ////////////////////////////////////////////////////////////////////////////////////////////////////
        //
        // custom operators for the OptionType enum (since it's an to be used as a bitmask)
        //
        ////////////////////////////////////////////////////////////////////////////////////////////////////

        // OR
        inline OptionType operator| (OptionType& lhs, OptionType rhs) noexcept
        {
            return static_cast<OptionType>(static_cast<unsigned long>(lhs) | static_cast<unsigned long>(rhs));
        }
        inline OptionType& operator|= (OptionType& lhs, OptionType rhs) noexcept
        {
            lhs = lhs | rhs;
            return lhs;
        }

        // AND
        inline OptionType operator& (OptionType lhs, OptionType rhs) noexcept
        {
            return static_cast<OptionType>(static_cast<unsigned long>(lhs) & static_cast<unsigned long>(rhs));
        }
        inline OptionType& operator&= (OptionType& lhs, OptionType rhs) noexcept
        {
            lhs = lhs & rhs;
            return lhs;
        }

        // XOR
        inline OptionType operator^ (OptionType lhs, OptionType rhs) noexcept
        {
            return static_cast<OptionType>(static_cast<unsigned long>(lhs) ^ static_cast<unsigned long>(rhs));
        }
        inline OptionType& operator^= (OptionType& lhs, OptionType rhs) noexcept
        {
            lhs = lhs ^ rhs;
            return lhs;
        }

        // NOT
        inline OptionType operator~ (OptionType lhs) noexcept
        {
            return static_cast<OptionType>(~static_cast<unsigned long>(lhs));
        }

        ////////////////////////////////////////////////////////////////////////////////////////////////////
        //
        // Members within the ctsConfig namespace that can be accessed anywhere within ctsTraffic
        //
        ////////////////////////////////////////////////////////////////////////////////////////////////////
        bool Startup(int argc, _In_reads_(argc) const wchar_t** argv);
        void Shutdown() noexcept;

        enum class PrintUsageOption
        {
            Default,
            Tcp,
            Udp,
            Logging,
            Advanced
        };
        void PrintUsage(PrintUsageOption option = PrintUsageOption::Default);
        void PrintSettings();

        void PrintLegend() noexcept;

        struct JitterFrameEntry
        {
            unsigned long m_bytesReceived = 0UL;
            long long m_sequenceNumber = 0LL;
            long long m_senderQpc = 0LL;
            long long m_senderQpf = 0LL;
            long long m_receiverQpc = 0LL;
            long long m_receiverQpf = 0LL;
            double m_estimatedTimeInFlightMs = 0;
        };
        void PrintJitterUpdate(const JitterFrameEntry& currentFrame, const JitterFrameEntry& previousFrame) noexcept;

        void PrintStatusUpdate() noexcept;
        void __cdecl PrintSummary(_In_z_ _Printf_format_string_ PCWSTR text, ...) noexcept;

        // Putting PrintDebugInfo as a macro to avoid running any code for debug printing if not necessary
#define PRINT_DEBUG_INFO(fmt, ...)                                            \
        do                                                                    \
        {                                                                     \
            if (!::ctsTraffic::ctsConfig::ShutdownCalled()) {                 \
                if (6 == ::ctsTraffic::ctsConfig::ConsoleVerbosity()) {       \
                    try { ::wprintf_s(fmt, ##__VA_ARGS__);  }  catch (...) {} \
                }                                                             \
            }                                                                 \
        }                                                                     \
        while ((void)0, 0)                                                    \

        void PrintErrorIfFailed(PCSTR what, unsigned long why) noexcept;
        void PrintErrorInfo(_In_z_ _Printf_format_string_ PCWSTR text, ...) noexcept;
        // Override will always print to console regardless of settings (important if can't even start)
        void PrintErrorInfoOverride(_In_ PCWSTR text) noexcept;

        inline DWORD Win32FromHresult(HRESULT hr) noexcept
        {
            if (HRESULT_SEVERITY(hr) == SEVERITY_ERROR && HRESULT_FACILITY(hr) == FACILITY_WIN32)
            {
                return HRESULT_CODE(hr);
            }
            return hr;
        }
        DWORD PrintThrownException() noexcept;
        void PrintException(DWORD why, _In_ PCWSTR what, _In_ PCWSTR where) noexcept;
        // Override will always print to console regardless of settings (important if can't even start)
        void PrintExceptionOverride(_In_ PCSTR exceptionText) noexcept;

        void PrintNewConnection(const ctl::ctSockaddr& localAddr, const ctl::ctSockaddr& remoteAddr) noexcept;
        void PrintConnectionResults(const ctl::ctSockaddr& localAddr, const ctl::ctSockaddr& remoteAddr, unsigned long error, const ctsTcpStatistics& stats) noexcept;
        void PrintConnectionResults(const ctl::ctSockaddr& localAddr, const ctl::ctSockaddr& remoteAddr, unsigned long error, const ctsUdpStatistics& stats) noexcept;
        void PrintConnectionResults(unsigned long error) noexcept;

        // Get* functions
        ctsSignedLongLong GetTcpBytesPerSecond() noexcept;
        ctsUnsignedLong GetMaxBufferSize() noexcept;
        ctsUnsignedLong GetMinBufferSize() noexcept;
        ctsUnsignedLong GetBufferSize() noexcept;
        ctsUnsignedLongLong GetTransferSize() noexcept;

        float GetStatusTimeStamp() noexcept;

        int  GetListenBacklog() noexcept;
        bool IsListening() noexcept;

        // Set* functions
        int SetPreBindOptions(SOCKET socket, const ctl::ctSockaddr& localAddress) noexcept;
        int SetPreConnectOptions(SOCKET) noexcept;

        // for the MediaStream pattern
        struct MediaStreamSettings
        {
            // set by ctsConfig from command-line arguments
            ctsSignedLongLong BitsPerSecond = 0;
            ctsUnsignedLong FramesPerSecond = 0;
            ctsUnsignedLong BufferDepthSeconds = 0;
            ctsUnsignedLong StreamLengthSeconds = 0;
            // internally calculated
            ctsUnsignedLong FrameSizeBytes = 0;
            ctsUnsignedLong StreamLengthFrames = 0;
            ctsUnsignedLong BufferedFrames = 0;

            ctsUnsignedLongLong CalculateTransferSize()
            {
                FAIL_FAST_IF_MSG(
                    0LL == BitsPerSecond,
                    "BitsPerSecond cannot be set to zero");
                FAIL_FAST_IF_MSG(
                    0 == FramesPerSecond,
                    "FramesPerSecond cannot be set to zero");
                FAIL_FAST_IF_MSG(
                    0 == StreamLengthSeconds,
                    "StreamLengthSeconds cannot be set to zero");
                FAIL_FAST_IF_MSG(
                    BitsPerSecond % 8LL != 0LL,
                    "The BitsPerSecond value (%lld) must be evenly divisible by 8", static_cast<long long>(BitsPerSecond));

                // number of frames to keep buffered - only relevant on the client
                if (!IsListening())
                {
                    FAIL_FAST_IF_MSG(
                        0 == BufferDepthSeconds,
                        "BufferDepthSeconds cannot be set to zero");

                    BufferedFrames = BufferDepthSeconds * FramesPerSecond;
                    if (BufferedFrames < BufferDepthSeconds || BufferedFrames < FramesPerSecond)
                    {
                        throw std::invalid_argument("The total buffered frames exceed the maximum allowed : review -BufferDepth and -FrameRate");
                    }
                }

                const ctsUnsignedLongLong totalStreamLengthFrames = StreamLengthSeconds * FramesPerSecond;
                if (totalStreamLengthFrames > MAXULONG32)
                {
                    throw std::invalid_argument("The total stream length in frame-count exceeds the maximum allowed to be streamed (2^32)");
                }

                // convert rate to bytes / second -> calculate the total # of bytes
                ctsUnsignedLongLong totalStreamLengthBytes = static_cast<unsigned long long>(static_cast<long long>(BitsPerSecond) / 8ULL * static_cast<unsigned long>(StreamLengthSeconds));

                // guarantee that the total stream length aligns evenly with total_frames
                if (totalStreamLengthBytes % totalStreamLengthFrames != 0)
                {
                    totalStreamLengthBytes -= totalStreamLengthBytes % totalStreamLengthFrames;
                }

                const ctsUnsignedLongLong totalFrameSizeBytes = totalStreamLengthBytes / totalStreamLengthFrames;
                if (totalFrameSizeBytes > MAXULONG32)
                {
                    throw std::invalid_argument("The frame size in bytes exceeds the maximum allowed to be streamed (2^32)");
                }

                FrameSizeBytes = static_cast<unsigned long>(totalFrameSizeBytes);
                if (FrameSizeBytes < 40)
                {
                    throw std::invalid_argument("The frame size is too small - it must be at least 40 bytes");
                }
                StreamLengthFrames = static_cast<unsigned long>(totalStreamLengthFrames);

                // guarantee frame alignment
                FAIL_FAST_IF_MSG(
                    static_cast<unsigned long long>(FrameSizeBytes) * static_cast<unsigned long long>(StreamLengthFrames) != totalStreamLengthBytes,
                    "FrameSizeBytes (%u) * StreamLengthFrames (%u) != TotalStreamLength (%llx)",
                    static_cast<unsigned long>(FrameSizeBytes), static_cast<unsigned long>(StreamLengthFrames), static_cast<unsigned long long>(totalStreamLengthBytes));

                return totalStreamLengthBytes;
            }
        };
        const MediaStreamSettings& GetMediaStream() noexcept;

        struct ctsConfigSettings
        {
            // dynamically initialize status details with current qpc
            ctsConfigSettings() noexcept :
                ConnectionStatusDetails(ctl::ctTimer::SnapQpcInMillis())
            {
            }
            ~ctsConfigSettings() noexcept = default;
            // non-copyable
            ctsConfigSettings(const ctsConfigSettings&) = delete;
            ctsConfigSettings& operator=(const ctsConfigSettings&) = delete;
            ctsConfigSettings(ctsConfigSettings&&) = delete;
            ctsConfigSettings& operator=(ctsConfigSettings&&) = delete;

            HANDLE CtrlCHandle = nullptr;
            PTP_CALLBACK_ENVIRON pTpEnvironment = nullptr;

            ctsSocketFunction CreateFunction;
            ctsSocketFunction ConnectFunction;
            ctsSocketFunction AcceptFunction;
            ctsSocketFunction IoFunction;
            ctsSocketFunction ClosingFunction; // optional

            ProtocolType Protocol = ProtocolType::NoProtocolSet;
            TcpShutdownType TcpShutdown = TcpShutdownType::NoShutdownOptionSet;
            IoPatternType IoPattern = IoPatternType::NoIoSet;
            OptionType Options = NoOptionSet;

            DWORD SocketFlags = 0;
            WORD  Port = 0;

            unsigned long long Iterations = 0;
            unsigned long long ServerExitLimit = 0;
            unsigned long AcceptLimit = 0;
            unsigned long ConnectionLimit = 0;
            unsigned long ConnectionThrottleLimit = 0;

            std::vector<ctl::ctSockaddr> ListenAddresses{};
            std::vector<ctl::ctSockaddr> TargetAddresses{};
            std::vector<ctl::ctSockaddr> BindAddresses{};

            // stats for status updates and summaries
            ctsConnectionStatistics ConnectionStatusDetails;
            ctsTcpStatistics TcpStatusDetails;
            ctsUdpStatistics UdpStatusDetails;

            unsigned long StatusUpdateFrequencyMilliseconds = 0;

            long long TcpBytesPerSecondPeriod = 100LL;
            long long StartTimeMilliseconds = 0;

            unsigned long TimeLimit = 0;
            unsigned long PrePostRecvs = 0;
            unsigned long PrePostSends = 0;
            unsigned long RecvBufValue = 0;
            unsigned long SendBufValue = 0;
            unsigned long KeepAliveValue = 0;

            unsigned long PushBytes = 0;
            unsigned long PullBytes = 0;

            unsigned long OutgoingIfIndex = 0;

            unsigned short LocalPortLow = 0;
            unsigned short LocalPortHigh = 0;

            bool UseSharedBuffer = false;
            bool ShouldVerifyBuffers = false;

            static const DWORD c_CriticalSectionSpinlock = 500ul;
        };

        ////////////////////////////////////////////////////////////////////////////////////////////////////
        ///
        /// Settings is defined in ctsConfig.cpp
        /// - it's made available to all consumers of ctsConfig.h through extern
        ///
        ////////////////////////////////////////////////////////////////////////////////////////////////////
        extern ctsConfigSettings* g_configSettings;

        SOCKET CreateSocket(int af, int type, int protocol, DWORD dwFlags);
        bool ShutdownCalled() noexcept;
        unsigned long ConsoleVerbosity() noexcept;
    } // namespace ctsConfig
} // namespace ctsTraffic
