/*
 * Copyright (c)2019 ZeroTier, Inc.
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file in the project's root directory.
 *
 * Change Date: 2023-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2.0 of the Apache License.
 */
/****/

#ifndef ZT_TOPOLOGY_HPP
#define ZT_TOPOLOGY_HPP

#include <cstdio>
#include <cstring>

#include <vector>
#include <algorithm>
#include <utility>
#include <set>

#include "Constants.hpp"
#include "../include/ZeroTierOne.h"

#include "Address.hpp"
#include "Identity.hpp"
#include "Peer.hpp"
#include "Path.hpp"
#include "Mutex.hpp"
#include "InetAddress.hpp"
#include "Hashtable.hpp"
#include "SharedPtr.hpp"
#include "ScopedPtr.hpp"

namespace ZeroTier {

class RuntimeEnvironment;

/**
 * Database of network topology
 */
class Topology
{
public:
	Topology(const RuntimeEnvironment *renv,const Identity &myId);
	~Topology();

	/**
	 * Add a peer to database
	 *
	 * This will not replace existing peers. In that case the existing peer
	 * record is returned.
	 *
	 * @param peer Peer to add
	 * @return New or existing peer (should replace 'peer')
	 */
	ZT_ALWAYS_INLINE SharedPtr<Peer> add(const SharedPtr<Peer> &peer)
	{
		RWMutex::Lock _l(_peers_l);
		SharedPtr<Peer> &hp = _peers[peer->address()];
		if (!hp)
			hp = peer;
		return hp;
	}

	/**
	 * Get a peer from its address
	 *
	 * @param tPtr Thread pointer to be handed through to any callbacks called as a result of this call
	 * @param zta ZeroTier address of peer
	 * @return Peer or NULL if not found
	 */
	ZT_ALWAYS_INLINE SharedPtr<Peer> get(const Address &zta) const
	{
		RWMutex::RLock l1(_peers_l);
		const SharedPtr<Peer> *const ap = _peers.get(zta);
		return (ap) ? *ap : SharedPtr<Peer>();
	}

	/**
	 * @param tPtr Thread pointer to be handed through to any callbacks called as a result of this call
	 * @param zta ZeroTier address of peer
	 * @return Identity or NULL identity if not found
	 */
	ZT_ALWAYS_INLINE Identity getIdentity(void *tPtr,const Address &zta) const
	{
		if (zta == _myIdentity.address()) {
			return _myIdentity;
		} else {
			RWMutex::RLock _l(_peers_l);
			const SharedPtr<Peer> *const ap = _peers.get(zta);
			if (ap)
				return (*ap)->identity();
		}
		return Identity();
	}

	/**
	 * Get a Path object for a given local and remote physical address, creating if needed
	 *
	 * @param l Local socket
	 * @param r Remote address
	 * @return Pointer to canonicalized Path object or NULL on error
	 */
	ZT_ALWAYS_INLINE SharedPtr<Path> getPath(const int64_t l,const InetAddress &r)
	{
		const Path::HashKey k(l,r);

		_paths_l.rlock();
		SharedPtr<Path> p(_paths[k]);
		_paths_l.unlock();
		if (p)
			return p;

		_paths_l.lock();
		SharedPtr<Path> &p2 = _paths[k];
		if (p2) {
			p = p2;
		} else {
			try {
				p.set(new Path(l,r));
			} catch ( ... ) {
				_paths_l.unlock();
				return SharedPtr<Path>();
			}
			p2 = p;
		}
		_paths_l.unlock();

		return p;
	}

	/**
	 * @return Current best root server
	 */
	ZT_ALWAYS_INLINE SharedPtr<Peer> root() const
	{
		RWMutex::RLock l(_peers_l);
		if (_rootPeers.empty())
			return SharedPtr<Peer>();
		return _rootPeers.front();
	}

	/**
	 * @param id Identity to check
	 * @return True if this identity corresponds to a root
	 */
	ZT_ALWAYS_INLINE bool isRoot(const Identity &id) const
	{
		RWMutex::RLock l(_peers_l);
		return (_roots.count(id) > 0);
	}

	/**
	 * Apply a function or function object to all peers
	 *
	 * This locks the peer map during execution, so calls to get() etc. during
	 * eachPeer() will deadlock.
	 *
	 * @param f Function to apply
	 * @tparam F Function or function object type
	 */
	template<typename F>
	ZT_ALWAYS_INLINE void eachPeer(F f) const
	{
		RWMutex::RLock l(_peers_l);
		Hashtable< Address,SharedPtr<Peer> >::Iterator i(const_cast<Topology *>(this)->_peers);
		Address *a = (Address *)0;
		SharedPtr<Peer> *p = (SharedPtr<Peer> *)0;
		while (i.next(a,p)) {
			if (!f(*((const SharedPtr<Peer> *)p)))
				break;
		}
	}

