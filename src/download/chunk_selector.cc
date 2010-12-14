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

#include <algorithm>
#include <stdlib.h>
#include <rak/functional.h>

#include "protocol/peer_chunks.h"
#include "torrent/exceptions.h"

#include "chunk_selector.h"
#include "chunk_statistics.h"

#include <iostream>
#include <vector>

namespace torrent {

// Consider making statistics a part of selector.
void
ChunkSelector::initialize(Bitfield* bf, ChunkStatistics* cs, STARTstatistics* ST) {
  m_position = invalid_chunk;
  m_statistics = cs;
  m_STARTstatistics = ST;

  STARTed.resize( bf->size_bits() );
  STsolve.resize( bf->size_bits() );

  m_bitfield.set_size_bits(bf->size_bits());
  m_bitfield.allocate();
  std::transform(bf->begin(), bf->end(), m_bitfield.begin(), rak::invert<Bitfield::value_type>());
  m_bitfield.update();

  m_sharedQueue.enable(32);
  m_sharedQueue.clear();
}

void
ChunkSelector::cleanup() {
  m_bitfield.clear();
  m_statistics = NULL;
}

// Consider if ChunksSelector::not_using_index(...) needs to be
// modified.
void
ChunkSelector::update_priorities() {
  if (empty())
    return;

  m_sharedQueue.clear();

  if (m_position == invalid_chunk)
    m_position = random() % size();

  advance_position();
}

uint32_t
ChunkSelector::find(PeerChunks* pc, __UNUSED bool highPriority) {
  // This needs to be re-enabled.
  if (m_position == invalid_chunk)
    return invalid_chunk;

  // When we're a seeder, 'm_sharedQueue' is used. Since the peer's
  // bitfield is guaranteed to be filled we can use the same code as
  // for non-seeders. This generalization does incur a slight
  // performance hit as it compares against a bitfield we know has all
  // set.
  rak::partial_queue* queue = pc->is_seeder() ? &m_sharedQueue : pc->download_cache();

  // Randomize position on average every 16 chunks to prevent
  // inefficient distribution with a slow seed and fast peers
  // all arriving at the same position.

//queue is download_cache. Everytime I receive HAVE from that peer, I put it into this cache. However, I don't want that behavior, since then all the peers that received HAVE from that
//peer will pick the same order of pieces to download, thus having duplicate transfers!!!, thus I go random && clear cache every time...

//Because of that, I could basically simplify/change/rewrite this whole chunk search/selection (which I am more or less going to do anyway)

  m_position = random() % size();
  queue->clear();

/*
  if ((random() & 1) == 0) {			//there was 63
    m_position = random() % size();
    queue->clear();						//why clear? no more clearing, only if I receive start for piece that's already in
  }
*/

  if (queue->is_enabled()) {

    // First check the cached queue.
    while (queue->prepare_pop()) {
      uint32_t pos = queue->pop();

      if (!m_bitfield.get(pos))
        continue;

      return pos;
    }

  } else {
    // Only non-seeders can reach this branch as m_sharedQueue is
    // always enabled.
    queue->enable(8);
  }

  queue->clear();  //why clear?

  if (highPriority)
			 (search_linear(pc->bitfield(), queue, &m_highPriority, m_position, size()) &&
			  search_linear(pc->bitfield(), queue, &m_highPriority, 0, m_position));

  if (queue->prepare_pop()) {
    // Set that the peer has high priority pieces cached.

  } else {
    // Set that the peer has normal priority pieces cached.

    // Urgh...
    queue->clear();

    //no high prio. chunk found? well, then that's it
    if (highPriority)
    	return invalid_chunk;

/*
    (search_ns_linear(pc->bitfield(), queue, &m_highPriority, m_position, size()) &&
	 search_ns_linear(pc->bitfield(), queue, &m_highPriority, 0, m_position));
*/
    //if no not STARTed chunk found, try to find some normal priority
    if (!queue->prepare_pop())

			(search_linear(pc->bitfield(), queue, &m_normalPriority, m_position, size()) &&
			 search_linear(pc->bitfield(), queue, &m_normalPriority, 0, m_position));



    if (!queue->prepare_pop())
      return invalid_chunk;
  }

  uint32_t pos = queue->pop();
  
  if (!m_bitfield.get(pos))
    throw internal_error("ChunkSelector::find(...) bad index.");
  
  return pos;
}

bool
ChunkSelector::is_wanted(uint32_t index) const {
  return m_bitfield.get(index) && (m_normalPriority.has(index) || m_highPriority.has(index));
}

void
ChunkSelector::using_index(uint32_t index) {
  if (index >= size())
    throw internal_error("ChunkSelector::select_index(...) index out of range.");

  if (!m_bitfield.get(index))
    throw internal_error("ChunkSelector::select_index(...) index already set.");

  m_bitfield.unset(index);

  // We always know 'm_position' points to a wanted chunk. If it
  // changes, we need to move m_position to the next one.
  if (index == m_position)
    advance_position();

  std::cout << "ChunkSelector::using_index finished ["<< index<<"]" << std::endl << std::flush;
}

void
ChunkSelector::not_using_index(uint32_t index) {
  if (index >= size())
    throw internal_error("ChunkSelector::deselect_index(...) index out of range.");

  if (m_bitfield.get(index))
    throw internal_error("ChunkSelector::deselect_index(...) index already unset.");

  //std::cout << "ChunkSelector::not_using_index about to set bitfield for ["<< index<<"]" << std::endl << std::flush;

  m_bitfield.set(index);

  // This will make sure that if we enable new chunks, it will start
  // downloading them event when 'index == invalid_chunk'.
  if (m_position == invalid_chunk)
    m_position = index;

  std::cout << "ChunkSelector::not_using_index finished ["<< index<<"]" << std::endl << std::flush;
}

// This could propably be split into two functions, one for checking
// if it shoul insert into the download_queue(), and the other
// whetever we are interested in the new piece.
//
// Since this gets called whenever a new piece arrives, we can skip
// the rarity-first/linear etc searches through bitfields and just
// start downloading.
bool
ChunkSelector::received_have_chunk(PeerChunks* pc, uint32_t index) {
  if (!m_bitfield.get(index))
    return false;

  // Also check if the peer only has high-priority chunks.
  if (!m_highPriority.has(index) && !m_normalPriority.has(index))
    return false;

  if (pc->download_cache()->is_enabled())
    pc->download_cache()->insert(m_statistics->rarity(index), index);

  return true;
}

bool
ChunkSelector::search_linear(const Bitfield* bf, rak::partial_queue* pq, priority_ranges* ranges, uint32_t first, uint32_t last) {
  priority_ranges::iterator itr = ranges->find(first);

  while (itr != ranges->end() && itr->first < last) {

    if (!search_linear_range(bf, pq, std::max(first, itr->first), std::min(last, itr->second)))
      return false;

    ++itr;    
  }

  return true;
}

bool
ChunkSelector::search_ns_linear(const Bitfield* bf, rak::partial_queue* pq, priority_ranges* ranges, uint32_t first, uint32_t last) {
  priority_ranges::iterator itr = ranges->find(first);

  while (itr != ranges->end() && itr->first < last) {

    if (!search_ns_linear_range(bf, pq, std::max(first, itr->first), std::min(last, itr->second)))
      return false;

    ++itr;
  }

  return true;
}
// Could probably add another argument for max seen or something, this
// would be used to find better chunks to request.
inline bool
ChunkSelector::search_linear_range(const Bitfield* bf, rak::partial_queue* pq, uint32_t first, uint32_t last) {
  if (first >= last || last > size())
    throw internal_error("ChunkSelector::search_linear_range(...) received an invalid range.");

  Bitfield::const_iterator local  = m_bitfield.begin() + first / 8;
  Bitfield::const_iterator source = bf->begin() + first / 8;

  // Unset any bits before 'first'.
  Bitfield::value_type wanted = (*source & *local) & Bitfield::mask_from(first % 8);

  while (m_bitfield.position(local + 1) < last) {
    if (wanted && !search_linear_byte(pq, m_bitfield.position(local), wanted))
      return false;

    wanted = (*++source & *++local);
  }
  
  // Unset any bits from 'last'.
  wanted &= Bitfield::mask_before(last - m_bitfield.position(local));

  if (wanted)
    return search_linear_byte(pq, m_bitfield.position(local), wanted);
  else
    return true;
}

inline bool
ChunkSelector::search_ns_linear_range(const Bitfield* bf, rak::partial_queue* pq, uint32_t first, uint32_t last) {
  if (first >= last || last > size())
    throw internal_error("ChunkSelector::search_linear_range(...) received an invalid range.");

  Bitfield::const_iterator local  = m_bitfield.begin() + first / 8;
  Bitfield::const_iterator source = bf->begin() + first / 8;

  // Unset any bits before 'first'.
  Bitfield::value_type wanted = (*source & *local) & Bitfield::mask_from(first % 8);

  while (m_bitfield.position(local + 1) < last) {
    if (wanted && !search_ns_linear_byte(pq, m_bitfield.position(local), wanted))
      return false;

    wanted = (*++source & *++local);
  }

  // Unset any bits from 'last'.
  wanted &= Bitfield::mask_before(last - m_bitfield.position(local));

  if (wanted)
    return search_linear_byte(pq, m_bitfield.position(local), wanted);
  else
    return true;
}

//Take pointer to partial_queue
//this will select and queue up to 8 pieces; but what for if I always clear the queue?

inline bool
ChunkSelector::search_linear_byte(rak::partial_queue* pq, uint32_t index, Bitfield::value_type wanted) {
  for (int i = 0; i < 8; ++i) {
    if (!(wanted & Bitfield::mask_at(i)))
      continue;

    if (!pq->insert(m_statistics->rarity(index + i), index + i) && pq->is_full())
      return false; else return true;			//one found piece is enough
  }

  return true;
}

inline bool
ChunkSelector::search_ns_linear_byte(rak::partial_queue* pq, uint32_t index, Bitfield::value_type wanted) {
  for (int i = 0; i < 8; ++i) {
    if (!(wanted & Bitfield::mask_at(i)) || ( (*m_STARTstatistics)[i] != 0 ))
      continue;

    if (!pq->insert(m_statistics->rarity(index + i), index + i) && pq->is_full())
      return false; else return true;			//one found piece is enough
  }

  return true;
}

void
ChunkSelector::advance_position() {

  // Need to replace with a special-purpose function for finding the
  // next position.

//   int position = m_position;

//   ((m_position = search_linear(&m_bitfield, &m_highPriority, position, size())) == invalid_chunk &&
//    (m_position = search_linear(&m_bitfield, &m_highPriority, 0, position)) == invalid_chunk &&
//    (m_position = search_linear(&m_bitfield, &m_normalPriority, position, size())) == invalid_chunk &&
//    (m_position = search_linear(&m_bitfield, &m_normalPriority, 0, position)) == invalid_chunk);
}

}

