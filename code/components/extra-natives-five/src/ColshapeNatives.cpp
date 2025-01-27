#include <StdInc.h>
#include <ScriptEngine.h>

#include <Resource.h>
#include <fxScripting.h>

#include <DisableChat.h>
#include <nutsnbolts.h>

#include <ICoreGameInit.h>

#include <ResourceManager.h>
#include <EntitySystem.h>
#include <ResourceEventComponent.h>
#include <ResourceCallbackComponent.h>

#include <chrono>
#include <scrEngine.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>


static constexpr float cellSize = 1000.0f; // covers a 1000x1000 area per cell
static constexpr int updateIntervalMs = 100; // 100ms = 10 times per second


enum class ColShapeType
{
	Circle,
	Cube,
	Cylinder,
	Rectangle
};


struct ColShape
{
	std::string id; // unique string ID
	ColShapeType type;
	Vector3 pos1; // e.g. center or first corner
	Vector3 pos2; // used by cube/rectangle (opposite corner, etc.)
	float radius; // used by circle/cylinder
	float height; // used by cylinder/rectangle
	bool infinite; // if user requests an "infinite" shape

	// For bounding extents (to place in grid cells):
	float minX, maxX, minY, maxY;

	// Track which grid cells this shape occupies, so we can efficiently remove it.
	std::vector<std::pair<int, int>> occupiedCells;
};


struct GridCellKey
{
	int cx;
	int cy;

	bool operator==(const GridCellKey& other) const
	{
		return (cx == other.cx && cy == other.cy);
	}
};

struct GridCellKeyHash
{
	size_t operator()(const GridCellKey& k) const
	{
		size_t h1 = std::hash<int>{}(k.cx);
		size_t h2 = std::hash<int>{}(k.cy);
		return h1 ^ (h2 << 1);
	}
};

class ColShapeManager
{
public:
	static ColShapeManager& Get()
	{
		static ColShapeManager instance;
		return instance;
	}

	bool CreateCircle(const std::string& colShapeId, const Vector3& center, float radius, bool infinite = false)
	{
		if (colShapes_.find(colShapeId) != colShapes_.end())
		{
			return false; // ID already taken
		}

		ColShape shape;
		shape.id = colShapeId;
		shape.type = ColShapeType::Circle;
		shape.pos1 = center;
		shape.pos2 = { 0, 0, 0 };
		shape.radius = radius;
		shape.height = 0.0f;
		shape.infinite = infinite;

		// Calculate bounding extents (2D).
		shape.minX = center.x - radius;
		shape.maxX = center.x + radius;
		shape.minY = center.y - radius;
		shape.maxY = center.y + radius;

		colShapes_.emplace(colShapeId, shape);
		AddToGrid(colShapes_[colShapeId]);
		return true;
	}

	bool CreateCube(const std::string& colShapeId, const Vector3& pos1, const Vector3& pos2, bool infinite = false)
	{
		if (colShapes_.find(colShapeId) != colShapes_.end())
		{
			return false;
		}

		ColShape shape;
		shape.id = colShapeId;
		shape.type = ColShapeType::Cube;
		shape.pos1 = pos1;
		shape.pos2 = pos2;
		shape.radius = 0.0f;
		shape.height = 0.0f;
		shape.infinite = infinite;

		shape.minX = std::min(pos1.x, pos2.x);
		shape.maxX = std::max(pos1.x, pos2.x);
		shape.minY = std::min(pos1.y, pos2.y);
		shape.maxY = std::max(pos1.y, pos2.y);

		colShapes_.emplace(colShapeId, shape);
		AddToGrid(colShapes_[colShapeId]);
		return true;
	}

	bool CreateCylinder(const std::string& colShapeId, const Vector3& center, float radius, float height, bool infinite = false)
	{
		if (colShapes_.find(colShapeId) != colShapes_.end())
		{
			return false;
		}

		ColShape shape;
		shape.id = colShapeId;
		shape.type = ColShapeType::Cylinder;
		shape.pos1 = center;
		shape.pos2 = { 0, 0, 0 };
		shape.radius = radius;
		shape.height = height;
		shape.infinite = infinite;

		shape.minX = center.x - radius;
		shape.maxX = center.x + radius;
		shape.minY = center.y - radius;
		shape.maxY = center.y + radius;

		colShapes_.emplace(colShapeId, shape);
		AddToGrid(colShapes_[colShapeId]);
		return true;
	}

