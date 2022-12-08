#pragma once
#include <ydb/library/accessor/accessor.h>
#include <ydb/core/base/appdata.h>

#include <ydb/services/metadata/abstract/decoder.h>
#include <ydb/services/metadata/abstract/manager.h>
#include <ydb/services/metadata/manager/object.h>
#include <ydb/services/metadata/manager/preparation_controller.h>
#include <ydb/services/metadata/secret/secret.h>

namespace NKikimr::NMetadata::NSecret {

class TAccess: public TSecretId, public NMetadataManager::TObject<TAccess> {
private:
    using TBase = NMetadataManager::TObject<TAccess>;
    YDB_ACCESSOR_DEF(TString, AccessSID);
public:
    class TDecoder: public NInternal::TDecoderBase {
    private:
        YDB_ACCESSOR(i32, OwnerUserIdIdx, -1);
        YDB_ACCESSOR(i32, SecretIdIdx, -1);
        YDB_ACCESSOR(i32, AccessSIDIdx, -1);
    public:
        static inline const TString OwnerUserId = "ownerUserId";
        static inline const TString SecretId = "secretId";
        static inline const TString AccessSID = "accessSID";
        static std::vector<TString> GetPKColumnIds();
        static std::vector<Ydb::Column> GetPKColumns();
        static std::vector<Ydb::Column> GetColumns();

        TDecoder(const Ydb::ResultSet& rawData) {
            OwnerUserIdIdx = GetFieldIndex(rawData, OwnerUserId);
            SecretIdIdx = GetFieldIndex(rawData, SecretId);
            AccessSIDIdx = GetFieldIndex(rawData, AccessSID);
        }
    };

    using TBase::TBase;

    static void AlteringPreparation(std::vector<TAccess>&& objects,
        NMetadataManager::IAlterPreparationController<TAccess>::TPtr controller,
        const NMetadata::IOperationsManager::TModificationContext& context);
    static TString GetInternalStorageTablePath();
    bool DeserializeFromRecord(const TDecoder& decoder, const Ydb::Value& rawValue);
    NMetadataManager::TTableRecord SerializeToRecord() const;

    static NMetadata::TOperationParsingResult BuildPatchFromSettings(const NYql::TObjectSettingsImpl& settings,
        const NMetadata::IOperationsManager::TModificationContext& context);
    static TString GetTypeId() {
        return "SECRET_ACCESS";
    }
};

}
