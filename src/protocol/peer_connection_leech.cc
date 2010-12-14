// libTorrent - BitTorrent library
// Copyright (C) 2005-2007, Jari Sundell
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//
// In addition, as a special exception, the copyright holders give
// permission to link the code of portions of this program with the
// OpenSSL library under certain conditions as described in each
// individual source file, and distribute linked combinations
// including the two.
//
// You must obey the GNU General Public License in all respects for
// all of the code used other than OpenSSL.  If you modify file(s)
// with this exception, you may extend this exception to your version
// of the file(s), but you are not obligated to do so.  If you do not
// wish to do so, delete this exception statement from your version.
// If you delete this exception statement from all source files in the
// program, then also delete it here.
//
// Contact:  Jari Sundell <jaris@ifi.uio.no>
//
//           Skomakerveien 33
//           3185 Skoppum, NORWAY

#include "config.h"

#include <cstring>
#include <sstream>
#include <rak/string_manip.h>

#include "data/chunk_list_node.h"
#include "download/choke_manager.h"
#include "download/chunk_selector.h"
#include "download/chunk_statistics.h"
#include "download/download_info.h"
#include "download/download_main.h"
#include "torrent/dht_manager.h"
#include "torrent/peer/connection_list.h"
#include "torrent/peer/peer_info.h"

#include "extensions.h"
#include "initial_seed.h"
#include "peer_connection_leech.h"

//there was no iostream
#include <iostream>
#include "torrent/data/block_list.h"
#include "torrent/data/block.h"
#include "torrent/data/block_transfer.h"
#include "torrent/data/transfer_list.h"

