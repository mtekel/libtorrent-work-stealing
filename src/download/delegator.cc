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

// Fucked up ugly piece of hack, this code.

#include "config.h"

#include <algorithm>
#include <inttypes.h>
#include <iostream>

#include "torrent/exceptions.h"
#include "torrent/bitfield.h"
#include "torrent/data/block.h"
#include "torrent/data/block_list.h"
#include "torrent/data/block_transfer.h"
#include "protocol/peer_chunks.h"

#include "delegator.h"
#include <iostream>

namespace torrent {

struct DelegatorCheckAffinity {
  DelegatorCheckAffinity(Delegator* delegator, Block** target, unsigned int index, const PeerInfo* peerInfo) :
    m_delegator(delegator), m_target(target), m_index(index), m_peerInfo(peerInfo) {}

  bool operator () (BlockList* d) {
    return m_index == d->index() && (*m_target = m_delegator->delegate_piece(d, m_peerInfo)) != NULL;
  }

  Delegator*          m_delegator;
  Block**             m_target;
  unsigned int        m_index;
  const PeerInfo*     m_peerInfo;
};

struct DelegatorCheckSeeder {
  DelegatorCheckSeeder(Delegator* delegator, Block** target, const PeerInfo* peerInfo) :
    m_delegator(delegator), m_target(target), m_peerInfo(peerInfo) {}

  bool operator () (BlockList* d) {
    return d->by_seeder() && (*m_target = m_delegator->delegate_piece(d, m_peerInfo)) != NULL;
  }

  Delegator*          m_delegator;
  Block**             m_target;
  const PeerInfo*     m_peerInfo;
};

struct DelegatorCheckPriority {
  DelegatorCheckPriority(Delegator* delegator, Block** target, priority_t p, const PeerChunks* peerChunks) :
    m_delegator(delegator), m_target(target), m_priority(p), m_peerChunks(peerChunks) {}

  bool operator () (BlockList* d) {
    return
      m_priority == d->priority() &&
      m_peerChunks->bitfield()->get(d->index()) &&
      (*m_target = m_delegator->delegate_piece(d, m_peerChunks->peer_info())) != NULL;
  }

  Delegator*          m_delegator;
  Block**             m_target;
  priority_t          m_priority;
  const PeerChunks*   m_peerChunks;
};

// TODO: Should this ensure we don't download pieces that are priority off?
struct DelegatorCheckAggressive {
  DelegatorCheckAggressive(Delegator* delegator, Block** target, uint16_t* o, const PeerChunks* peerChunks) :
    m_delegator(delegator), m_target(target), m_overlapp(o), m_peerChunks(peerChunks) {}

  bool operator () (BlockList* d) {
    Block* tmp;

    if (!m_peerChunks->bitfield()->get(d->index()) ||
        d->priority() == PRIORITY_OFF ||
        (tmp = m_delegator->delegate_aggressive(d, m_overlapp, m_peerChunks->peer_info())) == NULL)
      return false;

    *m_target = tmp;
    return m_overlapp == 0;
  }

