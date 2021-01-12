/*

Copyright (c) Microsoft Corporation
All rights reserved.

Licensed under the Apache License, Version 2.0 (the ""License""); you may not use this file except in compliance with the License. You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

THIS CODE IS PROVIDED ON AN  *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.

See the Apache Version 2.0 License for specific language governing permissions and limitations under the License.

*/

#pragma once

// ctl headers
#include <ctTimer.hpp>
// project headers
#include "ctsConfig.h"
#include "ctsIOTask.hpp"
#include "ctsSafeInt.hpp"

namespace ctsTraffic
{

    typedef struct ctsIOPatternRateLimitThrottle_t     ctsIOPatternRateLimitThrottle;
    typedef struct ctsIOPatternRateLimitDontThrottle_t ctsIOPatternRateLimitDontThrottle;

    template <typename Protocol>
    struct ctsIOPatternRateLimitPolicy
    {
        void update_time_offset(ctsTask&, const ctsSignedLongLong& _buffer_size) noexcept = delete;
    };


    ///
    /// ctsIOPatternRateLimitDontThrottle
    ///
    template<>
    struct ctsIOPatternRateLimitPolicy < ctsIOPatternRateLimitDontThrottle >
    {

        // ReSharper disable once CppMemberFunctionMayBeStatic
        void update_time_offset(ctsTask&, const ctsSignedLongLong&) const noexcept
        {
            // no-op
        }

    };

    ///
    /// ctsIOPatternRateLimitThrottle
    ///
    template<>
    struct ctsIOPatternRateLimitPolicy < ctsIOPatternRateLimitThrottle >
    {

    private:
        const ctsUnsignedLongLong BytesSendingPerQuantum;
        const ctsUnsignedLongLong QuantumPeriodMs;
        ctsUnsignedLongLong bytes_sent_this_quantum;
        ctsUnsignedLongLong quantum_start_time_ms;

    public:
        ctsIOPatternRateLimitPolicy() noexcept
            : BytesSendingPerQuantum(ctsConfig::GetTcpBytesPerSecond()* ctsConfig::g_configSettings->TcpBytesPerSecondPeriod / 1000LL),
            QuantumPeriodMs(ctsConfig::g_configSettings->TcpBytesPerSecondPeriod),
            bytes_sent_this_quantum(0ULL),
            quantum_start_time_ms(ctl::ctTimer::SnapQpcInMillis())
        {
#ifdef CTSTRAFFIC_UNIT_TESTS
            PRINT_DEBUG_INFO(
                L"\t\tctsIOPatternRateLimitPolicy: BytesSendingPerQuantum - %llu, QuantumPeriodMs - %llu\n",
                static_cast<unsigned long long>(this->BytesSendingPerQuantum),
                static_cast<unsigned long long>(this->QuantumPeriodMs));
#endif
        }

        void update_time_offset(ctsTask& _task, const ctsUnsignedLongLong& _buffer_size) noexcept
        {
            if (_task.m_ioAction != ctsTaskAction::Send)
            {
                return;
            }

            _task.m_timeOffsetMilliseconds = 0LL;
            const auto current_time_ms(ctl::ctTimer::SnapQpcInMillis());

            if (this->bytes_sent_this_quantum < this->BytesSendingPerQuantum)
            {
                if (current_time_ms < this->quantum_start_time_ms + this->QuantumPeriodMs)
                {
                    if (current_time_ms > this->quantum_start_time_ms)
                    {
                        // time is in the current quantum
                        this->bytes_sent_this_quantum += _buffer_size;
                    }
                    else
                    {
                        // time is still in a prior quantum
                        _task.m_timeOffsetMilliseconds = this->newQuantumStartTime() - current_time_ms;
                        this->bytes_sent_this_quantum += _buffer_size;
                    }
                }
                else
                {
                    // time is already in a new quantum - start over
                    this->bytes_sent_this_quantum = _buffer_size;
                    this->quantum_start_time_ms += (current_time_ms - this->quantum_start_time_ms);
                }
            }
            else
            {
                // have already fulfilled the prior quantum
                const auto new_quantum_start_time_ms = this->newQuantumStartTime();

                if (current_time_ms < new_quantum_start_time_ms)
                {
                    _task.m_timeOffsetMilliseconds = new_quantum_start_time_ms - current_time_ms;
                    this->bytes_sent_this_quantum = _buffer_size;
                    this->quantum_start_time_ms = new_quantum_start_time_ms;
                }
                else
                {
                    this->bytes_sent_this_quantum = _buffer_size;
                    this->quantum_start_time_ms += (current_time_ms - this->quantum_start_time_ms);
                }
            }
#ifdef CTSTRAFFIC_UNIT_TESTS
            PRINT_DEBUG_INFO(
                L"\t\tctsIOPatternRateLimitPolicy\n"
                L"\tcurrent_time_ms: %lld\n"
                L"\tquantum_start_time_ms: %llu\n"
                L"\tbytes_sent_this_quantum: %llu\n",
                current_time_ms,
                static_cast<long long>(this->quantum_start_time_ms),
                static_cast<long long>(this->bytes_sent_this_quantum));
#endif
        }

    private:
        [[nodiscard]] long long newQuantumStartTime() const
        {
            return this->quantum_start_time_ms + this->bytes_sent_this_quantum / this->BytesSendingPerQuantum * this->QuantumPeriodMs;
        }
    };
}