namespace torrent {

template<Download::ConnectionType type>
PeerConnection<type>::~PeerConnection() {
//   if (m_download != NULL && m_down->get_state() != ProtocolRead::READ_BITFIELD)
//     m_download->bitfield_counter().dec(m_peerChunks.bitfield()->bitfield());

//   priority_queue_erase(&taskScheduler, &m_taskSendChoke);
}

template<Download::ConnectionType type>
void
PeerConnection<type>::initialize_custom() {
  if (type == Download::CONNECTION_INITIAL_SEED) {
    if (m_download->initial_seeding() == NULL) {
      // Can't throw close_connection or network_error here, we're still
      // initializing. So close the socket and let that kill it later.
      get_fd().close();
      return;
    }

    m_download->initial_seeding()->new_peer(this);
  }
//   if (m_download->content()->chunks_completed() != 0) {
//     m_up->write_bitfield(m_download->file_list()->bitfield()->size_bytes());

//     m_up->buffer()->prepare_end();
//     m_up->set_position(0);
//     m_up->set_state(ProtocolWrite::WRITE_BITFIELD_HEADER);
//   }
}

template<Download::ConnectionType type>
void
PeerConnection<type>::update_interested() {
  if (type != Download::CONNECTION_LEECH)
    return;

  m_peerChunks.download_cache()->clear();

  if (m_downInterested)
    return;

  // Consider just setting to interested without checking the
  // bitfield. The status might change by the time we get unchoked
  // anyway.

  m_sendInterested = !m_downInterested;
  m_downInterested = true;

  // Hmm... does this belong here, or should we insert ourselves into
  // the queue when we receive the unchoke?
//   m_download->download_choke_manager()->set_queued(this, &m_downChoke);
}

template<Download::ConnectionType type>
bool
PeerConnection<type>::receive_keepalive() {
  if (cachedTime - m_timeLastRead > rak::timer::from_seconds(240))
    return false;

  // There's no point in adding ourselves to the write poll if the
  // buffer is full, as that will already have been taken care of.
  if (m_up->get_state() == ProtocolWrite::IDLE &&
      m_up->can_write_keepalive()) {

    write_insert_poll_safe();

    ProtocolBuffer<512>::iterator old_end = m_up->buffer()->end();
    m_up->write_keepalive();

    if (is_encrypted())
      m_encryption.encrypt(old_end, m_up->buffer()->end() - old_end);
  }

  if (type != Download::CONNECTION_LEECH)
    return true;

  m_tryRequest = true;

  // Stall pieces when more than one receive_keepalive() has been
  // called while a single piece is downloading.
  //
  // m_downStall is decremented for every successfull download, so it
  // should stay at zero or one when downloading at an acceptable
  // speed. Thus only when m_downStall >= 2 is the download actually
  // stalling.
  //
  // If more than 6 ticks have gone by, assume the peer forgot about
  // our requests or tried to cheat with empty piece messages, and try
  // again.

  // TODO: Do we need to remove from download throttle here?
  //
  // Do we also need to remove from download throttle? Check how it
  // worked again.

  if (!download_queue()->canceled_empty() && m_downStall >= 10)		//there was 6
    download_queue()->cancel();
  else if (!download_queue()->queued_empty() && m_downStall++ != 0)
    download_queue()->stall();

  return true;
}

// We keep the message in the buffer if it is incomplete instead of
// keeping the state and remembering the read information. This
// shouldn't happen very often compared to full reads.
template<Download::ConnectionType type>
inline bool
PeerConnection<type>::read_message() {
  ProtocolBuffer<512>* buf = m_down->buffer();

  if (buf->remaining() < 4)
    return false;

  // Remember the start of the message so we may reset it if we don't
  // have the whole message.
  ProtocolBuffer<512>::iterator beginning = buf->position();

  uint32_t length = buf->read_32();

  if (length == 0) {
    // Keepalive message.
    m_down->set_last_command(ProtocolBase::KEEP_ALIVE);

    return true;

  } else if (buf->remaining() < 1) {
    buf->set_position_itr(beginning);
    return false;

  } else if (length > (1 << 20)) {
    throw communication_error("PeerConnection::read_message() got an invalid message length.");
  }
    
  // We do not verify the message length of those with static
  // length. A bug in the remote client causing the message start to
  // be unsyncronized would in practically all cases be caught with
  // the above test.
  //
  // Those that do in some weird way manage to produce a valid
  // command, will not be able to do any more damage than a malicious
  // peer. Those cases should be caught elsewhere in the code.

  // Temporary.
  m_down->set_last_command((ProtocolBase::Protocol)buf->peek_8());

  switch (buf->read_8()) {
  case ProtocolBase::CHOKE:
    if (type != Download::CONNECTION_LEECH)
      return true;

    // Cancel before dequeueing so receive_download_choke knows if it
    // should remove us from throttle.
    //
    // Hmm... that won't work, as we arn't necessarily unchoked when
    // in throttle.

    // Which needs to be done before, and which after calling choke
    // manager?
    m_downUnchoked = false;

    std::cout << "received CHOKE !!!" << std::endl << std::flush;

    download_queue()->cancel();
    m_download->download_choke_manager()->set_not_queued(this, &m_downChoke);
    m_down->throttle()->erase(m_peerChunks.download_throttle());

    return true;

  case ProtocolBase::UNCHOKE:
	  std::cout << "received UNCHOKE !!!" << std::endl << std::flush;
    if (type != Download::CONNECTION_LEECH)
      return true;

    m_downUnchoked = true;

    // Some peers unchoke us even though we're not interested, so we
    // need to ensure it doesn't get added to the queue.
    if (!m_downInterested)
      return true;

    m_download->download_choke_manager()->set_queued(this, &m_downChoke);
    return true;

  case ProtocolBase::INTERESTED:
    if (type == Download::CONNECTION_LEECH && m_peerChunks.bitfield()->is_all_set())
      return true;

    m_download->upload_choke_manager()->set_queued(this, &m_upChoke);
    return true;

  case ProtocolBase::NOT_INTERESTED:
    m_download->upload_choke_manager()->set_not_queued(this, &m_upChoke);
    return true;

  case ProtocolBase::HAVE:
    if (!m_down->can_read_have_body())
      break;

    read_have_chunk(buf->read_32());
    return true;

  case ProtocolBase::START:
      if (!m_down->can_read_START_body())
        break;

      read_START_chunk(buf->read_32());
      return true;

  case ProtocolBase::STOP:
      if (!m_down->can_read_STOP_body())
        break;

      read_STOP_chunk(buf->read_32());
      return true;

  case ProtocolBase::REQUEST:
    if (!m_down->can_read_request_body())
      break;

    if (!m_upChoke.choked()) {
      write_insert_poll_safe();
      read_request_piece(m_down->read_request());

    } else {
      m_down->read_request();
    }

    return true;

  case ProtocolBase::PIECE:
    if (type != Download::CONNECTION_LEECH)
      throw communication_error("Received a piece but the connection is strictly for seeding.");

    if (!m_down->can_read_piece_body())
      break;

    if (!down_chunk_start(m_down->read_piece(length - 9))) {

      // We don't want this chunk.
      if (down_chunk_skip_from_buffer()) {
        m_tryRequest = true;
        down_chunk_finished();
        return true;

      } else {
        m_down->set_state(ProtocolRead::READ_SKIP_PIECE);
        m_down->throttle()->insert(m_peerChunks.download_throttle());
        return false;
      }
      
    } else {

      if (down_chunk_from_buffer()) {
        m_tryRequest = true;
        down_chunk_finished();
        return true;

      } else {
        m_down->set_state(ProtocolRead::READ_PIECE);
        m_down->throttle()->insert(m_peerChunks.download_throttle());
        return false;
      }
    }

  case ProtocolBase::CANCEL:
    if (!m_down->can_read_cancel_body())
      break;

    read_cancel_piece(m_down->read_request());
    return true;

  case ProtocolBase::PORT:
    if (!m_down->can_read_port_body())
      break;

    manager->dht_manager()->add_node(m_peerInfo->socket_address(), m_down->buffer()->read_16());
    return true;

  case ProtocolBase::EXTENSION_PROTOCOL:
    if (!m_down->can_read_extension_body())
      break;

    if (m_extensions->is_default()) {
      m_extensions = new ProtocolExtension();
      m_extensions->set_info(m_peerInfo, m_download);
    }

    {
      int extension = m_down->buffer()->read_8();
      m_extensions->read_start(extension, length - 2, (extension == ProtocolExtension::UT_PEX) && !m_download->want_pex_msg());
      m_down->set_state(ProtocolRead::READ_EXTENSION);
    }

    if (down_extension())
      m_down->set_state(ProtocolRead::IDLE);

    return true;

  default:
    throw communication_error("Received unsupported message type.");
  }

  // We were unsuccessfull in reading the message, need more data.
  buf->set_position_itr(beginning);
  return false;
}

template<Download::ConnectionType type>
void
PeerConnection<type>::event_read() {
  m_timeLastRead = cachedTime;

  // Need to make sure ProtocolBuffer::end() is pointing to the end of
  // the unread data, and that the unread data starts from the
  // beginning of the buffer. Or do we use position? Propably best,
  // therefor ProtocolBuffer::position() points to the beginning of
  // the unused data.

  try {
    
    // Normal read.
    //
    // We rarely will read zero bytes as the read of 64 bytes will
    // almost always either not fill up or it will require additional
    // reads.
    //
    // Only loop when end hits 64.

    do {

      switch (m_down->get_state()) {
      case ProtocolRead::IDLE:
        if (m_down->buffer()->size_end() < read_size) {
          unsigned int length = read_stream_throws(m_down->buffer()->end(), read_size - m_down->buffer()->size_end());
          m_down->throttle()->node_used_unthrottled(length);

          if (is_encrypted())
            m_encryption.decrypt(m_down->buffer()->end(), length);

          m_down->buffer()->move_end(length);
        }

        while (read_message());
        
        if (m_down->buffer()->size_end() == read_size) {
          m_down->buffer()->move_unused();
          break;
        } else {
          m_down->buffer()->move_unused();
          return;
        }

      case ProtocolRead::READ_PIECE:
        if (type != Download::CONNECTION_LEECH)
          return;

        if (!download_queue()->is_downloading())
          throw internal_error("ProtocolRead::READ_PIECE state but RequestList is not downloading.");

        if (!m_downloadQueue.transfer()->is_valid() || !m_downloadQueue.transfer()->is_leader()) {
          m_down->set_state(ProtocolRead::READ_SKIP_PIECE);
          break;
        }

        if (!down_chunk())
          return;

        m_tryRequest = true;
        m_down->set_state(ProtocolRead::IDLE);
        down_chunk_finished();
        break;

      case ProtocolRead::READ_SKIP_PIECE:
        if (type != Download::CONNECTION_LEECH)
          return;

        if (download_queue()->transfer()->is_leader()) {
          m_down->set_state(ProtocolRead::READ_PIECE);
          break;
        }

        if (!down_chunk_skip())
          return;

        m_tryRequest = true;
        m_down->set_state(ProtocolRead::IDLE);
        down_chunk_finished();
        break;

      case ProtocolRead::READ_EXTENSION:
        if (!down_extension())
          return;

        m_down->set_state(ProtocolRead::IDLE);
        break;

      default:
        throw internal_error("PeerConnection::event_read() wrong state.");
      }

      // Figure out how to get rid of the shouldLoop boolean.
    } while (true);

  // Exception handlers:

  } catch (close_connection& e) {
    m_download->connection_list()->erase(this, 0);

  } catch (blocked_connection& e) {
    m_download->info()->signal_network_log().emit("Momentarily blocked read connection.");
    m_download->connection_list()->erase(this, 0);

  } catch (network_error& e) {
    m_download->info()->signal_network_log().emit((rak::socket_address::cast_from(m_peerInfo->socket_address())->address_str() + " " + rak::copy_escape_html(std::string(m_peerInfo->id().c_str(), 8)) + ": " + e.what()).c_str());

    m_download->connection_list()->erase(this, 0);

  } catch (storage_error& e) {
    m_download->info()->signal_storage_error().emit(e.what());
    m_download->connection_list()->erase(this, 0);

  } catch (base_error& e) {
    std::stringstream s;
    s << "Connection read fd(" << get_fd().get_fd() << ',' << m_down->get_state() << ',' << m_down->last_command() << ") \"" << e.what() << '"';

    throw internal_error(s.str());
  }
}

template<Download::ConnectionType type>
inline void
PeerConnection<type>::fill_write_buffer() {
  ProtocolBuffer<512>::iterator old_end = m_up->buffer()->end();

  // No need to use delayed choke ever.
  if (m_sendChoked && m_up->can_write_choke()) {
    m_sendChoked = false;
    m_up->write_choke(m_upChoke.choked());

    m_upChoke.choked() ? std::cout << "sending CHOKE !" << std::endl << std::flush : std::cout << "sending UNCHOKE !" << std::endl << std::flush;

    if (m_upChoke.choked()) {
      m_up->throttle()->erase(m_peerChunks.upload_throttle());
      up_chunk_release();
      m_peerChunks.upload_queue()->clear();

      if (m_encryptBuffer != NULL) {
        if (m_encryptBuffer->remaining())
          throw internal_error("Deleting encryptBuffer with encrypted data remaining.");

        delete m_encryptBuffer;
        m_encryptBuffer = NULL;
      }

    } else {
      m_up->throttle()->insert(m_peerChunks.upload_throttle());
    }
  }

  // Send the interested state before any requests as some clients,
  // e.g. BitTornado 0.7.14 and uTorrent 0.3.0, disconnect if a
  // request has been received while uninterested. The problem arises
  // as they send unchoke before receiving interested.
  if (type == Download::CONNECTION_LEECH && m_sendInterested && m_up->can_write_interested()) {
    m_up->write_interested(m_downInterested);
    m_sendInterested = false;
  }

//THERE was this not
  while (type == Download::CONNECTION_LEECH && !m_peerChunks.cancel_queue()->empty() && m_up->can_write_cancel()) {
	  std::cout << "sending CANCEL " << m_peerChunks.cancel_queue()->front().index() << ", offset " << m_peerChunks.cancel_queue()->front().offset() << std::endl << std::flush;

	  if (m_download->delegator()->ChunkSelect->STARTed[m_peerChunks.cancel_queue()->front().index()] != NULL) {
	      	std::cout << "..." << m_peerChunks.cancel_queue()->front().index() << " is STARTed, and thus in the transfer list..." << std::endl << std::flush;
	      	//here check if it is maybe in m_queued twice? does it ever get removed when I send cancel?
	      }

	  m_up->write_cancel(m_peerChunks.cancel_queue()->front());

	  m_peerChunks.cancel_queue()->pop_front();
  }


//HERE i need to act as unchoked: is that possible at all? what if I am choked and other peer won't send me anything anyway?

  if (type == Download::CONNECTION_LEECH && m_tryRequest) {		//if i'm leech and should try to request
    if (!(m_tryRequest = !should_request()) &&			//not if I shouldn't request, thus if I should
        !(m_tryRequest = try_request_pieces()) &&		//not if I requested any pieces already, thus if I couldn't request any

        !download_queue()->is_interested_in_active()) { //and not if I'm interrested in active downloading blocks? (pieces) ? (if the peer doesn't have any of pieces queued)

    	//this all sends not interrested, chokes the peer and sends choke
      m_sendInterested = true;				//if I should send interrested message
      m_downInterested = false;				//means I am not interrested in download from this peer

      //it chokes here? but what about m_send and m_downInterested?
      m_download->download_choke_manager()->set_not_queued(this, &m_downChoke);


    }
  }

  DownloadMain::have_queue_type*  haveQueue  = m_download->have_queue();
  DownloadMain::START_queue_type* STARTQueue = m_download->START_queue();
  DownloadMain::START_queue_type* STOPQueue = m_download->STOP_queue();
  DownloadMain::START_queue_type::iterator idx;

  if (type == Download::CONNECTION_LEECH && 
      !haveQueue->empty() &&
      m_peerChunks.have_timer() <= haveQueue->front().first &&
      m_up->can_write_have()) {
    DownloadMain::have_queue_type::iterator last = std::find_if(haveQueue->begin(), haveQueue->end(),
                                                                rak::greater(m_peerChunks.have_timer(), rak::mem_ref(&DownloadMain::have_queue_type::value_type::first)));

    do {
      m_up->write_have((--last)->second);
      m_download->STOPed[last->second] = true;
      std::cout << rak::timer::current_usec() << " : sending HAVE " << (last)->second << std::endl << std::flush ;
    } while (last != haveQueue->begin() && m_up->can_write_have());

    m_peerChunks.set_have_timer(last->first + 1);
    //std::cout << "*" << std::flush;
  }


//START

//here I can expect that all HAVE that should have been sent, if not I don't have buffer space for sending START anyway
//here the START queue should not contain any pieces we have sent HAVE of, thus I can safely transmit... [because finished pieces get removed in Dwrapper::receive_hash_done ]
//check against START timer, to send only the STARTs that haven't been sent already

    if (type == Download::CONNECTION_LEECH &&
        !STARTQueue->empty() &&
        m_peerChunks.START_timer() <= STARTQueue->front().first &&
        m_up->can_write_START()) {

      DownloadMain::START_queue_type::iterator last = std::find_if(STARTQueue->begin(), STARTQueue->end(),
                                                                  rak::greater(m_peerChunks.START_timer(), rak::mem_ref(&DownloadMain::START_queue_type::value_type::first)));

    	//here the START queue should not contain any pieces we have sent HAVE of, thus I can safely transmit...
        std::cout << "START queue size is " << STARTQueue->size() << std::endl;

      do {
        m_up->write_START((--last)->second);
        m_download->STOPed[(last)->second] = true;
        std::cout << rak::timer::current_usec() << " : sending START " << (last)->second << std::endl << std::flush ;
      } while (last != STARTQueue->begin() && m_up->can_write_START());

      m_peerChunks.set_START_timer(last->first + 1);
      //std::cout << "*" << std::flush;
    }

//STOP
    if (type == Download::CONNECTION_LEECH &&
            !STOPQueue->empty() &&
            m_peerChunks.STOP_timer() <= STOPQueue->front().first &&
            m_up->can_write_STOP()) {

          DownloadMain::START_queue_type::iterator last = std::find_if(STOPQueue->begin(), STOPQueue->end(),
                                                                      rak::greater(m_peerChunks.STOP_timer(), rak::mem_ref(&DownloadMain::START_queue_type::value_type::first)));

        	//here the START queue should not contain any pieces we have sent HAVE of, thus I can safely transmit...
            std::cout << "STOP queue size is " << STOPQueue->size() << std::endl;

          do {
            m_up->write_STOP((--last)->second);
            //m_download->STOPed[(last)->second] = true;
            std::cout << rak::timer::current_usec() << " : sending STOP " << (last)->second << std::endl << std::flush ;
          } while (last != STOPQueue->begin() && m_up->can_write_STOP());

          m_peerChunks.set_STOP_timer(last->first + 1);
          //std::cout << "*" << std::flush;
        }


  if (type == Download::CONNECTION_INITIAL_SEED && m_up->can_write_have())
    offer_chunk();

//HERE was CANCEL sending, but had to move it forward, due to conflicts with queuing the piece, then cancelling it, then queuing again
//while the old cancel messages won't get send, but get sent then later, cancelling the second piece selection
/*
  std::vector<BlockList*>::iterator pos;

  while (type == Download::CONNECTION_LEECH && !m_peerChunks.cancel_queue()->empty() && m_up->can_write_cancel()) {
	  std::cout << "sending CANCEL " << m_peerChunks.cancel_queue()->front().index() << ", offset " << m_peerChunks.cancel_queue()->front().offset() << std::endl << std::flush;

	//only send CANCEL if it's not in the transfers list again...
	    pos =  m_download->delegator()->transfer_list()->find(m_peerChunks.cancel_queue()->front().index());


	    //ALWAYS SEND, there might be cancels in e.g. aggresive mode when you DO want to cancel, even though it is in transfer list...
    if ( /*pos == m_download->delegator()->transfer_list()->end() *//* true ) {
			m_up->write_cancel(m_peerChunks.cancel_queue()->front()); }

    else {
    	std::cout << "NOT SENDING CANCEL " << m_peerChunks.cancel_queue()->front().index() << " since it is in the transfer list..." << std::endl << std::flush;

    }

    m_peerChunks.cancel_queue()->pop_front();
  }
*/


  if (m_sendPEXMask && m_up->can_write_extension() &&
      send_pex_message()) {
    // Don't do anything else if send_pex_message() succeeded.

  } else if (!m_upChoke.choked() &&
             !m_peerChunks.upload_queue()->empty() &&
             m_up->can_write_piece() &&
             (type != Download::CONNECTION_INITIAL_SEED || should_upload())) {
    write_prepare_piece();
  }

  if (is_encrypted())
    m_encryption.encrypt(old_end, m_up->buffer()->end() - old_end);
}

template<Download::ConnectionType type>
void
PeerConnection<type>::event_write() {
  try {
  
    do {

      switch (m_up->get_state()) {
      case ProtocolWrite::IDLE:

        fill_write_buffer();

        if (m_up->buffer()->remaining() == 0) {
          manager->poll()->remove_write(this);
          return;
        }

        m_up->set_state(ProtocolWrite::MSG);

      case ProtocolWrite::MSG:
        if (!m_up->buffer()->consume(m_up->throttle()->node_used_unthrottled(write_stream_throws(m_up->buffer()->position(), m_up->buffer()->remaining()))))
          return;

        m_up->buffer()->reset();

        if (m_up->last_command() == ProtocolBase::PIECE) {
          // We're uploading a piece.
          load_up_chunk();
          m_up->set_state(ProtocolWrite::WRITE_PIECE);

          // fall through to WRITE_PIECE case below

        } else if (m_up->last_command() == ProtocolBase::EXTENSION_PROTOCOL) {
          m_up->set_state(ProtocolWrite::WRITE_EXTENSION);
          break;

        } else {
          // Break or loop? Might do an ifelse based on size of the
          // write buffer. Also the write buffer is relatively large.
          m_up->set_state(ProtocolWrite::IDLE);
          break;
        }

      case ProtocolWrite::WRITE_PIECE:
        if (!up_chunk())
          return;

        m_up->set_state(ProtocolWrite::IDLE);
        break;

      case ProtocolWrite::WRITE_EXTENSION:
        if (!up_extension())
          return;

        m_up->set_state(ProtocolWrite::IDLE);
        break;

      default:
        throw internal_error("PeerConnection::event_write() wrong state.");
      }

    } while (true);

  } catch (close_connection& e) {
    m_download->connection_list()->erase(this, 0);

  } catch (blocked_connection& e) {
    m_download->info()->signal_network_log().emit("Momentarily blocked write connection.");
    m_download->connection_list()->erase(this, 0);

  } catch (network_error& e) {
    m_download->info()->signal_network_log().emit(e.what());
    m_download->connection_list()->erase(this, 0);

  } catch (storage_error& e) {
    m_download->info()->signal_storage_error().emit(e.what());
    m_download->connection_list()->erase(this, 0);

  } catch (base_error& e) {
    std::stringstream s;
    s << "Connection write fd(" << get_fd().get_fd() << ',' << m_up->get_state() << ',' << m_up->last_command() << ") \"" << e.what() << '"';

    throw internal_error(s.str());
  }
}


template<Download::ConnectionType type>
void
PeerConnection<type>::read_STOP_chunk(uint32_t index) {
	m_download->START_statistics()->received_have_chunk(NULL,index,0);	//everything except index gets ignored so far
	std::cout << rak::timer::current_usec() << " : accounted STOP " << index << std::endl << std::flush;
}

template<Download::ConnectionType type>
void
PeerConnection<type>::read_START_chunk(uint32_t index) {

//THIS IS ENOUGH TO BREAK IT WITH 16K CHUNKS AND FAST SPEEDS (>20MB/s)
//	usleep(100);

//	goto ignore;

if ( type != Download::CONNECTION_LEECH ) { //If i'm not downloading, then ignore, since I cannot cause any conflicts...
	//std::cout << "NOT leech, ignoring START " << index << std::endl << std::flush;
	return;
}

if (!m_download->delegator()->get_aggressive()) {		//also don't mind any STARTs when in aggresive mode

	std::cout << rak::timer::current_usec() << " : received START " << index << " -> " ;

	if (index >= m_peerChunks.bitfield()->size_bits())
	    throw communication_error("Peer sent START message with out-of-range index.");

	if (m_peerChunks.bitfield()->get(index)){		//if the peer has sent us START for piece that he already has set in his bitfield, do nothing
		std::cout << "***received START for piece that is already in this peers bitfield!!!" << std::endl << std::flush ;
	    return;
	}

	//do nothing if you already have that chunk
	if (m_download->file_list()->bitfield()->get(index)) {
		std::cout << "...have that piece already" << std::endl << std::flush;
		return;
	}

	//here I can distinguish who did I receive the START from and act accordingly
	m_download->START_statistics()->received_START_chunk(&m_peerChunks, index, m_download->file_list()->chunk_size());		//actually, I don't need to know who it was from or how big the chunk is


	if (m_download->delegator()->ChunkSelect->STARTed[index] == NULL) {		//we haven't started to download this piece yet ==> there can be no conflict
		std::cout << "Chunk " << index << " not locally started" << std::endl << std::flush;
		return;
	}

	if (m_download->delegator()->ChunkSelect->STsolve[index] == true) {		//we have decided to keep downloading this piece
		std::cout << "Not solving conflicts for " << index << std::endl << std::flush;
		return;
	}

	//let's check for conflicts:
	bool hard = false;
	std::vector<Block>::iterator blk;
	BlockList* pos;

	pos = m_download->delegator()->ChunkSelect->STARTed[index];

	if (pos->finished() != 0) { std::cout << " some blocks of this chunk are already finished" ; hard = true; }

	blk = pos->begin();

	while (blk != pos->end() && !hard ) {
		if ( blk->leader() != NULL ) { hard = true; break;}
		blk++;
	}

	//if no HARD conflicts then it must be soft, since it is in the transfer list, thus must be queued somewhere
	if (hard) { std::cout << "==>HARD conflict " ; } else { std::cout << "==>SOFT conflict " ; }

//hard=false;
	if (!hard) {
		std::cout << ", solving soft: " << std::endl;

		//remove from the transfer list
		m_download->chunk_selector()->not_using_index( index );		//if you don't do this, the canceled pieces will never get queued again!!!
		if (m_download->delegator()->transfer_list()->find(index) != m_download->delegator()->transfer_list()->end() ) {
				m_download->delegator()->transfer_list()->erase(m_download->delegator()->transfer_list()->find(index));}
			else {
			std::cout << "CHUNK " << index << " NOT FOUND IN TRANSFER LIST!" << std::endl << std::flush;
		}

		//TODO: (search from the end - it's faster, there's higher probability that this piece has been recently added), or is it not?
		//remove from the START queue:

	//this is just find_if (can be modified using reverse iterators)
		typedef std::deque<std::pair<rak::timer, uint32_t> > START_queue_type;
		START_queue_type::iterator STpos = m_download->delegator()->START_queue->begin();

	    for ( ; STpos != m_download->delegator()->START_queue->end() ; STpos++ ) if ( STpos->second == index ) break;
	    if  (	STpos == m_download->delegator()->START_queue->end()) {std::cout << "*** !!! DID NOT FIND " << index << " in START list !!!" << std::endl << std::flush;} else {
	    	m_download->delegator()->START_queue->erase(STpos);
	    }


    	m_download->delegator()->ChunkSelect->STARTed[index]=NULL;
    	m_download->delegator()->ChunkSelect->STsolve[index]=false;

    	m_download->START_statistics()->received_have_chunk(NULL,index,0);	//everything except index gets ignored so far

    	//also update START statistics! - if it gets cancelled, you will never receive have;
    	//instead use st. like antiSTART, so that you can also inform others that you cancelled download, so that they have accurate statistics!!!

    	//also erase from peers queued queue - check the queues where they are being processed, not here

		std::cout << index << " : erase done" << std::endl << std::flush;

		//ONLY SEND STOP, if you have sent START already!!!

		if (m_download->STOPed[index]) {

			m_download->STOPed[index] = false;
			m_download->delegator()->STOP_queue->push_front(START_queue_type::value_type(cachedTime , index));
			std::cout << rak::timer::current_usec() << " : queued STOP " << index << std::flush;

		//send STOP right now, don't wait
		for (ConnectionList::iterator itr = m_download->delegator()->m_connectionList->begin(); itr != m_download->delegator()->m_connectionList->end(); ++itr){
											   (*itr)->m_ptr()->Twrite_insert_poll_safe();  }

		}
		//should call event write here??? or in the end of start processing? so that it can also select new pieces according to new statistics? every time a start arrives?
		//or rather not, wait, accumulate more starts, so that you have more accurate info when selecting new piece (when the time for the selection comes)

		//this is to immediately send out CANCELs, STOPs and select new pieces

		Twrite_insert_poll_safe();

		} else {
			std::cout << ", NOT solving soft." << std::endl;
	}//if !hard


} else {std::cout << "Aggressive mode, ignoring START" << std::endl; }

	std::cout << rak::timer::current_usec() << " : START " << index << " processing done." << std::endl << std::flush;


//ignore:		std::cout << "ignoring start" << std::endl;

}


template<Download::ConnectionType type>
void
PeerConnection<type>::read_have_chunk(uint32_t index) {
	std::cout << rak::timer::current_usec() << " : received HAVE " << index << " -> " ;
	//std::cout << "#" << std::flush;
  if (index >= m_peerChunks.bitfield()->size_bits())
    throw communication_error("Peer sent HAVE message with out-of-range index.");

  if (m_peerChunks.bitfield()->get(index))		//if the peer has sent us HAVE for piece that he already has set in his bitfield, do nothing
  { std::cout << "bitfield set already " << std::endl << std::flush ;
    return;}

  m_download->chunk_statistics()->received_have_chunk(&m_peerChunks, index, m_download->file_list()->chunk_size());		//updates bitfield, peer rate and other "statistics"
  m_download->START_statistics()->received_have_chunk(&m_peerChunks, index, m_download->file_list()->chunk_size());		//so that I know that this piece has been finished

  if (type == Download::CONNECTION_INITIAL_SEED)
    m_download->initial_seeding()->chunk_seen(index, this);

  // Disconnect seeds when we are seeding (but not for initial seeding
  // so that we keep accurate chunk statistics until that is done).
  if (m_peerChunks.bitfield()->is_all_set()) {
    if (type == Download::CONNECTION_SEED || 
        (type != Download::CONNECTION_INITIAL_SEED && m_download->file_list()->is_done()))
      throw close_connection();

    m_download->upload_choke_manager()->set_not_queued(this, &m_upChoke);		//upload choke??
  }

  if (type != Download::CONNECTION_LEECH || m_download->file_list()->is_done()){
	  std::cout << "not LEECH or file_list->is_done" << std::endl << std::flush ;
    return;}

  if (is_down_interested()) {
	  std::cout << " is down interested, " << m_tryRequest << ", ";

    if (!m_tryRequest && m_download->chunk_selector()->received_have_chunk(&m_peerChunks, index)) {
      m_tryRequest = true;
      std::cout << rak::timer::current_usec() ;
      //std::cout << std::endl << "Ri" ;
      Twrite_insert_poll_safe();
    }

  } else {
	  std::cout << " is NOT down interested, ";
    if (m_download->chunk_selector()->received_have_chunk(&m_peerChunks, index)) {
      m_sendInterested = !m_downInterested;		//thus m_sendInterested is set to true
      m_downInterested = true;
      
      // Ensure we get inserted into the choke manager queue in case
      // the peer keeps us unchoked even though we've said we're not
      // interested.
      if (m_downUnchoked)
        m_download->download_choke_manager()->set_queued(this, &m_downChoke);		//download unchoke??

      // Is it enough to insert into write here? Make the interested
      // check branch to include insert_write, even when not sending
      // interested.
      m_tryRequest = true;
      std::cout << rak::timer::current_usec() ;
      //std::cout << std::endl << "Rn" ;
      Twrite_insert_poll_safe();
    }
  }
  std::cout << rak::timer::current_usec() << " : HAVE " << index << " processing done." << std::endl << std::flush;
}

template<>
void
PeerConnection<Download::CONNECTION_INITIAL_SEED>::offer_chunk() {
  // If bytes left to send in this chunk minus bytes about to be sent is zero,
  // assume the peer will have got the chunk completely. In that case we may
  // get another one to offer if not enough other peers are interested even
  // if the peer would otherwise still be blocked.
  uint32_t bytesLeft = m_data.bytesLeft;
  if (!m_peerChunks.upload_queue()->empty() && m_peerChunks.upload_queue()->front().index() == m_data.lastIndex)
    bytesLeft -= m_peerChunks.upload_queue()->front().length();

  uint32_t index = m_download->initial_seeding()->chunk_offer(this, bytesLeft == 0 ? m_data.lastIndex : InitialSeeding::no_offer);

  if (index == InitialSeeding::no_offer || index == m_data.lastIndex)
    return;

  m_up->write_have(index);
  m_data.lastIndex = index;
  m_data.bytesLeft = m_download->file_list()->chunk_index_size(index);
}

template<>
bool
PeerConnection<Download::CONNECTION_INITIAL_SEED>::should_upload() {
  // For initial seeding, check if chunk is well seeded now, and if so
  // remove it from the queue to better use our bandwidth on rare chunks.
  while (!m_peerChunks.upload_queue()->empty() &&
         !m_download->initial_seeding()->should_upload(m_peerChunks.upload_queue()->front().index()))
    m_peerChunks.upload_queue()->pop_front();

  // If queue ends up empty, choke peer to let it know that it
  // shouldn't wait for the cancelled pieces to be sent.
  if (m_peerChunks.upload_queue()->empty()) {
    m_download->upload_choke_manager()->set_not_queued(this, &m_upChoke);
    m_download->upload_choke_manager()->set_queued(this, &m_upChoke);

  // If we're sending the chunk we last offered, adjust bytes left in it.
  } else if (m_peerChunks.upload_queue()->front().index() == m_data.lastIndex) {
    m_data.bytesLeft -= m_peerChunks.upload_queue()->front().length();

    if (!m_data.bytesLeft)
      m_data.lastIndex = InitialSeeding::no_offer;
  }

  return !m_peerChunks.upload_queue()->empty();
}

// Explicit instantiation of the member functions and vtable.
template class PeerConnection<Download::CONNECTION_LEECH>;
template class PeerConnection<Download::CONNECTION_SEED>;
template class PeerConnection<Download::CONNECTION_INITIAL_SEED>;

}