	bool CreateRectangle(const std::string& colShapeId, float x1, float y1, float x2, float y2, float height, bool infinite = false)
	{
		if (colShapes_.find(colShapeId) != colShapes_.end())
		{
			return false;
		}

		ColShape shape;
		shape.id = colShapeId;
		shape.type = ColShapeType::Rectangle;
		shape.pos1 = { x1, y1, 0.0f };
		shape.pos2 = { x2, y2, 0.0f };
		shape.radius = 0.0f;
		shape.height = height;
		shape.infinite = infinite;

		shape.minX = std::min(x1, x2);
		shape.maxX = std::max(x1, x2);
		shape.minY = std::min(y1, y2);
		shape.maxY = std::max(y1, y2);

		colShapes_.emplace(colShapeId, shape);
		AddToGrid(colShapes_[colShapeId]);
		return true;
	}

	bool DeleteColShape(const std::string& colShapeId)
	{
		auto it = colShapes_.find(colShapeId);
		if (it == colShapes_.end())
		{
			return false;
		}

		const auto& shape = it->second;
		for (auto& cellCoords : shape.occupiedCells)
		{
			GridCellKey key{ cellCoords.first, cellCoords.second };
			auto gridIt = grid_.find(key);
			if (gridIt != grid_.end())
			{
				auto& vec = gridIt->second;
				vec.erase(std::remove(vec.begin(), vec.end(), colShapeId), vec.end());
			}
		}

		playerInsideColShapes_.erase(colShapeId);

		// Finally erase from master map
		colShapes_.erase(it);
		return true;
	}

	void Update()
	{
		auto resman = Instance<fx::ResourceManager>::Get();
		if (!resman)
			return;

		auto rec = resman->GetComponent<fx::ResourceEventManagerComponent>();

#ifdef GTA_FIVE
		constexpr uint64_t HASH_PLAYER_PED_ID = 0xD80958FC74E988A6;
		constexpr uint64_t HASH_GET_ENTITY_COORDS = 0x3FEF770D40960D5A;
#elif IS_RDR3
		constexpr uint64_t HASH_PLAYER_PED_ID = 0xC190F27E12443814;
		constexpr uint64_t HASH_GET_ENTITY_COORDS = 0xA86D5F069399F44D;
#endif
		static auto getPlayerPed = fx::ScriptEngine::GetNativeHandler(HASH_PLAYER_PED_ID);
		static auto getEntityCoords = fx::ScriptEngine::GetNativeHandler(HASH_GET_ENTITY_COORDS);

		int playerPedId = FxNativeInvoke::Invoke<int>(getPlayerPed);
		if (playerPedId == 0)
		{
			return; // invalid
		}
		scrVector coords = FxNativeInvoke::Invoke<scrVector>(getEntityCoords, playerPedId, true);
		Vector3 playerPos{ coords.x, coords.y, coords.z };

		std::vector<std::string> nearbyShapes = GetColShapesForPosition(playerPos);
		std::unordered_set<std::string> currentInside;

		// test for infinite shapes that people might use (for some reason..?)
		for (auto& pair : colShapes_)
		{
			if (pair.second.infinite && IsPointInColShape(playerPos, pair.second))
			{
				currentInside.insert(pair.first);
			}
		}

		for (auto& shapeId : nearbyShapes)
		{
			auto it = colShapes_.find(shapeId);
			if (it == colShapes_.end())
				continue;

			const auto& shape = it->second;
			if (!shape.infinite && IsPointInColShape(playerPos, shape))
			{
				currentInside.insert(shapeId);
			}
		}

		std::vector<std::string> newlyEntered;
		std::vector<std::string> newlyLeft;

		for (auto& shapeId : currentInside)
		{
			if (playerInsideColShapes_.find(shapeId) == playerInsideColShapes_.end())
			{
				newlyEntered.push_back(shapeId);
			}
		}
		for (auto& shapeId : playerInsideColShapes_)
		{
			if (currentInside.find(shapeId) == currentInside.end())
			{
				newlyLeft.push_back(shapeId);
			}
		}

		playerInsideColShapes_ = currentInside;

		// Fire events
		for (auto& shapeId : newlyEntered)
		{
			trace("Player entered shape %s\n", shapeId.c_str());
			rec->QueueEvent2("onPlayerEnterColshape", {}, shapeId.c_str());
		}
		for (auto& shapeId : newlyLeft)
		{
			trace("Player left shape %s\n", shapeId.c_str());
			rec->QueueEvent2("onPlayerLeaveColshape", {}, shapeId.c_str());
		}
	}

private:
	ColShapeManager() = default;