  Delegator*          m_delegator;
  Block**             m_target;
  uint16_t*           m_overlapp;
  const PeerChunks*   m_peerChunks;
};

BlockTransfer*
Delegator::delegate(PeerChunks* peerChunks, int affinity) {
  // TODO: Make sure we don't queue the same piece several time on the same peer when
  // it timeout cancels them.
  Block* target = NULL;

  // Find piece with same index as affinity. This affinity should ensure that we
  // never start another piece while the chunk this peer used to download is still
  // in progress.
  //
  // TODO: What if the hash failed? Don't want data from that peer again.

  //order/preference of choosing a block to download:

  //1. another block(piece) from the chunk that is already in transfer
  //TODO: use STARTed instead of find_if on transfers
  if (affinity >= 0 && 
      std::find_if(m_transfers.begin(), m_transfers.end(), DelegatorCheckAffinity(this, &target, affinity, peerChunks->peer_info()))
      != m_transfers.end())
    return target->insert(peerChunks->peer_info());

  //2. a block(piece) from seeder (because it is rare): inside it does: current chunk, new high prio, new normal
  if (peerChunks->is_seeder() && (target = delegate_seeder(peerChunks)) != NULL)
    return target->insert(peerChunks->peer_info());

  //High priority pieces. [another block from high priority pieces currently in transfer list]
  //if it goes through transfers linearly anyway, why not use mod ID priority list?
  if (std::find_if(m_transfers.begin(), m_transfers.end(), DelegatorCheckPriority(this, &target, PRIORITY_HIGH, peerChunks))
      != m_transfers.end())
    return target->insert(peerChunks->peer_info());

  // New high priority piece
  //if the piece can then get downloaded from other peers too, why not pick here mod ID least started?
  if ((target = new_chunk(peerChunks, true)))
    return target->insert(peerChunks->peer_info());

  // Normal priority pieces.
  //if it goes through transfers linearly anyway, why not use mod ID priority list?
  if (std::find_if(m_transfers.begin(), m_transfers.end(), DelegatorCheckPriority(this, &target, PRIORITY_NORMAL, peerChunks))
      != m_transfers.end())
    return target->insert(peerChunks->peer_info());

  // New normal priority piece
  //if the piece can then get downloaded from other peers too, why not pick here mod ID least started?
  if ((target = new_chunk(peerChunks, false)))
    return target->insert(peerChunks->peer_info());

  if (!m_aggressive)
    return NULL;

  // Aggressive mode, look for possible downloads that already have
  // one or more queued.

  // No more than 4 per piece.
  uint16_t overlapped = 5;		//there was 5

  std::find_if(m_transfers.begin(), m_transfers.end(), DelegatorCheckAggressive(this, &target, &overlapped, peerChunks));

  return target ? target->insert(peerChunks->peer_info()) : NULL;
}
  
Block*
Delegator::delegate_seeder(PeerChunks* peerChunks) {
  Block* target = NULL;

  if (std::find_if(m_transfers.begin(), m_transfers.end(), DelegatorCheckSeeder(this, &target, peerChunks->peer_info()))
      != m_transfers.end())
    return target;

  if ((target = new_chunk(peerChunks, true)))
    return target;
  
  if ((target = new_chunk(peerChunks, false)))
    return target;

  return NULL;
}

Block*
Delegator::new_chunk(PeerChunks* pc, bool highPriority) {

//check how big the transfer list is, if too big, don't queue anymore:
//permit only up to 128MB queued (that is 134217728 B ), that should be enough (maybe not for 10gbit links)
//or 2048 entries - that is already quite large, maybe the preference should be given on receiving pieces than queuing them
  if ( ( m_transfers.size() > ( 134217728 / m_slotChunkSize(0) ) ) || ( m_transfers.size() > ( 2047 ) ) ) {
	  std::cout << "TRANSFER LIST SIZE > 2047 || TRANSFER LIST SIZE > 134217728 / ChunkSize = " << 134217728 / m_slotChunkSize(0) <<  " ! : " <<  m_transfers.size() << std::endl << std::flush;
	  return NULL;
  } else {

  uint32_t index = m_slotChunkFind(pc, highPriority);

  if (index == ~(uint32_t)0)
	  return NULL;


  TransferList::iterator itr = m_transfers.insert(Piece(index, 0, m_slotChunkSize(index)), block_size);

  (*itr)->set_by_seeder(pc->is_seeder());

  if (highPriority)
    (*itr)->set_priority(PRIORITY_HIGH);
  else
    (*itr)->set_priority(PRIORITY_NORMAL);

  //send out START to all my peers - if not STARTed already:

  if ( ChunkSelect->STARTed[index] == NULL) { //this is new piece we haven't started to download yet...
	   ChunkSelect->STARTed[index] = (*itr);


	   //DON'T ever drop/solve conflicts for special pieces of interest, that is:
	   //any HIGH priority piece
	   //mod myID least started piece?
	   //not currently started piece?

	   //let's decide if we will solve conflicts for this chunk (e.g. if it is HIGH prio, then not, if it is our other special piece of interrest, then also not)
	   //what for send start, if you expect to drop it later? => don't send start....

	   std::cout << "Chunk " << index << " is currently being STARTed " << (*STstat)[index] << " times and being HAD " << (int)(*CHstat)[index] <<" times." << std::endl;
	   //std::cout << "STsolve for " << index << " is " << ChunkSelect->STsolve[index] << std::endl << std::flush;
	   //std::cout << "priority for " << index << " is " << highPriority << std::endl << std::flush;
	   //std::cout << "(*STstat)[index] == 0 for " << index << " is " << ( (*STstat)[index] == 0 ) << std::endl << std::flush;
	   //ChunkSelect->STsolve[index] = false;		//DEFAULT

	   ChunkSelect->STsolve[index] = ( highPriority || ( (*STstat)[index] == 0 ) ) ? true : ((random() & 4) == 0 ); 	//for now, drop every second, if it has conflict [TRUE = don't drop]

	   //only send START if you're not willing to stop download of the piece
	   //OR if downloading from higher level
	   // or always?

	   if (/*ChunkSelect->STsolve[index] */ true) {
		   START_queue->push_front(START_queue_type::value_type(cachedTime , index));
		   std::cout << rak::timer::current_usec() << " : queued START " << index << std::flush;

		   //send START right now, don't wait
		   for (ConnectionList::iterator itr = m_connectionList->begin(); itr != m_connectionList->end(); ++itr){
														   (*itr)->m_ptr()->Twrite_insert_poll_safe();  }
	   }

  } else { //this should NEVER happen (unless aggressive):

	  if (!m_aggressive) { std::cout << rak::timer::current_usec() << " : *** !!! Already have queued START " << index /*<< " with time " << cachedTime */ << std::endl << std::flush;}

  }


/*

  START_queue_type::iterator idx = START_queue->end();

  if (!START_queue->empty()) {	  idx = std::find_if(START_queue->begin(), START_queue->end(),
										rak::equal(index, rak::mem_ref(&START_queue_type::value_type::second)));  }

  if ( idx == START_queue->end()) {	//this is new piece we haven't started to download yet...
	  START_queue->push_front(START_queue_type::value_type(cachedTime , index));				//rak::timer::current_usec()
	  std::cout << rak::timer::current_usec() << " : queued START " << index  << std::flush;
//send START right now, don't wait
	  for (ConnectionList::iterator itr = m_connectionList->begin(); itr != m_connectionList->end(); ++itr){
								(*itr)->m_ptr()->Twrite_insert_poll_safe();  }

  } else { //this should NEVER happen (unless aggressive):
	  if (!m_aggressive) { std::cout << rak::timer::current_usec() << " : *** !!! Already have queued START " << index << std::endl << std::flush;}
  }
*/


  return &*(*itr)->begin();

  } //if transfer list size <=2000
}

Block*
Delegator::delegate_piece(BlockList* c, const PeerInfo* peerInfo) {
  Block* p = NULL;

  for (BlockList::iterator i = c->begin(); i != c->end(); ++i) {
    if (i->is_finished() || !i->is_stalled())
      continue;

    if (i->size_all() == 0) {
      // No one is downloading this, assign.
      return &*i;

    } else if (p == NULL && i->find(peerInfo) == NULL) {
      // Stalled but we really want to finish this piece. Check 'p' so
      // that we don't end up queuing the pieces in reverse.
      p = &*i;
    }
  }
      
  return p;
}

Block*
Delegator::delegate_aggressive(BlockList* c, uint16_t* overlapped, const PeerInfo* peerInfo) {
  Block* p = NULL;

  for (BlockList::iterator i = c->begin(); i != c->end() && *overlapped != 0; ++i)
    if (!i->is_finished() && i->size_not_stalled() < *overlapped && i->find(peerInfo) == NULL) {
      p = &*i;
      *overlapped = i->size_not_stalled();
    }
      
  return p;
}

} // namespace torrent

/*	int a = 0;

	//i'll try at most 8 random chunks to see if any isn't started yet
	while ( ( (*STstat)[index] != 0) && (a < 7 ) )	{
		index = m_slotChunkFind(pc, highPriority);
		a++;
	}
	if (a==7) std::cout << "Tried 8 times to find unstarted chunk, couldn't " << std::endl << std::flush;
*/