/*
	if (!m_download->delegator()->transfer_list()->empty()) {
		pos = m_download->delegator()->transfer_list()->find(index);	//TIME EXPENSIVE, if many pieces!; only find search for it if you need to remove

		//TODO: Search from end, there's higher probability - really?; However, this is still TIME EXPENSIVE!
		//TODO: use "bitfield" just like with STARTed

			//check if any blocks are being transferred on any peers, if so = HARD conflict, if not = soft conflict & you can remove the piece from queued lists...
			//being transferred = if download_queue->transfer()->block()->(is_transferring) or index == index then this one is being currently in transfer
			//otherwise if it is only in download_queue->m_queued (or use queued_transfer[i]) then it's only queued - and you can dequeue it from there then,

			//every transfer that is in transfer list should be considered a soft conflict; if it is already being downloaded, then HARD
			//if HARD, then do nothing, just note statistics; maybe let's do something later
			//if soft, then cancel the download: remove it from the transfer list && all PCB queues && send out cancel msgs.
			//m_parent->finished tells how many block are finished, however what if first block is not yet finished, but already being transferred?
				//which should be if it is leader for any of the PCBs

			//by comparing size() and finished() of blocklist (representing a download of a chunk) I can maybe decide if I want to drop the download in favor of solving HARD conflicts?
			//e.g. if I have downloaded only 10% so far, then drop it?



		if ( pos != m_download->delegator()->transfer_list()->end() ) {

			if ((*pos)->finished() != 0) { std::cout << " some blocks of this chunk are already finished" ; hard = true; }
			blk = (*pos)->begin();

			//check out all the block transfers for that piece, if any of them is being transferred already
			while (blk != (*pos)->end() && !hard ) {
				if ( blk->leader() != NULL ) { hard = true; break;}
				blk++;
			}

			if (!hard){ soft = true; }		//if no HARD conflicts then it must be soft, since it is in the transfer list, thus must be queued somewhere
			if (soft) { std::cout << "==>SOFT conflict " ; }
			if (hard) { std::cout << "==>HARD conflict " ; }

		} else {std::cout << index << " , not in transfer list" ;} 	//if index found on transfer list

	} else {	std::cout << "transfer list empty" ; } 				//if transfer list empty
*/



