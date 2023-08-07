#pragma once
#include <ydb/core/tx/columnshard/hooks/abstract/abstract.h>

namespace NKikimr::NYDBTest::NColumnShard {

class TController: public ICSController {
private:
    YDB_READONLY(TAtomicCounter, SortingWithLimit, 0);
    YDB_READONLY(TAtomicCounter, AnySorting, 0);
    YDB_READONLY(TAtomicCounter, FilteredRecordsCount, 0);
    YDB_READONLY(TAtomicCounter, InternalCompactions, 0);
    YDB_READONLY(TAtomicCounter, SplitCompactions, 0);
protected:
    virtual bool DoOnSortingPolicy(std::shared_ptr<NOlap::NIndexedReader::IOrderPolicy> policy) override;
    virtual bool DoOnAfterFilterAssembling(const std::shared_ptr<arrow::RecordBatch>& batch) override;
    virtual bool DoOnStartCompaction(const std::shared_ptr<NOlap::TColumnEngineChanges>& changes) override;

public:
    bool HasPKSortingOnly() const;
    bool HasCompactions() const {
        return SplitCompactions.Val() + InternalCompactions.Val();
    }
};

}
