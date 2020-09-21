/*

Copyright (c) Microsoft Corporation
All rights reserved.

Licensed under the Apache License, Version 2.0 (the ""License""); you may not use this file except in compliance with the License. You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

THIS CODE IS PROVIDED ON AN  *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.

See the Apache Version 2.0 License for specific language governing permissions and limitations under the License.

*/

// cpp headers
#include <memory>
// os headers
#include <Windows.h>
#include <WinSock2.h>
// ctl headers
#include <ctThreadIocp.hpp>
#include <ctSockaddr.hpp>
// local headers
#include "ctsConfig.h"
#include "ctsSocket.h"
#include "ctsIOTask.hpp"

namespace ctsTraffic
{

    /// forward delcaration
    void ctsSendRecvIocp(const std::weak_ptr<ctsSocket>& _weak_socket) noexcept;

    struct ctsSendRecvStatus
    {
        // Winsock error code
        unsigned long ioErrorcode = NO_ERROR;
        // flag if to request another ctsIOTask
        bool ioDone = false;
        // returns if IO was started (since can return !io_done, but I/O wasn't started yet)
        bool ioStarted = false;
    };

    ///
    /// IO Threadpool completion callback 
    ///
    static void ctsIoCompletionCallback(
        _In_ OVERLAPPED* _overlapped,
        const std::weak_ptr<ctsSocket>& _weak_socket,
        const ctsIOTask& _io_task) noexcept
    {
        auto shared_socket(_weak_socket.lock());
        if (!shared_socket)
        {
            return;
        }

        int gle = NO_ERROR;
        // hold a reference on the iopattern
        auto shared_pattern = shared_socket->io_pattern();
        if (!shared_pattern)
        {
            gle = WSAECONNABORTED;
        }

        DWORD transferred = 0;
        if (NO_ERROR == gle)
        {
            // try to get the success/error code and bytes transferred (under the socket lock)
            const auto socket_ref(shared_socket->socket_reference());
            const SOCKET socket = socket_ref.socket();
            // if we no longer have a valid socket or the pattern was destroyed, return early
            if (INVALID_SOCKET == socket)
            {
                gle = WSAECONNABORTED;
            }
            else
            {
                DWORD flags;
                if (!WSAGetOverlappedResult(socket, _overlapped, &transferred, FALSE, &flags))
                {
                    gle = WSAGetLastError();
                }
            }
        }

        // write to PrintError if the IO failed
        const char* function_name = IOTaskAction::Send == _io_task.ioAction ? "WSASend" : "WSARecv";
        if (gle != 0) { PRINT_DEBUG_INFO(L"\t\tIO Failed: %hs (%d) [ctsSendRecvIocp]\n", function_name, gle) }

        if (shared_pattern)
        {
            // see if complete_io requests more IO
            const ctsIOStatus protocol_status = shared_pattern->complete_io(_io_task, transferred, gle);
            switch (protocol_status)
            {
                case ctsIOStatus::ContinueIo:
                    // more IO is requested from the protocol : invoke the new IO call while holding a refcount to the prior IO
                    ctsSendRecvIocp(_weak_socket);
                    break;

                case ctsIOStatus::CompletedIo:
                    // no more IO is requested from the protocol : indicate success
                    gle = NO_ERROR;
                    break;

                case ctsIOStatus::FailedIo:
                    // write out the error to the error log since the protocol sees this as a hard error
                    ctsConfig::PrintErrorIfFailed(function_name, gle);
                    // protocol sees this as a failure : capture the error the protocol recorded
                    gle = static_cast<int>(shared_pattern->get_last_error());
                    break;

                default:
                    FAIL_FAST_MSG("ctsSendRecvIocp : unknown ctsSocket::IOStatus (%u)", static_cast<unsigned>(protocol_status));
            }
        }

        // always decrement *after* attempting new IO : the prior IO is now formally "done"
        if (shared_socket->decrement_io() == 0)
        {
            // if we have no more IO pended, complete the state
            shared_socket->complete_state(gle);
        }
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    ///
    /// Attempts the IO specified in the ctsIOTask on the ctsSocket
    ///
    /// ** ctsSocket::increment_io must have been called before this function was invoked
    ///
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    static ctsSendRecvStatus ctsProcessIOTask(SOCKET _socket, const std::shared_ptr<ctsSocket>& _shared_socket, const std::shared_ptr<ctsIOPattern>& _shared_pattern, const ctsIOTask& next_io) noexcept
    {
        ctsSendRecvStatus return_status;

        // if we no longer have a valid socket return early
        if (INVALID_SOCKET == _socket)
        {
            return_status.ioErrorcode = WSAECONNABORTED;
            return_status.ioStarted = false;
            return_status.ioDone = true;
            // even if the socket was closed we still must complete the IO request
            _shared_pattern->complete_io(next_io, 0, return_status.ioErrorcode);
            return return_status;
        }

        if (IOTaskAction::GracefulShutdown == next_io.ioAction)
        {
            if (0 != shutdown(_socket, SD_SEND))
            {
                return_status.ioErrorcode = WSAGetLastError();
            }
            return_status.ioDone = _shared_pattern->complete_io(next_io, 0, return_status.ioErrorcode) != ctsIOStatus::ContinueIo;
            return_status.ioStarted = false;

        }
        else if (IOTaskAction::HardShutdown == next_io.ioAction)
        {
            // pass through -1 to force an RST with the closesocket
            return_status.ioErrorcode = _shared_socket->close_socket(-1);
            return_status.ioDone = _shared_pattern->complete_io(next_io, 0, return_status.ioErrorcode) != ctsIOStatus::ContinueIo;
            return_status.ioStarted = false;

        }
        else
        {
            try
            {
                // attempt to allocate an IO thread-pool object
                const std::shared_ptr<ctl::ctThreadIocp>& io_thread_pool(_shared_socket->thread_pool());
                OVERLAPPED* pov = io_thread_pool->new_request(
                    [weak_reference = std::weak_ptr<ctsSocket>(_shared_socket), next_io](OVERLAPPED* _ov) noexcept
                {
                    ctsIoCompletionCallback(_ov, weak_reference, next_io);
                });

                WSABUF wsabuf;
                wsabuf.buf = next_io.buffer + next_io.buffer_offset;
                wsabuf.len = next_io.buffer_length;

                PCSTR function_name{};
                if (IOTaskAction::Send == next_io.ioAction)
                {
                    function_name = "WSASend";
                    if (WSASend(_socket, &wsabuf, 1, nullptr, 0, pov, nullptr) != 0)
                    {
                        return_status.ioErrorcode = WSAGetLastError();
                    }
                }
                else
                {
                    function_name = "WSARecv";
                    DWORD flags = ctsConfig::Settings->Options & ctsConfig::OptionType::MSG_WAIT_ALL ? MSG_WAITALL : 0;
                    if (WSARecv(_socket, &wsabuf, 1, nullptr, &flags, pov, nullptr) != 0)
                    {
                        return_status.ioErrorcode = WSAGetLastError();
                    }
                }
                //
                // not calling complete_io if returned IO pended 
                // not calling complete_io if returned success but not handling inline completions
                //
                if (WSA_IO_PENDING == return_status.ioErrorcode ||
                    (NO_ERROR == return_status.ioErrorcode && !(ctsConfig::Settings->Options & ctsConfig::OptionType::HANDLE_INLINE_IOCP)))
                {
                    return_status.ioErrorcode = NO_ERROR;
                    return_status.ioStarted = true;
                    return_status.ioDone = false;

                }
                else
                {
                    // process the completion if the API call failed, or if it succeeded and we're handling the completion inline, 
                    return_status.ioStarted = false;
                    // determine # of bytes transferred, if any
                    DWORD bytes_transferred = 0;
                    if (NO_ERROR == return_status.ioErrorcode)
                    {
                        DWORD flags;
                        if (!WSAGetOverlappedResult(_socket, pov, &bytes_transferred, FALSE, &flags))
                        {
                            FAIL_FAST_MSG(
                                "WSAGetOverlappedResult failed (%d) after the IO request (%hs) succeeded", WSAGetLastError(), function_name);
                        }
                    }
                    // must cancel the IOCP TP since IO is not pended
                    io_thread_pool->cancel_request(pov);
                    // call back to the socket to see if wants more IO
                    const ctsIOStatus protocol_status = _shared_pattern->complete_io(next_io, bytes_transferred, return_status.ioErrorcode);
                    switch (protocol_status)
                    {
                        case ctsIOStatus::ContinueIo:
                            // The protocol layer wants to transfer more data
                            // if prior IO failed, the protocol wants to ignore the error
                            return_status.ioErrorcode = NO_ERROR;
                            return_status.ioDone = false;
                            break;

                        case ctsIOStatus::CompletedIo:
                            // The protocol layer has successfully complete all IO on this connection
                            // if prior IO failed, the protocol wants to ignore the error
                            return_status.ioErrorcode = NO_ERROR;
                            return_status.ioDone = true;
                            break;

                        case ctsIOStatus::FailedIo:
                            // write out the error
                            ctsConfig::PrintErrorIfFailed(function_name, _shared_pattern->get_last_error());
                            // the protocol acknoledged the failure - socket is done with IO
                            return_status.ioErrorcode = _shared_pattern->get_last_error();
                            return_status.ioDone = true;
                            break;

                        default:
                            FAIL_FAST_MSG("ctsSendRecvIocp: unknown ctsSocket::IOStatus - %u\n", static_cast<unsigned>(protocol_status));
                    }
                }
            }
            catch (...)
            {
                return_status.ioErrorcode = ctsConfig::PrintThrownException();
                return_status.ioDone = _shared_pattern->complete_io(next_io, 0, return_status.ioErrorcode) != ctsIOStatus::ContinueIo;
                return_status.ioStarted = false;
            }
        }

        return return_status;
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    ///
    /// This is the callback for the threadpool timer.
    /// Processes the given task and then calls ctsSendRecvIocp function to deal with any additional tasks
    ///
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    static void ctsProcessIOTaskCallback(const std::weak_ptr<ctsSocket>& _weak_socket, const ctsIOTask& next_io) noexcept
    {
        // attempt to get a reference to the socket
        auto shared_socket(_weak_socket.lock());
        if (!shared_socket)
        {
            return;
        }
        // take a lock on the socket before working with it
        const auto socket_ref(shared_socket->socket_reference());
        // increment IO for this IO request
        shared_socket->increment_io();

        // run the ctsIOTask (next_io) that was scheduled through the TP timer
        const ctsSendRecvStatus status = ctsProcessIOTask(socket_ref.socket(), shared_socket, shared_socket->io_pattern(), next_io);
        // if no IO was started, decrement the IO counter
        if (!status.ioStarted)
        {
            if (0 == shared_socket->decrement_io())
            {
                // this should never be zero since we should be holding a refcount for this callback
                FAIL_FAST_MSG(
                    "The refcount of the ctsSocket object (%p) fell to zero during a scheduled callback", shared_socket.get());
            }
        }
        // continue requesting IO if this connection still isn't done with all IO after scheduling the prior IO
        if (!status.ioDone)
        {
            ctsSendRecvIocp(_weak_socket);
        }
        // finally decrement the IO that was counted for this IO that was completed async
        if (shared_socket->decrement_io() == 0)
        {
            // if we have no more IO pended, complete the state
            shared_socket->complete_state(status.ioErrorcode);
        }
    }

    // The function registered with ctsConfig
    void ctsSendRecvIocp(const std::weak_ptr<ctsSocket>& _weak_socket) noexcept
    {
        // attempt to get a reference to the socket
        auto shared_socket(_weak_socket.lock());
        if (!shared_socket)
        {
            return;
        }
        // take a lock on the socket before working with it
        const auto socket_ref(shared_socket->socket_reference());
        // hold a reference on the iopattern
        auto shared_pattern(shared_socket->io_pattern());
        //
        // loop until failure or initiate_io returns None
        //
        // IO is always done in the ctsProcessIOTask function,
        // - either synchronously or scheduled through a timer object
        //
        // The IO refcount must be incremented here to hold an IO count on the socket
        // - so that we won't inadvertently call complete_state() while IO is still being scheduled
        //
        shared_socket->increment_io();

        ctsSendRecvStatus status;
        while (!status.ioDone)
        {
            const ctsIOTask next_io = shared_pattern->initiate_io();
            if (IOTaskAction::None == next_io.ioAction)
            {
                // nothing failed, just no more IO right now
                break;
            }

            // increment IO for each individual request
            shared_socket->increment_io();

            if (next_io.time_offset_milliseconds > 0)
            {
                // set_timer can throw
                try
                {
                    shared_socket->set_timer(next_io, ctsProcessIOTaskCallback);
                    status.ioStarted = true; // IO started in the context of keeping the count incremented
                    status.ioDone = true;
                }
                catch (...)
                {
                    status.ioErrorcode = ctsConfig::PrintThrownException();
                    status.ioStarted = false;
                }

            }
            else
            {
                status = ctsProcessIOTask(socket_ref.socket(), shared_socket, shared_pattern, next_io);
            }

            // if no IO was started, decrement the IO counter
            if (!status.ioStarted)
            {
                // since IO is not pended, remove the refcount
                if (0 == shared_socket->decrement_io())
                {
                    // this should never be zero as we are holding a reference outside the loop
                    FAIL_FAST_MSG(
                        "The ctsSocket (%p) refcount fell to zero while this function was holding a reference", shared_socket.get());
                }
            }
        }
        // decrement IO at the end to release the refcount held before the loop
        if (0 == shared_socket->decrement_io())
        {
            shared_socket->complete_state(status.ioErrorcode);
        }
    }

} // namespace
