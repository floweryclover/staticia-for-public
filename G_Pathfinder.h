//
// Created by floweryclover on 2024-12-17.
//

#ifndef CORE_F_PATHFINDER_H
#define CORE_F_PATHFINDER_H

#include "I_GlobalObject.h"
#include "M_Pathfind.h"
#include "U_PriorityQueue.h"
#include "U_TiledDatas.h"
#include "U_MemoryPool/SparseArray.h"
#include "U_MemoryPool/FreeListPool.h"
#include <memory>
#include <optional>

namespace Core
{
    struct F_ImmutableContext;
}

namespace Core
{
    struct F_MutableContext;
}

namespace Core
{
    class F_Executor;

    struct G_Pathfinder final : I_GlobalObject
    {
        GLOBAL_OBJECT(Core, G_Pathfinder)

    private:
#pragma pack(push, 1)
        struct PathNode final
        {
            uint16_t X;
            uint16_t Y;
            PathNode* Next;
        };

        struct PathEntry final // NOLINT(*-pro-type-member-init)
        {
            uint64_t ExpiryWorldTick;
            PathNode* Head;
            PathNode* Current;
            godot::Vector2i From;
            godot::Vector2i To;
        };

        struct AstarSearchNode // NOLINT(*-pro-type-member-init)
        {
            godot::Vector2i CurrentPosition;
            godot::Vector2i NextPosition;
            uint32_t G;
            uint32_t H;
            uint32_t Version;
        };

#pragma pack(pop)

        struct SearchNodeComparator
        {
            bool operator()(const AstarSearchNode* const lhs, const AstarSearchNode* const rhs) const
            {
                return lhs->G + lhs->H < rhs->G + rhs->H;
            }
        };

        struct PerThreadContext
        {
            uint32_t AstarSearchVersion;
            U_PriorityQueue<AstarSearchNode*, SearchNodeComparator> AstarSearchQueue;
            U_TiledDatas<AstarSearchNode> AstarSearchNodes;
            std::vector<AstarSearchNode*> AstarPathMakerStack;
            U_MemoryPool::FreeListPool<PathNode> AstarPathNodePool;
            U_MemoryPool::SparseArray<PathEntry> AstarPathEntryPool;
        };

    public:
        struct PathContext final
        {
            godot::Vector2i From;
            godot::Vector2i To;
            std::optional<godot::Vector2i> Current;
        };

        explicit G_Pathfinder();

        [[nodiscard]]
        M_Pathfind::PathHandle Pathfind(const U_TiledDatas<uint32_t>& costDatas,
                                        const godot::Vector2i& from,
                                        const godot::Vector2i& to) const;

        [[nodiscard]]
        bool CanReach(const U_TiledDatas<uint32_t>& floodFill, const godot::Vector2i& from, const godot::Vector2i& to) const;

        /**
         * 내부적으로 만료된 엔트리 삭제 등을 진행.
         * @param context
         */
        void Process(const F_MutableContext& context);

        void AdvancePath(const M_Pathfind::PathHandle pathHandle, const uint64_t currentWorldTick)
        {
            const auto pathEntry = GetPathEntry(pathHandle, currentWorldTick);
            if (!pathEntry || !pathEntry->Current)
            {
                return;
            }

            pathEntry->Current = pathEntry->Current->Next;
        }

        [[nodiscard]]
        std::optional<PathContext> GetPathContext(const M_Pathfind::PathHandle pathHandle, const uint64_t currentWorldTick) const
        {
            const auto pathEntry = GetPathEntry(pathHandle, currentWorldTick);
            if (!pathEntry)
            {
                return std::nullopt;
            }

            return PathContext
            {
                pathEntry->From,
                pathEntry->To,
                pathEntry->Current != nullptr
                    ? std::make_optional<godot::Vector2i>(pathEntry->Current->X, pathEntry->Current->Y)
                    : std::nullopt
            };
        }

    private:
        const std::unique_ptr<PerThreadContext[]> PerThreadContexts;

        [[nodiscard]]
        PathEntry* GetPathEntry(const M_Pathfind::PathHandle pathHandle, const uint64_t currentWorldTick) const
        {
            auto& threadContext = PerThreadContexts[pathHandle.GetPathHandlerThreadId()];
            const auto pathEntry = threadContext.AstarPathEntryPool.Get(pathHandle.GetPathEntryId());
            if (pathEntry)
            {
                pathEntry->ExpiryWorldTick = currentWorldTick + 2 * M_Pathfind::PathEntryRefreshIntervalWorldTick;
            }
            return pathEntry;
        }

        void ProcessImpl(uint32_t threadId, const F_ImmutableContext& context);
    };
}

#endif // CORE_F_PATHFINDER_H
