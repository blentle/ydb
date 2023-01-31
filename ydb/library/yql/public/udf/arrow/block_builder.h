#pragma once

#include "util.h"
#include "bit_util.h"

#include <ydb/library/yql/public/udf/udf_type_inspection.h>

#include <arrow/datum.h>
#include <arrow/c/bridge.h>

namespace NYql {
namespace NUdf {

class IArrayBuilder {
public:
    virtual ~IArrayBuilder() = default;
    virtual size_t MaxLength() const = 0;
    virtual void Add(NUdf::TUnboxedValuePod value) = 0;
    virtual void Add(TBlockItem value) = 0;
    virtual void AddMany(const arrow::ArrayData& array, size_t popCount, const ui8* sparseBitmap, size_t bitmapSize) = 0;
    virtual arrow::Datum Build(bool finish) = 0;
};

class IScalarBuilder {
public:
    virtual ~IScalarBuilder() = default;
    virtual arrow::Datum Build(TBlockItem value) const = 0;
};

inline std::shared_ptr<arrow::DataType> GetArrowType(const ITypeInfoHelper& typeInfoHelper, const TType* type) {
    auto arrowTypeHandle = typeInfoHelper.MakeArrowType(type);
    Y_ENSURE(arrowTypeHandle);
    ArrowSchema s;
    arrowTypeHandle->Export(&s);
    return ARROW_RESULT(arrow::ImportType(&s));
}

class TArrayBuilderBase : public IArrayBuilder {
public:
    using Ptr = std::unique_ptr<TArrayBuilderBase>;

    struct TBlockArrayTree {
        using Ptr = std::shared_ptr<TBlockArrayTree>;
        std::deque<std::shared_ptr<arrow::ArrayData>> Payload;
        std::vector<TBlockArrayTree::Ptr> Children;
    };

    TArrayBuilderBase(const ITypeInfoHelper& typeInfoHelper, const TType* type, arrow::MemoryPool& pool, size_t maxLen)
        : ArrowType(GetArrowType(typeInfoHelper, type))
        , Pool(&pool)
        , MaxLen(maxLen)
        , MaxBlockSizeInBytes(typeInfoHelper.GetMaxBlockBytes())
    {
        Y_VERIFY(ArrowType);
        Y_VERIFY(maxLen > 0);
    }

    size_t MaxLength() const final {
        return MaxLen;
    }

    void Add(NUdf::TUnboxedValuePod value) final {
        Y_VERIFY(CurrLen < MaxLen);
        DoAdd(value);
        CurrLen++;
    }

    void Add(TBlockItem value) final {
        Y_VERIFY(CurrLen < MaxLen);
        DoAdd(value);
        CurrLen++;
    }


    void AddDefault() {
        Y_VERIFY(CurrLen < MaxLen);
        DoAddDefault();
        CurrLen++;
    }

    void AddMany(const arrow::ArrayData& array, size_t popCount, const ui8* sparseBitmap, size_t bitmapSize) final {
        Y_VERIFY(size_t(array.length) == bitmapSize);
        Y_VERIFY(popCount <= bitmapSize);
        Y_VERIFY(CurrLen + popCount <= MaxLen);

        if (popCount) {
            DoAddMany(array, sparseBitmap, popCount);
        }

        CurrLen += popCount;
    }

    arrow::Datum Build(bool finish) final {
        auto tree = BuildTree(finish);
        TVector<std::shared_ptr<arrow::ArrayData>> chunks;
        while (size_t size = CalcSliceSize(*tree)) {
            chunks.push_back(Slice(*tree, size));
        }

        return MakeArray(chunks);
    }

    TBlockArrayTree::Ptr BuildTree(bool finish) {
        auto result = DoBuildTree(finish);
        CurrLen = 0;
        return result;
    }
protected:
    virtual void DoAdd(NUdf::TUnboxedValuePod value) = 0;
    virtual void DoAdd(TBlockItem value) = 0;
    virtual void DoAddDefault() = 0;
    virtual void DoAddMany(const arrow::ArrayData& array, const ui8* sparseBitmap, size_t popCount) = 0;
    virtual TBlockArrayTree::Ptr DoBuildTree(bool finish) = 0;

private:
    static size_t CalcSliceSize(const TBlockArrayTree& tree) {
        if (tree.Payload.empty()) {
            return 0;
        }

        if (!tree.Children.empty()) {
            Y_VERIFY(tree.Payload.size() == 1);
            size_t result = std::numeric_limits<size_t>::max();
            for (auto& child : tree.Children) {
                size_t childSize = CalcSliceSize(*child);
                result = std::min(result, childSize);
            }
            Y_VERIFY(result <= size_t(tree.Payload.front()->length));
            return result;
        }

        int64_t result = std::numeric_limits<int64_t>::max();
        for (auto& data : tree.Payload) {
            result = std::min(result, data->length);
        }

        Y_VERIFY(result > 0);
        return static_cast<size_t>(result);
    }