/*		ALL THIS WAS INSIDE read_START_chunk:
		 // if (!m_download->delegator()->transfer_list()->empty()) {
		 //		if ( pos != m_download->delegator()->transfer_list()->end() ) {
		 // 			while (blk != (*pos)->end() && !hard ) {
		 //						(THIS BLOCK)

		//						(some more code that is still there)
		// }  } } etc.

				//IT's ALWAYS OK

				//if (blk->leader() == blk->leader()->peer_info()->connection()->download_queue()->transfer()) {std::cout << "it's OK";} else { std::cout << "it's not OK" ;}
				//blk->leader()->peer_info()->connection()->download_queue()->transfer();
				//if any of them is already being in transfer, then it's a HARD conflict, otherwise soft


			//if ( blk->is_transfering() ) {std::cout << "HARD conflict, a block from this chunk is already in transfer " ; soft=false; hard=true;break; }
			//if ( blk->leader() != NULL ) {std::cout << "HARD conflict, leader is non NULL, and is finished? " << blk->leader()->is_finished() ; soft=false; hard=true;break; } //is always finished
					//the last condition happens most of the time, because you are able to receive START msg. only after normal piece is done, thus, only after transfer is done

			else { //if it is in queued or transfer lists of any of the peers, then it's soft conflict

				//THIS NEVER HAPPENS, thus no need to check
						const transfer_list_type* Btransfer = blk->transfers();
						const_transfer_list_type::const_iterator TRlist_itr = (*Btransfer).begin();

						if (TRlist_itr==(*Btransfer).end()) {std::cout << "transfers: list empty" << std::endl;}

						while (TRlist_itr != (*Btransfer).end() && !hard ) {
							if ((*TRlist_itr)->is_leader())  { std::cout << "***this block is transfer LEADER but is not in transfer => HARD conflict " << std:: endl; soft = false ; hard=true; } //so far NEVER EVER HAPPENED, can it at all?
							if ((*TRlist_itr)->is_erased())  { std::cout << "transfers: this block is ERASED " << std:: endl;}		//so far NEVER EVER HAPPENED, can it at all?
							if ((*TRlist_itr)->is_finished()){ std::cout << "transfers: this block is FINISHED => HARD conflict " << std:: endl; soft = false ; hard=true; }		//so far NEVER EVER HAPPENED, can it at all?
							if ((*TRlist_itr)->is_queued() ) { std::cout << "transfers: this block is QUEUED => soft conflict " << std:: endl; soft = true; } //so far NEVER EVER HAPPENED, can it at all?
							if ((*TRlist_itr)->is_not_leader() ) { std::cout << "transfers: this block is NOT_LEADER => ??? " << std:: endl;  soft = true;  }
							if (!(*TRlist_itr)->is_valid() ) { std::cout << "transfers: this block is NOT VALID! => ??? " << std:: endl;  soft = true;  }
							std::cout << "transfers: state of the block " << (*TRlist_itr)->index() << "is: " << (*TRlist_itr)->state()  << std::endl;

								bool found = false;
								for (uint32_t i = 0; i<(*TRlist_itr)->peer_info()->connection()->download_queue()->queued_size(); i++){
									if ( (*TRlist_itr) == (*TRlist_itr)->peer_info()->connection()->download_queue()->queued_transfer(i)) {found = true;}
								}

								if (found) {std::cout << "transfers: it's OK";}
									 else { std::cout << "transfers: it's not OK" ;}

							TRlist_itr++;
						}

//should I check this at all? - if it's not hard, then it has to be soft, since it is in the transfer list

				//THIS ALWAYS HAPPENS, thus no need to check

						Btransfer = blk->queued();
						TRlist_itr = (*Btransfer).begin();

						if (TRlist_itr==(*Btransfer).end()) {std::cout << "queued: list empty" << std::endl;}

						while (TRlist_itr != (*Btransfer).end() && !hard ) {
							if ((*TRlist_itr)->is_leader())  { std::cout << "***this block is queued LEADER => HARD conflict " << std:: endl; ; soft = false ; hard=true; } //so far NEVER EVER HAPPENED, can it at all?
							if ((*TRlist_itr)->is_erased())  { std::cout << "queued: this block is ERASED " << std:: endl;;}		//so far NEVER EVER HAPPENED, can it at all?
							if ((*TRlist_itr)->is_finished()){ std::cout << "queued: this block is FINISHED => HARD conflict " << std:: endl; soft = false ; hard=true; }		//so far NEVER EVER HAPPENED, can it at all?
							if ((*TRlist_itr)->is_not_leader())  { std::cout << "queued: this block is NOT_LEADER => ??? " << std:: endl;;}
							if (!(*TRlist_itr)->is_valid())  { std::cout << "queued: this block is NOT VALID => ??? " << std:: endl;;}

							if ((*TRlist_itr)->is_queued() ) { std::cout << "queued: this block is QUEUED => soft conflict " << std:: endl; soft = true; }  //break, because there's no further chance for hard here

							std::cout << "queued: state of the block " << (*TRlist_itr)->index() << " is: " << (*TRlist_itr)->state()  << std::endl;

							bool found = false;
							for (uint32_t i = 0; i<(*TRlist_itr)->peer_info()->connection()->download_queue()->queued_size(); i++){
								if ( (*TRlist_itr) == (*TRlist_itr)->peer_info()->connection()->download_queue()->queued_transfer(i)) {found = true;}
							}

							if (found) {std::cout << "queued: it's OK";}
								 else { std::cout << "queued: it's not OK" ;}

							TRlist_itr++;
						}
						if (!soft) {std::cout << "finished queued check and found no soft conflict" ;}
			}

*/


