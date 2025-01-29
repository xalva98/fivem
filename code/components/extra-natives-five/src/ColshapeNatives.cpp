#include <StdInc.h>
#include <ScriptEngine.h>

#include <Resource.h>

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
#include <memory>
#include <thread>
#include <atomic>

static constexpr float cellSize = 1000.0f; // covers a 1000x1000 area per grid cell
static constexpr int updateIntervalMs = 100; // 100ms thread update interval

// we are using a grid based approach so we don't have to check every shape for our current position
// if a shape is bigger than this threshold in width or height, we will treat it as infinite
// and skip the grid for it, adding it to a separate set that we check every updateTick
static constexpr float kAutoInfiniteThreshold = 2000.0f;

enum class ColShapeType
{
	Circle, // 2D circle
	Cube, // 3D cube
	Cylinder, // cylindrical shape
	Rectangle, // 2D rectangle (with bottomZ + height in Z)
	Sphere // 3D sphere
};

struct ColShape
{
	std::string id; // unique string ID
	ColShapeType type;
	Vector3 pos1; // often center or first corner
	Vector3 pos2; // used by cube/rectangle
	float radius; // used by circle/cylinder/sphere
	float height; // used by cylinder/rectangle
	bool infinite; // whether we skip the grid (very large shapes, etc.)

	// For bounding extents in X/Y (to place in grid cells)
	float minX, maxX;
	float minY, maxY;

	std::vector<std::pair<int, int>> occupiedCells;

	//int dimension = 0; for future implementations maybe?
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
	// Combination hash for two integers
	// magic number (0x9e3779b97f4a7c15ULL) is fractional part of the golden ratio scaled to 64 bits.
	size_t operator()(const GridCellKey& k) const
	{
		size_t h1 = std::hash<int>{}(k.cx);
		size_t h2 = std::hash<int>{}(k.cy);
		return (h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2)));
	}
};

