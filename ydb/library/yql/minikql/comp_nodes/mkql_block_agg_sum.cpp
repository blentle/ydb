#include "mkql_block_agg_sum.h"

#include <ydb/library/yql/minikql/mkql_node_builder.h>
#include <ydb/library/yql/minikql/mkql_node_cast.h>

#include <ydb/library/yql/minikql/computation/mkql_computation_node_holders.h>

#include <arrow/scalar.h>

namespace NKikimr {
namespace NMiniKQL {

template <typename TIn, typename TSum, typename TInScalar>
class TSumBlockAggregatorNullableOrScalar : public TBlockAggregatorBase {
public:
    struct TState {
        TSum Sum_ = 0;
        bool IsValid_ = false;
    };

    TSumBlockAggregatorNullableOrScalar(std::optional<ui32> filterColumn, ui32 argColumn)
        : TBlockAggregatorBase(sizeof(TState), filterColumn)
        , ArgColumn_(argColumn)
    {
    }

    void InitState(void* state) final {
        new(state) TState();
    }

    void AddMany(void* state, const NUdf::TUnboxedValue* columns, ui64 batchLength, std::optional<ui64> filtered) final {
        auto typedState = static_cast<TState*>(state);
        const auto& datum = TArrowBlock::From(columns[ArgColumn_]).GetDatum();
        if (datum.is_scalar()) {
            if (datum.scalar()->is_valid) {
                typedState->Sum_ += (filtered ? *filtered : batchLength) * datum.scalar_as<TInScalar>().value;
                typedState->IsValid_ = true;
            }
        } else {
            const auto& array = datum.array();
            auto ptr = array->GetValues<TIn>(1);
            auto len = array->length;
            auto count = len - array->GetNullCount();
            if (!count) {
                return;
            }

            if (!filtered) {
                typedState->IsValid_ = true;
                TSum sum = typedState->Sum_;
                if (array->GetNullCount() == 0) {
                    for (int64_t i = 0; i < len; ++i) {
                        sum += ptr[i];
                    }
                } else {
                    auto nullBitmapPtr = array->GetValues<uint8_t>(0, 0);
                    for (int64_t i = 0; i < len; ++i) {
                        ui64 fullIndex = i + array->offset;
                        // bit 1 -> mask 0xFF..FF, bit 0 -> mask 0x00..00
                        TIn mask = (((nullBitmapPtr[fullIndex >> 3] >> (fullIndex & 0x07)) & 1) ^ 1) - TIn(1);
                        sum += (ptr[i] & mask);
                    }
                }

                typedState->Sum_ = sum;
            } else {
                const auto& filterDatum = TArrowBlock::From(columns[*FilterColumn_]).GetDatum();
                const auto& filterArray = filterDatum.array();
                MKQL_ENSURE(filterArray->GetNullCount() == 0, "Expected non-nullable bool column");
                auto filterBitmap = filterArray->template GetValues<uint8_t>(1, 0);
                TSum sum = typedState->Sum_;
                if (array->GetNullCount() == 0) {
                    typedState->IsValid_ = true;
                    for (int64_t i = 0; i < len; ++i) {
                        ui64 fullIndex = i + array->offset;
                        // bit 1 -> mask 0xFF..FF, bit 0 -> mask 0x00..00
                        TIn filterMask = (((filterBitmap[fullIndex >> 3] >> (fullIndex & 0x07)) & 1) ^ 1) - TIn(1);
                        sum += ptr[i] & filterMask;
                    }
                } else {
                    ui64 count = 0;
                    auto nullBitmapPtr = array->template GetValues<uint8_t>(0, 0);
                    for (int64_t i = 0; i < len; ++i) {
                        ui64 fullIndex = i + array->offset;
                        // bit 1 -> mask 0xFF..FF, bit 0 -> mask 0x00..00
                        TIn mask = (((nullBitmapPtr[fullIndex >> 3] >> (fullIndex & 0x07)) & 1) ^ 1) - TIn(1);
                        TIn filterMask = (((filterBitmap[fullIndex >> 3] >> (fullIndex & 0x07)) & 1) ^ 1) - TIn(1);
                        mask &= filterMask;
                        sum += (ptr[i] & mask);
                        count += mask & 1;
                    }

                    typedState->IsValid_ = typedState->IsValid_ || count > 0;
                }

                typedState->Sum_ = sum;
            }
        }
    }

    NUdf::TUnboxedValue FinishOne(const void* state) final {
        auto typedState = static_cast<const TState*>(state);
        if (!typedState->IsValid_) {
            return NUdf::TUnboxedValuePod();
        }

        return NUdf::TUnboxedValuePod(typedState->Sum_);
    }

private:
    const ui32 ArgColumn_;
};

template <typename TIn, typename TSum, typename TInScalar>
class TSumBlockAggregator : public TBlockAggregatorBase {
public:
    struct TState {
        TSum Sum_ = 0;
    };