    static std::shared_ptr<arrow::ArrayData> Slice(TBlockArrayTree& tree, size_t size) {
        Y_VERIFY(size > 0);

        Y_VERIFY(!tree.Payload.empty());
        auto& main = tree.Payload.front();
        std::shared_ptr<arrow::ArrayData> sliced;
        if (size == size_t(main->length)) {
            sliced = main;
            tree.Payload.pop_front();
        } else {
            Y_VERIFY(size < size_t(main->length));
            sliced = Chop(main, size);
        }

        if (!tree.Children.empty()) {
            std::vector<std::shared_ptr<arrow::ArrayData>> children;
            for (auto& child : tree.Children) {
                children.push_back(Slice(*child, size));
            }

            sliced->child_data = std::move(children);
            if (tree.Payload.empty()) {
                tree.Children.clear();
            }
        }
        return sliced;
    }

protected:
    size_t GetCurrLen() const {
        return CurrLen;
    }

    const std::shared_ptr<arrow::DataType> ArrowType;
    arrow::MemoryPool* const Pool;
    const size_t MaxLen;
    const size_t MaxBlockSizeInBytes;
private:
    size_t CurrLen = 0;
};

template <typename T, bool Nullable>
class TFixedSizeArrayBuilder : public TArrayBuilderBase {
public:
    TFixedSizeArrayBuilder(const ITypeInfoHelper& typeInfoHelper, const TType* type, arrow::MemoryPool& pool, size_t maxLen)
        : TArrayBuilderBase(typeInfoHelper, type, pool, maxLen)
    {
        Reserve();
    }

    void DoAdd(NUdf::TUnboxedValuePod value) final {
        if constexpr (Nullable) {
            if (!value) {
                return DoAdd(TBlockItem{});
            }
        }
        DoAdd(TBlockItem(value.Get<T>()));
    }

    void DoAdd(TBlockItem value) final {
        if constexpr (Nullable) {
            if (!value) {
                NullBuilder->UnsafeAppend(0);
                DataBuilder->UnsafeAppend(T{});
                return;
            }
            NullBuilder->UnsafeAppend(1);
        }

        DataBuilder->UnsafeAppend(value.As<T>());
    }

    void DoAddDefault() final {
        if constexpr (Nullable) {
            NullBuilder->UnsafeAppend(1);
        }
        DataBuilder->UnsafeAppend(T{});
    }

    void DoAddMany(const arrow::ArrayData& array, const ui8* sparseBitmap, size_t popCount) final {
        Y_VERIFY(array.buffers.size() > 1);
        if constexpr (Nullable) {
            Y_VERIFY(NullBuilder->Length() == DataBuilder->Length());
            if (array.buffers.front()) {
                ui8* dstBitmap = NullBuilder->End();
                CompressAsSparseBitmap(array.GetValues<ui8>(0, 0), array.offset, sparseBitmap, dstBitmap, array.length);
                NullBuilder->UnsafeAdvance(popCount);
            } else {
                NullBuilder->UnsafeAppend(popCount, 1);
            }
        }

        const T* src = array.GetValues<T>(1);
        T* dst = DataBuilder->End();
        CompressArray(src, sparseBitmap, dst, array.length);
        DataBuilder->UnsafeAdvance(popCount);
    }