using ShapeSet = std::unordered_set<ColShape*>;

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
			trace("CreateCircle: ID already taken\n");
			return false;
		}

		auto shape = std::make_unique<ColShape>();
		shape->id = colShapeId;
		shape->type = ColShapeType::Circle;
		shape->pos1 = center;
		shape->pos2 = { 0, 0, 0 };
		shape->radius = radius;
		shape->height = 0.0f;
		shape->infinite = infinite;

		shape->minX = center.x - radius;
		shape->maxX = center.x + radius;
		shape->minY = center.y - radius;
		shape->maxY = center.y + radius;

		// Auto-detect if bounding box is huge -> force infinite
		MaybeMarkInfinite(*shape);

		ColShape* rawPtr = shape.get();
		colShapes_.emplace(colShapeId, std::move(shape));

		if (!rawPtr->infinite)
		{
			AddToGrid(rawPtr);
		}
		else
		{
			infiniteShapes_.insert(rawPtr);
		}
		return true;
	}

	bool CreateCube(const std::string& colShapeId, const Vector3& pos1, const Vector3& pos2, bool infinite = false)
	{
		if (colShapes_.find(colShapeId) != colShapes_.end())
		{
			trace("CreateCube: ID already taken\n");
			return false;
		}

		auto shape = std::make_unique<ColShape>();
		shape->id = colShapeId;
		shape->type = ColShapeType::Cube;
		shape->pos1 = pos1;
		shape->pos2 = pos2;
		shape->radius = 0.0f;
		shape->height = 0.0f;
		shape->infinite = infinite;

		shape->minX = std::min(pos1.x, pos2.x);
		shape->maxX = std::max(pos1.x, pos2.x);
		shape->minY = std::min(pos1.y, pos2.y);
		shape->maxY = std::max(pos1.y, pos2.y);

		// Auto-detect if bounding box is huge -> force infinite
		MaybeMarkInfinite(*shape);

		ColShape* rawPtr = shape.get();
		colShapes_.emplace(colShapeId, std::move(shape));

		if (!rawPtr->infinite)
		{
			AddToGrid(rawPtr);
		}
		else
		{
			infiniteShapes_.insert(rawPtr);
		}
		return true;
	}

	bool CreateCylinder(const std::string& colShapeId, const Vector3& center, float radius, float height, bool infinite = false)
	{
		if (colShapes_.find(colShapeId) != colShapes_.end())
		{
			trace("CreateCylinder: ID already taken\n");
			return false;
		}

		auto shape = std::make_unique<ColShape>();
		shape->id = colShapeId;
		shape->type = ColShapeType::Cylinder;
		shape->pos1 = center;
		shape->pos2 = { 0, 0, 0 };
		shape->radius = radius;
		shape->height = height;
		shape->infinite = infinite;

		shape->minX = center.x - radius;
		shape->maxX = center.x + radius;
		shape->minY = center.y - radius;
		shape->maxY = center.y + radius;

		// Auto-detect if bounding box is huge -> force infinite
		MaybeMarkInfinite(*shape);

		ColShape* rawPtr = shape.get();
		colShapes_.emplace(colShapeId, std::move(shape));

		if (!rawPtr->infinite)
		{
			AddToGrid(rawPtr);
		}
		else
		{
			infiniteShapes_.insert(rawPtr);
		}
		return true;
	}

	bool CreateRectangleZ(const std::string& colShapeId, float x1, float y1, float x2, float y2,
	float bottomZ, float height, bool infinite = false)
	{
		if (colShapes_.find(colShapeId) != colShapes_.end())
		{
			trace("CreateRectangleZ: ID already taken\n");
			return false;
		}

		auto shape = std::make_unique<ColShape>();
		shape->id = colShapeId;
		shape->type = ColShapeType::Rectangle;
		shape->pos1 = { x1, y1, bottomZ };
		shape->pos2 = { x2, y2, bottomZ };
		shape->radius = 0.0f;
		shape->height = height;
		shape->infinite = infinite;

		shape->minX = std::min(x1, x2);
		shape->maxX = std::max(x1, x2);
		shape->minY = std::min(y1, y2);
		shape->maxY = std::max(y1, y2);

		// Auto-detect if bounding box is huge -> force infinite
		MaybeMarkInfinite(*shape);

		ColShape* rawPtr = shape.get();
		colShapes_.emplace(colShapeId, std::move(shape));

		if (!rawPtr->infinite)
		{
			AddToGrid(rawPtr);
		}
		else
		{
			infiniteShapes_.insert(rawPtr);
		}
		return true;
	}

	// Alias that defaults bottomZ = 0.0f
	bool CreateRectangle(const std::string& colShapeId, float x1, float y1, float x2, float y2,
	float height, bool infinite = false)
	{
		return CreateRectangleZ(colShapeId, x1, y1, x2, y2, 0.0f, height, infinite);
	}

	bool CreateSphere(const std::string& colShapeId, const Vector3& center, float radius, bool infinite = false)
	{
		if (colShapes_.find(colShapeId) != colShapes_.end())
		{
			trace("CreateSphere: ID already taken\n");
			return false;
		}

		auto shape = std::make_unique<ColShape>();
		shape->id = colShapeId;
		shape->type = ColShapeType::Sphere;
		shape->pos1 = center;
		shape->pos2 = { 0, 0, 0 };
		shape->radius = radius;
		shape->height = 0.0f;
		shape->infinite = infinite;

		shape->minX = center.x - radius;
		shape->maxX = center.x + radius;
		shape->minY = center.y - radius;
		shape->maxY = center.y + radius;

		// Auto-detect if bounding box is huge -> force infinite
		MaybeMarkInfinite(*shape);

		ColShape* rawPtr = shape.get();
		colShapes_.emplace(colShapeId, std::move(shape));

		if (!rawPtr->infinite)
		{
			AddToGrid(rawPtr);
		}
		else
		{
			infiniteShapes_.insert(rawPtr);
		}
		return true;
	}

	bool DeleteColShape(const std::string& colShapeId)
	{
		auto it = colShapes_.find(colShapeId);
		if (it == colShapes_.end())
		{
			return false;
		}
		ColShape* shapePtr = it->second.get();
		if (shapePtr->infinite)
		{
			infiniteShapes_.erase(shapePtr);
		}
		else
		{
			// Remove from every cell that references it
			for (auto& cellCoords : shapePtr->occupiedCells)
			{
				GridCellKey key{ cellCoords.first, cellCoords.second };
				auto gridIt = grid_.find(key);
				if (gridIt != grid_.end())
				{
					gridIt->second.erase(shapePtr);
				}
			}
		}

		// Remove from "player inside" set
		playerInsideColShapes_.erase(shapePtr);

		// Remove from master map
		colShapes_.erase(it);
		return true;
	}

	void Update()
	{
		trace("ColShapeManager::Update\n");
		static auto resman = Instance<fx::ResourceManager>::Get();
		if (!resman)
			return;

		static auto rec = resman->GetComponent<fx::ResourceEventManagerComponent>();

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
			return;
		}
		scrVector coords = FxNativeInvoke::Invoke<scrVector>(getEntityCoords, playerPedId, true);
		Vector3 playerPos{ coords.x, coords.y, coords.z };

		// Collect shapes in the relevant grid cell
		std::unordered_set<ColShape*> currentInside;
		{
			auto cellShapes = GetColShapesForPosition(playerPos);
			for (ColShape* shape : cellShapes)
			{
				if (IsPointInColShape(playerPos, *shape))
				{
					currentInside.insert(shape);
				}
			}
		}

		// Also check all infinite shapes
		for (ColShape* infShape : infiniteShapes_)
		{
			if (IsPointInColShape(playerPos, *infShape))
			{
				currentInside.insert(infShape);
			}
		}

		// Determine which shapes we just entered / just left
		std::vector<ColShape*> newlyEntered;
		std::vector<ColShape*> newlyLeft;

		// Shapes that are now inside, but weren't before
		for (ColShape* shape : currentInside)
		{
			if (playerInsideColShapes_.find(shape) == playerInsideColShapes_.end())
			{
				newlyEntered.push_back(shape);
			}
		}
		// Shapes that used to be inside, but no longer are
		for (ColShape* shape : playerInsideColShapes_)
		{
			if (currentInside.find(shape) == currentInside.end())
			{
				newlyLeft.push_back(shape);
			}
		}

		// Update the set of shapes we’re inside
		playerInsideColShapes_ = std::move(currentInside);

		// Trigger events
		for (auto* shape : newlyEntered)
		{
			trace("Player entered shape %s\n", shape->id.c_str());
			rec->QueueEvent2("onPlayerEnterColshape", {}, shape->id.c_str());
		}
		for (auto* shape : newlyLeft)
		{
			trace("Player left shape %s\n", shape->id.c_str());
			rec->QueueEvent2("onPlayerLeaveColshape", {}, shape->id.c_str());
		}
	}

