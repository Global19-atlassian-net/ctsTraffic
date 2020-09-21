/*

Copyright (c) Microsoft Corporation
All rights reserved.

Licensed under the Apache License, Version 2.0 (the ""License""); you may not use this file except in compliance with the License. You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

THIS CODE IS PROVIDED ON AN  *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.

See the Apache Version 2.0 License for specific language governing permissions and limitations under the License.

*/

#pragma once

// os headers
#include <Windows.h>
// project headers
#include "ctsSafeInt.hpp"
#include "ctsIOTask.hpp"
#include "ctsConfig.h"

namespace ctsTraffic
{

    enum class ctsIOPatternProtocolTask
    {
        NoIo,
        SendConnectionId,
        RecvConnectionId,
        MoreIo,
        SendCompletion,
        RecvCompletion,
        GracefulShutdown,
        HardShutdown,
        RequestFIN
    };
    enum class ctsIOPatternProtocolError
    {
        NoError,
        TooManyBytes,
        TooFewBytes,
        CorruptedBytes,
        ErrorIOFailed,
        SuccessfullyCompleted
    };

    class ctsIOPatternState
    {
    private:
        enum class InternalPatternState
        {
            Initialized,
            MoreIo,
            ServerSendConnectionId,
            ClientRecvConnectionId,
            ServerSendCompletion,
            ClientRecvCompletion,
            GracefulShutdown,  // TCP: instruct the function to call shutdown(SD_SEND) on the socket
            HardShutdown,      // TCP: force a RST instead of a 4-way-FIN
            RequestFIN,        // TCP: next ask for IO will be a recv for the zero-byte FIN
            CompletedTransfer,
            ErrorIOFailed
        };

        // tracking current bytes 
        ctsUnsignedLongLong confirmed_bytes = 0ULL;
        // need to know when to stop
        ctsUnsignedLongLong max_transfer = ctsConfig::GetTransferSize();
        // need to know in-flight bytes
        ctsUnsignedLongLong inflight_bytes = 0UL;
        // ideal send backlog value
        ctsUnsignedLong isb = (ctsConfig::Settings->PrePostSends == 0) ?
            ctsConfig::GetMaxBufferSize() :
            ctsConfig::GetMaxBufferSize() * ctsConfig::Settings->PrePostSends;

        InternalPatternState internal_state = InternalPatternState::Initialized;
        // track if waiting for the prior state to complete
        bool pended_state = false;

    public:
        ctsIOPatternState() noexcept;

        [[nodiscard]] ctsUnsignedLongLong get_remaining_transfer() const noexcept;
        [[nodiscard]] ctsUnsignedLongLong get_max_transfer() const noexcept;
        void set_max_transfer(const ctsUnsignedLongLong& _new_max_transfer) noexcept;
        [[nodiscard]] ctsUnsignedLong get_ideal_send_backlog() const noexcept;
        void set_ideal_send_backlog(const ctsUnsignedLong& _new_isb) noexcept;

        [[nodiscard]] bool is_completed() const noexcept;

        [[nodiscard]] bool is_current_task_more_io() const noexcept;
        ctsIOPatternProtocolTask get_next_task() noexcept;
        void notify_next_task(const ctsIOTask& _next_task) noexcept;
        ctsIOPatternProtocolError completed_task(const ctsIOTask& _completed_task, unsigned long _completed_transfer_bytes) noexcept;

        ctsIOPatternProtocolError update_error(DWORD _error_code) noexcept;
    };


    inline ctsIOPatternState::ctsIOPatternState() noexcept
    {
        if (ctsConfig::ProtocolType::UDP == ctsConfig::Settings->Protocol)
        {
            internal_state = InternalPatternState::MoreIo;
        }
    }