    TBlockArrayTree::Ptr DoBuildTree(bool finish) final {
        const size_t len = DataBuilder->Length();
        std::shared_ptr<arrow::Buffer> nulls;
        if constexpr (Nullable) {
            Y_VERIFY(NullBuilder->Length() == len);
            nulls = NullBuilder->Finish();
            nulls = MakeDenseBitmap(nulls->data(), len, Pool);
        }
        std::shared_ptr<arrow::Buffer> data = DataBuilder->Finish();

        TBlockArrayTree::Ptr result = std::make_shared<TBlockArrayTree>();
        result->Payload.push_back(arrow::ArrayData::Make(ArrowType, len, {nulls, data}));

        NullBuilder.reset();
        DataBuilder.reset();
        if (!finish) {
            Reserve();
        }
        return result;
    }

private:
    void Reserve() {
        DataBuilder = std::make_unique<TTypedBufferBuilder<T>>(Pool);
        DataBuilder->Reserve(MaxLen + 1);
        if constexpr (Nullable) {
            NullBuilder = std::make_unique<TTypedBufferBuilder<ui8>>(Pool);
            NullBuilder->Reserve(MaxLen + 1);
        }
    }

    std::unique_ptr<TTypedBufferBuilder<ui8>> NullBuilder;
    std::unique_ptr<TTypedBufferBuilder<T>> DataBuilder;
};

template<typename TStringType, bool Nullable>
class TStringArrayBuilder : public TArrayBuilderBase {
public:
    using TOffset = typename TStringType::offset_type;

    TStringArrayBuilder(const ITypeInfoHelper& typeInfoHelper, const TType* type, arrow::MemoryPool& pool, size_t maxLen)
        : TArrayBuilderBase(typeInfoHelper, type, pool, maxLen)
    {
        Reserve();
    }

    void DoAdd(NUdf::TUnboxedValuePod value) final {
        if constexpr (Nullable) {
            if (!value) {
                return DoAdd(TBlockItem{});
            }
        }

        DoAdd(TBlockItem(value.AsStringRef()));
    }

    void DoAdd(TBlockItem value) final {
        if constexpr (Nullable) {
            if (!value) {
                NullBuilder->UnsafeAppend(0);
                AppendCurrentOffset();
                return;
            }
        }

        const std::string_view str = value.AsStringRef();

        size_t currentLen = DataBuilder->Length();
        // empty string can always be appended
        if (!str.empty() && currentLen + str.size() > MaxBlockSizeInBytes) {
            if (currentLen) {
                FlushChunk(false);
            }
            if (str.size() > MaxBlockSizeInBytes) {
                DataBuilder->Reserve(str.size());
            }
        }

        AppendCurrentOffset();
        DataBuilder->UnsafeAppend((const ui8*)str.data(), str.size());
        if constexpr (Nullable) {
            NullBuilder->UnsafeAppend(1);
        }
    }


    void DoAddDefault() final {
        if constexpr (Nullable) {
            NullBuilder->UnsafeAppend(1);
        }
        AppendCurrentOffset();
    }

    void DoAddMany(const arrow::ArrayData& array, const ui8* sparseBitmap, size_t popCount) final {
        Y_UNUSED(popCount);
        Y_VERIFY(array.buffers.size() > 2);
        Y_VERIFY(!Nullable || NullBuilder->Length() == OffsetsBuilder->Length());

        const ui8* srcNulls = array.GetValues<ui8>(0, 0);
        const TOffset* srcOffset = array.GetValues<TOffset>(1);
        const ui8* srcData = array.GetValues<ui8>(2, 0);

        const ui8* chunkStart = srcData;
        const ui8* chunkEnd = chunkStart;
        size_t dataLen = DataBuilder->Length();

        ui8* dstNulls = Nullable ? NullBuilder->End() : nullptr;
        TOffset* dstOffset = OffsetsBuilder->End();
        size_t countAdded = 0;
        for (size_t i = 0; i < size_t(array.length); i++) {
            if (!sparseBitmap[i]) {
                continue;
            }

            const ui8* begin = srcData + srcOffset[i];
            const ui8* end   = srcData + srcOffset[i + 1];
            const size_t strSize = end - begin;

            size_t availBytes = std::max(dataLen, MaxBlockSizeInBytes)  - dataLen;

            for (;;) {
                // try to append ith string
                if (strSize <= availBytes) {
                    if (begin == chunkEnd)  {
                        chunkEnd = end;
                    } else {
                        DataBuilder->UnsafeAppend(chunkStart, chunkEnd - chunkStart);
                        chunkStart = begin;
                        chunkEnd = end;
                    }

                    size_t nullOffset = i + array.offset;
                    if constexpr (Nullable) {
                        *dstNulls++ = srcNulls ? ((srcNulls[nullOffset >> 3] >> (nullOffset & 7)) & 1) : 1u;
                    }
                    *dstOffset++ = dataLen;

                    dataLen += strSize;
                    ++countAdded;

                    break;
                }

                if (dataLen) {
                    if (chunkStart != chunkEnd) {
                        DataBuilder->UnsafeAppend(chunkStart, chunkEnd - chunkStart);
                        chunkStart = chunkEnd = srcData;
                    }
                    Y_VERIFY(dataLen == DataBuilder->Length());
                    OffsetsBuilder->UnsafeAdvance(countAdded);
                    if constexpr (Nullable) {
                        NullBuilder->UnsafeAdvance(countAdded);
                    }
                    FlushChunk(false);

                    dataLen = 0;
                    countAdded = 0;
                    if constexpr (Nullable) {
                        dstNulls = NullBuilder->End();
                    }
                    dstOffset = OffsetsBuilder->End();
                } else {
                    DataBuilder->Reserve(strSize);
                    availBytes = strSize;
                }
            }
        }
        if (chunkStart != chunkEnd) {
            DataBuilder->UnsafeAppend(chunkStart, chunkEnd - chunkStart);
        }
        Y_VERIFY(dataLen == DataBuilder->Length());
        OffsetsBuilder->UnsafeAdvance(countAdded);
        if constexpr (Nullable) {
            NullBuilder->UnsafeAdvance(countAdded);
        }
    }

