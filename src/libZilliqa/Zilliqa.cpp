/**
* Copyright (c) 2018 Zilliqa 
* This source code is being disclosed to you solely for the purpose of your participation in 
* testing Zilliqa. You may view, compile and run the code for that purpose and pursuant to 
* the protocols and algorithms that are programmed into, and intended by, the code. You may 
* not do anything else with the code without express permission from Zilliqa Research Pte. Ltd., 
* including modifying or publishing the code (or any part of it), and developing or forming 
* another public or private blockchain network. This source code is provided ‘as is’ and no 
* warranties are given as to title or non-infringement, merchantability or fitness for purpose 
* and, to the extent permitted by law, all liability for your use of the code is disclaimed. 
* Some programs in this code are governed by the GNU General Public License v3.0 (available at 
* https://www.gnu.org/licenses/gpl-3.0.en.html) (‘GPLv3’). The programs that are governed by 
* GPLv3.0 are those programs that are located in the folders src/depends and tests/depends 
* and which include a reference to GPLv3 in their program files.
**/

#include <jsonrpccpp/common/exception.h>
#include <jsonrpccpp/server/connectors/httpserver.h>

#include "Zilliqa.h"
#include "common/Constants.h"
#include "common/Messages.h"
#include "common/Serializable.h"
#include "libCrypto/Schnorr.h"
#include "libCrypto/Sha2.h"
#include "libData/AccountData/Address.h"
#include "libNetwork/Whitelist.h"
#include "libUtils/DataConversion.h"
#include "libUtils/DetachedFunction.h"
#include "libUtils/Logger.h"

using namespace std;
using namespace jsonrpc;

void Zilliqa::LogSelfNodeInfo(const std::pair<PrivKey, PubKey>& key,
                              const Peer& peer)
{
    vector<unsigned char> tmp1;
    vector<unsigned char> tmp2;

    key.first.Serialize(tmp1, 0);
    key.second.Serialize(tmp2, 0);

    LOG_PAYLOAD(INFO, "Private Key", tmp1, PRIV_KEY_SIZE * 2);
    LOG_PAYLOAD(INFO, "Public Key", tmp2, PUB_KEY_SIZE * 2);

    SHA2<HASH_TYPE::HASH_VARIANT_256> sha2;
    sha2.Reset();
    vector<unsigned char> message;
    key.second.Serialize(message, 0);
    sha2.Update(message, 0, PUB_KEY_SIZE);
    const vector<unsigned char>& tmp3 = sha2.Finalize();
    Address toAddr;
    copy(tmp3.end() - ACC_ADDR_SIZE, tmp3.end(), toAddr.asArray().begin());

    LOG_GENERAL(INFO,
                "My address is " << toAddr << " and port is "
                                 << peer.m_listenPortHost);
}

void Zilliqa::ProcessMessage(pair<vector<unsigned char>, Peer>* message)
{
    if (message->first.size() >= MessageOffset::BODY)
    {
        const unsigned char msg_type = message->first.at(MessageOffset::TYPE);

        Executable* msg_handlers[] = {&m_pm, &m_ds, &m_n, &m_cu, &m_lookup};

        const unsigned int msg_handlers_count
            = sizeof(msg_handlers) / sizeof(Executable*);

        if (msg_type < msg_handlers_count)
        {
            bool result = msg_handlers[msg_type]->Execute(
                message->first, MessageOffset::INST, message->second);

            if (result == false)
            {
                // To-do: Error recovery
            }
        }
        else
        {
            LOG_GENERAL(WARNING,
                        "Unknown message type " << std::hex
                                                << (unsigned int)msg_type);
        }
    }

    delete message;
}

Zilliqa::Zilliqa(const std::pair<PrivKey, PubKey>& key, const Peer& peer,
                 bool loadConfig, unsigned int syncType, bool toRetrieveHistory)
    : m_pm(key, peer, loadConfig)
    , m_mediator(key, peer)
    , m_ds(m_mediator)
    , m_lookup(m_mediator)
    , m_n(m_mediator, syncType, toRetrieveHistory)
    , m_cu(key, peer)
    , m_msgQueue(MSGQUEUE_SIZE)
#ifdef IS_LOOKUP_NODE
    , m_httpserver(SERVER_PORT)
    , m_server(m_mediator, m_httpserver)
