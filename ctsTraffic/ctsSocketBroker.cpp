/*

Copyright (c) Microsoft Corporation
All rights reserved.

Licensed under the Apache License, Version 2.0 (the ""License""); you may not use this file except in compliance with the License. You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

THIS CODE IS PROVIDED ON AN  *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.

See the Apache Version 2.0 License for specific language governing permissions and limitations under the License.

*/

// parent header
#include "ctsSocketBroker.h"
// cpp headers
#include <memory>
#include <iterator>
// os headers
#include <Windows.h>
// wil headers
#include <wil/stl.h>
#include <wil/resource.h>
// ctl headers
#include <ctThreadPoolTimer.hpp>
// project headers
#include "ctsConfig.h"
#include "ctsSocketState.h"

namespace ctsTraffic
{

    using namespace ctl;
    using namespace std;

    // timer to wake up and clean up the socket pool
    // - delete any closed sockets
    // - create new sockets
    unsigned long ctsSocketBroker::m_timerCallbackTimeoutMs = 333; // millseconds

    ctsSocketBroker::ctsSocketBroker()
    {
        if (ctsConfig::g_configSettings->AcceptFunction)
        {
            // server 'accept' settings
            m_totalConnectionsRemaining = ctsConfig::g_configSettings->ServerExitLimit;
            m_pendingLimit = ctsConfig::g_configSettings->AcceptLimit;

        }
        else
        {
            // client 'connect' settings
            if (ctsConfig::g_configSettings->Iterations == MAXULONGLONG)
            {
                m_totalConnectionsRemaining = MAXULONGLONG;
            }
            else
            {
                m_totalConnectionsRemaining = ctsConfig::g_configSettings->Iterations * static_cast<ULONGLONG>(ctsConfig::g_configSettings->ConnectionLimit);
            }
            m_pendingLimit = ctsConfig::g_configSettings->ConnectionLimit;
        }

        // make sure pending_limit cannot be larger than total_connections_remaining
        if (m_pendingLimit > m_totalConnectionsRemaining)
        {
            m_pendingLimit = static_cast<unsigned long>(m_totalConnectionsRemaining);
        }

        // create our manual-reset notification event
        m_doneEvent.create(wil::EventOptions::ManualReset, nullptr);
    }

    ctsSocketBroker::~ctsSocketBroker() noexcept
    {
        // first, turn off the timer to stop creating/tearing down the socket pool
        m_wakeupTimer.stop_all_timers();

        // now delete all children, guaranteeing they stop processing
        // - must do this explicitly before deleting the CS
        //   in case they were calling back while we called detach
        m_socketPool.clear();
    }

    void ctsSocketBroker::Start()
    {
        PRINT_DEBUG_INFO(
            L"\t\tStarting broker: total connections remaining (%llu), pending limit (%u)\n",
            m_totalConnectionsRemaining, m_pendingLimit);

        // must always guard access to the vector
        const auto lock = m_lock.lock();

        // only loop to pending_limit
        while (m_totalConnectionsRemaining > 0 && m_pendingSockets < m_pendingLimit)
        {
            // for outgoing connections, limit to ConnectionThrottleLimit
            // - to prevent killing the box with DPCs with too many concurrent connect attempts
            // checking first since TimerCallback might have already established connections
            if (!ctsConfig::g_configSettings->AcceptFunction &&
                m_pendingSockets >= ctsConfig::g_configSettings->ConnectionThrottleLimit)
            {
                break;
            }

            m_socketPool.push_back(make_shared<ctsSocketState>(shared_from_this()));
            (*m_socketPool.rbegin())->Start();
            ++m_pendingSockets;
            --m_totalConnectionsRemaining;
        }

        // intiate the threadpool timer
        m_wakeupTimer.schedule_reoccuring(
            [this]() noexcept { TimerCallback(this); },
            0LL,
            m_timerCallbackTimeoutMs);
    }
    //
    // SocketState is indicating the socket is now 'connected'
    // - and will be pumping IO
    // Update pending and active counts under guard
    //
    void ctsSocketBroker::InitiatingIo() noexcept
    {
        const auto lock = m_lock.lock();

        FAIL_FAST_IF_MSG(
            m_pendingSockets == 0,
            "ctsSocketBroker::initiating_io - About to decrement pending_sockets, but pending_sockets == 0 (active_sockets == %u)",
            m_activeSockets);

        --m_pendingSockets;
        ++m_activeSockets;
    }
    //
    // SocketState is indicating the socket is now 'closed'
    // Update pending or active counts (depending on prior state) under guard
    //
    void ctsSocketBroker::Closing(bool wasActive) noexcept
    {
        const auto lock = m_lock.lock();

        if (wasActive)
        {
            FAIL_FAST_IF_MSG(
                m_activeSockets == 0,
                "ctsSocketBroker::closing - About to decrement active_sockets, but active_sockets == 0 (pending_sockets == %u)",
                m_pendingSockets);
            --m_activeSockets;
        }
        else
        {
            FAIL_FAST_IF_MSG(
                m_pendingSockets == 0,
                "ctsSocketBroker::closing - About to decrement pending_sockets, but pending_sockets == 0 (active_sockets == %u)",
                m_activeSockets);
            --m_pendingSockets;
        }
    }