    TBlockArrayTree::Ptr DoBuildTree(bool finish) final {
        FlushChunk(finish);
        TBlockArrayTree::Ptr result = std::make_shared<TBlockArrayTree>();
        result->Payload = std::move(Chunks);
        Chunks.clear();
        return result;
    }

private:
    void Reserve() {
        if constexpr (Nullable) {
            NullBuilder = std::make_unique<TTypedBufferBuilder<ui8>>(Pool);
            NullBuilder->Reserve(MaxLen + 1);
        }
        OffsetsBuilder = std::make_unique<TTypedBufferBuilder<TOffset>>(Pool);
        OffsetsBuilder->Reserve(MaxLen + 1);
        DataBuilder = std::make_unique<TTypedBufferBuilder<ui8>>(Pool);
        DataBuilder->Reserve(MaxBlockSizeInBytes);
    }

    void AppendCurrentOffset() {
        OffsetsBuilder->UnsafeAppend(DataBuilder->Length());
    }

    void FlushChunk(bool finish) {
        const auto length = OffsetsBuilder->Length();
        Y_VERIFY(length > 0);

        AppendCurrentOffset();
        std::shared_ptr<arrow::Buffer> nullBitmap;
        if constexpr (Nullable) {
            nullBitmap = NullBuilder->Finish();
            nullBitmap = MakeDenseBitmap(nullBitmap->data(), length, Pool);
        }
        std::shared_ptr<arrow::Buffer> offsets = OffsetsBuilder->Finish();
        std::shared_ptr<arrow::Buffer> data = DataBuilder->Finish();

        auto arrowType = std::make_shared<TStringType>();
        Chunks.push_back(arrow::ArrayData::Make(arrowType, length, { nullBitmap, offsets, data }));
        if (!finish) {
            Reserve();
        }
    }

    std::unique_ptr<TTypedBufferBuilder<ui8>> NullBuilder;
    std::unique_ptr<TTypedBufferBuilder<TOffset>> OffsetsBuilder;
    std::unique_ptr<TTypedBufferBuilder<ui8>> DataBuilder;

    std::deque<std::shared_ptr<arrow::ArrayData>> Chunks;
};

template<bool Nullable>
class TTupleArrayBuilder : public TArrayBuilderBase {
public:
    TTupleArrayBuilder(const ITypeInfoHelper& typeInfoHelper, const TType* type, arrow::MemoryPool& pool, size_t maxLen,
                       TVector<TArrayBuilderBase::Ptr>&& children)
        : TArrayBuilderBase(typeInfoHelper, type, pool, maxLen)
        , Children(std::move(children))
    {
        Reserve();
    }