    inline ctsUnsignedLongLong ctsIOPatternState::get_remaining_transfer() const noexcept
    {
        //
        // Guard our internal tracking - all protocol logic assumes these rules
        //
        const auto already_transferred = this->confirmed_bytes + this->inflight_bytes;
        FAIL_FAST_IF_MSG(
            (already_transferred < this->confirmed_bytes) || (already_transferred < this->inflight_bytes),
            "ctsIOPatternState internal overflow (already_transferred = this->current_transfer + this->inflight_bytes)\n"
            "already_transferred: %llu\n"
            "this->current_transfer: %llu\n"
            "this->inflight_bytes: %llu\n",
            static_cast<unsigned long long>(already_transferred),
            static_cast<unsigned long long>(this->confirmed_bytes),
            static_cast<unsigned long long>(this->inflight_bytes));

        FAIL_FAST_IF_MSG(
            already_transferred > this->max_transfer,
            "ctsIOPatternState internal error: bytes already transferred (%llu) is >= the total we're expected to transfer (%llu)\n",
            static_cast<unsigned long long>(already_transferred), static_cast<unsigned long long>(this->max_transfer));

        return this->max_transfer - already_transferred;
    }
    inline ctsUnsignedLongLong ctsIOPatternState::get_max_transfer() const noexcept
    {
        return this->max_transfer;
    }
    inline void ctsIOPatternState::set_max_transfer(const ctsUnsignedLongLong& _new_max_transfer) noexcept
    {
        this->max_transfer = _new_max_transfer;
    }
    inline ctsUnsignedLong ctsIOPatternState::get_ideal_send_backlog() const noexcept
    {
        return this->isb;
    }
    inline void ctsIOPatternState::set_ideal_send_backlog(const ctsUnsignedLong& _new_isb) noexcept
    {
        this->isb = _new_isb;
    }

    inline bool ctsIOPatternState::is_completed() const noexcept
    {
        return (InternalPatternState::CompletedTransfer == this->internal_state || InternalPatternState::ErrorIOFailed == this->internal_state);
    }

    inline bool ctsIOPatternState::is_current_task_more_io() const noexcept
    {
        return (this->internal_state == InternalPatternState::MoreIo);
    }

    inline ctsIOPatternProtocolTask ctsIOPatternState::get_next_task() noexcept
    {
        if (this->pended_state)
        {
            // already indicated the next state: waiting for it to complete
            return ctsIOPatternProtocolTask::NoIo;
        }

        switch (this->internal_state)
        {
            case InternalPatternState::Initialized:
                if (ctsConfig::IsListening())
                {
                    PRINT_DEBUG_INFO(L"\t\tctsIOPatternState::get_next_task : ServerSendConnectionId\n")
                    this->pended_state = true;
                    this->internal_state = InternalPatternState::ServerSendConnectionId;
                    return ctsIOPatternProtocolTask::SendConnectionId;
                }
                else
                {
                    PRINT_DEBUG_INFO(L"\t\tctsIOPatternState::get_next_task : RecvConnectionId\n")
                    this->pended_state = true;
                    this->internal_state = InternalPatternState::ClientRecvConnectionId;
                    return ctsIOPatternProtocolTask::RecvConnectionId;
                }

            case InternalPatternState::ServerSendConnectionId: // both client and server start IO after the connection ID is shared
            case InternalPatternState::ClientRecvConnectionId:
                PRINT_DEBUG_INFO(L"\t\tctsIOPatternState::get_next_task : MoreIo\n")
                this->internal_state = InternalPatternState::MoreIo;
                return ctsIOPatternProtocolTask::MoreIo;

            case InternalPatternState::MoreIo:
                if ((this->confirmed_bytes + this->inflight_bytes) < this->max_transfer)
                {
                    return ctsIOPatternProtocolTask::MoreIo;
                }
                else
                {
                    return ctsIOPatternProtocolTask::NoIo;
                }

            case InternalPatternState::ServerSendCompletion:
                this->pended_state = true;
                return ctsIOPatternProtocolTask::SendCompletion;

            case InternalPatternState::ClientRecvCompletion:
                this->pended_state = true;
                return ctsIOPatternProtocolTask::RecvCompletion;

            case InternalPatternState::GracefulShutdown:
                this->pended_state = true;
                return ctsIOPatternProtocolTask::GracefulShutdown;

            case InternalPatternState::HardShutdown:
                this->pended_state = true;
                return ctsIOPatternProtocolTask::HardShutdown;

            case InternalPatternState::RequestFIN:
                this->pended_state = true;
                return ctsIOPatternProtocolTask::RequestFIN;

            case InternalPatternState::CompletedTransfer: // fall-through
            case InternalPatternState::ErrorIOFailed:
                return ctsIOPatternProtocolTask::NoIo;

            default:
                FAIL_FAST_MSG(
                    "ctsIOPatternState::get_next_task was called in an invalid state (%u): dt %p ctsTraffic!ctsTraffic::ctsIOPatternState",
                    static_cast<unsigned long>(this->internal_state), this);
        }
    }