private:
	ColShapeManager() = default;

	// If bounding box is huge, automatically treat as infinite
	void MaybeMarkInfinite(ColShape& shape)
	{
		// Example condition: if bounding box is at least 5000 units in width or height
		float width = shape.maxX - shape.minX;
		float height = shape.maxY - shape.minY;

		if (width >= kAutoInfiniteThreshold || height >= kAutoInfiniteThreshold)
		{
			shape.infinite = true;
		}
	}

	void AddToGrid(ColShape* shape)
	{
		int startCx = static_cast<int>(std::floor(shape->minX / cellSize));
		int endCx = static_cast<int>(std::floor(shape->maxX / cellSize));
		int startCy = static_cast<int>(std::floor(shape->minY / cellSize));
		int endCy = static_cast<int>(std::floor(shape->maxY / cellSize));

		shape->occupiedCells.clear();

		for (int cx = startCx; cx <= endCx; ++cx)
		{
			for (int cy = startCy; cy <= endCy; ++cy)
			{
				GridCellKey key{ cx, cy };
				grid_[key].insert(shape);
				shape->occupiedCells.push_back({ cx, cy });
			}
		}
	}

	std::unordered_set<ColShape*> GetColShapesForPosition(const Vector3& pos)
	{
		std::unordered_set<ColShape*> result;
		int cx = static_cast<int>(std::floor(pos.x / cellSize));
		int cy = static_cast<int>(std::floor(pos.y / cellSize));

		GridCellKey key{ cx, cy };
		auto it = grid_.find(key);
		if (it != grid_.end())
		{
			// Insert shapes in this cell
			result.insert(it->second.begin(), it->second.end());
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
				// Circle in XY
				float dx = p.x - shape.pos1.x;
				float dy = p.y - shape.pos1.y;
				if ((dx * dx + dy * dy) > (shape.radius * shape.radius))
					return false;

				// Check Z range
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
			case ColShapeType::Sphere:
			{
				float dx = p.x - shape.pos1.x;
				float dy = p.y - shape.pos1.y;
				float dz = p.z - shape.pos1.z;
				float distSq = dx * dx + dy * dy + dz * dz;
				float rSq = shape.radius * shape.radius;
				return distSq <= rSq;
			}
		}
		return false;
	}