    TSumBlockAggregator(std::optional<ui32> filterColumn, ui32 argColumn)
        : TBlockAggregatorBase(sizeof(TState), filterColumn)
        , ArgColumn_(argColumn)
    {
    }

    void InitState(void* state) final {
        new(state) TState();
    }

    void AddMany(void* state, const NUdf::TUnboxedValue* columns, ui64 batchLength, std::optional<ui64> filtered) final {
        auto typedState = static_cast<TState*>(state);
        Y_UNUSED(batchLength);
        const auto& datum = TArrowBlock::From(columns[ArgColumn_]).GetDatum();
        MKQL_ENSURE(datum.is_array(), "Expected array");
        const auto& array = datum.array();
        auto ptr = array->GetValues<TIn>(1);
        auto len = array->length;
        MKQL_ENSURE(array->GetNullCount() == 0, "Expected no nulls");
        MKQL_ENSURE(len > 0, "Expected at least one value");

        TSum sum = typedState->Sum_;
        if (!filtered) {
            for (int64_t i = 0; i < len; ++i) {
                sum += ptr[i];
            }
        } else {
            const auto& filterDatum = TArrowBlock::From(columns[*FilterColumn_]).GetDatum();
            const auto& filterArray = filterDatum.array();
            MKQL_ENSURE(filterArray->GetNullCount() == 0, "Expected non-nullable bool column");
            auto filterBitmap = filterArray->template GetValues<uint8_t>(1, 0);
            for (int64_t i = 0; i < len; ++i) {
                ui64 fullIndex = i + array->offset;
                // bit 1 -> mask 0xFF..FF, bit 0 -> mask 0x00..00
                TIn filterMask = (((filterBitmap[fullIndex >> 3] >> (fullIndex & 0x07)) & 1) ^ 1) - TIn(1);
                sum += ptr[i] & filterMask;
            }
        }

        typedState->Sum_ = sum;
    }

    NUdf::TUnboxedValue FinishOne(const void* state) final {
        auto typedState = static_cast<const TState*>(state);
        return NUdf::TUnboxedValuePod(typedState->Sum_);
    }

private:
    const ui32 ArgColumn_;
};

template <typename TIn, typename TInScalar>
class TAvgBlockAggregator : public TBlockAggregatorBase {
public:
    struct TState {
        double Sum_ = 0;
        ui64 Count_ = 0;
    };

    TAvgBlockAggregator(std::optional<ui32> filterColumn, ui32 argColumn, const THolderFactory& holderFactory)
        : TBlockAggregatorBase(sizeof(TState), filterColumn)
        , ArgColumn_(argColumn)
        , HolderFactory_(holderFactory)
    {
    }

    void InitState(void* state) final {
        new(state) TState();
    }

    void AddMany(void* state, const NUdf::TUnboxedValue* columns, ui64 batchLength, std::optional<ui64> filtered) final {
        auto typedState = static_cast<TState*>(state);
        const auto& datum = TArrowBlock::From(columns[ArgColumn_]).GetDatum();
        if (datum.is_scalar()) {
            if (datum.scalar()->is_valid) {
                typedState->Sum_ += double((filtered ? *filtered : batchLength) * datum.scalar_as<TInScalar>().value);
                typedState->Count_ += batchLength;
            }
        } else {
            const auto& array = datum.array();
            auto ptr = array->GetValues<TIn>(1);
            auto len = array->length;
            auto count = len - array->GetNullCount();
            if (!count) {
                return;
            }

            if (!filtered) {
                typedState->Count_ += count;
                double sum = typedState->Sum_;
                if (array->GetNullCount() == 0) {
                    for (int64_t i = 0; i < len; ++i) {
                        sum += double(ptr[i]);
                    }
                } else {
                    auto nullBitmapPtr = array->GetValues<uint8_t>(0, 0);
                    for (int64_t i = 0; i < len; ++i) {
                        ui64 fullIndex = i + array->offset;
                        // bit 1 -> mask 0xFF..FF, bit 0 -> mask 0x00..00
                        TIn mask = (((nullBitmapPtr[fullIndex >> 3] >> (fullIndex & 0x07)) & 1) ^ 1) - TIn(1);
                        sum += double(ptr[i] & mask);
                    }
                }

                typedState->Sum_ = sum;
            } else {
                const auto& filterDatum = TArrowBlock::From(columns[*FilterColumn_]).GetDatum();
                const auto& filterArray = filterDatum.array();
                MKQL_ENSURE(filterArray->GetNullCount() == 0, "Expected non-nullable bool column");
                auto filterBitmap = filterArray->template GetValues<uint8_t>(1, 0);

                double sum = typedState->Sum_;
                ui64 count = typedState->Count_;
                if (array->GetNullCount() == 0) {
                    for (int64_t i = 0; i < len; ++i) {
                        ui64 fullIndex = i + array->offset;
                        // bit 1 -> mask 0xFF..FF, bit 0 -> mask 0x00..00
                        TIn filterMask = (((filterBitmap[fullIndex >> 3] >> (fullIndex & 0x07)) & 1) ^ 1) - TIn(1);
                        sum += double(ptr[i] & filterMask);
                        count += filterMask & 1;
                    }
                } else {
                    auto nullBitmapPtr = array->GetValues<uint8_t>(0, 0);
                    for (int64_t i = 0; i < len; ++i) {
                        ui64 fullIndex = i + array->offset;
                        // bit 1 -> mask 0xFF..FF, bit 0 -> mask 0x00..00
                        TIn mask = (((nullBitmapPtr[fullIndex >> 3] >> (fullIndex & 0x07)) & 1) ^ 1) - TIn(1);
                        TIn filterMask = (((filterBitmap[fullIndex >> 3] >> (fullIndex & 0x07)) & 1) ^ 1) - TIn(1);
                        mask &= filterMask;
                        sum += double(ptr[i] & mask);
                        count += mask & 1;
                    }
                }

                typedState->Sum_ = sum;
                typedState->Count_ = count;
            }
        }
    }

