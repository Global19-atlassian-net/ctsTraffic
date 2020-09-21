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
    void ctsReadWriteIocp(const std::weak_ptr<ctsSocket>& _weak_socket) noexcept;

    // IO Threadpool completion callback 
    static void ctsReadWriteIocpIoCompletionCallback(
        _In_ OVERLAPPED* _overlapped,
        const std::weak_ptr<ctsSocket>& _weak_socket,
        const ctsIOTask& _io_task) noexcept
    {
        auto shared_socket(_weak_socket.lock());
        if (!shared_socket)
        {
            return;
        }
        // hold a reference on the iopattern
        auto shared_pattern(shared_socket->io_pattern());

        int gle = NO_ERROR;
        DWORD transferred = 0;
        // lock the socket just long enough to read the result
        {
            const auto socket_ref(shared_socket->socket_reference());
            const SOCKET socket = socket_ref.socket();
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

        const char* function_name = IOTaskAction::Send == _io_task.ioAction ? "WriteFile" : "ReadFile";
        if (gle != 0) PRINT_DEBUG_INFO(L"\t\tIO Failed: %hs (%d) [ctsReadWriteIocp]\n", function_name, gle)
        // see if complete_io requests more IO
        DWORD readwrite_status = NO_ERROR;
        const ctsIOStatus protocol_status = shared_pattern->complete_io(_io_task, transferred, gle);
        switch (protocol_status)
        {
            case ctsIOStatus::ContinueIo:
                // more IO is requested from the protocol
                // - invoke the new IO call while holding a refcount to the prior IO
                ctsReadWriteIocp(_weak_socket);
                break;

            case ctsIOStatus::CompletedIo:
                // protocol didn't fail this IO: no more IO is requested from the protocol
                readwrite_status = NO_ERROR;
                break;

            case ctsIOStatus::FailedIo:
                // write out the error
                ctsConfig::PrintErrorIfFailed(function_name, gle);
                // protocol sees this as a failure - capture the error the protocol recorded
                readwrite_status = shared_pattern->get_last_error();
                break;

            default:
                FAIL_FAST_MSG("ctsReadWriteIocp: unknown ctsSocket::IOStatus - %u\n", static_cast<unsigned>(protocol_status));
        }

        // always decrement *after* attempting new IO - the prior IO is now formally "done"
        if (shared_socket->decrement_io() == 0)
        {
            // if we have no more IO pended, complete the state
            shared_socket->complete_state(readwrite_status);
        }
    }

    // The registered function with ctsConfig
    void ctsReadWriteIocp(const std::weak_ptr<ctsSocket>& _weak_socket) noexcept
    {
        // must get a reference to the socket and the IO pattern
        auto shared_socket(_weak_socket.lock());
        if (!shared_socket)
        {
            return;
        }
        // hold a reference on the iopattern
        auto shared_pattern(shared_socket->io_pattern());

        // can't initialize to zero - zero indicates to complete_state()
        long io_count = -1;
        bool io_done = false;
        int io_error = NO_ERROR;

        // lock the socket while doing IO
        const auto socket_ref(shared_socket->socket_reference());
        SOCKET socket = socket_ref.socket();
        if (socket != INVALID_SOCKET)
        {
            // loop until failure or initiate_io returns None
            while (!io_done && NO_ERROR == io_error)
            {
                // each loop requests the next task
                ctsIOTask next_io = shared_pattern->initiate_io();
                if (IOTaskAction::None == next_io.ioAction)
                {
                    // nothing failed, just no more IO right now
                    io_done = true;
                    continue;
                }

                if (IOTaskAction::GracefulShutdown == next_io.ioAction)
                {
                    if (0 != shutdown(socket, SD_SEND))
                    {
                        io_error = WSAGetLastError();
                    }
                    io_done = shared_pattern->complete_io(next_io, 0, io_error) != ctsIOStatus::ContinueIo;
                    continue;
                }

                if (IOTaskAction::HardShutdown == next_io.ioAction)
                {
                    // pass through -1 to force an RST with the closesocket
                    io_error = shared_socket->close_socket(-1);
                    socket = INVALID_SOCKET;

                    io_done = shared_pattern->complete_io(next_io, 0, io_error) != ctsIOStatus::ContinueIo;
                    continue;
                }

                // else we need to initiate another IO
                // add-ref the IO about to start
                io_count = shared_socket->increment_io();

                std::shared_ptr<ctl::ctThreadIocp> io_thread_pool;
                OVERLAPPED* pov = nullptr;
                try
                {
                    // these are the only calls which can throw in this function
                    io_thread_pool = shared_socket->thread_pool();
                    pov = io_thread_pool->new_request(
                        [_weak_socket, next_io](OVERLAPPED* _ov) noexcept { ctsReadWriteIocpIoCompletionCallback(_ov, _weak_socket, next_io); });
                }
                catch (...)
                {
                    io_error = ctsConfig::PrintThrownException();
                }

                // if an exception prevented this IO from initiating,
                if (io_error != NO_ERROR)
                {
                    io_count = shared_socket->decrement_io();
                    io_done = shared_pattern->complete_io(next_io, 0, io_error) != ctsIOStatus::ContinueIo;
                    continue;
                }

                char* io_buffer = next_io.buffer + next_io.buffer_offset;
                if (IOTaskAction::Send == next_io.ioAction)
                {
                    if (!WriteFile(reinterpret_cast<HANDLE>(socket), io_buffer, next_io.buffer_length, nullptr, pov))
                    {
                        io_error = GetLastError();
                    }
                }
                else
                {
                    if (!ReadFile(reinterpret_cast<HANDLE>(socket), io_buffer, next_io.buffer_length, nullptr, pov))
                    {
                        io_error = GetLastError();
                    }
                }
                //
                // not calling complete_io on success, since the IO completion will handle that in the callback
                //
                if (ERROR_IO_PENDING == io_error)
                {
                    io_error = NO_ERROR;
                }

                if (io_error != NO_ERROR)
                {
                    // must cancel the IOCP TP if the IO call fails
                    io_thread_pool->cancel_request(pov);
                    // decrement the IO count since it was not pended
                    io_count = shared_socket->decrement_io();

                    const char* function_name = IOTaskAction::Send == next_io.ioAction ? "WriteFile" : "ReadFile";
                    PRINT_DEBUG_INFO(L"\t\tIO Failed: %hs (%d) [ctsReadWriteIocp]\n", function_name, io_error)

                    // call back to the socket that it failed to see if wants more IO
                    const ctsIOStatus protocol_status = shared_pattern->complete_io(next_io, 0, io_error);
                    switch (protocol_status)
                    {
                        case ctsIOStatus::ContinueIo:
                            // the protocol wants to ignore the error and send more data
                            io_error = NO_ERROR;
                            io_done = false;
                            break;

                        case ctsIOStatus::CompletedIo:
                            // the protocol wants to ignore the error but is done with IO
                            io_error = NO_ERROR;
                            io_done = true;
                            break;

                        case ctsIOStatus::FailedIo:
                            // print the error on failure
                            ctsConfig::PrintErrorIfFailed(function_name, io_error);
                            // the protocol acknoledged the failure - socket is done with IO
                            io_error = static_cast<int>(shared_pattern->get_last_error());
                            io_done = true;
                            break;

                        default:
                            FAIL_FAST_MSG("ctsReadWriteIocp: unknown ctsSocket::IOStatus - %u\n", static_cast<unsigned>(protocol_status));
                    }
                }
            }
        }
        else
        {
            io_error = WSAECONNABORTED;
        }

        if (0 == io_count)
        {
            // complete the ctsSocket if we have no IO pended
            shared_socket->complete_state(io_error);
        }
    }
} // namespace
