//
// Created by flowe on 2024-11-18.
//

#ifndef STATICIACORE_F_SPARSESET_H
#define STATICIACORE_F_SPARSESET_H

#include "Concept_Common.h"
#include "F_Entity.h"
#include "U_ErrorMacros.h"
#include <vector>

namespace Core
{
    class F_RawSparseSet
    {
    public:
        virtual ~F_RawSparseSet() = default;

        virtual void DestroyOfDynamic(F_Entity entity) = 0;

        [[nodiscard]]
        virtual I_Component* GetOfDynamic(F_Entity entity) = 0;

        virtual I_Component* CreateForDynamic(F_Entity entity) = 0;
    };

    template<IsComponent TComponent>
    class F_SparseSet final : public F_RawSparseSet
    {
    #pragma pack(push, 1)
        struct DenseBlock
        {
            F_Entity Entity;
            char ComponentData[];
        };

        using SparseBlock = uint32_t; // F_Entity의 비트 배치 규칙 따르되, EntityId는 DenseIndex를 의미함.
    #pragma pack(pop)

    public:
        template<bool Const>
        class IteratorBase final
        {
        public:
            using DataType = std::conditional_t<Const, const TComponent&, TComponent&>;
            using SparseSetType = std::conditional_t<Const, const F_SparseSet&, F_SparseSet&>;
            using iterator_category = std::forward_iterator_tag;
            using value_type = std::pair<F_Entity, DataType>;

            explicit IteratorBase(SparseSetType sparseSet,
                                  const uint32_t denseIndex)
                : sparseSet_{ sparseSet },
                  denseIndex_{ denseIndex }
            {
            }

            bool operator==(const IteratorBase& rhs) const
            {
                // == end()이다. oob이다.
                return denseIndex_ >= sparseSet_.count_;
            }

            bool operator!=(const IteratorBase& rhs) const
            {
                return !(*this == rhs);
            }

            IteratorBase& operator=(const IteratorBase&) = default;

            value_type operator*() const
            {
                const auto [entity, component] = sparseSet_.GetByDenseIndex(denseIndex_);
                return { entity, *component };
            }

            IteratorBase& operator++()
            {
                denseIndex_ = denseIndex_ + 1;
                if constexpr (!Const)
                {
                    denseIndex_ -= static_cast<int>(sparseSet_.shouldInvalidateIterator_);
                    sparseSet_.shouldInvalidateIterator_ = false;
                }

                return *this;
            }

        private:
            SparseSetType sparseSet_;
            uint32_t denseIndex_;
        };

        using Iterator = IteratorBase<false>;

        using ConstIterator = IteratorBase<true>;

        template<bool Const>
        class IterableBase final
        {
        public:
            using SparseSetType = std::conditional_t<Const, const F_SparseSet&, F_SparseSet&>;

            explicit IterableBase() = default;

            explicit IterableBase(SparseSetType sparseSet)
                : sparseSet_{ sparseSet }
            {
            }

            [[nodiscard]]
            ConstIterator cbegin() const
            {
                return ConstIterator{ sparseSet_, 0 };
            }

            [[nodiscard]]
            ConstIterator cend() const
            {
                return ConstIterator{ sparseSet_, NullIndex };
            }

            [[nodiscard]]
            ConstIterator begin() const
            {
                return cbegin();
            }

            [[nodiscard]]
            ConstIterator end() const
            {
                return cend();
            }

            [[nodiscard]]
            Iterator begin() requires !Const
            {
                return Iterator{ sparseSet_, 0 };
            }

            [[nodiscard]]
            Iterator end() requires !Const
            {
                return Iterator{ sparseSet_, NullIndex };
            }

        private:
            SparseSetType sparseSet_;
        };

        using Iterable = IterableBase<false>;

        using ConstIterable = IterableBase<true>;

        static constexpr size_t SparseBlockSize = sizeof(SparseBlock);
        static constexpr size_t SparsePageSize = 16384;
        static constexpr size_t SparseBlocksPerPage = SparsePageSize / SparseBlockSize;
        static constexpr size_t DataSize = sizeof(TComponent);
        static constexpr size_t DenseBlockSize = sizeof(DenseBlock) + DataSize;
        static constexpr size_t DenseBlocksPerPage = 65536 / DenseBlockSize;
        static constexpr size_t DensePageSize = DenseBlockSize * DenseBlocksPerPage;


