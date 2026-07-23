/*
 * FakeClients.cpp - DEV-ONLY fake client spawner for the ihatemylife-128 branch.
 *
 * NEVER UPSTREAM. This injects socketless fake fx::Client entries that occupy OneSync
 * slots so the sync path can be load-tested to 128 players without real clients. It is
 * the capability bot/DDoS services abuse, and is only here to drive local crash-discovery.
 *
 * It replays the state mutations the real msgType-1 admit path performs
 * (GameServer::ProcessPacket, GameServer.cpp:659-761) directly on the client object,
 * since a fake client has no ENet peer to go through ProcessPacket with.
 */

#include "StdInc.h"

#include <ClientRegistry.h>
#include <GameServer.h>
#include <ServerInstanceBase.h>

#include "packethandlers/RequestObjectIdsPacketHandler.h"

#include <ScriptEngine.h>
#include <console/Console.h>
#include <NetAddress.h>

#include <atomic>
#include <algorithm>
#include <cmath>

namespace fx
{
	// fake peer ids start well above any real ENet peer id so they never collide
	static std::atomic<int> g_fakePeerId{ 0x40000000 };

	// Valentine main street - fakes are placed in a spiral here so they're in-scope
	static constexpr float kSpawnX = -321.0f;
	static constexpr float kSpawnY = 785.0f;
	static constexpr float kSpawnZ = 117.0f;

	static void ClearFakeClients(ServerInstanceBase* instance);

	static void SpawnFakeClients(ServerInstanceBase* instance, int count)
	{
		auto clientRegistry = instance->GetComponent<fx::ClientRegistry>();
		auto gameServer = instance->GetComponent<fx::GameServer>();

		if (!fx::IsOneSync())
		{
			console::Printf("fakeclients", "OneSync is off - fake clients need OneSync/BigMode to take slots. Aborting.\n");
			return;
		}

		// clear any existing fakes first so `spawn_fake_clients N` always means
		// EXACTLY N (never accumulates across repeated runs -> reliable testing).
		ClearFakeClients(instance);

		const auto local = net::PeerAddress::FromString("127.0.0.1").get();

		int spawned = 0;
		for (int i = 0; i < count; i++)
		{
			int peerId = g_fakePeerId.fetch_add(1);
			std::string guid = fmt::sprintf("fake:%08x", peerId);

			auto client = clientRegistry->MakeClient(guid);

			// mark first so IsDead/GetPing can treat this as a live socketless client
			client->SetData("fakeClient", true);
			client->SetData("canBeDead", false);

			client->SetData("passedValidation", true);
			client->SetConnectionToken(fmt::sprintf("faketoken:%08x", peerId));
			client->SetTcpEndPoint("127.0.0.1");
			client->SetName(fmt::sprintf("bot-%d", i + 1));
			client->Touch();

			// assigns the OneSync slot via OnAssignPeer (ClientRegistry.cpp:96-134)
			client->SetPeer(peerId, local);

			if (!client->HasSlotId())
			{
				console::Printf("fakeclients", "no free slot at #%d - slot pool exhausted.\n", spawned);
				clientRegistry->RemoveClient(client);
				break;
			}

			clientRegistry->HandleConnectingClient(client);

			auto oldNetId = client->GetNetId();
			auto netId = client->GetNetId();

			// Valentine spiral placement so each fake is in-scope of a player at spawn
			float angle = i * 137.5f * 0.01745329252f;
			float radius = std::min(3.0f + i * 0.6f, 35.0f);
			float px = kSpawnX + std::cos(angle) * radius;
			float py = kSpawnY + std::sin(angle) * radius;
			float pz = kSpawnZ;

			gscomms_execute_callback_on_main_thread([clientRegistry, client, oldNetId, netId, px, py, pz]()
			{
				clientRegistry->HandleConnectedClient(client, oldNetId);

				// create the fake client's synced player entity (state-side native)
				if (auto handler = fx::ScriptEngine::GetNativeHandler(HashString("CREATE_FAKE_PLAYER")))
				{
					FxNativeInvoke::Invoke<uint32_t>(handler, netId, px, py, pz);
				}
			});

			RequestObjectIdsPacketHandler::SendObjectIds(instance, client, fx::IsBigMode() ? 4 : 64);

			spawned++;
		}

		console::Printf("fakeclients", "spawned %d fake client(s).\n", spawned);
	}

	static void ClearFakeClients(ServerInstanceBase* instance)
	{
		auto clientRegistry = instance->GetComponent<fx::ClientRegistry>();

		std::vector<fx::ClientSharedPtr> toDrop;
		clientRegistry->ForAllClients([&toDrop](const fx::ClientSharedPtr& client)
		{
			auto fake = client->GetData("fakeClient");
			if (fake && fx::AnyCast<bool>(fake))
			{
				toDrop.push_back(client);
			}
		});

		for (auto& client : toDrop)
		{
			clientRegistry->RemoveClient(client);
		}

		console::Printf("fakeclients", "cleared %d fake client(s).\n", (int)toDrop.size());
	}
}

static InitFunction initFunction([]()
{
	fx::ServerInstanceBase::OnServerCreate.Connect([](fx::ServerInstanceBase* instance)
	{
		static ConsoleCommand spawnFakeCmd("spawn_fake_clients", [instance](int count)
		{
			fx::SpawnFakeClients(instance, count);
		});

		static ConsoleCommand clearFakeCmd("clear_fake_clients", [instance]()
		{
			fx::ClearFakeClients(instance);
		});
	});
});