	/**
	 * Apply a function or function object to all peers
	 *
	 * This locks the peer map during execution, so calls to get() etc. during
	 * eachPeer() will deadlock.
	 *
	 * @param f Function to apply
	 * @tparam F Function or function object type
	 */
	template<typename F>
	ZT_ALWAYS_INLINE void eachPeerWithRoot(F f) const
	{
		RWMutex::RLock l(_peers_l);

		std::vector<uintptr_t> rootPeerPtrs;
		for(std::vector< SharedPtr<Peer> >::const_iterator i(_rootPeers.begin());i!=_rootPeers.end();++i)
			rootPeerPtrs.push_back((uintptr_t)i->ptr());
		std::sort(rootPeerPtrs.begin(),rootPeerPtrs.end());

		Hashtable< Address,SharedPtr<Peer> >::Iterator i(const_cast<Topology *>(this)->_peers);
		Address *a = (Address *)0;
		SharedPtr<Peer> *p = (SharedPtr<Peer> *)0;
		while (i.next(a,p)) {
			if (!f(*((const SharedPtr<Peer> *)p),std::binary_search(rootPeerPtrs.begin(),rootPeerPtrs.end(),(uintptr_t)p->ptr())))
				break;
		}
	}

	/**
	 * Get the best relay to a given address, which may or may not be a root
	 *
	 * @param now Current time
	 * @param toAddr Destination address
	 * @return Best current relay or NULL if none
	 */
	ZT_ALWAYS_INLINE SharedPtr<Peer> findRelayTo(const int64_t now,const Address &toAddr)
	{
		RWMutex::RLock l(_peers_l);
		if (_rootPeers.empty())
			return SharedPtr<Peer>();
		return _rootPeers[0];
	}

	/**
	 * @param allPeers vector to fill with all current peers
	 */
	void getAllPeers(std::vector< SharedPtr<Peer> > &allPeers) const;

	/**
	 * Get info about a path
	 *
	 * The supplied result variables are not modified if no special config info is found.
	 *
	 * @param physicalAddress Physical endpoint address
	 * @param mtu Variable set to MTU
	 * @param trustedPathId Variable set to trusted path ID
	 */
	ZT_ALWAYS_INLINE void getOutboundPathInfo(const InetAddress &physicalAddress,unsigned int &mtu,uint64_t &trustedPathId)
	{
		for(unsigned int i=0,j=_numConfiguredPhysicalPaths;i<j;++i) {
			if (_physicalPathConfig[i].first.containsAddress(physicalAddress)) {
				trustedPathId = _physicalPathConfig[i].second.trustedPathId;
				mtu = _physicalPathConfig[i].second.mtu;
				return;
			}
		}
	}

	/**
	 * Get the outbound trusted path ID for a physical address, or 0 if none
	 *
	 * @param physicalAddress Physical address to which we are sending the packet
	 * @return Trusted path ID or 0 if none (0 is not a valid trusted path ID)
	 */
	ZT_ALWAYS_INLINE uint64_t getOutboundPathTrust(const InetAddress &physicalAddress)
	{
		for(unsigned int i=0,j=_numConfiguredPhysicalPaths;i<j;++i) {
			if (_physicalPathConfig[i].first.containsAddress(physicalAddress))
				return _physicalPathConfig[i].second.trustedPathId;
		}
		return 0;
	}

	/**
	 * Check whether in incoming trusted path marked packet is valid
	 *
	 * @param physicalAddress Originating physical address
	 * @param trustedPathId Trusted path ID from packet (from MAC field)
	 */
	ZT_ALWAYS_INLINE bool shouldInboundPathBeTrusted(const InetAddress &physicalAddress,const uint64_t trustedPathId)
	{
		for(unsigned int i=0,j=_numConfiguredPhysicalPaths;i<j;++i) {
			if ((_physicalPathConfig[i].second.trustedPathId == trustedPathId)&&(_physicalPathConfig[i].first.containsAddress(physicalAddress)))
				return true;
		}
		return false;
	}

	/**
	 * Set or clear physical path configuration (called via Node::setPhysicalPathConfiguration)
	 */
	void setPhysicalPathConfiguration(const struct sockaddr_storage *pathNetwork,const ZT_PhysicalPathConfiguration *pathConfig);

	/**
	 * Add a root server's identity to the root server set
	 *
	 * @param id Root server identity
	 */
	void addRoot(const Identity &id);

	/**
	 * Remove a root server's identity from the root server set
	 *
	 * @param id Root server identity
	 * @return True if root found and removed, false if not found
	 */
	bool removeRoot(const Identity &id);

	/**
	 * Sort roots in asecnding order of apparent latency
	 *
	 * @param now Current time
	 */
	void rankRoots(const int64_t now);

	/**
	 * Do periodic tasks such as database cleanup
	 */
	void doPeriodicTasks(const int64_t now);

private:
	const RuntimeEnvironment *const RR;
	const Identity _myIdentity;

	RWMutex _peers_l;
	RWMutex _paths_l;

	std::pair< InetAddress,ZT_PhysicalPathConfiguration > _physicalPathConfig[ZT_MAX_CONFIGURABLE_PATHS];
	unsigned int _numConfiguredPhysicalPaths;

	Hashtable< Address,SharedPtr<Peer> > _peers;
	Hashtable< Path::HashKey,SharedPtr<Path> > _paths;
	std::set< Identity > _roots; // locked by _peers_l
	std::vector< SharedPtr<Peer> > _rootPeers; // locked by _peers_l
};

} // namespace ZeroTier

#endif