        explicit F_SparseSet()
            : shouldInvalidateIterator_{ false },
              count_{ 0 }
        {
        }

        ~F_SparseSet() override
        {
            for (char* densePage : densePages_)
            {
                if (densePage)
                {
                    operator delete(densePage);
                }
            }

            for (char* sparsePage : sparsePages_)
            {
                if (sparsePage)
                {
                    operator delete(sparsePage);
                }
            }
        }

        F_SparseSet(const F_SparseSet&) = delete;

        F_SparseSet& operator=(const F_SparseSet&) = delete;

        F_SparseSet(F_SparseSet&&) = delete;

        F_SparseSet& operator=(F_SparseSet&&) = delete;

        [[nodiscard]]
        TComponent* CreateFor(const F_Entity entity)
        {
            const uint32_t sparsePageIndex = entity.GetId() / SparseBlocksPerPage;
            const uint32_t sparseBlockIndex = entity.GetId() % SparseBlocksPerPage;

            // ReSharper disable once CppDFALoopConditionNotUpdated
            while (sparsePageIndex >= sparsePages_.size())
            {
                sparsePages_.push_back(nullptr);
            }

            if (!sparsePages_[sparsePageIndex])
            {
                const auto newPage = static_cast<char*>(operator new(SparsePageSize));
                sparsePages_[sparsePageIndex] = newPage;
                memset(newPage, 0xff, SparsePageSize);
            }
            const auto sparsePage = sparsePages_[sparsePageIndex];
            const auto sparseBlock = reinterpret_cast<SparseBlock*>(sparsePage + sparseBlockIndex * SparseBlockSize);

            const bool isEntityIdDuplicated = F_Entity::ParseIdOf(*sparseBlock) != NullIndex;
            SCRASH_COND_MSG(isEntityIdDuplicated, entity.GetId());

            const uint32_t denseIndex = count_;
            count_ += 1;
            *sparseBlock = entity.GetVersion() << F_Entity::IdBitSize | denseIndex;

            const size_t densePageIndex = denseIndex / DenseBlocksPerPage;
            const size_t denseBlockIndex = denseIndex % DenseBlocksPerPage;

            // ReSharper disable once CppDFALoopConditionNotUpdated
            while (densePageIndex >= densePages_.size())
            {
                densePages_.push_back(static_cast<char*>(operator new(DensePageSize)));
            }
            const auto densePage = densePages_[densePageIndex];
            const auto denseBlock = reinterpret_cast<DenseBlock*>(densePage + denseBlockIndex * DenseBlockSize);
            denseBlock->Entity = entity;

            return new(denseBlock->ComponentData) TComponent;;
        }

        I_Component* CreateForDynamic(const F_Entity entity) override
        {
            return CreateFor(entity);
        }

        /**
         * Godot Node를 확장하는 컴포넌트의 경우 자원 관리 일관성을 위해 반드시 내부 컴포넌트를 정리한 후에 이 함수를 호출하여야 함 - F_SparseSet::CreateComponentFor()에서 생성하지 않기 때문.
         * @param entity
         */
        void DestroyOf(const F_Entity entity)
        {
            const auto [popSparseBlock, popDenseBlock] = GetBlocksOf(entity);
            if (!popSparseBlock || !popDenseBlock)
            {
                return;
            }

            // swap and pop
            const auto lastDenseBlock = reinterpret_cast<DenseBlock*>(
                densePages_[(count_ - 1) / DenseBlocksPerPage] + ((count_ - 1) % DenseBlocksPerPage) * DenseBlockSize);
            const auto lastSparseBlock = reinterpret_cast<SparseBlock*>(
                sparsePages_[lastDenseBlock->Entity.GetId() / SparseBlocksPerPage] + (
                    lastDenseBlock->Entity.GetId() % SparseBlocksPerPage) * SparseBlockSize);
            *lastSparseBlock = *lastSparseBlock & ~((0b1 << F_Entity::IdBitSize) - 1) |
                               F_Entity::ParseIdOf(*popSparseBlock);
            *popSparseBlock = std::numeric_limits<uint32_t>::max(); // Version | DenseIndex 모두 무효화
            popDenseBlock->Entity = lastDenseBlock->Entity;
            *reinterpret_cast<TComponent*>(popDenseBlock->ComponentData) = std::move(
                *reinterpret_cast<TComponent*>(lastDenseBlock->ComponentData));
            reinterpret_cast<TComponent*>(lastDenseBlock->ComponentData)->~TComponent();

            count_ -= 1;
            shouldInvalidateIterator_ = true;
        }