#endif // IS_LOOKUP_NODE

{
    LOG_MARKER();

    // Launch the thread that reads messages from the queue
    auto funcCheckMsgQueue = [this]() mutable -> void {
        pair<vector<unsigned char>, Peer>* message = NULL;
        while (true)
        {
            while (m_msgQueue.pop(message))
            {
                // For now, we use a thread pool to handle this message
                // Eventually processing will be single-threaded
                m_queuePool.AddJob([this, message]() mutable -> void {
                    ProcessMessage(message);
                });
            }
        }
    };
    DetachedFunction(1, funcCheckMsgQueue);

    m_validator = make_shared<Validator>(m_mediator);
    m_mediator.RegisterColleagues(&m_ds, &m_n, &m_lookup, m_validator.get());
    m_n.Install(syncType, toRetrieveHistory);

    LogSelfNodeInfo(key, peer);

    P2PComm::GetInstance().SetSelfPeer(peer);

    switch (syncType)
    {
    case SyncType::NO_SYNC:
        LOG_GENERAL(INFO, "No Sync Needed");
        Whitelist::GetInstance().Init();
        break;
#ifndef IS_LOOKUP_NODE
    case SyncType::NEW_SYNC:
        LOG_GENERAL(INFO, "Sync as a new node");
        if (!toRetrieveHistory)
        {
            m_mediator.m_lookup->m_syncType = SyncType::NEW_SYNC;
            m_n.m_runFromLate = true;
            m_n.StartSynchronization();
        }
        else
        {
            LOG_GENERAL(WARNING,
                        "Error: Sync for new node shouldn't retrieve history");
        }
        break;
    case SyncType::NORMAL_SYNC:
        LOG_GENERAL(INFO, "Sync as a normal node");
        m_mediator.m_lookup->m_syncType = SyncType::NORMAL_SYNC;
        m_n.m_runFromLate = true;
        m_n.StartSynchronization();
        break;
    case SyncType::DS_SYNC:
        LOG_GENERAL(INFO, "Sync as a ds node");
        m_mediator.m_lookup->m_syncType = SyncType::DS_SYNC;
        m_ds.StartSynchronization();
        break;
#else // IS_LOOKUP_NODE
    case SyncType::LOOKUP_SYNC:
        LOG_GENERAL(INFO, "Sync as a lookup node");
        m_mediator.m_lookup->m_syncType = SyncType::LOOKUP_SYNC;
        m_lookup.StartSynchronization();
        break;
#endif // IS_LOOKUP_NODE
    default:
        LOG_GENERAL(WARNING, "Invalid Sync Type");
        break;
    }

#ifndef IS_LOOKUP_NODE
    LOG_GENERAL(INFO, "I am a normal node.");
#else // else for IS_LOOKUP_NODE
    LOG_GENERAL(INFO, "I am a lookup node.");
    if (m_server.StartListening())
    {
        LOG_GENERAL(INFO, "API Server started successfully");
    }
    else
    {
        LOG_GENERAL(WARNING, "API Server couldn't start");
    }
#endif // IS_LOOKUP_NODE
}

Zilliqa::~Zilliqa()
{
    pair<vector<unsigned char>, Peer>* message = NULL;
    while (m_msgQueue.pop(message))
    {
        delete message;
    }
}

void Zilliqa::Dispatch(pair<vector<unsigned char>, Peer>* message)
{
    //LOG_MARKER();

    // Queue message
    while (!m_msgQueue.push(message))
    {
        // Keep attempting to push until success
    }
}

vector<Peer> Zilliqa::RetrieveBroadcastList(unsigned char msg_type,
                                            unsigned char ins_type,
                                            const Peer& from)
{
    // LOG_MARKER();

    Broadcastable* msg_handlers[] = {&m_pm, &m_ds, &m_n, &m_cu, &m_lookup};

    const unsigned int msg_handlers_count
        = sizeof(msg_handlers) / sizeof(Broadcastable*);

    if (msg_type < msg_handlers_count)
    {
        return msg_handlers[msg_type]->GetBroadcastList(ins_type, from);
    }
    else
    {
        LOG_GENERAL(WARNING,
                    "Unknown message type " << std::hex
                                            << (unsigned int)msg_type);
    }

    return vector<Peer>();
}