	void AddToGrid(ColShape& shape)
	{
		if (shape.infinite)
		{
			return;
		}

		int startCx = static_cast<int>(std::floor(shape.minX / cellSize));
		int endCx = static_cast<int>(std::floor(shape.maxX / cellSize));
		int startCy = static_cast<int>(std::floor(shape.minY / cellSize));
		int endCy = static_cast<int>(std::floor(shape.maxY / cellSize));

		shape.occupiedCells.clear();

		for (int cx = startCx; cx <= endCx; ++cx)
		{
			for (int cy = startCy; cy <= endCy; ++cy)
			{
				GridCellKey key{ cx, cy };
				grid_[key].push_back(shape.id);
				shape.occupiedCells.push_back({ cx, cy });
			}
		}
	}

	// we only get the shapes for certain grids based on the player's position to reduce the number of checks
	std::vector<std::string> GetColShapesForPosition(const Vector3& pos)
	{
		std::vector<std::string> result;

		int cx = static_cast<int>(std::floor(pos.x / cellSize));
		int cy = static_cast<int>(std::floor(pos.y / cellSize));
		GridCellKey key{ cx, cy };

		auto it = grid_.find(key);
		if (it != grid_.end())
		{
			result.insert(result.end(), it->second.begin(), it->second.end());
		}
		return result;
	}

	bool IsPointInColShape(const Vector3& p, const ColShape& shape)
	{
		switch (shape.type)
		{
			case ColShapeType::Circle:
			{
				float dx = p.x - shape.pos1.x;
				float dy = p.y - shape.pos1.y;
				return (dx * dx + dy * dy) <= (shape.radius * shape.radius);
			}
			case ColShapeType::Cube:
			{
				float minX = std::min(shape.pos1.x, shape.pos2.x);
				float maxX = std::max(shape.pos1.x, shape.pos2.x);
				float minY = std::min(shape.pos1.y, shape.pos2.y);
				float maxY = std::max(shape.pos1.y, shape.pos2.y);
				float minZ = std::min(shape.pos1.z, shape.pos2.z);
				float maxZ = std::max(shape.pos1.z, shape.pos2.z);
				return (p.x >= minX && p.x <= maxX && p.y >= minY && p.y <= maxY && p.z >= minZ && p.z <= maxZ);
			}
			case ColShapeType::Cylinder:
			{
				float dx = p.x - shape.pos1.x;
				float dy = p.y - shape.pos1.y;
				if ((dx * dx + dy * dy) > (shape.radius * shape.radius))
					return false;
				float bottomZ = shape.pos1.z;
				float topZ = shape.pos1.z + shape.height;
				if (topZ < bottomZ)
					std::swap(topZ, bottomZ);
				return (p.z >= bottomZ && p.z <= topZ);
			}
			case ColShapeType::Rectangle:
			{
				float minX = std::min(shape.pos1.x, shape.pos2.x);
				float maxX = std::max(shape.pos1.x, shape.pos2.x);
				float minY = std::min(shape.pos1.y, shape.pos2.y);
				float maxY = std::max(shape.pos1.y, shape.pos2.y);

				float bottomZ = shape.pos1.z;
				float topZ = shape.pos1.z + shape.height;
				if (topZ < bottomZ)
					std::swap(topZ, bottomZ);

				bool inside2D = (p.x >= minX && p.x <= maxX && p.y >= minY && p.y <= maxY);
				bool insideZ = (p.z >= bottomZ && p.z <= topZ);
				return (inside2D && insideZ);
			}
		}
		return false;
	}

private:
	std::unordered_map<std::string, ColShape> colShapes_;
	std::unordered_map<GridCellKey, std::vector<std::string>, GridCellKeyHash> grid_;
	std::unordered_set<std::string> playerInsideColShapes_;
};

class ColShapeThread
{
public:
	ColShapeThread()
		: shutdown_(false)
	{
	}

	void Start()
	{
		thread_ = std::thread(&ColShapeThread::Run, this);
	}