    void DoAdd(NUdf::TUnboxedValuePod value) final {
        if constexpr (Nullable) {
            if (!value) {
                NullBuilder->UnsafeAppend(0);
                for (ui32 i = 0; i < Children.size(); ++i) {
                    Children[i]->AddDefault();
                }
                return;
            }
            NullBuilder->UnsafeAppend(1);
        }

        auto elements = value.GetElements();
        if (elements) {
            for (ui32 i = 0; i < Children.size(); ++i) {
                Children[i]->Add(elements[i]);
            }
        } else {
            for (ui32 i = 0; i < Children.size(); ++i) {
                auto element = value.GetElement(i);
                Children[i]->Add(element);
            }
        }
    }

    void DoAdd(TBlockItem value) final {
        if constexpr (Nullable) {
            if (!value) {
                NullBuilder->UnsafeAppend(0);
                for (ui32 i = 0; i < Children.size(); ++i) {
                    Children[i]->AddDefault();
                }
                return;
            }
            NullBuilder->UnsafeAppend(1);
        }

        auto elements = value.AsTuple();
        for (ui32 i = 0; i < Children.size(); ++i) {
            Children[i]->Add(elements[i]);
        }
    }

    void DoAddDefault() final {
        if constexpr (Nullable) {
            NullBuilder->UnsafeAppend(1);
        }
        for (ui32 i = 0; i < Children.size(); ++i) {
            Children[i]->AddDefault();
        }
    }

    void DoAddMany(const arrow::ArrayData& array, const ui8* sparseBitmap, size_t popCount) final {
        Y_VERIFY(!array.buffers.empty());
        Y_VERIFY(array.child_data.size() == Children.size());

        if constexpr (Nullable) {
            if (array.buffers.front()) {
                ui8* dstBitmap = NullBuilder->End();
                CompressAsSparseBitmap(array.GetValues<ui8>(0, 0), array.offset, sparseBitmap, dstBitmap, array.length);
                NullBuilder->UnsafeAdvance(popCount);
            } else {
                NullBuilder->UnsafeAppend(popCount, 1);
            }
        }

        for (size_t i = 0; i < Children.size(); ++i) {
            Children[i]->AddMany(*array.child_data[i], popCount, sparseBitmap, array.length);
        }
    }

    TBlockArrayTree::Ptr DoBuildTree(bool finish) final {
        TBlockArrayTree::Ptr result = std::make_shared<TBlockArrayTree>();

        std::shared_ptr<arrow::Buffer> nullBitmap;
        const size_t length = GetCurrLen();
        if constexpr (Nullable) {
            Y_ENSURE(length == NullBuilder->Length(), "Unexpected NullBuilder length");
            nullBitmap = NullBuilder->Finish();
            nullBitmap = MakeDenseBitmap(nullBitmap->data(), length, Pool);
        }

        Y_VERIFY(length);
        result->Payload.push_back(arrow::ArrayData::Make(ArrowType, length, { nullBitmap }));
        result->Children.reserve(Children.size());
        for (ui32 i = 0; i < Children.size(); ++i) {
            result->Children.emplace_back(Children[i]->BuildTree(finish));
        }

        if (!finish) {
            Reserve();
        }

        return result;
    }

private:
    void Reserve() {
        if constexpr (Nullable) {
            NullBuilder = std::make_unique<TTypedBufferBuilder<ui8>>(Pool);
            NullBuilder->Reserve(MaxLen + 1);
        }
    }

private:
    TVector<std::unique_ptr<TArrayBuilderBase>> Children;
    std::unique_ptr<TTypedBufferBuilder<ui8>> NullBuilder;
};

class TExternalOptionalArrayBuilder : public TArrayBuilderBase {
public:
    TExternalOptionalArrayBuilder(const ITypeInfoHelper& typeInfoHelper, const TType* type, arrow::MemoryPool& pool, size_t maxLen, std::unique_ptr<TArrayBuilderBase>&& inner)
        : TArrayBuilderBase(typeInfoHelper, type, pool, maxLen)
        , Inner(std::move(inner))
    {
        Reserve();
    }

    void DoAdd(NUdf::TUnboxedValuePod value) final {
        if (!value) {
            NullBuilder->UnsafeAppend(0);
            Inner->AddDefault();
            return;
        }

        NullBuilder->UnsafeAppend(1);
        Inner->Add(value.GetOptionalValue());
    }

