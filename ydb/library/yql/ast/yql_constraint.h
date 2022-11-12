#pragma once

#include <ydb/library/yql/utils/yql_panic.h>

#include <library/cpp/containers/stack_vector/stack_vec.h>
#include <library/cpp/containers/sorted_vector/sorted_vector.h>
#include <library/cpp/json/json_writer.h>

#include <util/generic/strbuf.h>
#include <util/generic/string.h>
#include <util/stream/output.h>

#include <deque>
#include <unordered_map>

namespace NYql {

struct TExprContext;
class TStructExprType;
class TTupleExprType;
class TMultiExprType;
class TVariantExprType;

class TConstraintNode {
protected:
    TConstraintNode(TExprContext& ctx, std::string_view name);
    TConstraintNode(TConstraintNode&& constr);

public:
    using TListType = std::vector<const TConstraintNode*>;

    struct THash {
        size_t operator()(const TConstraintNode* node) const {
            return node->GetHash();
        }
    };

    struct TEqual {
        bool operator()(const TConstraintNode* one, const TConstraintNode* two) const {
            return one->Equals(*two);
        }
    };

    struct TCompare {
        inline bool operator()(const TConstraintNode* l, const TConstraintNode* r) const {
            return l->GetName() < r->GetName();
        }

        inline bool operator()(const std::string_view name, const TConstraintNode* r) const {
            return name < r->GetName();
        }

        inline bool operator()(const TConstraintNode* l, const std::string_view name) const {
            return l->GetName() < name;
        }
    };

    virtual ~TConstraintNode() = default;

    ui64 GetHash() const {
        return Hash_;
    }

    virtual bool Equals(const TConstraintNode& node) const = 0;
    virtual bool Includes(const TConstraintNode& node) const {
        return Equals(node);
    }
    virtual void Out(IOutputStream& out) const;
    virtual void ToJson(NJson::TJsonWriter& out) const = 0;

    template <typename T>
    const T* Cast() const {
        static_assert(std::is_base_of<TConstraintNode, T>::value,
                      "Should be derived from TConstraintNode");

        const auto ret = dynamic_cast<const T*>(this);
        YQL_ENSURE(ret, "Cannot cast " << TString{Name_}.Quote() << " constraint to " << T::Name());
        return ret;
    }

    std::string_view GetName() const {
        return Name_;
    }

protected:
    ui64 Hash_;
    std::string_view Name_;
};

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class TConstraintSet {
public:
    TConstraintSet() = default;
    TConstraintSet(const TConstraintSet&) = default;
    TConstraintSet(TConstraintSet&&) = default;

    TConstraintSet& operator =(const TConstraintSet&) = default;
    TConstraintSet& operator =(TConstraintSet&&) = default;

    template <class TConstraintType>
    const TConstraintType* GetConstraint() const {
        auto res = GetConstraint(TConstraintType::Name());
        return res ? res->template Cast<TConstraintType>() : nullptr;
    }

    template <class TConstraintType>
    const TConstraintType* RemoveConstraint() {
        auto res = RemoveConstraint(TConstraintType::Name());
        return res ? res->template Cast<TConstraintType>() : nullptr;
    }

    const TConstraintNode::TListType& GetAllConstraints() const {
        return Constraints_;
    }

    void Clear() {
        Constraints_.clear();
    }

    bool operator ==(const TConstraintSet& s) const {
        return Constraints_ == s.Constraints_;
    }

    bool operator !=(const TConstraintSet& s) const {
        return Constraints_ != s.Constraints_;
    }

    const TConstraintNode* GetConstraint(std::string_view name) const;
    void AddConstraint(const TConstraintNode* node);
    const TConstraintNode* RemoveConstraint(std::string_view name);

    void ToJson(NJson::TJsonWriter& writer) const;
private:
    TConstraintNode::TListType Constraints_;
};

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class TColumnSetConstraintNodeBase: public TConstraintNode {
public:
    using TSetType = NSorted::TSimpleSet<TStringBuf>;

protected:
    TColumnSetConstraintNodeBase(TExprContext& ctx, TStringBuf name, const TSetType& columns);
    TColumnSetConstraintNodeBase(TExprContext& ctx, TStringBuf name, const std::vector<TStringBuf>& columns);
    TColumnSetConstraintNodeBase(TExprContext& ctx, TStringBuf name, const std::vector<TString>& columns);
    TColumnSetConstraintNodeBase(TColumnSetConstraintNodeBase&& constr);

public:
    const TSetType& GetColumns() const {
        return Columns_;
    }

    bool Equals(const TConstraintNode& node) const override;
    bool Includes(const TConstraintNode& node) const override;
    void Out(IOutputStream& out) const override;
    void ToJson(NJson::TJsonWriter& out) const override;

protected:
    TSetType Columns_;
};

class TSortedConstraintNode final: public TConstraintNode {
public:
    using TContainerType = TSmallVec<std::pair<NSorted::TSimpleSet<std::string_view>, bool>>;

private:
    friend struct TExprContext;

