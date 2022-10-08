#include "yql_kikimr_gateway.h"

#include <ydb/library/yql/public/issue/yql_issue_message.h>
#include <ydb/library/yql/providers/common/proto/gateways_config.pb.h>
#include <ydb/library/yql/parser/pg_wrapper/interface/type_desc.h>
#include <ydb/library/yql/utils/yql_panic.h>

#include <ydb/core/base/table_index.h>

#include <util/string/split.h>

namespace NYql {

using namespace NThreading;

static void CreateDirs(std::shared_ptr<TVector<TString>> partsHolder, size_t index,
    TPromise<IKikimrGateway::TGenericResult>& promise, IKikimrGateway::TCreateDirFunc createDir)
{
    auto partPromise = NewPromise<IKikimrGateway::TGenericResult>();
    auto& parts = *partsHolder;

    partPromise.GetFuture().Apply(
        [partsHolder, index, promise, createDir](const TFuture<IKikimrGateway::TGenericResult>& future) mutable {
            auto& result = future.GetValue();
            if (index == partsHolder->size() - 1) {
                promise.SetValue(result);
                return;
            }

            if (!result.Success() && result.Status() != TIssuesIds::KIKIMR_ACCESS_DENIED) {
                promise.SetValue(result);
                return;
            }

            CreateDirs(partsHolder, index + 1, promise, createDir);
        });

    TString basePath = IKikimrGateway::CombinePath(parts.begin(), parts.begin() + index);

    createDir(basePath, parts[index], partPromise);
}

TKikimrClusterMapping::TKikimrClusterMapping(const TKikimrGatewayConfig& config) {
    THashMap<TString, TString> defaultSettings;
    for (auto& setting : config.GetDefaultSettings()) {
        defaultSettings[setting.GetName()] = setting.GetValue();
    }

    for (size_t i = 0; i < config.ClusterMappingSize(); ++i) {
        const TKikimrClusterConfig& cluster = config.GetClusterMapping(i);

        auto name = cluster.GetName();

        if (Clusters.contains(name)) {
            ythrow yexception() << "TKikimrGatewayConfig: Duplicate cluster name: " << name;
        }

        Clusters[name] = cluster;

        if (cluster.GetDefault()) {
            if (DefaultClusterName) {
                ythrow yexception() << "TKikimrGatewayConfig: More than one default cluster (current: "
                    << name << ", previous: " << DefaultClusterName << ")";
            }
            DefaultClusterName = name;
        }

        ClusterSettings[name] = defaultSettings;
        for (auto& setting : cluster.GetSettings()) {
            ClusterSettings[name][setting.GetName()] = setting.GetValue();
        }
    }
}

void TKikimrClusterMapping::GetAllClusterNames(TVector<TString>& names) const {
    names.clear();
    for (const auto& c: Clusters) {
        names.push_back(c.first);
    }
}

const TKikimrClusterConfig& TKikimrClusterMapping::GetClusterConfig(const TString& name) const {
    if (const TKikimrClusterConfig* config = Clusters.FindPtr(name)) {
        return *config;
    } else {
        throw yexception() << "Unknown cluster name: " << name;
    }
}

TMaybe<TString> TKikimrClusterMapping::GetClusterSetting(const TString& cluster, const TString& name) const {
    auto clusterSettings = ClusterSettings.FindPtr(cluster);
    YQL_ENSURE(clusterSettings);

    auto setting = clusterSettings->FindPtr(name);
    return setting ? *setting : TMaybe<TString>();
}

TString TKikimrClusterMapping::GetDefaultClusterName() const {
    if (!DefaultClusterName) {
        ythrow yexception() << "TKikimrGatewayConfig: No default cluster";
    }

    return DefaultClusterName;
}

bool TKikimrClusterMapping::HasCluster(const TString& cluster) const {
    return Clusters.contains(cluster);
}

TKikimrPathId TKikimrPathId::Parse(const TStringBuf& str) {
    TStringBuf ownerStr;
    TStringBuf idStr;
    YQL_ENSURE(str.TrySplit(':', ownerStr, idStr));

    return TKikimrPathId(FromString<ui64>(ownerStr), FromString<ui64>(idStr));
}

TString IKikimrGateway::CanonizePath(const TString& path) {
    if (path.empty()) {
        return "/";
    }

    if (path[0] != '/') {
        return "/" + path;
    }

    return path;
}

TVector<TString> IKikimrGateway::SplitPath(const TString& path) {
    TVector<TString> parts;
    Split(path, "/", parts);
    return parts;
}

bool IKikimrGateway::TrySplitTablePath(const TString& path, std::pair<TString, TString>& result, TString& error) {
    auto parts = SplitPath(path);

    if (parts.size() < 2) {
        error = TString("Missing scheme root in table path: ") + path;
        return false;
    }

    result = std::make_pair(
        CombinePath(parts.begin(), parts.end() - 1),
        parts.back());

    return true;
}

TFuture<IKikimrGateway::TGenericResult> IKikimrGateway::CreatePath(const TString& path, TCreateDirFunc createDir) {
    auto partsHolder = std::make_shared<TVector<TString>>(SplitPath(path));
    auto &parts = *partsHolder;

    if (parts.size() < 2) {
        TGenericResult result;
        result.SetSuccess();
        return MakeFuture<TGenericResult>(result);
    }

    auto pathPromise = NewPromise<TGenericResult>();
    CreateDirs(partsHolder, 1, pathPromise, createDir);

    return pathPromise.GetFuture();
}

TString IKikimrGateway::CreateIndexTablePath(const TString& tableName, const TString& indexName) {
    return tableName + "/" + indexName + "/indexImplTable";
}


void IKikimrGateway::BuildIndexMetadata(TTableMetadataResult& loadTableMetadataResult) {
    auto tableMetadata = loadTableMetadataResult.Metadata;
    YQL_ENSURE(tableMetadata);

    if (tableMetadata->Indexes.empty()) {
        return;
    }

    const auto& cluster = tableMetadata->Cluster;
    const auto& tableName = tableMetadata->Name;
    const size_t indexesCount = tableMetadata->Indexes.size();

    NKikimr::NTableIndex::TTableColumns tableColumns;
    tableColumns.Columns.reserve(tableMetadata->Columns.size());
    for (auto& column: tableMetadata->Columns) {
        tableColumns.Columns.insert_noresize(column.first);
    }
    tableColumns.Keys = tableMetadata->KeyColumnNames;

    tableMetadata->SecondaryGlobalIndexMetadata.resize(indexesCount);
    for (size_t i = 0; i < indexesCount; i++) {
        const auto& index = tableMetadata->Indexes[i];
        auto indexTablePath = CreateIndexTablePath(tableName, index.Name);
        NKikimr::NTableIndex::TTableColumns indexTableColumns = NKikimr::NTableIndex::CalcTableImplDescription(
                    tableColumns,
                    NKikimr::NTableIndex::TIndexColumns{index.KeyColumns, {}});

        TKikimrTableMetadataPtr indexTableMetadata = new TKikimrTableMetadata(cluster, indexTablePath);
        indexTableMetadata->DoesExist = true;
        indexTableMetadata->KeyColumnNames = indexTableColumns.Keys;
        for (auto& column: indexTableColumns.Columns) {
            indexTableMetadata->Columns[column] = tableMetadata->Columns.at(column);
        }

        tableMetadata->SecondaryGlobalIndexMetadata[i] = indexTableMetadata;
    }
}

bool TTtlSettings::TryParse(const NNodes::TCoNameValueTupleList& node, TTtlSettings& settings, TString& error) {
    using namespace NNodes;

    for (const auto& field : node) {
        auto name = field.Name().Value();
        if (name == "columnName") {
            YQL_ENSURE(field.Value().Maybe<TCoAtom>());
            settings.ColumnName = field.Value().Cast<TCoAtom>().StringValue();
        } else if (name == "expireAfter") {
            YQL_ENSURE(field.Value().Maybe<TCoInterval>());
            auto value = FromString<i64>(field.Value().Cast<TCoInterval>().Literal().Value());
            if (value < 0) {
                error = "Interval value cannot be negative";
                return false;
            }

            settings.ExpireAfter = TDuration::FromValue(value);
        } else {
            error = TStringBuilder() << "Unknown field: " << name;
            return false;
        }
    }

    return true;
}

bool TTableSettings::IsSet() const {
    return CompactionPolicy || PartitionBy || AutoPartitioningBySize || UniformPartitions || PartitionAtKeys
        || PartitionSizeMb || AutoPartitioningByLoad || MinPartitions || MaxPartitions || KeyBloomFilter
        || ReadReplicasSettings || TtlSettings;
}

EYqlIssueCode YqlStatusFromYdbStatus(ui32 ydbStatus) {
    switch (ydbStatus) {
        case Ydb::StatusIds::SUCCESS:
            return TIssuesIds::SUCCESS;
        case Ydb::StatusIds::BAD_REQUEST:
            return TIssuesIds::KIKIMR_BAD_REQUEST;
        case Ydb::StatusIds::UNAUTHORIZED:
            return TIssuesIds::KIKIMR_ACCESS_DENIED;
        case Ydb::StatusIds::INTERNAL_ERROR:
            return TIssuesIds::DEFAULT_ERROR;
        case Ydb::StatusIds::ABORTED:
            return TIssuesIds::KIKIMR_OPERATION_ABORTED;
        case Ydb::StatusIds::UNAVAILABLE:
            return TIssuesIds::KIKIMR_TEMPORARILY_UNAVAILABLE;
        case Ydb::StatusIds::OVERLOADED:
            return TIssuesIds::KIKIMR_OVERLOADED;
        case Ydb::StatusIds::SCHEME_ERROR:
            return TIssuesIds::KIKIMR_SCHEME_ERROR;
        case Ydb::StatusIds::GENERIC_ERROR:
            return TIssuesIds::DEFAULT_ERROR;
        case Ydb::StatusIds::TIMEOUT:
            return TIssuesIds::KIKIMR_TIMEOUT;
        case Ydb::StatusIds::BAD_SESSION:
            return TIssuesIds::KIKIMR_TOO_MANY_TRANSACTIONS;
        case Ydb::StatusIds::PRECONDITION_FAILED:
            return TIssuesIds::KIKIMR_CONSTRAINT_VIOLATION;
        case Ydb::StatusIds::CANCELLED:
            return TIssuesIds::KIKIMR_OPERATION_CANCELLED;
        case Ydb::StatusIds::UNSUPPORTED:
            return TIssuesIds::KIKIMR_UNSUPPORTED;
        default:
            return TIssuesIds::DEFAULT_ERROR;
    }
}

void SetColumnType(Ydb::Type& protoType, const TString& typeName, bool notNull) {
    auto* typeDesc = NKikimr::NPg::TypeDescFromPgTypeName(typeName);
    if (typeDesc) {
        auto pg = notNull ? protoType.mutable_pg_type() :
            protoType.mutable_optional_type()->mutable_item()->mutable_pg_type();
        pg->set_oid(NKikimr::NPg::PgTypeIdFromTypeDesc(typeDesc));
        return;
    }

    NUdf::EDataSlot dataSlot = NUdf::GetDataSlot(typeName);
    if (dataSlot == NUdf::EDataSlot::Decimal) {
        auto decimal = notNull ? protoType.mutable_decimal_type() :
            protoType.mutable_optional_type()->mutable_item()->mutable_decimal_type();
        // We have no params right now
        // TODO: Fix decimal params support for kikimr
        decimal->set_precision(22);
        decimal->set_scale(9);
    } else {
        auto& primitive = notNull ? protoType : *protoType.mutable_optional_type()->mutable_item();
        auto id = NUdf::GetDataTypeInfo(dataSlot).TypeId;
        primitive.set_type_id(static_cast<Ydb::Type::PrimitiveTypeId>(id));
    }
}

bool ConvertReadReplicasSettingsToProto(const TString settings, Ydb::Table::ReadReplicasSettings& proto,
    Ydb::StatusIds::StatusCode& code, TString& error)
{
    TVector<TString> azs = StringSplitter(to_lower(settings)).Split(',').SkipEmpty();
    bool wrongFormat = false;
    if (azs) {
        for (const auto& az : azs) {
            TVector<TString> azSettings = StringSplitter(az).Split(':').SkipEmpty();
            if (azSettings.size() != 2) {
                wrongFormat = true;
                break;
            }
            TVector<TString> valueString = StringSplitter(azSettings[1]).Split(' ').SkipEmpty();
            TVector<TString> nameString = StringSplitter(azSettings[0]).Split(' ').SkipEmpty();
            ui64 value;
            if (valueString.size() != 1 || !TryFromString<ui64>(valueString[0], value) || nameString.size() != 1) {
                wrongFormat = true;
                break;
            }
            if ("per_az" == nameString[0]) {
                if (azs.size() != 1) {
                    wrongFormat = true;
                    break;
                }
                proto.set_per_az_read_replicas_count(value);
            } else if ("any_az" == nameString[0]) {
                if (azs.size() != 1) {
                    wrongFormat = true;
                    break;
                }
                proto.set_any_az_read_replicas_count(value);
            } else {
                code = Ydb::StatusIds::UNSUPPORTED;
                error = "Specifying read replicas count for each AZ in cluster is not supported yet";
                return false;
                //auto& clusterReplicasSettings = *proto.mutable_cluster_replicas_settings();
                //auto& azDesc = *clusterReplicasSettings.add_az_read_replicas_settings();
                //azDesc.set_name(nameString[0]);
                //azDesc.set_read_replicas_count(value);
            }
        }
    } else {
        wrongFormat = true;
    }
    if (wrongFormat) {
        code = Ydb::StatusIds::BAD_REQUEST;
        error = TStringBuilder() << "Wrong format for read replicas settings '" << settings
            << "'. It should be one of: "
            << "1) 'PER_AZ:<read_replicas_count>' to set equal read replicas count for every AZ; "
            << "2) 'ANY_AZ:<read_replicas_count>' to set total read replicas count between all AZs; "
            << "3) '<az1_name>:<read_replicas_count1>, <az2_name>:<read_replicas_count2>, ...' "
            << "to specify read replicas count for each AZ in cluster.";
        return false;
    }
    return true;
}

void ConvertTtlSettingsToProto(const NYql::TTtlSettings& settings, Ydb::Table::TtlSettings& proto) {
    proto.mutable_date_type_column()->set_column_name(settings.ColumnName);
    proto.mutable_date_type_column()->set_expire_after_seconds(settings.ExpireAfter.Seconds());
}

Ydb::FeatureFlag::Status GetFlagValue(const TMaybe<bool>& value) {
    if (!value) {
        return Ydb::FeatureFlag::STATUS_UNSPECIFIED;
    }

    return *value
        ? Ydb::FeatureFlag::ENABLED
        : Ydb::FeatureFlag::DISABLED;
}

} // namespace NYql
