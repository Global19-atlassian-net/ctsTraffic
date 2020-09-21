/*

Copyright (c) Microsoft Corporation
All rights reserved.

Licensed under the Apache License, Version 2.0 (the ""License""); you may not use this file except in compliance with the License. You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

THIS CODE IS PROVIDED ON AN  *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.

See the Apache Version 2.0 License for specific language governing permissions and limitations under the License.

*/

#pragma once

// cpp headers
#include <array>
#include <string>
// os headers
#include <Windows.h>
#include <WinSock2.h>
// ctl headers
#include <ctTimer.hpp>
// local headers
#include "ctsConfig.h"
#include "ctsIOTask.hpp"
#include "ctsSafeInt.hpp"
#include "ctsStatistics.hpp"
// wil headers
#include <wil/stl.h>
#include <wil/resource.h>

namespace ctsTraffic
{
    //
    // ctsMediaStreamMessage encapsulates requests sent from clients
    //
    // Grammar:
    //
    //   REQUEST_ID
    //   START
    //
    constexpr unsigned short UdpDatagramProtocolHeaderFlagData = 0x0000;
    constexpr unsigned short UdpDatagramProtocolHeaderFlagId = 0x1000;

    constexpr unsigned long UdpDatagramProtocolHeaderFlagLength = 2;
    constexpr unsigned long UdpDatagramConnectionIdHeaderLength = UdpDatagramProtocolHeaderFlagLength + ctsStatistics::ConnectionIdLength;

    constexpr unsigned long UdpDatagramSequenceNumberLength = 8; // 64-bit value
    constexpr unsigned long UdpDatagramQPCLength = 8; // 64-bit value
    constexpr unsigned long UdpDatagramQPFLength = 8; // 64-bit value
    constexpr unsigned long UdpDatagramDataHeaderLength = UdpDatagramProtocolHeaderFlagLength + UdpDatagramSequenceNumberLength + UdpDatagramQPCLength + UdpDatagramQPFLength;

    constexpr unsigned long UdpDatagramMaximumSizeBytes = 64000UL;

    static const char* UdpDatagramStartString = "START";
    constexpr unsigned long UdpDatagramStartStringLength = 5;

    enum class MediaStreamAction : char
    {
        START
    };

    class ctsMediaStreamSendRequests
    {
    public:
        ~ctsMediaStreamSendRequests() = default;
        ctsMediaStreamSendRequests() = delete;
        ctsMediaStreamSendRequests(const ctsMediaStreamSendRequests&) = delete;
        ctsMediaStreamSendRequests& operator=(const ctsMediaStreamSendRequests&) = delete;
        ctsMediaStreamSendRequests(ctsMediaStreamSendRequests&&) = delete;
        ctsMediaStreamSendRequests& operator=(ctsMediaStreamSendRequests&&) = delete;


        static constexpr unsigned long BufferArraySize = 5;
        ///
        /// compose iteration across buffers to be sent per instantiated request
        ///
        class iterator
        {
        public:
            ////////////////////////////////////////////////////////////////////////////////
            ///
            /// iterator_traits
            /// - allows <algorithm> functions to be used
            ///
            ////////////////////////////////////////////////////////////////////////////////
            typedef std::forward_iterator_tag iterator_category;
            typedef std::array<WSABUF, BufferArraySize> value_type;
            typedef size_t difference_type;
            typedef std::array<WSABUF, BufferArraySize>* pointer;
            typedef std::array<WSABUF, BufferArraySize>& reference;

            ~iterator() = default;
            iterator(const iterator&) = default;
            iterator& operator=(const iterator&) = default;
            iterator(iterator&&) = default;
            iterator& operator=(iterator&&) = default;

            ///
            /// Dereferencing operators
            /// - returning non-const array references as Winsock APIs don't take const WSABUF*
            ///
            std::array<WSABUF, BufferArraySize>* operator->() noexcept
            {
                FAIL_FAST_IF_MSG(
                    nullptr == this->qpc_address,
                    "Invalid ctsMediaStreamSendRequests::iterator being dereferenced (%p)", this);

                // refresh the QPC value at the last possible moment before returning the array to the user
                _Analysis_assume_(this->qpc_address != nullptr);
                QueryPerformanceCounter(this->qpc_address);
                return &this->wsa_buf_array;
            }

            std::array<WSABUF, BufferArraySize>& operator*() noexcept
            {
                FAIL_FAST_IF_MSG(
                    nullptr == this->qpc_address,
                    "Invalid ctsMediaStreamSendRequests::iterator being dereferenced (%p)", this);

                // refresh the QPC value at the last possible moment before returning the array to the user
                _Analysis_assume_(this->qpc_address != nullptr);
                QueryPerformanceCounter(this->qpc_address);
                return this->wsa_buf_array;
            }

