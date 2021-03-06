// this is for emacs file handling -*- mode: c++; indent-tabs-mode: nil -*-

// -- BEGIN LICENSE BLOCK ----------------------------------------------

/*!
*  Copyright (C) 2018, SICK AG, Waldkirch
*  Copyright (C) 2018, FZI Forschungszentrum Informatik, Karlsruhe, Germany
*
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*    http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.

*/

// -- END LICENSE BLOCK ------------------------------------------------

//----------------------------------------------------------------------
/*!
 * \file Cola2Session.cpp
 *
 * \author  Lennart Puck <puck@fzi.de>
 * \date    2018-09-24
 */
//----------------------------------------------------------------------

#include <sick_safetyscanners_base/cola2/Cola2Session.h>

namespace sick {
namespace cola2 {

Cola2Session::Cola2Session(const std::shared_ptr<communication::AsyncTCPClient>& async_tcp_client)
  : m_async_tcp_client_ptr(async_tcp_client)
  , m_session_id(0)
  , m_last_request_id(0)
{
  m_packet_merger_ptr = std::make_shared<sick::data_processing::TCPPacketMerger>();
}

Cola2Session::~Cola2Session()
{
//  std::clog << "Descruct Cola2Session" << std::endl;
}

bool Cola2Session::open(CompleteHandler handler)
{
  CommandPtr command_ptr = std::make_shared<CreateSession>(boost::ref(*this));
  return executeCommand(command_ptr, handler);
}

bool Cola2Session::close(CompleteHandler handler)
{
  CommandPtr command_ptr = std::make_shared<CloseSession>(boost::ref(*this));
  return executeCommand(command_ptr, handler);
}

boost::system::error_code Cola2Session::doDisconnect()
{
  return m_async_tcp_client_ptr->doDisconnect();
}

bool Cola2Session::executeCommand(const CommandPtr& command, CompleteHandler handler)
{
  addCommand(command->getRequestID(), command);
  sendTelegramAndListenForAnswer(command, handler );
  return true;
}

void Cola2Session::sendTelegramAndListenForAnswer(const CommandPtr& command, CompleteHandler handler)
{
  std::vector<uint8_t> telegram;
  telegram = command->constructTelegram(telegram);

  m_async_tcp_client_ptr->send(telegram, [this, handler]( boost::system::error_code ec ) {
    if ( ec )
    {
      handler(ec);
      return;
    }

    m_async_tcp_client_ptr->readSome(std::bind(&Cola2Session::processPacket,
                                               this, std::placeholders::_1, std::placeholders::_2,
                                               handler));
  });
}


uint32_t Cola2Session::getSessionID() const
{
  return m_session_id;
}

void Cola2Session::setSessionID(const uint32_t& session_id)
{
  m_session_id = session_id;
}

void Cola2Session::processPacket(const datastructure::PacketBuffer& packet,
                                 const boost::system::error_code& ec,
                                 CompleteHandler handler)
{
  if ( ec )
  {
    handler( ec );
    return;
  }

  addPacketToMerger(packet);

  // checkIfPacketIsCompleteAndOtherwiseListenForMorePackets
  if (m_packet_merger_ptr->isComplete())
  {
    sick::datastructure::PacketBuffer deployed_packet =
      m_packet_merger_ptr->getDeployedPacketBuffer();

    startProcessingAndRemovePendingCommandAfterwards(deployed_packet);
    handler( ec );
  }
  else
  {
    m_async_tcp_client_ptr->readSome(std::bind(&Cola2Session::processPacket,
                                               this, std::placeholders::_1, std::placeholders::_2,
                                               handler));
  }
}

bool Cola2Session::addPacketToMerger(const sick::datastructure::PacketBuffer& packet)
{
  if (m_packet_merger_ptr->isEmpty() || m_packet_merger_ptr->isComplete())
  {
    m_packet_merger_ptr->setTargetSize( sick::data_processing::ParseTCPPacket::getExpectedPacketLength(packet));
  }

  m_packet_merger_ptr->addTCPPacket(packet);

  return true;
}

bool Cola2Session::startProcessingAndRemovePendingCommandAfterwards(
  const sick::datastructure::PacketBuffer& packet)
{
  uint16_t request_id = sick::data_processing::ParseTCPPacket::getRequestID(packet);
  CommandPtr pending_command;

  if (findCommand(request_id, pending_command))
  {
    pending_command->processReplyBase(*packet.getBuffer());
    removeCommand(request_id);
  }

  return true;
}

bool Cola2Session::addCommand(const uint16_t& request_id, const CommandPtr& command)
{
  boost::mutex::scoped_lock lock(m_pending_commands_map_mutex);

  if (m_pending_commands_map.find(request_id) != m_pending_commands_map.end())
  {
    return false;
  }

  m_pending_commands_map[request_id] = command;

  return true;
}

bool Cola2Session::findCommand(const uint16_t& request_id, CommandPtr& command)
{
  boost::mutex::scoped_lock lock(m_pending_commands_map_mutex);

  if (m_pending_commands_map.find(request_id) == m_pending_commands_map.end())
  {
    return false;
  }
  command = m_pending_commands_map[request_id];

  return true;
}

bool Cola2Session::removeCommand(const uint16_t& request_id)
{
  boost::mutex::scoped_lock lock(m_pending_commands_map_mutex);

  auto it = m_pending_commands_map.find(request_id);
  if (it == m_pending_commands_map.end())
  {
    return false;
  }

  m_pending_commands_map.erase(it);

  return true;
}

uint16_t Cola2Session::getNextRequestID()
{
  if (m_last_request_id == std::numeric_limits<uint16_t>::max())
  {
    m_last_request_id = 0;
  }
  return ++m_last_request_id;
}

} // namespace cola2
} // namespace sick