        void DestroyOfDynamic(const F_Entity entity) override
        {
            DestroyOf(entity);
        }

        [[nodiscard]]
        const TComponent* GetOf(const F_Entity entity) const
        {
            const auto [popSparseBlock, popDenseBlock] = GetBlocksOf(entity);
            return popDenseBlock ? reinterpret_cast<const TComponent*>(popDenseBlock->ComponentData) : nullptr;
        }

        [[nodiscard]]
        TComponent* GetOf(const F_Entity entity)
        {
            return const_cast<TComponent*>(static_cast<const F_SparseSet*>(this)->GetOf(entity));
        }

        [[nodiscard]]
        I_Component* GetOfDynamic(const F_Entity entity) override
        {
            return GetOf(entity);
        }

        [[nodiscard]]
        std::pair<F_Entity, const TComponent*> GetByDenseIndex(const uint32_t denseIndex) const
        {
            std::pair<F_Entity, const TComponent*> result
            {
                F_Entity::NullEntity(),
                nullptr
            };

            const auto densePage = denseIndex < count_ ? densePages_[denseIndex / DenseBlocksPerPage] : nullptr;

            const auto denseBlock = densePage ? reinterpret_cast<DenseBlock*>(densePage + (denseIndex % DenseBlocksPerPage) * DenseBlockSize) : nullptr;

            if (denseBlock)
            {
                result.first = denseBlock->Entity;
                result.second = reinterpret_cast<const TComponent*>(denseBlock->ComponentData);
            }

            return result;
        }

        [[nodiscard]]
        std::pair<F_Entity, TComponent*> GetByDenseIndex(const uint32_t denseIndex)
        {
            const auto [entity, constComponent] = static_cast<const F_SparseSet*>(this)->GetByDenseIndex(denseIndex);
            return { entity, const_cast<TComponent*>(constComponent) };
        }

        [[nodiscard]]
        Iterable GetIterable()
        {
            return Iterable{ *this };
        }

        [[nodiscard]]
        ConstIterable GetIterable() const
        {
            return ConstIterable{ *this };
        }

    private:
        static constexpr uint32_t NullIndex = F_Entity::NullId;

        bool shouldInvalidateIterator_;
        size_t count_;
        std::vector<char*> densePages_; // DenseBlock
        std::vector<char*> sparsePages_; // uint32_t, Version(F_Entity::VersionBitSize) | DenseIndex(F_Entity::IdBitSize)

        std::pair<SparseBlock*, DenseBlock*> GetBlocksOf(const F_Entity entity) const
        {
            const uint32_t entityId = entity.GetId();

            const size_t sparsePageIndex = entityId / SparseBlocksPerPage;
            if (entityId == NullIndex || sparsePageIndex >= sparsePages_.size())
            {
                return { nullptr, nullptr };
            }
            const auto sparsePage = sparsePages_[sparsePageIndex];
            if (sparsePageIndex >= sparsePages_.size() || !sparsePage)
            {
                return { nullptr, nullptr };
            }

            const size_t sparseBlockIndex = entityId % SparseBlocksPerPage;
            const auto sparseBlock = reinterpret_cast<SparseBlock*>(
                sparsePages_[sparsePageIndex] + SparseBlockSize * sparseBlockIndex);

            const uint32_t sparseBlockVersion = F_Entity::ParseVersionOf(*sparseBlock);
            const uint32_t sparseBlockDenseIndex = F_Entity::ParseIdOf(*sparseBlock);
            if (entity.GetVersion() != sparseBlockVersion || sparseBlockDenseIndex == NullIndex)
            {
                return { nullptr, nullptr };
            }
            const auto denseBlock = reinterpret_cast<DenseBlock*>(
                densePages_[sparseBlockDenseIndex / DenseBlocksPerPage] + (sparseBlockDenseIndex % DenseBlocksPerPage)
                * DenseBlockSize);

            return { sparseBlock, denseBlock };
        }
    };

}

#endif //STATICIACORE_F_SPARSESET_H