            ///
            /// Equality operators
            ///
            bool operator ==(const iterator& _comparand) const noexcept
            {
                return this->qpc_address == _comparand.qpc_address &&
                    this->bytes_to_send == _comparand.bytes_to_send;
            }
            bool operator !=(const iterator& _comparand) const noexcept
            {
                return !(*this == _comparand);
            }

            /// preincrement
            iterator& operator++() noexcept
            {
                FAIL_FAST_IF_MSG(
                    nullptr == this->qpc_address,
                    "Invalid ctsMediaStreamSendRequests::iterator being dereferenced (%p)", this);

                // if bytes is zero, then increment to the end() iterator
                if (0 == this->bytes_to_send)
                {
                    this->qpc_address = nullptr;
                }
                else
                {
                    this->bytes_to_send -= this->update_buffer_length();
                }

                return *this;
            }
            // postincrement
            iterator operator++(int) noexcept
            {
                iterator temp(*this);
                ++* this;
                return temp;
            }

        private:
            // c'tor is only available to the begin() and end() methods of ctsMediaStreamSendRequests
            friend class ctsMediaStreamSendRequests;
            iterator(_In_opt_ LARGE_INTEGER* _qpc_address, long long _bytes_to_send, const std::array<WSABUF, BufferArraySize>& _wsa_buf_array) noexcept :
                qpc_address(_qpc_address),
                bytes_to_send(_bytes_to_send),
                wsa_buf_array(_wsa_buf_array)
            {
                // set the buffer length for the first iterator
                this->bytes_to_send -= this->update_buffer_length();
            }

            unsigned long update_buffer_length() noexcept
            {
                // only update when not the end() iterator
                if (this->qpc_address)
                {
                    if (this->bytes_to_send > UdpDatagramMaximumSizeBytes)
                    {
                        this->wsa_buf_array[4].len = UdpDatagramMaximumSizeBytes - UdpDatagramDataHeaderLength;
                    }
                    else
                    {
                        this->wsa_buf_array[4].len = static_cast<unsigned long>(this->bytes_to_send - UdpDatagramDataHeaderLength);
                    }

                    ctsUnsignedLong total_bytes_to_send =
                        this->wsa_buf_array[0].len + this->wsa_buf_array[1].len + this->wsa_buf_array[2].len + this->wsa_buf_array[3].len + this->wsa_buf_array[4].len;

                    // must guarantee that after we send this datagram we have enough bytes for the next send if there are bytes left over
                    const ctsSignedLongLong bytes_remaining = this->bytes_to_send - static_cast<long long>(total_bytes_to_send);

                    if (bytes_remaining > 0 && bytes_remaining <= UdpDatagramDataHeaderLength)
                    {
                        // subtract out enough bytes so the next datagram will be large enough for the header and at least one byte of data
                        ctsUnsignedLong new_length = this->wsa_buf_array[4].len;
                        const ctsUnsignedLong delta_to_remove = UdpDatagramDataHeaderLength + 1 - static_cast<unsigned long>(bytes_remaining);
                        new_length -= delta_to_remove;

                        this->wsa_buf_array[4].len = new_length;
                        total_bytes_to_send -= delta_to_remove;
                    }

                    return total_bytes_to_send;
                }

                return 0UL;
            }

            LARGE_INTEGER* qpc_address;
            ctsSignedLongLong bytes_to_send;
            std::array<WSABUF, BufferArraySize> wsa_buf_array;
        };


        ///
        /// Constructor of the ctsMediaStreamSendRequests captures the properties of the next Send() request
        /// - the total # of bytes to send (across X number of send requests)
        /// - the sequence number to tag in every send request
        ///
        ctsMediaStreamSendRequests(long long _bytes_to_send, long long _sequence_number, const char* _send_buffer) noexcept :
            qpf(ctl::ctTimer::ctSnapQpf()),
            bytes_to_send(_bytes_to_send),
            sequence_number(_sequence_number)
        {
            FAIL_FAST_IF_MSG(
                _bytes_to_send <= UdpDatagramDataHeaderLength,
                "ctsMediaStreamSendRequests requires a buffer size to send larger than the ctsTraffic UDP header");

            // buffer layout: header#, seq. number, qpc, qpf, then the buffered data
            this->wsabuf[0].buf = reinterpret_cast<char*>(const_cast<unsigned short*>(&UdpDatagramProtocolHeaderFlagData));
            this->wsabuf[0].len = UdpDatagramProtocolHeaderFlagLength;

            this->wsabuf[1].buf = reinterpret_cast<char*>(&this->sequence_number);
            this->wsabuf[1].len = UdpDatagramSequenceNumberLength;

            this->wsabuf[2].buf = reinterpret_cast<char*>(&this->qpc_value.QuadPart);
            this->wsabuf[2].len = UdpDatagramQPCLength;

            this->wsabuf[3].buf = reinterpret_cast<char*>(&this->qpf);
            this->wsabuf[3].len = UdpDatagramQPFLength;

            this->wsabuf[4].buf = const_cast<char*>(_send_buffer);
            // the this->wsabuf[4].len field is dependent on bytes_to_send and can change by iterator()
        }

