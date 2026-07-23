#include "StdInc.h"
#include <Client.h>
#include <GameServer.h>

namespace fx
{
	Client::Client(const std::string& guid)
	: m_guid(guid), m_netId(0xFFFF), m_previousNetId(0xFFFF), m_netBase(-1), m_lastSeen(0), m_hasRouted(false), m_slotId(-1), m_dropping(false), 
	  m_firstSeen(msec()), m_clientNetworkMetricsSendCallback(nullptr), m_clientNetworkMetricsRecvCallback(nullptr)
	{

	}

	void Client::SetPeer(int peer, const net::PeerAddress& peerAddress)
	{
		m_peer.reset(new int(peer));
		m_peerAddress = peerAddress;

		OnAssignPeer();
	}

	void Client::SetNetId(uint32_t netId)
	{
		m_previousNetId = m_netId;
		m_netId = netId;

		OnAssignNetId(m_previousNetId);

		UpdateCachedPrincipalValues();
	}

	void Client::SetTcpEndPoint(const std::string& value)
	{
		m_tcpEndPoint = value;

		OnAssignTcpEndPoint();
	}

	void Client::Touch()
	{
		m_lastSeen = msec();
	}

	bool Client::IsDead()
	{
		// FAKE CLIENT (ihatemylife-128, never upstream): socketless fake clients have no
		// ENet peer and no real connection to time out. Never reap them, so the fake
		// population stays STABLE (otherwise they die at CLIENT_DEAD_TIMEOUT and churn).
		{
			auto fake = GetData("fakeClient");
			if (fake && fx::AnyCast<bool>(fake))
			{
				return false;
			}
		}

		// if we've not connected yet, we can't be dead
		if (!HasConnected())
		{
			auto canBeDead = GetData("canBeDead");

			if (!canBeDead || !fx::AnyCast<bool>(canBeDead))
			{
				return (msec() - m_lastSeen) > CLIENT_VERY_DEAD_TIMEOUT;
			}
		}

		// new policy (2021-01-17): if the ENet peer has vanished, we shall be dead
		fx::NetPeerStackBuffer stackBuffer;
		gscomms_get_peer(GetPeer(), stackBuffer);
		auto peer = stackBuffer.GetBase();

		if (!peer || peer->GetPing() == -1)
		{
			return true;
		}

		return (msec() - m_lastSeen) > CLIENT_DEAD_TIMEOUT;
	}

	int Client::GetPing()
	{
		// FAKE CLIENT (ihatemylife-128, never upstream): no ENet peer -> peer->GetPing()
		// would deref a wrapper with no real handle; return a sane fake ping instead.
		{
			auto fake = GetData("fakeClient");
			if (fake && fx::AnyCast<bool>(fake))
			{
				return 1;
			}
		}

		fx::NetPeerStackBuffer stackBuffer;
		gscomms_get_peer(GetPeer(), stackBuffer);
		auto peer = stackBuffer.GetBase();

		return peer->GetPing();
	}

	void Client::SetDataRaw(const std::string& key, const std::shared_ptr<AnyBase>& data)
	{
		std::unique_lock _(m_userDataMutex);
		m_userData[key] = data;
	}

	std::shared_ptr<AnyBase> Client::GetData(const std::string& key)
	{
		std::shared_lock _(m_userDataMutex);
		auto it = m_userData.find(key);

		if (it == m_userData.end())
		{
			return {};
		}

		return it->second;
	}

	void Client::SendPacket(int channel, const net::Buffer& buffer, NetPacketType type /* = (ENetPacketFlag)0 */)
	{
		if (m_peer)
		{
			gscomms_send_packet(this, *m_peer, channel, buffer, type);
			if (m_clientNetworkMetricsSendCallback)
			{
				m_clientNetworkMetricsSendCallback(this, channel, buffer, type);
			}
		}
	}

	DLL_EXPORT object_pool<Client, 512 * MAX_CLIENTS> clientPool;
}