/*		THIS WAS INSIDE read_START_chunk:
 * 			//	if (soft && ((random() & 15) == 0 )) {
 * 			//		while (blk != (*pos)->end() ){
 * 							(THIS BLOCK)
 *
 * 							(some more code)
 * 				} } etc.
 *

			//even though transfer list should be empty in this case, but let's try
			Btransfer = blk->transfers();
			TRlist_itr = (*Btransfer).begin();

			if (TRlist_itr==(*Btransfer).end()) {std::cout << "transfers: list empty" << std::endl;}
			while (TRlist_itr != (*Btransfer).end() ) {
					//remove it from peers queues, send CANCEL messages
				itr = (*TRlist_itr)->peer_info()->connection()->download_queue()->m_queued.begin();
				end = (*TRlist_itr)->peer_info()->connection()->download_queue()->m_queued.end();

				while (itr != end)
				//for (uint32_t i = 0; i<(*TRlist_itr)->peer_info()->connection()->download_queue()->queued_size(); i++)
				{	//checking the peer's queue for the block, so that I can process it

					if ( (*TRlist_itr) == (*itr)) {	//found it
						//(*TRlist_itr)->peer_info()->connection()->cancel_transfer( (*TRlist_itr) );		//this puts it into CANCEL msg. queue and actually I can do this outside the while loop
						std::cout << "t: would cancel_transfer"<< std::endl;
						//SHOULD i remove it from download_queue at all? if not, then I don't need this for loop
						//(*TRlist_itr)->peer_info()->connection()->download_queue()->m_queued.erase(itr);
						std::cout << "t: would m_queued.erase"<< std::endl;
						if (itr == end ) {break;}
						//(*TRlist_itr)->peer_info()->connection()->download_queue()->m_canceled.push_back(*TRlist_itr);
						std::cout << "t: would m_canceled.push_back"<< std::endl;
					}
					itr++;
				}
				//blk->release(*TRlist_itr);	//this removes it from the transfers  and queued and also deletes the object
				std::cout << "t: would blk->release"<< std::endl;
				TRlist_itr++;
			}
*/