        [[nodiscard]] iterator begin() noexcept
        {
            return { &this->qpc_value, this->bytes_to_send, this->wsabuf };
        }

        [[nodiscard]] iterator end() const noexcept
        {
            // end == null qpc + 0 byte length
            return { nullptr, 0, this->wsabuf };
        }


    private:
        std::array<WSABUF, BufferArraySize> wsabuf{};
        LARGE_INTEGER qpc_value{};
        long long qpf{};
        long long bytes_to_send{};
        long long sequence_number{};
    };


    struct ctsMediaStreamMessage
    {
        long long sequence_number = 0ll;
        MediaStreamAction action{};

        explicit ctsMediaStreamMessage(MediaStreamAction _action) noexcept : action(_action)
        {
        }
        ~ctsMediaStreamMessage() noexcept = default;
        ctsMediaStreamMessage(const ctsMediaStreamMessage&) = default;
        ctsMediaStreamMessage& operator=(const ctsMediaStreamMessage&) = default;
        ctsMediaStreamMessage(ctsMediaStreamMessage&&) = default;
        ctsMediaStreamMessage& operator=(ctsMediaStreamMessage&&) = default;

        static bool ValidateBufferLengthFromTask(const ctsIOTask& _task, unsigned long _completed_bytes) noexcept
            try
        {
            if (_completed_bytes < UdpDatagramProtocolHeaderFlagLength)
            {
                ctsConfig::PrintErrorInfo(
                    wil::str_printf<std::wstring>(L"ValidateBufferLengthFromTask rejecting the datagram: the datagram size (%u) is less than UdpDatagramProtocolHeaderFlagLength (%u)",
                        _completed_bytes,
                        UdpDatagramProtocolHeaderFlagLength).c_str());
                return false;
            }

            switch (GetProtocolHeaderFromTask(_task))
            {
                case UdpDatagramProtocolHeaderFlagData:
                    if (_completed_bytes < UdpDatagramDataHeaderLength)
                    {
                        ctsConfig::PrintErrorInfo(
                            wil::str_printf<std::wstring>(L"ValidateBufferLengthFromTask rejecting the datagram type UdpDatagramProtocolHeaderFlagData: the datagram size (%u) is less than UdpDatagramDataHeaderLength (%u)",
                                _completed_bytes,
                                UdpDatagramDataHeaderLength).c_str());
                        return false;
                    }
                    break;

                case UdpDatagramProtocolHeaderFlagId:
                    if (_completed_bytes < UdpDatagramConnectionIdHeaderLength)
                    {
                        ctsConfig::PrintErrorInfo(
                            wil::str_printf<std::wstring>(L"ValidateBufferLengthFromTask rejecting the datagram type UdpDatagramProtocolHeaderFlagId: the datagram size (%u) is less than UdpDatagramConnectionIdHeaderLength (%u)",
                                _completed_bytes,
                                UdpDatagramConnectionIdHeaderLength).c_str());
                        return false;
                    }
                    break;

                default:
                    ctsConfig::PrintErrorInfo(
                        wil::str_printf<std::wstring>(L"ValidateBufferLengthFromTask rejecting the datagram of unknown frame type (%u) - expecting UdpDatagramProtocolHeaderFlagData (%u) or UdpDatagramProtocolHeaderFlagId (%u)",
                            GetProtocolHeaderFromTask(_task),
                            UdpDatagramProtocolHeaderFlagData,
                            UdpDatagramProtocolHeaderFlagId).c_str());
                    return false;
            }

            return true;
        }
        catch (...)
        {
            return false;
        }

        static unsigned short GetProtocolHeaderFromTask(const ctsIOTask& _task) noexcept
        {
            return *reinterpret_cast<unsigned short*>(_task.buffer);
        }

        static void SetConnectionIdFromTask(_Inout_updates_(ctsStatistics::ConnectionIdLength) char* _connection_id, const ctsIOTask& _task) noexcept
        {
            const auto copy_error = memcpy_s(
                _connection_id,
                ctsStatistics::ConnectionIdLength,
                _task.buffer + _task.buffer_offset + UdpDatagramProtocolHeaderFlagLength,
                ctsStatistics::ConnectionIdLength);
            FAIL_FAST_IF_MSG(
                copy_error != 0,
                "ctsMediaStreamMessage::GetConnectionIdFromTask : memcpy_s failed trying to copy the connection ID - target buffer (%p) ctsIOTask (%p) (error : %d)",
                _connection_id, &_task, copy_error);
        }