    bool ctsSocketBroker::Wait(DWORD milliseconds) const noexcept
    {
        HANDLE arWait[2]{ m_doneEvent.get(), ctsConfig::g_configSettings->CtrlCHandle };

        bool fReturn = false;
        switch (WaitForMultipleObjects(2, arWait, FALSE, milliseconds))
        {
            // we are done with our sockets, or user hit ctrl'c
            // - in either case we need to tell the caller to exit
            case WAIT_OBJECT_0:
            case WAIT_OBJECT_0 + 1:
                fReturn = true;
                break;

            case WAIT_TIMEOUT:
                fReturn = false;
                break;

            default:
                FAIL_FAST_MSG(
                    "ctsSocketBroker - WaitForMultipleObjects(%p) failed [%u]",
                    arWait, GetLastError());
        }
        return fReturn;
    }

    //
    // Timer callback to scavenge any closed sockets
    // Then refresh sockets that should be created anew
    //
    void ctsSocketBroker::TimerCallback(_In_ ctsSocketBroker* pBroker) noexcept
    {
        // removed_objects will delete the closed objects outside of the broker lock
        vector<shared_ptr<ctsSocketState>> removedObjects;
        {
            const auto lock = pBroker->m_lock.try_lock();
            if (!lock)
            {
                return;
            }

            // refresh our pool of sockets if more sockets should be added
            try
            {
                //
                // Everything must occur under the broker lock
                // - touching the socket_pool
                // - touching the socket / connection counters
                //
                for (auto& socketPoolEntry : pBroker->m_socketPool)
                {
                    if (ctsSocketState::InternalState::Closed == socketPoolEntry->GetCurrentState())
                    {
                        removedObjects.push_back(socketPoolEntry);
                        socketPoolEntry.reset();
                    }
                }

                pBroker->m_socketPool.erase(
                    remove(
                        begin(pBroker->m_socketPool),
                        end(pBroker->m_socketPool),
                        nullptr),
                    end(pBroker->m_socketPool));

                if (0 == pBroker->m_totalConnectionsRemaining &&
                    0 == pBroker->m_pendingSockets &&
                    0 == pBroker->m_activeSockets)
                {
                    // it's time to exit if no more work is to be done
                    SetEvent(pBroker->m_doneEvent.get());

                }
                else
                {
                    // don't spin up more if the user asked to shutdown
                    if (WAIT_OBJECT_0 != WaitForSingleObject(pBroker->m_doneEvent.get(), 0))
                    {
                        // catch up to the expected # of pended connections
                        while (pBroker->m_pendingSockets < pBroker->m_pendingLimit && pBroker->m_totalConnectionsRemaining > 0)
                        {
                            // not throttling the server accepting sockets based off total # of connections (pending + active)
                            // - only throttling total connections for outgoing connections
                            if (!ctsConfig::g_configSettings->AcceptFunction)
                            {
                                // ReSharper disable once CppRedundantParentheses
                                if ((pBroker->m_pendingSockets + pBroker->m_activeSockets) >= ctsConfig::g_configSettings->ConnectionLimit)
                                {
                                    break;
                                }
                                // throttle pending connection attempts as specified
                                if (pBroker->m_pendingSockets >= ctsConfig::g_configSettings->ConnectionThrottleLimit)
                                {
                                    break;
                                }
                            }

                            pBroker->m_socketPool.push_back(make_shared<ctsSocketState>(pBroker->shared_from_this()));
                            (*pBroker->m_socketPool.rbegin())->Start();
                            ++pBroker->m_pendingSockets;
                            --pBroker->m_totalConnectionsRemaining;
                        }
                    }
                }
            }
            CATCH_LOG()
        }
    }

} // namespace