	void Shutdown()
	{
		shutdown_ = true;
		if (thread_.joinable())
		{
			thread_.join();
		}
	}

private:
	void Run()
	{
		trace("ColShapeThread started.\n");

		while (!shutdown_)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(updateIntervalMs));
			ColShapeManager::Get().Update();
		}

		trace("ColShapeThread shutting down.\n");
	}

	std::thread thread_;
	std::atomic<bool> shutdown_;
};

static InitFunction initFunction([]()
{

	static ColShapeThread colShapeThread;

	fx::ScriptEngine::RegisterNativeHandler("CREATE_COLSHAPE_CIRCLE", [](fx::ScriptContext& context)
	{
		// Args: colShapeId, x, y, z, radius, (bool infinite)
		std::string colShapeId = context.CheckArgument<const char*>(0);
		float x = context.GetArgument<float>(1);
		float y = context.GetArgument<float>(2);
		float z = context.GetArgument<float>(3);
		float radius = context.GetArgument<float>(4);
		bool infinite = false;
		if (context.GetArgumentCount() > 5)
		{
			infinite = context.GetArgument<bool>(5);
		}

		Vector3 center{ x, y, z };
		bool success = ColShapeManager::Get().CreateCircle(colShapeId, center, radius, infinite);
		context.SetResult<bool>(success);
	});

	fx::ScriptEngine::RegisterNativeHandler("CREATE_COLSHAPE_CUBE", [](fx::ScriptContext& context)
	{
		// Args: colShapeId, x1, y1, z1, x2, y2, z2, (bool infinite)
		std::string colShapeId = context.CheckArgument<const char*>(0);
		float x1 = context.GetArgument<float>(1);
		float y1 = context.GetArgument<float>(2);
		float z1 = context.GetArgument<float>(3);
		float x2 = context.GetArgument<float>(4);
		float y2 = context.GetArgument<float>(5);
		float z2 = context.GetArgument<float>(6);
		bool infinite = false;
		if (context.GetArgumentCount() > 7)
		{
			infinite = context.GetArgument<bool>(7);
		}

		Vector3 pos1{ x1, y1, z1 };
		Vector3 pos2{ x2, y2, z2 };
		bool success = ColShapeManager::Get().CreateCube(colShapeId, pos1, pos2, infinite);
		context.SetResult<bool>(success);
	});

	fx::ScriptEngine::RegisterNativeHandler("CREATE_COLSHAPE_CYLINDER", [](fx::ScriptContext& context)
	{
		// Args: colShapeId, x, y, z, radius, height, (bool infinite)
		std::string colShapeId = context.CheckArgument<const char*>(0);
		float x = context.GetArgument<float>(1);
		float y = context.GetArgument<float>(2);
		float z = context.GetArgument<float>(3);
		float radius = context.GetArgument<float>(4);
		float height = context.GetArgument<float>(5);
		bool infinite = false;
		if (context.GetArgumentCount() > 6)
		{
			infinite = context.GetArgument<bool>(6);
		}

		Vector3 center{ x, y, z };
		bool success = ColShapeManager::Get().CreateCylinder(colShapeId, center, radius, height, infinite);
		context.SetResult<bool>(success);
	});

	fx::ScriptEngine::RegisterNativeHandler("CREATE_COLSHAPE_RECTANGLE", [](fx::ScriptContext& context)
	{
		// Args: colShapeId, x1, y1, x2, y2, height, (bool infinite)
		std::string colShapeId = context.CheckArgument<const char*>(0);
		float x1 = context.GetArgument<float>(1);
		float y1 = context.GetArgument<float>(2);
		float x2 = context.GetArgument<float>(3);
		float y2 = context.GetArgument<float>(4);
		float height = context.GetArgument<float>(5);
		bool infinite = false;
		if (context.GetArgumentCount() > 6)
		{
			infinite = context.GetArgument<bool>(6);
		}

		bool success = ColShapeManager::Get().CreateRectangle(colShapeId, x1, y1, x2, y2, height, infinite);
		context.SetResult<bool>(success);
	});

	fx::ScriptEngine::RegisterNativeHandler("DELETE_COLSHAPE", [](fx::ScriptContext& context)
	{
		// Args: colShapeId
		std::string colShapeId = context.CheckArgument<const char*>(0);
		bool success = ColShapeManager::Get().DeleteColShape(colShapeId);
		context.SetResult<bool>(success);
	});


    rage::scrEngine::OnScriptInit.Connect([]()
	{
		//trace("Starting ColShapeThread...\n");
		colShapeThread.Start();
	});

});