    inline void ctsIOPatternState::notify_next_task(const ctsIOTask& _next_task) noexcept
    {
        if (_next_task.track_io)
        {
            this->inflight_bytes += _next_task.buffer_length;
        }
    }

    inline ctsIOPatternProtocolError ctsIOPatternState::update_error(DWORD _error_code) noexcept
    {
        // if we have already failed, return early
        if (InternalPatternState::ErrorIOFailed == this->internal_state)
        {
            return ctsIOPatternProtocolError::ErrorIOFailed;
        }

        if (ctsConfig::ProtocolType::UDP == ctsConfig::Settings->Protocol)
        {
            if (_error_code != 0)
            {
                PRINT_DEBUG_INFO(L"\t\tctsIOPatternState::update_error : ErrorIOFailed\n")
                this->internal_state = InternalPatternState::ErrorIOFailed;
                return ctsIOPatternProtocolError::ErrorIOFailed;
            }
        }
        else
        {
            // ctsConfig::ProtocolType::TCP
            if (_error_code != 0 && !this->is_completed())
            {
                if (ctsConfig::IsListening() &&
                    InternalPatternState::RequestFIN == this->internal_state &&
                    (WSAETIMEDOUT == _error_code || WSAECONNRESET == _error_code || WSAECONNABORTED == _error_code))
                {
                    // this is actually OK - the client may have just RST instead of a graceful FIN after receiving our status
                    return ctsIOPatternProtocolError::NoError;
                }
                else
                {
                    PRINT_DEBUG_INFO(L"\t\tctsIOPatternState::update_error : ErrorIOFailed\n")
                    this->internal_state = InternalPatternState::ErrorIOFailed;
                    return ctsIOPatternProtocolError::ErrorIOFailed;
                }
            }
        }

        return ctsIOPatternProtocolError::NoError;
    }