/*  NO NEED TO TAKE CARE OF THIS, destructors will do it for us
 *
 *
	  blk->queued()->erase(blk->queued()->begin(), blk->queued()->end());
	  blk->queued()->clear();	//might not be necessary, since erase should already remove all the items

 */

/*
 *
 * int BLpos=0;
	//check out all the blocktransfers of this block
while (TRlist_itr != (*Btransfer).end() ) {
		//remove it from peers queues, send CANCEL messages

	std::cout << "peer's download_queue size is " << (*TRlist_itr)->peer_info()->connection()->download_queue()->m_queued.size() << std::endl <<std::flush;
	itr = (*TRlist_itr)->peer_info()->connection()->download_queue()->m_queued.begin();
	end = (*TRlist_itr)->peer_info()->connection()->download_queue()->m_queued.end();
	int found=0;
	bool erased = false;
	int PQpos=0;

		//for (uint32_t i = 0; i<(*TRlist_itr)->peer_info()->connection()->download_queue()->queued_size(); i++)

		//check all the peer's queued pieces to find the blocktransfer we want to stop
	while (itr != end){

		if ( 	((*TRlist_itr)->state() == (*itr)->state()) &&
				((*TRlist_itr)->position() == (*itr)->position()) &&
				((*TRlist_itr)->piece() == (*itr)->piece())
			) 	{	//found it

			//CAN I DO == on BlockTransfer??? maybe I should define operator?

			found++;

			std::cout <<  "Found this BlockTransfer in the peers queued queue on position " << PQpos << ", index " << (*itr)->piece().index() << ", offset " << (*itr)->piece().offset() << std::endl << std::flush;

			(*TRlist_itr)->peer_info()->connection()->cancel_transfer( (*TRlist_itr) );		//this puts it into CANCEL msg. queue and actually I can do this outside the while loop


			//or maybe just blk->invalidate_transfer(*TRlist_itr); ?


			std::cout << "q!: cancel_transfer"<< std::endl;
				//I SHOULD, other things get computed based upon queue size etc.
			//SHOULD i remove it from download_queue at all? if not, then I don't need this for loop
			(*TRlist_itr)->peer_info()->connection()->download_queue()->m_queued.erase(itr);erased = true;break;

			std::cout << "q!: m_queued.erase"<< std::endl;
			//actually, there's no need to push it back to the peers cancelled queue
			//(*TRlist_itr)->peer_info()->connection()->download_queue()->m_canceled.push_back(*TRlist_itr);
			//std::cout << "q: m_canceled.push_back"<< std::endl;
			if (itr == end ) {break;}
		}
		if (!erased) {itr++;} else {break;}
		PQpos++;
	}

	std::cout << std::flush;

	if (found >1) {std::cout << "q!: found it " << found << " times" << std::endl;}

	//tmp = TRlist_itr;

	if (blk->size_all() != 1 ) {std::cout << "size of all block's queues combined is " << blk->size_all() << std::endl << std::flush;}

	//On first call it works, the second call on even different block it fails to find the transfer in that block's queue; as if it was trying to call it on the same first block
	//or is it that just a problem with const iterator? or const list_Type* ?
	//BLpos is ALWAYS 0, thus there's ALWAYS ONLY ONE BlockTransfer per Block.... (so why need blockTransfer queue then???)
	std::cout << "<"<< BLpos <<">the BlockTransfer to release is "<< blk->index() << ", offset " << blk->piece().offset() << std::endl << std::flush;
	//blk->erase(*TRlist_itr);	//this removes it from the transfers  and queued and also deletes the object

	//if (tmp!=TRlist_itr) {std::cout << " TRlist_itr modified! " << std::endl;}
	//TRlist_itr = tmp;
	std::cout << "q: did blk->erase"<< std::endl << std::flush;
	if ( TRlist_itr != (*Btransfer).end() ) {TRlist_itr++;std::cout << "TRlist_itr++" << std::endl;} else {break; }
	BLpos++;
}
*/