    void DoAdd(TBlockItem value) final {
        if (!value) {
            NullBuilder->UnsafeAppend(0);
            Inner->AddDefault();
            return;
        }

        NullBuilder->UnsafeAppend(1);
        Inner->Add(value.GetOptionalValue());
    }

    void DoAddDefault() final {
        NullBuilder->UnsafeAppend(1);
        Inner->AddDefault();
    }

    void DoAddMany(const arrow::ArrayData& array, const ui8* sparseBitmap, size_t popCount) final {
        Y_VERIFY(!array.buffers.empty());
        Y_VERIFY(array.child_data.size() == 1);

        if (array.buffers.front()) {
            ui8* dstBitmap = NullBuilder->End();
            CompressAsSparseBitmap(array.GetValues<ui8>(0, 0), array.offset, sparseBitmap, dstBitmap, array.length);
            NullBuilder->UnsafeAdvance(popCount);
        } else {
            NullBuilder->UnsafeAppend(popCount, 1);
        }

        Inner->AddMany(*array.child_data[0], popCount, sparseBitmap, array.length);
    }

    TBlockArrayTree::Ptr DoBuildTree(bool finish) final {
        TBlockArrayTree::Ptr result = std::make_shared<TBlockArrayTree>();

        std::shared_ptr<arrow::Buffer> nullBitmap;
        const size_t length = GetCurrLen();
        Y_ENSURE(length == NullBuilder->Length(), "Unexpected NullBuilder length");
        nullBitmap = NullBuilder->Finish();
        nullBitmap = MakeDenseBitmap(nullBitmap->data(), length, Pool);

        Y_VERIFY(length);
        result->Payload.push_back(arrow::ArrayData::Make(ArrowType, length, { nullBitmap }));
        result->Children.emplace_back(Inner->BuildTree(finish));

        if (!finish) {
            Reserve();
        }

        return result;
    }

private:
    void Reserve() {
        NullBuilder = std::make_unique<TTypedBufferBuilder<ui8>>(Pool);
        NullBuilder->Reserve(MaxLen + 1);
    }

private:
    std::unique_ptr<TArrayBuilderBase> Inner;
    std::unique_ptr<TTypedBufferBuilder<ui8>> NullBuilder;
};

std::unique_ptr<TArrayBuilderBase> MakeArrayBuilderBase(const ITypeInfoHelper& typeInfoHelper, const TType* type, arrow::MemoryPool& pool, size_t maxBlockLength);

template<bool Nullable>
inline std::unique_ptr<TArrayBuilderBase> MakeArrayBuilderImpl(const ITypeInfoHelper& typeInfoHelper, const TType* type, arrow::MemoryPool& pool, size_t maxLen) {
    if constexpr (Nullable) {
        TOptionalTypeInspector typeOpt(typeInfoHelper, type);
        type = typeOpt.GetItemType();
    }

    TTupleTypeInspector typeTuple(typeInfoHelper, type);
    if (typeTuple) {
        TVector<std::unique_ptr<TArrayBuilderBase>> children;
        for (ui32 i = 0; i < typeTuple.GetElementsCount(); ++i) {
            const TType* childType = typeTuple.GetElementType(i);
            auto childBuilder = MakeArrayBuilderBase(typeInfoHelper, childType, pool, maxLen);
            children.push_back(std::move(childBuilder));
        }

        return std::make_unique<TTupleArrayBuilder<Nullable>>(typeInfoHelper, type, pool, maxLen, std::move(children));
    }

    TDataTypeInspector typeData(typeInfoHelper, type);
    if (typeData) {
        auto typeId = typeData.GetTypeId();
        switch (GetDataSlot(typeId)) {
        case NUdf::EDataSlot::Int8:
            return std::make_unique<TFixedSizeArrayBuilder<i8, Nullable>>(typeInfoHelper, type, pool, maxLen);
        case NUdf::EDataSlot::Uint8:
        case NUdf::EDataSlot::Bool:
            return std::make_unique<TFixedSizeArrayBuilder<ui8, Nullable>>(typeInfoHelper, type, pool, maxLen);
        case NUdf::EDataSlot::Int16:
            return std::make_unique<TFixedSizeArrayBuilder<i16, Nullable>>(typeInfoHelper, type, pool, maxLen);
        case NUdf::EDataSlot::Uint16:
        case NUdf::EDataSlot::Date:
            return std::make_unique<TFixedSizeArrayBuilder<ui16, Nullable>>(typeInfoHelper, type, pool, maxLen);
        case NUdf::EDataSlot::Int32:
            return std::make_unique<TFixedSizeArrayBuilder<i32, Nullable>>(typeInfoHelper, type, pool, maxLen);
        case NUdf::EDataSlot::Uint32:
        case NUdf::EDataSlot::Datetime:
            return std::make_unique<TFixedSizeArrayBuilder<ui32, Nullable>>(typeInfoHelper, type, pool, maxLen);
        case NUdf::EDataSlot::Int64:
        case NUdf::EDataSlot::Interval:
            return std::make_unique<TFixedSizeArrayBuilder<i64, Nullable>>(typeInfoHelper, type, pool, maxLen);
        case NUdf::EDataSlot::Uint64:
        case NUdf::EDataSlot::Timestamp:
            return std::make_unique<TFixedSizeArrayBuilder<ui64, Nullable>>(typeInfoHelper, type, pool, maxLen);
        case NUdf::EDataSlot::Float:
            return std::make_unique<TFixedSizeArrayBuilder<float, Nullable>>(typeInfoHelper, type, pool, maxLen);
        case NUdf::EDataSlot::Double:
            return std::make_unique<TFixedSizeArrayBuilder<double, Nullable>>(typeInfoHelper, type, pool, maxLen);
        case NUdf::EDataSlot::String:
            return std::make_unique<TStringArrayBuilder<arrow::BinaryType, Nullable>>(typeInfoHelper, type, pool, maxLen);
        case NUdf::EDataSlot::Utf8:
            return std::make_unique<TStringArrayBuilder<arrow::StringType, Nullable>>(typeInfoHelper, type, pool, maxLen);
        default:
            Y_ENSURE(false, "Unsupported data slot");
        }
    }

    Y_ENSURE(false, "Unsupported type");
}

inline std::unique_ptr<TArrayBuilderBase> MakeArrayBuilderBase(const ITypeInfoHelper& typeInfoHelper, const TType* type, arrow::MemoryPool& pool, size_t maxBlockLength) {
    const TType* unpacked = type;
    TOptionalTypeInspector typeOpt(typeInfoHelper, type);
    if (typeOpt) {
        unpacked = typeOpt.GetItemType();
    }

    TOptionalTypeInspector unpackedOpt(typeInfoHelper, unpacked);
    if (unpackedOpt) {
        // at least 2 levels of optionals
        ui32 nestLevel = 0;
        auto currentType = type;
        auto previousType = type;
        TVector<const TType*> types;
        for (;;) {
            ++nestLevel;
            previousType = currentType;
            types.push_back(currentType);
            TOptionalTypeInspector currentOpt(typeInfoHelper, currentType);
            currentType = currentOpt.GetItemType();
            TOptionalTypeInspector nexOpt(typeInfoHelper, currentType);
            if (!nexOpt) {
                break;
            }
        }

        auto builder = MakeArrayBuilderBase(typeInfoHelper, previousType, pool, maxBlockLength);
        for (ui32 i = 1; i < nestLevel; ++i) {
            builder = std::make_unique<TExternalOptionalArrayBuilder>(typeInfoHelper, types[nestLevel - 1 - i], pool, maxBlockLength, std::move(builder));
        }

        return builder;
    } else {
        if (typeOpt) {
            return MakeArrayBuilderImpl<true>(typeInfoHelper, type, pool, maxBlockLength);
        } else {
            return MakeArrayBuilderImpl<false>(typeInfoHelper, type, pool, maxBlockLength);
        }
    }
}

inline std::unique_ptr<IArrayBuilder> MakeArrayBuilder(const ITypeInfoHelper& typeInfoHelper, const TType* type, arrow::MemoryPool& pool, size_t maxBlockLength) {
    return MakeArrayBuilderBase(typeInfoHelper, type, pool, maxBlockLength);
}

inline std::unique_ptr<IScalarBuilder> MakeScalarBuilder(const ITypeInfoHelper& typeInfoHelper, const TType* type) {
    Y_UNUSED(typeInfoHelper);
    Y_UNUSED(type);
    Y_ENSURE(false);
    return nullptr;
}

}
}