/*
bool
ChunkSelector::search_notSTART(const Bitfield* bf, rak::partial_queue* pq, uint32_t first, uint32_t last) {

	if (first >= last || last > size())
	    throw internal_error("ChunkSelector::search_linear_range(...) received an invalid range.");

	  Bitfield::const_iterator local  = m_bitfield.begin() + first / 8;
	  Bitfield::const_iterator source = bf->begin() + first / 8;

	  // Unset any bits before 'first'.
	  Bitfield::value_type wanted = (*source & *local) & Bitfield::mask_from(first % 8);

	  while (m_bitfield.position(local + 1) < last) {
	    if (wanted && !search_ns_linear_byte(pq, m_bitfield.position(local), wanted))
	      return false;

	    wanted = (*++source & *++local);
	  }

	  // Unset any bits from 'last'.
	  wanted &= Bitfield::mask_before(last - m_bitfield.position(local));

	  if (wanted)
	    return search_linear_byte(pq, m_bitfield.position(local), wanted);
	  else
	    return true;

/*
	if (first >= last || last > size())
	    throw internal_error("ChunkSelector::search_linear_range(...) received an invalid range.");

	  Bitfield::const_iterator local  = m_bitfield.begin() + first / 8;
	  Bitfield::const_iterator source = bf->begin() + first / 8;

	  // Unset any bits before 'first'.
	  Bitfield::value_type wanted = (*source & *local) & Bitfield::mask_from(first % 8);

	  uint8_t started = 0;		//started byte should represent all chunks that are NOT started (in the range of that byte)

	  for (int a = first - ( first % 8 ); ( a < first - ( first % 8 ) + 8 ) && ( a < m_bitfield.size_bits()) ; a++){
		  started <<= 1;
		  if ((*m_STARTstatistics)[a] == 0) { //i am interested in those NOT started yet
			  started += 1;
			  std::cout << "Started for index " << a << ", started BYTE = " << (int)started  << " (*m_STARTstatistics)[a] == 0 ? " << ( (*m_STARTstatistics)[a] == 0) << std::endl << std::flush;
		  }
		}

	  std::cout << "started = " << (int)started << std::endl << std::flush;
	  std::cout << "wanted = " << (int)wanted << " ; wanted & started = " << (int)(wanted & started)<<std::endl << std::flush;

	  //wanted = (Bitfield::value_type)(wanted & started);


	  while (m_bitfield.position(local + 1) < last) {
	    if (wanted && !search_linear_byte(pq, m_bitfield.position(local), wanted))
	      return false;

	    started = 0;		//started byte should represent all chunks that are NOT started (in the range of that byte)
	    first += 8;

	    	  for (int a = first - ( first % 8 ); ( a < first - ( first % 8 ) + 8 ) && ( a < m_bitfield.size_bits()) ; a++){
	    		  started <<= 1;
	    		  if ((*m_STARTstatistics)[a] == 0) { //i am interested in those NOT started yet
	    			  started += 1;
	    			  std::cout << "Started for index " << a << ", started BYTE = " << (int)started  << " (*m_STARTstatistics)[a] == 0 ? " << ( (*m_STARTstatistics)[a] == 0) << std::endl << std::flush;
	    		  }
	    		}

	    wanted = (*++source & *++local );	//& started
	    std::cout << "wanted = " << (int)wanted << std::endl << std::flush;
	  }

	  // Unset any bits from 'last'.
	  wanted &= Bitfield::mask_before(last - m_bitfield.position(local));

	  if (wanted)
	    return search_linear_byte(pq, m_bitfield.position(local), wanted);
	  else
	    return true;
	    */

//}
