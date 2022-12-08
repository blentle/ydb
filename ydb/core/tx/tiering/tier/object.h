#pragma once
#include <ydb/core/protos/flat_scheme_op.pb.h>
#include <ydb/services/metadata/abstract/decoder.h>
#include <ydb/services/metadata/manager/preparation_controller.h>
#include <ydb/services/metadata/manager/table_record.h>
#include <ydb/services/metadata/manager/object.h>
#include <ydb/services/metadata/service.h>

#include <library/cpp/json/writer/json_value.h>

namespace NKikimr::NMetadata::NSecret {
class TSnapshot;
}

namespace NKikimr::NColumnShard::NTiers {

class TTierConfig: public NMetadataManager::TObject<TTierConfig> {
private:
    using TTierProto = NKikimrSchemeOp::TStorageTierConfig;
    YDB_ACCESSOR_DEF(TString, TierName);
    YDB_ACCESSOR_DEF(TTierProto, ProtoConfig);
public:
    TTierConfig() = default;
    TTierConfig(const TString& tierName)
        : TierName(tierName)
    {

    }

    static TString GetInternalStorageTablePath();
    NKikimrSchemeOp::TS3Settings GetPatchedConfig(std::shared_ptr<NMetadata::NSecret::TSnapshot> secrets) const;

    class TDecoder: public NInternal::TDecoderBase {
    private:
        YDB_READONLY(i32, TierNameIdx, -1);
        YDB_READONLY(i32, TierConfigIdx, -1);
    public:
        static inline const TString TierName = "tierName";
        static inline const TString TierConfig = "tierConfig";
        static std::vector<Ydb::Column> GetPKColumns();
        static std::vector<Ydb::Column> GetColumns();
        static std::vector<TString> GetPKColumnIds();
        TDecoder(const Ydb::ResultSet& rawData) {
            TierNameIdx = GetFieldIndex(rawData, TierName);
            TierConfigIdx = GetFieldIndex(rawData, TierConfig);
        }
    };
    bool DeserializeFromRecord(const TDecoder& decoder, const Ydb::Value& r);
    NMetadataManager::TTableRecord SerializeToRecord() const;
    static NMetadata::TOperationParsingResult BuildPatchFromSettings(const NYql::TObjectSettingsImpl& settings,
        const NMetadata::IOperationsManager::TModificationContext& context);

    static void AlteringPreparation(std::vector<TTierConfig>&& objects,
        NMetadataManager::IAlterPreparationController<TTierConfig>::TPtr controller,
        const NMetadata::IOperationsManager::TModificationContext& context);

    bool NeedExport() const {
        return ProtoConfig.HasObjectStorage();
    }
    bool IsSame(const TTierConfig& item) const;
    NJson::TJsonValue GetDebugJson() const;
    static TString GetTypeId() {
        return "TIER";
    }
};

}