    TSortedConstraintNode(TExprContext& ctx, TContainerType&& content);
    TSortedConstraintNode(TExprContext& ctx, const TSortedConstraintNode& constr, size_t prefixLength);
    TSortedConstraintNode(TSortedConstraintNode&& constr);
public:
    static constexpr std::string_view Name() {
        return "Sorted";
    }

    const TContainerType& GetContent() const {
        return Content_;
    }

    // TODO: deprecated, drop
    const std::vector<TStringBuf> GetColumns() const {
        std::vector<TStringBuf> result;
        result.reserve(Content_.size());
        for (const auto& c : Content_)
            result.emplace_back(c.first.front());
        return result;
    }

    bool Equals(const TConstraintNode& node) const override;
    bool Includes(const TConstraintNode& node) const override;
    void Out(IOutputStream& out) const override;
    void ToJson(NJson::TJsonWriter& out) const override;

    bool IsPrefixOf(const TSortedConstraintNode& node) const;
    size_t GetCommonPrefixLength(const TSortedConstraintNode& node) const;

    const TSortedConstraintNode* CutPrefix(size_t newPrefixLength, TExprContext& ctx) const;

    static const TSortedConstraintNode* MakeCommon(const std::vector<const TConstraintSet*>& constraints, TExprContext& ctx);
    static const TSortedConstraintNode* FilterByType(const TSortedConstraintNode* sorted, const TStructExprType* outItemType, TExprContext& ctx);

protected:
    TContainerType Content_;
};

class TGroupByConstraintNode final: public TColumnSetConstraintNodeBase {
protected:
    friend struct TExprContext;

    TGroupByConstraintNode(TExprContext& ctx, const std::vector<TStringBuf>& columns);
    TGroupByConstraintNode(TExprContext& ctx, const std::vector<TString>& columns);
    TGroupByConstraintNode(TExprContext& ctx, const TGroupByConstraintNode& constr, size_t prefixLength);
    TGroupByConstraintNode(TGroupByConstraintNode&& constr);

    size_t GetCommonPrefixLength(const TGroupByConstraintNode& node) const;
public:
    static constexpr std::string_view Name() {
        return "GroupBy";
    }

    static const TGroupByConstraintNode* MakeCommon(const std::vector<const TConstraintSet*>& constraints, TExprContext& ctx);
};

class TUniqueConstraintNode final: public TColumnSetConstraintNodeBase {
protected:
    friend struct TExprContext;

    TUniqueConstraintNode(TExprContext& ctx, const TSetType& columns);
    TUniqueConstraintNode(TExprContext& ctx, const std::vector<TStringBuf>& columns);
    TUniqueConstraintNode(TExprContext& ctx, const std::vector<TString>& columns);
    TUniqueConstraintNode(TExprContext& ctx, const TStructExprType& itemType);
    TUniqueConstraintNode(TUniqueConstraintNode&& constr);

public:
    static constexpr std::string_view Name() {
        return "Unique";
    }

    bool HasEqualColumns(std::vector<TString> columns) const;
    bool HasEqualColumns(std::vector<TStringBuf> columns) const;

    static const TUniqueConstraintNode* MakeCommon(const std::vector<const TConstraintSet*>& constraints, TExprContext& ctx);
    const TConstraintNode* ExtractField(TExprContext& ctx, const std::string_view& field) const;
};

class TPartOfUniqueConstraintNode final: public TConstraintNode {
public:
    using TKeyType = std::deque<std::string_view>;
    using TPartType = NSorted::TSimpleMap<TKeyType, std::string_view>;
    using TMapType = std::unordered_map<const TUniqueConstraintNode*, TPartType>;
    using TReverseMapType = NSorted::TSimpleMap<std::string_view, std::string_view>;
private:
    friend struct TExprContext;

    TPartOfUniqueConstraintNode(TPartOfUniqueConstraintNode&& constr);
    TPartOfUniqueConstraintNode(TExprContext& ctx, TMapType&& mapping);
public:
    static constexpr std::string_view Name() {
        return "PartOfUnique";
    }

    const TMapType& GetColumnMapping() const;
    TReverseMapType GetReverseMapping() const;

    bool Equals(const TConstraintNode& node) const override;
    bool Includes(const TConstraintNode& node) const override;
    void Out(IOutputStream& out) const override;
    void ToJson(NJson::TJsonWriter& out) const override;

    const TPartOfUniqueConstraintNode* ExtractField(TExprContext& ctx, const std::string_view& field) const;

    const TUniqueConstraintNode* ExtractIfComplete(TExprContext& ctx) const;