/*
 *
 * 	NOT EVEN NEED TO DO THIS, since the destructors work well and WILL send out even cancel messages too
 *

 		typedef std::vector<BlockTransfer*> transfer_list_type;
		transfer_list_type* Btransfer;
		transfer_list_type::iterator TRlist_itr;
		transfer_list_type::iterator tmp;
		std::deque<BlockTransfer*>::iterator itr, end;

		blk = (*pos)->begin();		//start with first block on the blocklist


        int BLKpos=0;

		while (blk != (*pos)->end() ){
			//for this block, call release on all it's BlockTransfer* s from queued, (it's never in transfers)

			Btransfer = blk->queued();
			TRlist_itr = (*Btransfer).begin();

			//optimization - if the block is empty, break (seems like after one empty block, all the rest is "unallocated" too)
			if (TRlist_itr==(*Btransfer).end()) {std::cout << "queued: list empty" << std::endl; break;}

			if ( blk->queued()->begin() != blk->queued()->end()) {
			 	  //std::for_each(blk->queued()->begin(), blk->queued()->end(), std::bind1st(std::mem_fun(&PeerConnectionBase::cancel_transfer), this) );
			 	  //std::for_each(blk->queued()->begin(), blk->queued()->end(), std::bind1st(std::mem_fun(&Block::remove_from_peer), this) );  	//also erase from the peer's queue
			 	  //std::for_each(blk->queued()->begin(), blk->queued()->end(), std::bind1st(std::mem_fun(&Block::destroy), this) );
			} else {std::cout << "blk empty queue!" << std::endl << std::flush;}

			std::cout << "[" << BLKpos <<"] cancelled block "<< blk->index() << ", offset " << blk->piece().offset() << std::endl << std::flush;
			blk++;BLKpos++;
		}
*/

/*
 *
 * 	REMOVE THE PIECES FROM THE PEER'S QUEUED queues

		typedef std::vector<BlockTransfer*> transfer_list_type;

		transfer_list_type* Btransfer = blk->transfers();
		transfer_list_type::iterator TRlist_itr = (*Btransfer).begin();

		if (TRlist_itr==(*Btransfer).end()) {std::cout << "transfers: list empty" << std::endl;}

		while (TRlist_itr != (*Btransfer).end() ) {

				std::deque<BlockTransfer*>::iterator itr = (*TRlist_itr)->peer_info()->connection()->download_queue()->m_queued.begin();
				std::deque<BlockTransfer*>::iterator end = (*TRlist_itr)->peer_info()->connection()->download_queue()->m_queued.end();

		    	while (itr != end)
	    			{	//checking the peer's queue for the block, so that I can process it
		    		if ( (*TRlist_itr) == (*itr)) {	//found it
		   					(*TRlist_itr)->peer_info()->connection()->download_queue()->m_queued.erase(itr);
 							std::cout << "t: would m_queued.erase"<< std::endl;
 							break;
		    		}
		    		itr++;
	    		}
				TRlist_itr++;
		}
*/