    NUdf::TUnboxedValue FinishOne(const void* state) final {
        auto typedState = static_cast<const TState*>(state);
        if (!typedState->Count_) {
            return NUdf::TUnboxedValuePod();
        }

        NUdf::TUnboxedValue* items;
        auto arr = HolderFactory_.CreateDirectArrayHolder(2, items);
        items[0] = NUdf::TUnboxedValuePod(typedState->Sum_);
        items[1] = NUdf::TUnboxedValuePod(typedState->Count_);
        return arr;
    }

private:
    const ui32 ArgColumn_;
    const THolderFactory& HolderFactory_;
};

class TBlockSumFactory : public IBlockAggregatorFactory {
public:
   std::unique_ptr<IBlockAggregator> Make(
       TTupleType* tupleType,
       std::optional<ui32> filterColumn,
       const std::vector<ui32>& argsColumns,
       const THolderFactory& holderFactory) const final {
       Y_UNUSED(holderFactory);
       auto blockType = AS_TYPE(TBlockType, tupleType->GetElementType(argsColumns[0]));
       auto argType = blockType->GetItemType();
       bool isOptional;
       auto dataType = UnpackOptionalData(argType, isOptional);
       if (blockType->GetShape() == TBlockType::EShape::Scalar || isOptional) {
           switch (*dataType->GetDataSlot()) {
           case NUdf::EDataSlot::Int8:
               return std::make_unique<TSumBlockAggregatorNullableOrScalar<i8, i64, arrow::Int8Scalar>>(filterColumn, argsColumns[0]);
           case NUdf::EDataSlot::Uint8:
               return std::make_unique<TSumBlockAggregatorNullableOrScalar<ui8, ui64, arrow::UInt8Scalar>>(filterColumn, argsColumns[0]);
           case NUdf::EDataSlot::Int16:
               return std::make_unique<TSumBlockAggregatorNullableOrScalar<i16, i64, arrow::Int16Scalar>>(filterColumn, argsColumns[0]);
           case NUdf::EDataSlot::Uint16:
           case NUdf::EDataSlot::Date:
               return std::make_unique<TSumBlockAggregatorNullableOrScalar<ui16, ui64, arrow::UInt16Scalar>>(filterColumn, argsColumns[0]);
           case NUdf::EDataSlot::Int32:
               return std::make_unique<TSumBlockAggregatorNullableOrScalar<i32, i64, arrow::Int32Scalar>>(filterColumn, argsColumns[0]);
           case NUdf::EDataSlot::Uint32:
           case NUdf::EDataSlot::Datetime:
               return std::make_unique<TSumBlockAggregatorNullableOrScalar<ui32, ui64, arrow::UInt32Scalar>>(filterColumn, argsColumns[0]);
           case NUdf::EDataSlot::Int64:
           case NUdf::EDataSlot::Interval:
               return std::make_unique<TSumBlockAggregatorNullableOrScalar<i64, i64, arrow::Int64Scalar>>(filterColumn, argsColumns[0]);
           case NUdf::EDataSlot::Uint64:
           case NUdf::EDataSlot::Timestamp:
               return std::make_unique<TSumBlockAggregatorNullableOrScalar<ui64, ui64, arrow::UInt64Scalar>>(filterColumn, argsColumns[0]);
           default:
               throw yexception() << "Unsupported SUM input type";
           }
       } else {
           switch (*dataType->GetDataSlot()) {
           case NUdf::EDataSlot::Int8:
               return std::make_unique<TSumBlockAggregator<i8, i64, arrow::Int8Scalar>>(filterColumn, argsColumns[0]);
           case NUdf::EDataSlot::Uint8:
               return std::make_unique<TSumBlockAggregator<ui8, ui64, arrow::UInt8Scalar>>(filterColumn, argsColumns[0]);
           case NUdf::EDataSlot::Int16:
               return std::make_unique<TSumBlockAggregator<i16, i64, arrow::Int16Scalar>>(filterColumn, argsColumns[0]);
           case NUdf::EDataSlot::Uint16:
           case NUdf::EDataSlot::Date:
               return std::make_unique<TSumBlockAggregator<ui16, ui64, arrow::UInt16Scalar>>(filterColumn, argsColumns[0]);
           case NUdf::EDataSlot::Int32:
               return std::make_unique<TSumBlockAggregator<i32, i64, arrow::Int32Scalar>>(filterColumn, argsColumns[0]);
           case NUdf::EDataSlot::Uint32:
           case NUdf::EDataSlot::Datetime:
               return std::make_unique<TSumBlockAggregator<ui32, ui64, arrow::UInt32Scalar>>(filterColumn, argsColumns[0]);
           case NUdf::EDataSlot::Int64:
           case NUdf::EDataSlot::Interval:
               return std::make_unique<TSumBlockAggregator<i64, i64, arrow::Int64Scalar>>(filterColumn, argsColumns[0]);
           case NUdf::EDataSlot::Uint64:
           case NUdf::EDataSlot::Timestamp:
               return std::make_unique<TSumBlockAggregator<ui64, ui64, arrow::UInt64Scalar>>(filterColumn, argsColumns[0]);
           default:
               throw yexception() << "Unsupported SUM input type";
           }
       }
   }
};

class TBlockAvgFactory : public IBlockAggregatorFactory {
public:
   std::unique_ptr<IBlockAggregator> Make(
       TTupleType* tupleType,
       std::optional<ui32> filterColumn,
       const std::vector<ui32>& argsColumns,
       const THolderFactory& holderFactory) const final {
       auto argType = AS_TYPE(TBlockType, tupleType->GetElementType(argsColumns[0]))->GetItemType();
       bool isOptional;
       auto dataType = UnpackOptionalData(argType, isOptional);
       switch (*dataType->GetDataSlot()) {
       case NUdf::EDataSlot::Int8:
           return std::make_unique<TAvgBlockAggregator<i8, arrow::Int8Scalar>>(filterColumn, argsColumns[0], holderFactory);
       case NUdf::EDataSlot::Uint8:
           return std::make_unique<TAvgBlockAggregator<ui8, arrow::UInt8Scalar>>(filterColumn, argsColumns[0], holderFactory);
       case NUdf::EDataSlot::Int16:
           return std::make_unique<TAvgBlockAggregator<i16, arrow::Int16Scalar>>(filterColumn, argsColumns[0], holderFactory);
       case NUdf::EDataSlot::Uint16:
       case NUdf::EDataSlot::Date:
           return std::make_unique<TAvgBlockAggregator<ui16, arrow::UInt16Scalar>>(filterColumn, argsColumns[0], holderFactory);
       case NUdf::EDataSlot::Int32:
           return std::make_unique<TAvgBlockAggregator<i32, arrow::Int32Scalar>>(filterColumn, argsColumns[0], holderFactory);
       case NUdf::EDataSlot::Uint32:
       case NUdf::EDataSlot::Datetime:
           return std::make_unique<TAvgBlockAggregator<ui32, arrow::UInt32Scalar>>(filterColumn, argsColumns[0], holderFactory);
       case NUdf::EDataSlot::Int64:
       case NUdf::EDataSlot::Interval:
           return std::make_unique<TAvgBlockAggregator<i64, arrow::Int64Scalar>>(filterColumn, argsColumns[0], holderFactory);
       case NUdf::EDataSlot::Uint64:
       case NUdf::EDataSlot::Timestamp:
           return std::make_unique<TAvgBlockAggregator<ui64, arrow::UInt64Scalar>>(filterColumn, argsColumns[0], holderFactory);
       default:
           throw yexception() << "Unsupported AVG input type";
       }
   }
};

std::unique_ptr<IBlockAggregatorFactory> MakeBlockSumFactory() {
    return std::make_unique<TBlockSumFactory>();
}

std::unique_ptr<IBlockAggregatorFactory> MakeBlockAvgFactory() {
    return std::make_unique<TBlockAvgFactory>();
}
 
}
}