    static const TPartOfUniqueConstraintNode* MakeCommon(const std::vector<const TConstraintSet*>& constraints, TExprContext& ctx);
private:
    TMapType Mapping_;
};

class TPassthroughConstraintNode final: public TConstraintNode {
public:
    using TKeyType = std::deque<std::string_view>;
    using TPartType = NSorted::TSimpleMap<TKeyType, std::string_view>;
    using TMapType = std::unordered_map<const TPassthroughConstraintNode*, TPartType>;
    using TReverseMapType = NSorted::TSimpleMap<std::string_view, std::string_view>;
private:
    friend struct TExprContext;

    TPassthroughConstraintNode(TExprContext& ctx, const TStructExprType& itemType);
    TPassthroughConstraintNode(TExprContext& ctx, const TTupleExprType& itemType);
    TPassthroughConstraintNode(TExprContext& ctx, const TMultiExprType& itemType);
    TPassthroughConstraintNode(TPassthroughConstraintNode&& constr);
    TPassthroughConstraintNode(TExprContext& ctx, TMapType&& mapping);
public:
    static constexpr std::string_view Name() {
        return "Passthrough";
    }

    const TMapType& GetColumnMapping() const;
    TReverseMapType GetReverseMapping() const;

    bool Equals(const TConstraintNode& node) const override;
    bool Includes(const TConstraintNode& node) const override;
    void Out(IOutputStream& out) const override;
    void ToJson(NJson::TJsonWriter& out) const override;

    const TPassthroughConstraintNode* ExtractField(TExprContext& ctx, const std::string_view& field) const;

    static const TPassthroughConstraintNode* MakeCommon(const std::vector<const TConstraintSet*>& constraints, TExprContext& ctx);
private:
    TMapType Mapping_;
};

class TEmptyConstraintNode final: public TConstraintNode {
protected:
    friend struct TExprContext;

    TEmptyConstraintNode(TExprContext& ctx);
    TEmptyConstraintNode(TEmptyConstraintNode&& constr);

public:
    static constexpr std::string_view Name() {
        return "Empty";
    }

    bool Equals(const TConstraintNode& node) const override;
    void ToJson(NJson::TJsonWriter& out) const override;

    static const TEmptyConstraintNode* MakeCommon(const std::vector<const TConstraintSet*>& constraints, TExprContext& ctx);
};

class TVarIndexConstraintNode final: public TConstraintNode {
public:
    using TMapType = NSorted::TSimpleMap<ui32, ui32>;

protected:
    friend struct TExprContext;

    TVarIndexConstraintNode(TExprContext& ctx, const TMapType& mapping);
    TVarIndexConstraintNode(TExprContext& ctx, const TVariantExprType& itemType);
    TVarIndexConstraintNode(TExprContext& ctx, size_t mapItemsCount);
    TVarIndexConstraintNode(TVarIndexConstraintNode&& constr);

public:
    static constexpr std::string_view Name() {
        return "VarIndex";
    }

    // multimap: result index -> {original indices}
    const TMapType& GetIndexMapping() const {
        return Mapping_;
    }

    // original index -> {result indices}
    TMapType GetReverseMapping() const;

    bool Equals(const TConstraintNode& node) const override;
    bool Includes(const TConstraintNode& node) const override;
    void Out(IOutputStream& out) const override;
    void ToJson(NJson::TJsonWriter& out) const override;

    static const TVarIndexConstraintNode* MakeCommon(const std::vector<const TConstraintSet*>& constraints, TExprContext& ctx);

private:
    TMapType Mapping_;
};

class TMultiConstraintNode: public TConstraintNode {
public:
    struct TConstraintKey {
        TStringBuf operator()(const TConstraintNode* node) const {
            return node->GetName();
        }
    };
    using TMapType = NSorted::TSimpleMap<ui32, TConstraintSet>;

public:
    TMultiConstraintNode(TExprContext& ctx, const TMapType& items);
    TMultiConstraintNode(TExprContext& ctx, ui32 index, const TConstraintSet& constraints);
    TMultiConstraintNode(TMultiConstraintNode&& constr);

public:
    static constexpr std::string_view Name() {
        return "Multi";
    }

    const TMapType& GetItems() const {
        return Items_;
    }

    const TConstraintSet* GetItem(ui32 index) const {
        return Items_.FindPtr(index);
    }

    bool Equals(const TConstraintNode& node) const override;
    bool Includes(const TConstraintNode& node) const override;
    void Out(IOutputStream& out) const override;
    void ToJson(NJson::TJsonWriter& out) const override;

    static const TMultiConstraintNode* MakeCommon(const std::vector<const TConstraintSet*>& constraints, TExprContext& ctx);

    bool FilteredIncludes(const TConstraintNode& node, const THashSet<TString>& blacklist) const;
protected:
    TMapType Items_;
};

} // namespace NYql

