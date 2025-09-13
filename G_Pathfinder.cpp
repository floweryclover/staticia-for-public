//
// Created by floweryclover on 2025-02-20.
//


#include "G_Pathfinder.h"

#include "F_Executor.h"
#include "F_Threads.h"
#include "G_Map.h"

#include <godot_cpp/variant/vector2i.hpp>
#include <ranges>

using namespace Core;
using namespace M_Pathfind;
using namespace godot;

G_Pathfinder::G_Pathfinder()
    : PerThreadContexts{ std::make_unique<PerThreadContext[]>(F_Threads::GetSingleton().GetThreadCount()) }
{
}

PathHandle G_Pathfinder::Pathfind(const U_TiledDatas<uint32_t>& costDatas,
                                  const Vector2i& from,
                                  const Vector2i& to) const
{
    const auto threadId = F_Threads::GetSingleton().GetCurrentThreadId();
    auto& context = PerThreadContexts[threadId];
    context.AstarSearchVersion += 1;

    const auto mapSize = costDatas.GetSize();
    if (context.AstarSearchNodes.GetSize() != mapSize)
    {
        context.AstarSearchNodes.Resize(mapSize, {});
    }

    /// FROM, TO 범위 체크 ....

    context.AstarSearchQueue.Reset();
    context.AstarSearchNodes.GetDataAt(to) = AstarSearchNode
    {
        to,
        to,
        0,
        GetH(from, to),
        context.AstarSearchVersion
    };
    context.AstarSearchQueue.Push(&context.AstarSearchNodes.GetDataAt(to));

    while (!context.AstarSearchQueue.IsEmpty()
           && context.AstarSearchNodes.GetDataAt(from).Version != context.AstarSearchVersion)
    {
        const auto currentNode = context.AstarSearchQueue.Pop();

        for (const auto [offsetX, offsetY] : DirectionOffsets)
        {
            const auto nearPosition = currentNode->CurrentPosition + Vector2i{ offsetX, offsetY };

            if (!IsValidPosition(mapSize, nearPosition)
                || context.AstarSearchNodes.GetDataAt(nearPosition).Version == context.AstarSearchVersion)
            {
                continue;
            }
            //
            // TODO 현재 벽, 지형지물 등 bool 기반의 장애물 체계에서 비용 기반 체계 변경 중
            // TODO 비용 검사하는 코드 여기 삽입할 것
            //
            auto& nearSearchNode = context.AstarSearchNodes.GetDataAt(nearPosition);
            nearSearchNode.Version = context.AstarSearchVersion;
            nearSearchNode.NextPosition = currentNode->CurrentPosition;
            nearSearchNode.CurrentPosition = nearPosition;
            nearSearchNode.G = currentNode->G + (offsetX != 0 && offsetY != 0 ? 14 : 10);
            nearSearchNode.H = GetH(from, nearPosition);
            context.AstarSearchQueue.Push(&nearSearchNode);
        }
    }

    const auto& fromSearchNode = context.AstarSearchNodes.GetDataAt(from);
    if (fromSearchNode.Version != context.AstarSearchVersion)
    {
        const auto [pathEntryId, pathEntry] = context.AstarPathEntryPool.Emplace(
            uint64_t{ 0 },
            nullptr,
            nullptr,
            from,
            to);
        return PathHandle{ threadId, pathEntryId };
    }

    context.AstarPathMakerStack.clear();
    for (auto currentNode = &context.AstarSearchNodes.GetDataAt(from);
         ;
         currentNode = &context.AstarSearchNodes.GetDataAt(currentNode->NextPosition))
    {
        context.AstarPathMakerStack.push_back(currentNode);
        if (currentNode->CurrentPosition == to)
        {
            break;
        }
    }

    const auto headNode = [this, &context]
    {
        PathNode* nextNode = nullptr;
        for (const auto node : std::views::reverse(context.AstarPathMakerStack))
        {
            const auto currentPathNode = context.AstarPathNodePool.Acquire();
            *currentPathNode = PathNode
            {
                static_cast<uint16_t>(node->CurrentPosition.x),
                static_cast<uint16_t>(node->CurrentPosition.y),
                nextNode
            };
            nextNode = currentPathNode;
        }
        return nextNode;
    }();

    const auto [pathEntryId, pathEntry] = context.AstarPathEntryPool.Emplace(
        uint64_t{ 0 },
        headNode,
        headNode,
        from,
        to);
    return PathHandle{ threadId, pathEntryId };
}

bool G_Pathfinder::CanReach(const U_TiledDatas<uint32_t>& floodFill, const Vector2i& from, const Vector2i& to) const
{
    const auto fromTileData = floodFill.TryGetDataAt(from);
    const auto toTileData = floodFill.TryGetDataAt(to);

    return fromTileData && toTileData &&
           *fromTileData != UnintializedFloodFillCell &&
           *fromTileData == *toTileData;
}


void G_Pathfinder::Process(const F_MutableContext& context)
{
    ProcessImpl(F_Threads::MainThreadId, static_cast<F_ImmutableContext>(context));

    struct Result
    {
    };
    context.Executor.ParallelForWorkerThreads<Result>(
        context,
        [this](const F_ImmutableContext& immutableContext) -> std::optional<Result>
        {
            ProcessImpl(F_Threads::GetSingleton().GetCurrentThreadId(), immutableContext);
            return std::nullopt;
        });
}

void G_Pathfinder::ProcessImpl(const uint32_t threadId, const F_ImmutableContext& context)
{
    auto& threadContext = PerThreadContexts[threadId];
    for (const auto [pathEntryId, pathEntry] : threadContext.AstarPathEntryPool)
    {
        if (pathEntry.ExpiryWorldTick == 0)
        {
            pathEntry.ExpiryWorldTick = context.WorldCurrentTick + 2 * PathEntryRefreshIntervalWorldTick;
        }
        else if (pathEntry.ExpiryWorldTick < context.WorldCurrentTick)
        {
            for (PathNode* currentNode = pathEntry.Head,* nextNode = nullptr; currentNode; currentNode = nextNode)
            {
                nextNode = currentNode->Next;
                threadContext.AstarPathNodePool.Release(currentNode);
            }

            threadContext.AstarPathEntryPool.EraseBySparseIndex(pathEntryId);
        }
    }
}