    inline ctsIOPatternProtocolError ctsIOPatternState::completed_task(const ctsIOTask& _completed_task, unsigned long _completed_transfer_bytes) noexcept
    {
        //
        // If already failed, don't continue processing
        //
        if (InternalPatternState::ErrorIOFailed == this->internal_state)
        {
            return ctsIOPatternProtocolError::ErrorIOFailed;
        }

        //
        // if completed our connection id request, immediately return
        // (not validating IO below)
        //
        if (InternalPatternState::ServerSendConnectionId == this->internal_state || InternalPatternState::ClientRecvConnectionId == this->internal_state)
        {
            // must have received the full id
            if (_completed_transfer_bytes != ctsStatistics::ConnectionIdLength)
            {
                PRINT_DEBUG_INFO(
                    L"\t\tctsIOPatternState::completed_task : ErrorIOFailed (TooFewBytes) [transfered %llu, Expected ConnectionID (%u)]\n",
                    static_cast<unsigned long long>(_completed_transfer_bytes),
                    ctsStatistics::ConnectionIdLength)

                this->internal_state = InternalPatternState::ErrorIOFailed;
                return ctsIOPatternProtocolError::TooFewBytes;
            }

            this->pended_state = false;
        }

        if (_completed_task.track_io)
        {
            // 
            // Checking for an inconsistent internal state 
            //
            FAIL_FAST_IF_MSG(
                _completed_transfer_bytes > this->inflight_bytes,
                "ctsIOPatternState::completed_task : ctsIOTask (%p) returned more bytes (%u) than were in flight (%llu)",
                &_completed_task, _completed_transfer_bytes, static_cast<unsigned long long>(this->inflight_bytes));
            FAIL_FAST_IF_MSG(
                _completed_task.buffer_length > this->inflight_bytes,
                "ctsIOPatternState::completed_task : the ctsIOTask (%p) had requested more bytes (%u) than were in-flight (%llu)\n",
                &_completed_task, _completed_task.buffer_length, static_cast<unsigned long long>(this->inflight_bytes));
            FAIL_FAST_IF_MSG(
                _completed_transfer_bytes > _completed_task.buffer_length,
                "ctsIOPatternState::completed_task : ctsIOTask (%p) returned more bytes (%u) than were posted (%u)\n",
                &_completed_task, _completed_transfer_bytes, _completed_task.buffer_length);

            //
            // now update our internal tracking of bytes in-flight / completed
            //
            this->inflight_bytes -= _completed_task.buffer_length;
            this->confirmed_bytes += _completed_transfer_bytes;
        }

        //
        // Verify IO Post-condition protocol contracts haven't been violated
        //
        const auto already_transferred = this->confirmed_bytes + this->inflight_bytes;

        //
        // Udp just tracks bytes
        //
        if (ctsConfig::ProtocolType::UDP == ctsConfig::Settings->Protocol)
        {
            if (already_transferred == this->max_transfer)
            {
                return ctsIOPatternProtocolError::SuccessfullyCompleted;
            }
            return ctsIOPatternProtocolError::NoError;
        }

        //
        // Tcp has a full state machine
        //
        if (already_transferred < this->max_transfer)
        {
            // guard against the client gracefully exiting before the completion of the transfer
            if (0 == _completed_transfer_bytes)
            {
                PRINT_DEBUG_INFO(
                    L"\t\tctsIOPatternState::completed_task : ErrorIOFailed (TooFewBytes) [transferred %llu, expected transfer %llu]\n",
                    static_cast<unsigned long long>(already_transferred),
                    static_cast<unsigned long long>(this->max_transfer))
                this->internal_state = InternalPatternState::ErrorIOFailed;
                return ctsIOPatternProtocolError::TooFewBytes;
            }
        }

        else if (already_transferred == this->max_transfer)
        {
            // With TCP, if inflight_bytes > 0, we are not yet done
            // - we need to wait for that pended IO to complete
            if (0 == this->inflight_bytes)
            {
                //
                // All TCP data has been sent/received
                //
                if (ctsConfig::IsListening())
                {
                    // servers will first send their final status before starting their shutdown sequence
                    switch (this->internal_state)
                    {
                        case InternalPatternState::MoreIo:
                            PRINT_DEBUG_INFO(L"\t\tctsIOPatternState::completed_task (MoreIo) : ServerSendCompletion\n")
                            this->internal_state = InternalPatternState::ServerSendCompletion;
                            this->pended_state = false;
                            break;

                        case InternalPatternState::ServerSendCompletion:
                            PRINT_DEBUG_INFO(L"\t\tctsIOPatternState::completed_task (ServerSendCompletion) : RequestFIN\n")
                            this->internal_state = InternalPatternState::RequestFIN;
                            this->pended_state = false;
                            break;

                        case InternalPatternState::RequestFIN:
                            if (_completed_transfer_bytes != 0)
                            {
                                PRINT_DEBUG_INFO(L"\t\tctsIOPatternState::completed_task (RequestFIN) : ErrorIOFailed (TooManyBytes)\n")
                                this->internal_state = InternalPatternState::ErrorIOFailed;
                                return ctsIOPatternProtocolError::TooManyBytes;
                            }
                            else
                            {
                                PRINT_DEBUG_INFO(L"\t\tctsIOPatternState::completed_task (RequestFIN) : CompletedTransfer\n")
                                this->internal_state = InternalPatternState::CompletedTransfer;
                                return ctsIOPatternProtocolError::SuccessfullyCompleted;
                            }

                        default:
                            FAIL_FAST_MSG(
                                "ctsIOPatternState::completed_task - invalid internal_status (%u): dt %p ctsTraffic!ctsTraffic::ctsIOPatternState",
                                static_cast<unsigned long>(this->internal_state), this);
                    }
                }
                else
                {
                    // clients will recv the server status, then process their shutdown sequence
                    switch (this->internal_state)
                    {
                        case InternalPatternState::MoreIo:
                            PRINT_DEBUG_INFO(L"\t\tctsIOPatternState::completed_task (MoreIo) : ClientRecvCompletion\n")
                            this->internal_state = InternalPatternState::ClientRecvCompletion;
                            this->pended_state = false;
                            break;

                        case InternalPatternState::ClientRecvCompletion:
                            // process the server's returned status
                            if (_completed_transfer_bytes != 4)
                            {
                                PRINT_DEBUG_INFO(
                                    L"\t\tctsIOPatternState::completed_task (ClientRecvCompletion) : ErrorIOFailed (Server didn't return a completion - returned %u bytes)\n",
                                    _completed_transfer_bytes)
                                this->internal_state = InternalPatternState::ErrorIOFailed;
                                return ctsIOPatternProtocolError::TooFewBytes;
                            }

                            if (ctsConfig::TcpShutdownType::GracefulShutdown == ctsConfig::Settings->TcpShutdown)
                            {
                                PRINT_DEBUG_INFO(L"\t\tctsIOPatternState::completed_task (ClientRecvCompletion) : GracefulShutdown\n")
                                this->internal_state = InternalPatternState::GracefulShutdown;
                                this->pended_state = false;
                            }
                            else
                            {
                                PRINT_DEBUG_INFO(L"\t\tctsIOPatternState::completed_task (ClientRecvCompletion) : HardShutdown\n")
                                this->internal_state = InternalPatternState::HardShutdown;
                                this->pended_state = false;
                            }
                            break;

                        case InternalPatternState::GracefulShutdown:
                            PRINT_DEBUG_INFO(L"\t\tctsIOPatternState::completed_task (GracefulShutdown) : RequestFIN\n")
                            this->internal_state = InternalPatternState::RequestFIN;
                            this->pended_state = false;
                            break;

                        case InternalPatternState::RequestFIN:
                            if (_completed_transfer_bytes != 0)
                            {
                                PRINT_DEBUG_INFO(L"\t\tctsIOPatternState::completed_task (RequestFIN) : ErrorIOFailed (TooManyBytes)\n")
                                this->internal_state = InternalPatternState::ErrorIOFailed;
                                return ctsIOPatternProtocolError::TooManyBytes;
                            }

                            PRINT_DEBUG_INFO(L"\t\tctsIOPatternState::completed_task (RequestFIN) : CompletedTransfer\n")
                            this->internal_state = InternalPatternState::CompletedTransfer;
                            return ctsIOPatternProtocolError::SuccessfullyCompleted;

                        case InternalPatternState::HardShutdown:
                            PRINT_DEBUG_INFO(L"\t\tctsIOPatternState::completed_task (HardShutdown) : CompletedTransfer\n")
                            this->internal_state = InternalPatternState::CompletedTransfer;
                            return ctsIOPatternProtocolError::SuccessfullyCompleted;

                        default:
                            FAIL_FAST_MSG(
                                "ctsIOPatternState::completed_task - invalid internal_status (%u): dt %p ctsTraffic!ctsTraffic::ctsIOPatternState, dt %p ctsTraffic!ctstraffic::ctsIOTask",
                                static_cast<unsigned long>(this->internal_state), this, &_completed_task);
                    }
                }

            }
        }

        else if (already_transferred > this->max_transfer)
        {
            PRINT_DEBUG_INFO(
                L"\t\tctsIOPatternState::completed_task : ErrorIOFailed (TooManyBytes) [transferred %llu, expected transfer %llu]\n",
                static_cast<unsigned long long>(already_transferred),
                static_cast<unsigned long long>(this->max_transfer))
            this->internal_state = InternalPatternState::ErrorIOFailed;
            return ctsIOPatternProtocolError::TooManyBytes;
        }

        return ctsIOPatternProtocolError::NoError;
    }
}
