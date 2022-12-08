#include "object.h"

#include <ydb/core/tx/tiering/tier/checker.h>

#include <ydb/services/metadata/manager/ydb_value_operator.h>
#include <ydb/services/metadata/secret/fetcher.h>

#include <library/cpp/json/writer/json_value.h>
#include <library/cpp/protobuf/json/proto2json.h>

namespace NKikimr::NColumnShard::NTiers {

NJson::TJsonValue TTierConfig::GetDebugJson() const {
    NJson::TJsonValue result = NJson::JSON_MAP;
    result.InsertValue(TDecoder::TierName, TierName);
    NProtobufJson::Proto2Json(ProtoConfig, result.InsertValue(TDecoder::TierConfig, NJson::JSON_MAP));
    return result;
}

bool TTierConfig::IsSame(const TTierConfig& item) const {
    return TierName == item.TierName && ProtoConfig.SerializeAsString() == item.ProtoConfig.SerializeAsString();
}

bool TTierConfig::DeserializeFromRecord(const TDecoder& decoder, const Ydb::Value& r) {
    if (!decoder.Read(decoder.GetTierNameIdx(), TierName, r)) {
        return false;
    }
    if (!decoder.ReadDebugProto(decoder.GetTierConfigIdx(), ProtoConfig, r)) {
        return false;
    }
    return true;
}

NKikimr::NMetadataManager::TTableRecord TTierConfig::SerializeToRecord() const {
    NMetadataManager::TTableRecord result;
    result.SetColumn(TDecoder::TierName, NMetadataManager::TYDBValue::Bytes(TierName));
    result.SetColumn(TDecoder::TierConfig, NMetadataManager::TYDBValue::Bytes(ProtoConfig.DebugString()));
    return result;
}

TString TTierConfig::GetInternalStorageTablePath() {
    return "tiering/tiers";
}

void TTierConfig::AlteringPreparation(std::vector<TTierConfig>&& objects,
    NMetadataManager::IAlterPreparationController<TTierConfig>::TPtr controller,
    const NMetadata::IOperationsManager::TModificationContext& context)
{
    TActivationContext::Register(new TTierPreparationActor(std::move(objects), controller, context));
}

NMetadata::TOperationParsingResult TTierConfig::BuildPatchFromSettings(const NYql::TObjectSettingsImpl& settings,
    const NMetadata::IOperationsManager::TModificationContext& /*context*/) {
    NKikimr::NMetadataManager::TTableRecord result;
    result.SetColumn(TDecoder::TierName, NMetadataManager::TYDBValue::Bytes(settings.GetObjectId()));
    {
        auto it = settings.GetFeatures().find(TDecoder::TierConfig);
        if (it != settings.GetFeatures().end()) {
            TTierProto proto;
            if (!::google::protobuf::TextFormat::ParseFromString(it->second, &proto)) {
                return "incorrect proto format";
            } else {
                result.SetColumn(TDecoder::TierConfig, NMetadataManager::TYDBValue::Bytes(it->second));
            }
        }
    }
    return result;
}

NKikimrSchemeOp::TS3Settings TTierConfig::GetPatchedConfig(
    std::shared_ptr<NMetadata::NSecret::TSnapshot> secrets) const
{
    auto config = ProtoConfig.GetObjectStorage();
    if (secrets) {
        secrets->PatchString(*config.MutableAccessKey());
        secrets->PatchString(*config.MutableSecretKey());
    }
    return config;
}

std::vector<Ydb::Column> TTierConfig::TDecoder::GetPKColumns() {
    return { NMetadataManager::TYDBColumn::Bytes(TierName) };
}

std::vector<Ydb::Column> TTierConfig::TDecoder::GetColumns() {
    return
    {
        NMetadataManager::TYDBColumn::Bytes(TierName),
        NMetadataManager::TYDBColumn::Bytes(TierConfig)
    };
}

std::vector<TString> TTierConfig::TDecoder::GetPKColumnIds() {
    return { TierName };
}

}