        static long long GetSequenceNumberFromTask(const ctsIOTask& _task) noexcept
        {
            long long return_value;
            const auto copy_error = memcpy_s(
                &return_value,
                UdpDatagramSequenceNumberLength,
                _task.buffer + _task.buffer_offset + UdpDatagramProtocolHeaderFlagLength,
                UdpDatagramSequenceNumberLength);
            FAIL_FAST_IF_MSG(
                copy_error != 0,
                "ctsMediaStreamMessage::GetSequenceNumberFromTask : memcpy_s failed trying to copy the sequence number - ctsIOTask (%p) (error : %d)",
                &_task, copy_error);

            return return_value;
        }

        static long long GetQueryPerfCounterFromTask(const ctsIOTask& _task) noexcept
        {
            long long return_value;
            const auto copy_error = memcpy_s(&return_value, UdpDatagramSequenceNumberLength, _task.buffer + _task.buffer_offset + UdpDatagramProtocolHeaderFlagLength, UdpDatagramSequenceNumberLength);
            FAIL_FAST_IF_MSG(
                copy_error != 0,
                "ctsMediaStreamMessage::GetSequenceNumberFromTask : memcpy_s failed trying to copy the sequence number - ctsIOTask (%p) (error : %d)",
                &_task, copy_error);

            return return_value;
        }

        static long long GetQueryPerfFrequencyFromTask(const ctsIOTask& _task) noexcept
        {
            long long return_value;
            const auto copy_error = memcpy_s(&return_value, UdpDatagramSequenceNumberLength, _task.buffer + _task.buffer_offset + UdpDatagramProtocolHeaderFlagLength, UdpDatagramSequenceNumberLength);
            FAIL_FAST_IF_MSG(
                copy_error != 0,
                "ctsMediaStreamMessage::GetSequenceNumberFromTask : memcpy_s failed trying to copy the sequence number - target buffer (%p) ctsIOTask (%p) (error : %d)",
                &return_value, &_task, copy_error);

            return return_value;
        }

        static ctsIOTask MakeConnectionIdTask(const ctsIOTask& _raw_task, _In_reads_(ctsStatistics::ConnectionIdLength) char* const _connection_id) noexcept
        {
            FAIL_FAST_IF_MSG(
                _raw_task.buffer_length != ctsStatistics::ConnectionIdLength + UdpDatagramProtocolHeaderFlagLength,
                "ctsMediaStreamMessage::GetConnectionIdFromTask : the buffer_length in the provided task (%u) is not the expected buffer length (%u)",
                _raw_task.buffer_length, ctsStatistics::ConnectionIdLength + UdpDatagramProtocolHeaderFlagLength);

            ctsIOTask return_task(_raw_task);
            // populate the buffer with the connection Id and protocol field
            memcpy_s(return_task.buffer, UdpDatagramProtocolHeaderFlagLength, &UdpDatagramProtocolHeaderFlagId, UdpDatagramProtocolHeaderFlagLength);
            memcpy_s(return_task.buffer + UdpDatagramProtocolHeaderFlagLength, ctsStatistics::ConnectionIdLength, _connection_id, ctsStatistics::ConnectionIdLength);

            return_task.ioAction = IOTaskAction::Send;
            return_task.buffer_type = ctsIOTask::BufferType::UdpConnectionId;
            return_task.track_io = false;
            return return_task;
        }

        static ctsIOTask Construct(MediaStreamAction _action) noexcept
        {
            ctsIOTask return_task;
            return_task.ioAction = IOTaskAction::Send;
            return_task.buffer_type = ctsIOTask::BufferType::Static;
            return_task.track_io = false;

            // safe to const-cast as we are sending these buffers
            switch (_action)
            {
                case MediaStreamAction::START:
                    return_task.buffer = const_cast<char*>(UdpDatagramStartString);
                    return_task.buffer_length = UdpDatagramStartStringLength;
                    break;

                default: FAIL_FAST();
            }

            return return_task;
        }

        static ctsMediaStreamMessage Extract(_In_reads_bytes_(inputLength) const char* inputBuffer, unsigned inputLength)
        {
            if (inputLength == UdpDatagramStartStringLength)
            {
                if (0 == memcmp(inputBuffer, UdpDatagramStartString, UdpDatagramStartStringLength))
                {
                    return ctsMediaStreamMessage(MediaStreamAction::START);
                }
            }

            THROW_HR_MSG(HRESULT_FROM_WIN32(ERROR_INVALID_DATA),
                "Invalid MediaStream message: %hs",
                std::string(inputBuffer, inputLength).c_str());
        }
    };
}