private:
	// colShapeId -> unique_ptr for ownership
	std::unordered_map<std::string, std::unique_ptr<ColShape>> colShapes_;

	// For large shapes, we skip the grid and store them here
	std::unordered_set<ColShape*> infiniteShapes_;

	// The grid: (cx, cy) -> set of shape pointers
	std::unordered_map<GridCellKey, ShapeSet, GridCellKeyHash> grid_;

	// Track which shapes the (local) player is inside
	std::unordered_set<ColShape*> playerInsideColShapes_;
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

	// COLSHAPE_CIRCLE => Creates a 2D circle shape
	fx::ScriptEngine::RegisterNativeHandler("COLSHAPE_CIRCLE", [](fx::ScriptContext& context)
	{
		// Args: colShapeId, x, y, z, radius, (bool infinite)
		std::string colShapeId = context.CheckArgument<const char*>(0);
		float x = context.GetArgument<float>(1);
		float y = context.GetArgument<float>(2);
		float z = context.GetArgument<float>(3);
		float radius = context.GetArgument<float>(4);

		bool infinite = false;
		//if (context.GetArgumentCount() > 5)
		//{
		//	infinite = context.GetArgument<bool>(5);
		//}

		Vector3 center{ x, y, z };
		bool success = ColShapeManager::Get().CreateCircle(colShapeId, center, radius, infinite);
		context.SetResult<bool>(success);
	});

	// COLSHAPE_CUBE => Creates a 3D cube shape
	fx::ScriptEngine::RegisterNativeHandler("COLSHAPE_CUBE", [](fx::ScriptContext& context)
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
		//if (context.GetArgumentCount() > 7)
		//{
		//	infinite = context.GetArgument<bool>(7);
		//}

		Vector3 pos1{ x1, y1, z1 };
		Vector3 pos2{ x2, y2, z2 };
		bool success = ColShapeManager::Get().CreateCube(colShapeId, pos1, pos2, infinite);
		context.SetResult<bool>(success);
	});

	// COLSHAPE_CYLINDER => Creates a cylinder shape
	fx::ScriptEngine::RegisterNativeHandler("COLSHAPE_CYLINDER", [](fx::ScriptContext& context)
	{
		// Args: colShapeId, x, y, z, radius, height, (bool infinite)
		std::string colShapeId = context.CheckArgument<const char*>(0);
		float x = context.GetArgument<float>(1);
		float y = context.GetArgument<float>(2);
		float z = context.GetArgument<float>(3);
		float radius = context.GetArgument<float>(4);
		float height = context.GetArgument<float>(5);

		bool infinite = false;
		//if (context.GetArgumentCount() > 6)
		//{
		//	infinite = context.GetArgument<bool>(6);
		//}

		Vector3 center{ x, y, z };
		bool success = ColShapeManager::Get().CreateCylinder(colShapeId, center, radius, height, infinite);
		context.SetResult<bool>(success);
	});

	// COLSHAPE_RECTANGLE => Creates a rectangle with bottomZ and height in Z
	fx::ScriptEngine::RegisterNativeHandler("COLSHAPE_RECTANGLE", [](fx::ScriptContext& context)
	{
		// Args: colShapeId, x1, y1, x2, y2, bottomZ, height, (bool infinite)
		std::string colShapeId = context.CheckArgument<const char*>(0);
		float x1 = context.GetArgument<float>(1);
		float y1 = context.GetArgument<float>(2);
		float x2 = context.GetArgument<float>(3);
		float y2 = context.GetArgument<float>(4);
		float bottomZ = context.GetArgument<float>(5);
		float height = context.GetArgument<float>(6);

		bool infinite = false;
		//if (context.GetArgumentCount() > 7)
		//{
		//	infinite = context.GetArgument<bool>(7);
		//}

		bool success = ColShapeManager::Get().CreateRectangleZ(colShapeId, x1, y1, x2, y2, bottomZ, height, infinite);
		context.SetResult<bool>(success);
	});

	// COLSHAPE_SPHERE => Creates a 3D sphere shape
	fx::ScriptEngine::RegisterNativeHandler("COLSHAPE_SPHERE", [](fx::ScriptContext& context)
	{
		// Args: colShapeId, x, y, z, radius, (bool infinite)
		std::string colShapeId = context.CheckArgument<const char*>(0);
		float x = context.GetArgument<float>(1);
		float y = context.GetArgument<float>(2);
		float z = context.GetArgument<float>(3);
		float radius = context.GetArgument<float>(4);

		bool infinite = false;
		//if (context.GetArgumentCount() > 5)
		//{
		//	infinite = context.GetArgument<bool>(5);
		//}

		Vector3 center{ x, y, z };
		bool success = ColShapeManager::Get().CreateSphere(colShapeId, center, radius, infinite);
		context.SetResult<bool>(success);
	});

	// COLSHAPE_DELETE => Deletes a colShape by ID
	fx::ScriptEngine::RegisterNativeHandler("COLSHAPE_DELETE", [](fx::ScriptContext& context)
	{
		// Args: colShapeId
		std::string colShapeId = context.CheckArgument<const char*>(0);
		bool success = ColShapeManager::Get().DeleteColShape(colShapeId);
		context.SetResult<bool>(success);
	});

	// Start the colshape thread once scripts are ready
	rage::scrEngine::OnScriptInit.Connect([]()
	{
		colShapeThread.Start();
	});
});
