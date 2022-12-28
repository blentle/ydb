#include "schemeshard_audit_log_fragment.h"

#include <ydb/core/base/path.h>
#include <ydb/core/protos/flat_scheme_op.pb.h>
#include <ydb/library/aclib/aclib.h>

#include <util/string/builder.h>

namespace NKikimr::NSchemeShard {

TString DefineUserOperationName(NKikimrSchemeOp::EOperationType type) {
    switch (type) {
    // common
    case NKikimrSchemeOp::EOperationType::ESchemeOpModifyACL:
        return "MODIFY ACL";
    case NKikimrSchemeOp::EOperationType::ESchemeOpAlterUserAttributes:
        return "ALTER USER ATTRIBUTES";
    case NKikimrSchemeOp::EOperationType::ESchemeOpForceDropUnsafe:
        return "DROP PATH UNSAFE";
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateLock:
        return "CREATE LOCK";
    case NKikimrSchemeOp::EOperationType::ESchemeOpDropLock:
        return "DROP LOCK";
    case NKikimrSchemeOp::EOperationType::ESchemeOpAlterLogin:
        return "ALTER LOGIN";
    case NKikimrSchemeOp::EOperationType::ESchemeOp_DEPRECATED_35:
        return "ESchemeOp_DEPRECATED_35";
    // dir
    case NKikimrSchemeOp::EOperationType::ESchemeOpMkDir:
        return "CREATE DIRECTORY";
    case NKikimrSchemeOp::EOperationType::ESchemeOpRmDir:
        return "DROP DIRECTORY";
    // table
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateTable:
        return "CREATE TABLE";
    case NKikimrSchemeOp::EOperationType::ESchemeOpAlterTable:
        return "ALTER TABLE";
    case NKikimrSchemeOp::EOperationType::ESchemeOpDropTable:
        return "DROP TABLE";
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateConsistentCopyTables:
        return "CREATE TABLE COPY FROM";
    case NKikimrSchemeOp::EOperationType::ESchemeOpSplitMergeTablePartitions:
        return "ALTER TABLE PARTITIONS";
    case NKikimrSchemeOp::EOperationType::ESchemeOpBackup:
        return "BACKUP TABLE";
    case NKikimrSchemeOp::EOperationType::ESchemeOpRestore:
        return "RESTORE TABLE";
    // topic
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreatePersQueueGroup:
        return "CREATE PERSISTENT QUEUE";
    case NKikimrSchemeOp::EOperationType::ESchemeOpAlterPersQueueGroup:
        return "ALTER PERSISTENT QUEUE";
    case NKikimrSchemeOp::EOperationType::ESchemeOpDropPersQueueGroup:
        return "DROP PERSISTENT QUEUE";
    case NKikimrSchemeOp::EOperationType::ESchemeOpAllocatePersQueueGroup:
        return "ALLOCATE PERSISTENT QUEUE";
    case NKikimrSchemeOp::EOperationType::ESchemeOpDeallocatePersQueueGroup:
        return "DEALLOCATE PERSISTENT QUEUE";
    // database
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateSubDomain:
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateExtSubDomain:
        return "CREATE DATABASE";
    case NKikimrSchemeOp::EOperationType::ESchemeOpAlterSubDomain:
    case NKikimrSchemeOp::EOperationType::ESchemeOpAlterExtSubDomain:
    case NKikimrSchemeOp::EOperationType::ESchemeOpAlterExtSubDomainCreateHive:
        return "ALTER DATABASE";
    case NKikimrSchemeOp::EOperationType::ESchemeOpDropSubDomain:
    case NKikimrSchemeOp::EOperationType::ESchemeOpForceDropSubDomain:
    case NKikimrSchemeOp::EOperationType::ESchemeOpForceDropExtSubDomain:
        return "DROP DATABASE";
    case NKikimrSchemeOp::EOperationType::ESchemeOpUpgradeSubDomain:
        return "ALTER DATABASE MIGRATE";
    case NKikimrSchemeOp::EOperationType::ESchemeOpUpgradeSubDomainDecision:
        return "ALTER DATABASE MIGRATE DECISION";
    // rtmr
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateRtmrVolume:
        return "CREATE RTMR VOLUME";
    // blockstore
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateBlockStoreVolume:
        return "CREATE BLOCK STORE VOLUME";
    case NKikimrSchemeOp::EOperationType::ESchemeOpAlterBlockStoreVolume:
        return "ALTER BLOCK STORE VOLUME";
    case NKikimrSchemeOp::EOperationType::ESchemeOpAssignBlockStoreVolume:
        return "ALTER BLOCK STORE VOLUME ASSIGN";
    case NKikimrSchemeOp::EOperationType::ESchemeOpDropBlockStoreVolume:
        return "DROP BLOCK STORE VOLUME";
    // kesus
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateKesus:
        return "CREATE KESUS";
    case NKikimrSchemeOp::EOperationType::ESchemeOpAlterKesus:
        return "ALTER KESUS";
    case NKikimrSchemeOp::EOperationType::ESchemeOpDropKesus:
        return "DROP KESUS";
    // solomon
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateSolomonVolume:
        return "CREATE SOLOMON VOLUME";
    case NKikimrSchemeOp::EOperationType::ESchemeOpAlterSolomonVolume:
        return "ALTER SOLOMON VOLUME";
    case NKikimrSchemeOp::EOperationType::ESchemeOpDropSolomonVolume:
        return "DROP SOLOMON VOLUME";
    // index
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateIndexedTable:
        return "CREATE TABLE WITH INDEXES";
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateTableIndex:
        return "CREATE INDEX";
    case NKikimrSchemeOp::EOperationType::ESchemeOpDropTableIndex:
        return "DROP INDEX";
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateIndexBuild:
        return "BUILD INDEX";
    case NKikimrSchemeOp::EOperationType::ESchemeOpInitiateBuildIndexMainTable:
        return "ALTER TABLE BUILD INDEX INIT";
    case NKikimrSchemeOp::EOperationType::ESchemeOpApplyIndexBuild:
        return "ALTER TABLE BUILD INDEX APPLY";
    case NKikimrSchemeOp::EOperationType::ESchemeOpFinalizeBuildIndexMainTable:
        return "ALTER TABLE BUILD INDEX FINISH";
    case NKikimrSchemeOp::EOperationType::ESchemeOpAlterTableIndex:
        return "ALTER INDEX";
    case NKikimrSchemeOp::EOperationType::ESchemeOpFinalizeBuildIndexImplTable:
        return "ALTER TABLE BUILD INDEX FINISH";
    case NKikimrSchemeOp::EOperationType::ESchemeOpInitiateBuildIndexImplTable:
        return "ALTER TABLE BUILD INDEX INIT";
    case NKikimrSchemeOp::EOperationType::ESchemeOpDropIndex:
        return "ALTER TABLE DROP INDEX";
    case NKikimrSchemeOp::EOperationType::ESchemeOpDropTableIndexAtMainTable:
        return "ALTER TABLE DROP INDEX";
    case NKikimrSchemeOp::EOperationType::ESchemeOpCancelIndexBuild:
        return "ALTER TABLE BUILD INDEX CANCEL";
    // rename
    case NKikimrSchemeOp::EOperationType::ESchemeOpMoveTable:
        return "ALTER TABLE RENAME";
    case NKikimrSchemeOp::EOperationType::ESchemeOpMoveIndex:
    case NKikimrSchemeOp::EOperationType::ESchemeOpMoveTableIndex:
        return "ALTER TABLE INDEX RENAME";
    // filestore
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateFileStore:
        return "CREATE FILE STORE";
    case NKikimrSchemeOp::EOperationType::ESchemeOpAlterFileStore:
        return "ALTER FILE STORE";
    case NKikimrSchemeOp::EOperationType::ESchemeOpDropFileStore:
        return "DROP FILE STORE";
    // columnstore
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateColumnStore:
        return "CREATE COLUMN STORE";
    case NKikimrSchemeOp::EOperationType::ESchemeOpAlterColumnStore:
        return "ALTER COLUMN STORE";
    case NKikimrSchemeOp::EOperationType::ESchemeOpDropColumnStore:
        return "DROP COLUMN STORE";
    // columntable
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateColumnTable:
        return "CREATE COLUMN TABLE";
    case NKikimrSchemeOp::EOperationType::ESchemeOpAlterColumnTable:
        return "ALTER COLUMN TABLE";
    case NKikimrSchemeOp::EOperationType::ESchemeOpDropColumnTable:
        return "DROP COLUMN TABLE";
    // changefeed
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateCdcStream:
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateCdcStreamImpl:
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateCdcStreamAtTable:
        return "ALTER TABLE ADD CHANGEFEED";
    case NKikimrSchemeOp::EOperationType::ESchemeOpAlterCdcStream:
    case NKikimrSchemeOp::EOperationType::ESchemeOpAlterCdcStreamImpl:
    case NKikimrSchemeOp::EOperationType::ESchemeOpAlterCdcStreamAtTable:
        return "ALTER TABLE ALTER CHANGEFEED";
    case NKikimrSchemeOp::EOperationType::ESchemeOpDropCdcStream:
    case NKikimrSchemeOp::EOperationType::ESchemeOpDropCdcStreamImpl:
    case NKikimrSchemeOp::EOperationType::ESchemeOpDropCdcStreamAtTable:
        return "ALTER TABLE DROP CHANGEFEED";
    // sequence
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateSequence:
        return "CREATE SEQUENCE";
    case NKikimrSchemeOp::EOperationType::ESchemeOpAlterSequence:
        return "ALTER SEQUENCE";
    case NKikimrSchemeOp::EOperationType::ESchemeOpDropSequence:
        return "DROP SEQUENCE";
    // replication
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateReplication:
        return "CREATE REPLICATION";
    case NKikimrSchemeOp::EOperationType::ESchemeOpAlterReplication:
        return "ALTER REPLICATION";
    case NKikimrSchemeOp::EOperationType::ESchemeOpDropReplication:
        return "DROP REPLICATION";
    // blob depot
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateBlobDepot:
        return "CREATE BLOB DEPOT";
    case NKikimrSchemeOp::EOperationType::ESchemeOpAlterBlobDepot:
        return "ALTER BLOB DEPOT";
    case NKikimrSchemeOp::EOperationType::ESchemeOpDropBlobDepot:
        return "DROP BLOB DEPOT";
    }
    Y_FAIL("switch should cover all operation types");
}

TAuditLogFragment::TAuditLogFragment(const NKikimrSchemeOp::TModifyScheme& tx)
    : Operation(DefineUserOperationName(tx.GetOperationType()))
    , ProtoRequest(tx.ShortDebugString())
{
    FillPaths(tx);
    FillACL(tx);
}

void TAuditLogFragment::FillACL(const NKikimrSchemeOp::TModifyScheme& tx) {
    using namespace NACLib;

    bool hasACL = tx.HasModifyACL() && tx.GetModifyACL().HasDiffACL();
    if (hasACL) {
        NACLib::TDiffACL diffACL(tx.GetModifyACL().GetDiffACL());
        for (const auto& diffACE : diffACL.GetDiffACE()) {
            const NACLibProto::TACE& ace = diffACE.GetACE();
            switch (static_cast<EDiffType>(diffACE.GetDiffType())) {
            case EDiffType::Add:
                AddACL.push_back(TACL::ToString(ace));
                break;
            case EDiffType::Remove:
                RmACL.push_back(TACL::ToString(ace));
                break;
            }
        }
    }

    bool hasOwner = tx.HasModifyACL() && tx.GetModifyACL().HasNewOwner();
    if (hasOwner) {
        NewOwner = tx.GetModifyACL().GetNewOwner();
    }
}

void TAuditLogFragment::FillPaths(const NKikimrSchemeOp::TModifyScheme& tx) {
    switch (tx.GetOperationType()) {
    case NKikimrSchemeOp::EOperationType::ESchemeOpMkDir:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetMkDir().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateTable:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetCreateTable().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreatePersQueueGroup:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetCreatePersQueueGroup().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpAllocatePersQueueGroup:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetAllocatePersQueueGroup().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpDropTable:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetDrop().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpDropPersQueueGroup:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetDrop().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpDeallocatePersQueueGroup:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetDeallocatePersQueueGroup().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpAlterTable:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetAlterTable().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpAlterPersQueueGroup:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetAlterPersQueueGroup().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpModifyACL:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetModifyACL().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpRmDir:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetDrop().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpSplitMergeTablePartitions:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetSplitMergeTablePartitions().GetTablePath()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpBackup:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetBackup().GetTableName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateSubDomain:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetSubDomain().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpDropSubDomain:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetDrop().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateRtmrVolume:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetCreateRtmrVolume().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateBlockStoreVolume:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetCreateBlockStoreVolume().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpAlterBlockStoreVolume:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetAlterBlockStoreVolume().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpAssignBlockStoreVolume:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetAssignBlockStoreVolume().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpDropBlockStoreVolume:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetDrop().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateKesus:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetKesus().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpDropKesus:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetDrop().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpForceDropSubDomain:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetDrop().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateSolomonVolume:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetCreateSolomonVolume().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpDropSolomonVolume:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetDrop().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpAlterKesus:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetKesus().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpAlterSubDomain:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetSubDomain().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpAlterUserAttributes:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetAlterUserAttributes().GetPathName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpForceDropUnsafe:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetDrop().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateIndexedTable:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetCreateIndexedTable().GetTableDescription().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateTableIndex:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetCreateTableIndex().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateConsistentCopyTables:
        for (const auto& item : tx.GetCreateConsistentCopyTables().GetCopyTableDescriptions()) {
            SrcPaths.push_back(item.GetSrcPath());
            DstPaths.push_back(item.GetDstPath());
        }
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpDropTableIndex:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetDrop().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateExtSubDomain:
    case NKikimrSchemeOp::EOperationType::ESchemeOpAlterExtSubDomain:
    case NKikimrSchemeOp::EOperationType::ESchemeOpAlterExtSubDomainCreateHive:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetSubDomain().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpForceDropExtSubDomain:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetSubDomain().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOp_DEPRECATED_35:
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpUpgradeSubDomain:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetUpgradeSubDomain().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpUpgradeSubDomainDecision:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetUpgradeSubDomain().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateIndexBuild:
        Path = JoinPath({tx.GetInitiateIndexBuild().GetTable(), tx.GetInitiateIndexBuild().GetIndex().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpInitiateBuildIndexMainTable:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetInitiateBuildIndexMainTable().GetTableName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateLock:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetLockConfig().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpApplyIndexBuild:
        Path = JoinPath({tx.GetApplyIndexBuild().GetTablePath(), tx.GetApplyIndexBuild().GetIndexName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpFinalizeBuildIndexMainTable:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetFinalizeBuildIndexMainTable().GetTableName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpAlterTableIndex:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetAlterTableIndex().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpAlterSolomonVolume:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetAlterSolomonVolume().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpDropLock:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetLockConfig().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpFinalizeBuildIndexImplTable:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetAlterTable().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpInitiateBuildIndexImplTable:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetCreateTable().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpDropIndex:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetDropIndex().GetTableName(), tx.GetDropIndex().GetIndexName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpDropTableIndexAtMainTable:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetDropIndex().GetTableName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpCancelIndexBuild:
        Path = JoinPath({tx.GetCancelIndexBuild().GetTablePath(), tx.GetCancelIndexBuild().GetIndexName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateFileStore:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetCreateFileStore().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpAlterFileStore:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetAlterFileStore().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpDropFileStore:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetDrop().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpRestore:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetRestore().GetTableName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateColumnStore:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetCreateColumnStore().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpAlterColumnStore:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetAlterColumnStore().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpDropColumnStore:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetDrop().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateColumnTable:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetAlterColumnTable().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpAlterColumnTable:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetAlterColumnTable().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpDropColumnTable:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetDrop().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpAlterLogin:
        Path = tx.GetWorkingDir();
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateCdcStream:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetCreateCdcStream().GetTableName(), tx.GetCreateCdcStream().GetStreamDescription().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateCdcStreamImpl:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetCreateCdcStream().GetStreamDescription().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateCdcStreamAtTable:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetCreateCdcStream().GetTableName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpAlterCdcStream:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetAlterCdcStream().GetTableName(), tx.GetAlterCdcStream().GetStreamName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpAlterCdcStreamImpl:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetAlterCdcStream().GetStreamName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpAlterCdcStreamAtTable:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetAlterCdcStream().GetTableName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpDropCdcStream:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetDropCdcStream().GetTableName(), tx.GetDropCdcStream().GetStreamName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpDropCdcStreamImpl:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetDrop().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpDropCdcStreamAtTable:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetDropCdcStream().GetTableName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpMoveTable:
        SrcPaths.push_back(tx.GetMoveTable().GetSrcPath());
        DstPaths.push_back(tx.GetMoveTable().GetDstPath());
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpMoveTableIndex:
        SrcPaths.push_back(tx.GetMoveTableIndex().GetSrcPath());
        DstPaths.push_back(tx.GetMoveTableIndex().GetDstPath());
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateSequence:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetSequence().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpAlterSequence:
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpDropSequence:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetDrop().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateReplication:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetReplication().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpAlterReplication:
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpDropReplication:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetDrop().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpCreateBlobDepot:
        Path = JoinPath({tx.GetWorkingDir(), tx.GetBlobDepot().GetName()});
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpAlterBlobDepot:
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpDropBlobDepot:
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOpMoveIndex:
        SrcPaths.push_back(JoinPath({tx.GetMoveIndex().GetTablePath(), tx.GetMoveIndex().GetSrcPath()}));
        DstPaths.push_back(JoinPath({tx.GetMoveIndex().GetTablePath(), tx.GetMoveIndex().GetDstPath()}));
        break;
    }
}

TString TAuditLogFragment::GetAnyPath() const {
    if (Path) {
        return *Path;
    } else if (SrcPaths) {
        return SrcPaths.front();
    } else if (DstPaths) {
        return DstPaths.front();
    } else {
        return "";
    }
}

TString TAuditLogFragment::GetOperation() const {
    return Operation;
}

TString TAuditLogFragment::GetPath() const {
    if (!Path && SrcPaths && DstPaths) {
        return "";
    }
    return Path.GetOrElse("");
}

TString TAuditLogFragment::GetSrcPath() const {
    Y_VERIFY_DEBUG(SrcPaths.size() == DstPaths.size());
    auto minSize = Min(SrcPaths.size(), DstPaths.size());
    if (minSize == 0)
        return "";

    auto result = TStringBuilder();
    result << "{";
    for (size_t i = 0; i < minSize; ++i) {
        result << SrcPaths[i];
        if (i < minSize - 1)
            result << ", ";
    }
    result << "}";
    return result;
}

TString TAuditLogFragment::GetDstPath() const {
    Y_VERIFY_DEBUG(SrcPaths.size() == DstPaths.size());
    auto minSize = Min(SrcPaths.size(), DstPaths.size());
    if (minSize == 0)
        return "";

    auto result = TStringBuilder();
    result << "{";
    for (size_t i = 0; i < minSize; ++i) {
        result << DstPaths[i];
        if (i < minSize - 1)
            result << ", ";
    }
    result << "}";
    return result;
}

TString TAuditLogFragment::GetSetOwner() const {
    return NewOwner.GetOrElse("");
}

TString TAuditLogFragment::GetAddAccess() const {
    if (AddACL.empty())
        return "";

    auto result = TStringBuilder();
    result << "{";
    for (size_t i = 0; i < AddACL.size(); ++i) {
        result << AddACL[i];
        if (i < AddACL.size() - 1)
            result << ", ";
    }
    result << "}";
    return result;
}

TString TAuditLogFragment::GetRemoveAccess() const {
    if (RmACL.empty())
        return "";

    auto result = TStringBuilder();
    result << "{";
    for (size_t i = 0; i < RmACL.size(); ++i) {
        result << RmACL[i];
        if (i < RmACL.size() - 1)
            result << ", ";
    }
    result << "}";
    return result;
}

TString TAuditLogFragment::GetProtoRequest() const {
    return ProtoRequest.GetOrElse("");
}
}
