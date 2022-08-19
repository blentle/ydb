#include "yql_s3_provider_impl.h"
#include "yql_s3_list.h"

#include <ydb/library/yql/providers/s3/expr_nodes/yql_s3_expr_nodes.h>
#include <ydb/library/yql/providers/s3/path_generator/yql_s3_path_generator.h>
#include <ydb/library/yql/providers/s3/range_helpers/path_list_reader.h>
#include <ydb/library/yql/core/yql_expr_optimize.h>
#include <ydb/library/yql/core/yql_opt_utils.h>
#include <ydb/library/yql/utils/log/log.h>
#include <ydb/library/yql/utils/url_builder.h>

#include <util/generic/size_literals.h>

namespace NYql {

namespace {

using namespace NNodes;

std::array<TExprNode::TPtr, 2U> ExtractSchema(TExprNode::TListType& settings) {
    for (auto it = settings.cbegin(); settings.cend() != it; ++it) {
        if (const auto item = *it; item->Head().IsAtom("userschema")) {
            settings.erase(it);
            return {item->ChildPtr(1), item->ChildrenSize() > 2 ? item->TailPtr() : TExprNode::TPtr()};
        }
    }

    return {};
}

using namespace NPathGenerator;

struct TListRequest {
    TString Token;
    TString Url;
    TString Pattern;
    TString UserPath;
    TVector<IPathGenerator::TColumnWithValue> ColumnValues;
};

bool operator<(const TListRequest& a, const TListRequest& b) {
    return std::tie(a.Token, a.Url, a.Pattern, a.UserPath) < std::tie(b.Token, b.Url, b.Pattern, b.UserPath);
}

using TPendingRequests = TMap<TListRequest, NThreading::TFuture<IS3Lister::TListResult>>;

struct TGeneratedColumnsConfig {
    TVector<TString> Columns;
    TPathGeneratorPtr Generator;
    TExprNode::TPtr SchemaTypeNode;
};

class TS3IODiscoveryTransformer : public TGraphTransformerBase {
public:
    TS3IODiscoveryTransformer(TS3State::TPtr state, IHTTPGateway::TPtr gateway)
        : State_(std::move(state))
        , Lister_(IS3Lister::Make(gateway, State_->Configuration->MaxDiscoveryFilesPerQuery))
    {}

    TStatus DoTransform(TExprNode::TPtr input, TExprNode::TPtr& output, TExprContext& ctx) final {
        output = input;
        if (ctx.Step.IsDone(TExprStep::DiscoveryIO)) {
            return TStatus::Ok;
        }

        auto reads = FindNodes(input, [&](const TExprNode::TPtr& node) {
            if (const auto maybeRead = TMaybeNode<TS3Read>(node)) {
                if (maybeRead.DataSource()) {
                    return maybeRead.Cast().Arg(2).Ref().IsCallable("MrTableConcat");
                }
            }
            return false;
        });

        TVector<NThreading::TFuture<IS3Lister::TListResult>> futures;
        for (auto& r : reads) {
            const TS3Read read(std::move(r));
            try {
                if (!LaunchListsForNode(read, futures, ctx)) {
                    return TStatus::Error;
                }
            } catch (const std::exception& ex) {
                ctx.AddError(TIssue(ctx.GetPosition(read.Pos()), TStringBuilder() << "Error while doing S3 discovery: " << ex.what()));
                return TStatus::Error;
            }
        }

        if (futures.empty()) {
            return TStatus::Ok;
        }

        AllFuture_ = NThreading::WaitExceptionOrAll(futures);
        return TStatus::Async;
    }

    NThreading::TFuture<void> DoGetAsyncFuture(const TExprNode&) final {
        return AllFuture_;
    }

    TStatus DoApplyAsyncChanges(TExprNode::TPtr input, TExprNode::TPtr& output, TExprContext& ctx) final {
        // Raise errors if any
        AllFuture_.GetValue();

        TPendingRequests pendingRequests;
        TNodeMap<TVector<TListRequest>> requestsByNode;
        TNodeMap<TGeneratedColumnsConfig> genColumnsByNode;

        pendingRequests.swap(PendingRequests_);
        requestsByNode.swap(RequestsByNode_);
        genColumnsByNode.swap(GenColumnsByNode_);

        TNodeOnNodeOwnedMap replaces;
        size_t count = 0;
        size_t totalSize = 0;
        for (auto& [node, requests] : requestsByNode) {
            const TS3Read read(node);
            const auto& object = read.Arg(2).Ref();
            YQL_ENSURE(object.IsCallable("MrTableConcat"));
            size_t readSize = 0;
            TExprNode::TListType pathNodes;

            TString formatName;
            {
                const auto& settings = *read.Ref().Child(4);
                auto format = GetSetting(settings, "format");
                if (format && format->ChildrenSize() >= 2) {
                    formatName = format->Child(1)->Content();
                }
            }
            auto fileSizeLimit = State_->Configuration->FileSizeLimit;
            if (formatName) {
                auto it = State_->Configuration->FormatSizeLimits.find(formatName);
                if (it != State_->Configuration->FormatSizeLimits.end() && fileSizeLimit > it->second) {
                    fileSizeLimit = it->second;
                }
            }

            struct TExtraColumnValue {
                TString Name;
                TMaybe<NUdf::EDataSlot> Type;
                TString Value;
                bool operator<(const TExtraColumnValue& other) const {
                    return std::tie(Name, Type, Value) < std::tie(other.Name, other.Type, other.Value);
                }
            };

            TMap<TMaybe<TVector<TExtraColumnValue>>, NS3Details::TPathList> pathsByExtraValues;
            const TGeneratedColumnsConfig* generatedColumnsConfig = nullptr;
            if (auto it = genColumnsByNode.find(node); it != genColumnsByNode.end()) {
                generatedColumnsConfig = &it->second;
            }

            for (auto& req : requests) {
                auto it = pendingRequests.find(req);
                YQL_ENSURE(it != pendingRequests.end());
                YQL_ENSURE(it->second.HasValue());

                const IS3Lister::TListResult& listResult = it->second.GetValue();
                if (listResult.index() == 1) {
                    const auto& issues = std::get<TIssues>(listResult);
                    YQL_CLOG(INFO, ProviderS3) << "Discovery " << req.Url << req.Pattern << " error " << issues.ToString();
                    std::for_each(issues.begin(), issues.end(), std::bind(&TExprContext::AddError, std::ref(ctx), std::placeholders::_1));
                    return TStatus::Error;
                }

                const auto& listEntries = std::get<IS3Lister::TListEntries>(listResult);
                if (listEntries.empty() && !generatedColumnsConfig && !req.UserPath.EndsWith("/")) {
                    // request to list particular files that are missing
                    ctx.AddError(TIssue(ctx.GetPosition(object.Pos()),
                        TStringBuilder() << "Object " << req.UserPath << " doesn't exist."));
                    return TStatus::Error;
                }

                for (auto& entry : listEntries) {
                    if (entry.Size > fileSizeLimit) {
                        ctx.AddError(TIssue(ctx.GetPosition(object.Pos()),
                            TStringBuilder() << "Size of object " << entry.Path << " = " << entry.Size << " and exceeds limit = " << fileSizeLimit << " specified for format " << formatName));
                        return TStatus::Error;
                    }

                    TMaybe<TVector<TExtraColumnValue>> extraValues;
                    if (generatedColumnsConfig) {
                        extraValues = TVector<TExtraColumnValue>{};
                        if (!req.ColumnValues.empty()) {
                            // explicit partitioning
                            YQL_ENSURE(req.ColumnValues.size() == generatedColumnsConfig->Columns.size());
                            for (auto& cv : req.ColumnValues) {
                                TExtraColumnValue value;
                                value.Name = cv.Name;
                                value.Type = cv.Type;
                                value.Value = cv.Value;
                                extraValues->push_back(std::move(value));
                            }
                        } else {
                            // last entry matches file name
                            YQL_ENSURE(entry.MatchedGlobs.size() == generatedColumnsConfig->Columns.size() + 1);
                            for (size_t i = 0; i < generatedColumnsConfig->Columns.size(); ++i) {
                                TExtraColumnValue value;
                                value.Name = generatedColumnsConfig->Columns[i];
                                value.Value = entry.MatchedGlobs[i];
                                extraValues->push_back(std::move(value));
                            }
                        }
                    }

                    auto& pathList = pathsByExtraValues[extraValues];
                    pathList.emplace_back(entry.Path, entry.Size);
                    ++count;
                    readSize += entry.Size;
                }

                YQL_CLOG(INFO, ProviderS3) << "Object " << req.Pattern << " has " << listEntries.size() << " items with total size " << readSize;
                totalSize += readSize;
            }

            for (const auto& [extraValues, pathList] : pathsByExtraValues) {
                TExprNodeList extraColumnsAsStructArgs;
                if (extraValues) {
                    YQL_ENSURE(generatedColumnsConfig);
                    YQL_ENSURE(generatedColumnsConfig->SchemaTypeNode);

                    for (auto& ev : *extraValues) {
                        auto resultType = ctx.Builder(object.Pos())
                            .Callable("StructMemberType")
                                .Add(0, generatedColumnsConfig->SchemaTypeNode)
                                .Atom(1, ev.Name)
                            .Seal()
                            .Build();
                        TExprNode::TPtr value;
                        if (ev.Type.Defined()) {
                            value = ctx.Builder(object.Pos())
                                .Callable("StrictCast")
                                    .Callable(0, "Data")
                                        .Add(0, ExpandType(object.Pos(), *ctx.MakeType<TDataExprType>(*ev.Type), ctx))
                                        .Atom(1, ev.Value)
                                    .Seal()
                                    .Add(1, resultType)
                                .Seal()
                                .Build();
                        } else {
                            value = ctx.Builder(object.Pos())
                                .Callable("DataOrOptionalData")
                                    .Add(0, resultType)
                                    .Atom(1, ev.Value)
                                .Seal()
                                .Build();
                        }
                        extraColumnsAsStructArgs.push_back(
                            ctx.Builder(object.Pos())
                                .List()
                                    .Atom(0, ev.Name)
                                    .Add(1, value)
                                .Seal()
                                .Build()
                        );
                    }
                }

                auto extraColumns = ctx.NewCallable(object.Pos(), "AsStruct", std::move(extraColumnsAsStructArgs));

                TString packedPaths;
                bool isTextFormat;
                NS3Details::PackPathsList(pathList, packedPaths, isTextFormat);

                pathNodes.emplace_back(
                    Build<TS3Path>(ctx, object.Pos())
                        .Data<TCoString>()
                            .Literal()
                            .Build(packedPaths)
                        .Build()
                        .IsText<TCoBool>()
                            .Literal()
                            .Build(ToString(isTextFormat))
                        .Build()
                        .ExtraColumns(extraColumns)
                    .Done().Ptr()
                );
            }

            auto settings = read.Ref().Child(4)->ChildrenList();
            auto userSchema = ExtractSchema(settings);
            if (pathNodes.empty()) {
                auto data = ctx.Builder(read.Pos())
                    .Callable("List")
                        .Callable(0, "ListType")
                            .Add(0, userSchema.front())
                        .Seal()
                    .Seal()
                    .Build();
                if (userSchema.back()) {
                    data = ctx.NewCallable(read.Pos(), "AssumeColumnOrder", { data, userSchema.back() });
                }
                replaces.emplace(node, ctx.NewCallable(read.Pos(), "Cons!", { read.World().Ptr(), data }));
                continue;
            }

            TExprNode::TPtr s3Object;
            s3Object = Build<TS3Object>(ctx, object.Pos())
                    .Paths(ctx.NewList(object.Pos(), std::move(pathNodes)))
                    .Format(ExtractFormat(settings))
                    .Settings(ctx.NewList(object.Pos(), std::move(settings)))
                .Done().Ptr();

            replaces.emplace(node, userSchema.back() ?
                Build<TS3ReadObject>(ctx, read.Pos())
                    .World(read.World())
                    .DataSource(read.DataSource())
                    .Object(std::move(s3Object))
                    .RowType(std::move(userSchema.front()))
                    .ColumnOrder(std::move(userSchema.back()))
                .Done().Ptr():
                Build<TS3ReadObject>(ctx, read.Pos())
                    .World(read.World())
                    .DataSource(read.DataSource())
                    .Object(std::move(s3Object))
                    .RowType(std::move(userSchema.front()))
                .Done().Ptr());
        }

        const auto maxFiles = State_->Configuration->MaxFilesPerQuery;
        if (count > maxFiles) {
            ctx.AddError(TIssue(ctx.GetPosition(input->Pos()), TStringBuilder() << "Too many objects to read: " << count << ", but limit is " << maxFiles));
            return TStatus::Error;
        }

        const auto maxSize = State_->Configuration->MaxReadSizePerQuery;
        if (totalSize > maxSize) {
            ctx.AddError(TIssue(ctx.GetPosition(input->Pos()), TStringBuilder() << "Too large objects to read: " << totalSize << ", but limit is " << maxSize));
            return TStatus::Error;
        }

        return RemapExpr(input, output, replaces, ctx, TOptimizeExprSettings(nullptr));
    }
private:
    bool LaunchListsForNode(const TS3Read& read, TVector<NThreading::TFuture<IS3Lister::TListResult>>& futures, TExprContext& ctx) {
        const auto& settings = *read.Ref().Child(4);

        // schema is required
        auto schema = GetSetting(settings, "userschema");
        if (!schema) {
            ctx.AddError(TIssue(ctx.GetPosition(read.Pos()), "Missing schema - please use WITH SCHEMA when reading from S3"));
            return false;
        }
        if (!EnsureTupleMinSize(*schema, 2, ctx)) {
            return false;
        }

        TVector<TString> partitionedBy;
        if (auto partitionedBySetting = GetSetting(settings, "partitionedby")) {
            if (!EnsureTupleMinSize(*partitionedBySetting, 2, ctx)) {
                return false;
            }

            THashSet<TStringBuf> uniqs;
            for (size_t i = 1; i < partitionedBySetting->ChildrenSize(); ++i) {
                const auto& column = partitionedBySetting->Child(i);
                if (!EnsureAtom(*column, ctx)) {
                    return false;
                }
                if (!uniqs.emplace(column->Content()).second) {
                    ctx.AddError(TIssue(ctx.GetPosition(column->Pos()), TStringBuilder() << "Duplicate partitioned_by column '" << column->Content() << "'"));
                    return false;
                }
                partitionedBy.push_back(ToString(column->Content()));

            }
        }

        TString projection;
        if (auto projectionSetting = GetSetting(settings, "projection")) {
            if (!EnsureTupleSize(*projectionSetting, 2, ctx)) {
                return false;
            }

            if (!EnsureAtom(projectionSetting->Tail(), ctx)) {
                return false;
            }

            if (projectionSetting->Tail().Content().empty()) {
                ctx.AddError(TIssue(ctx.GetPosition(projectionSetting->Pos()), "Expecting non-empty projection setting"));
                return false;
            }

            if (partitionedBy.empty()) {
                ctx.AddError(TIssue(ctx.GetPosition(projectionSetting->Pos()), "Missing partitioned_by setting for projection"));
                return false;
            }
            projection = projectionSetting->Tail().Content();
        }

        TVector<TString> paths;
        const auto& object = read.Arg(2).Ref();
        YQL_ENSURE(object.IsCallable("MrTableConcat"));
        object.ForEachChild([&paths](const TExprNode& child){ paths.push_back(ToString(child.Head().Tail().Head().Content())); });

        const auto& connect = State_->Configuration->Clusters.at(read.DataSource().Cluster().StringValue());
        const auto& token = State_->Configuration->Tokens.at(read.DataSource().Cluster().StringValue());
        const auto credentialsProviderFactory = CreateCredentialsProviderFactoryForStructuredToken(State_->CredentialsFactory, token);

        const TString url = connect.Url;
        const TString tokenStr = credentialsProviderFactory->CreateProvider()->GetAuthInfo();

        TGeneratedColumnsConfig config;
        if (!partitionedBy.empty()) {
            config.Columns = partitionedBy;
            config.SchemaTypeNode = schema->ChildPtr(1);
            if (!projection.empty()) {
                config.Generator = CreatePathGenerator(projection, partitionedBy);
            }
            GenColumnsByNode_[read.Raw()] = config;
        }

        for (const auto& path : paths) {
            // each path in CONCAT() can generate multiple list requests for explicit partitioning
            TVector<TListRequest> reqs;

            TListRequest req;
            req.Token = tokenStr;
            req.Url = url;
            req.UserPath = path;

            if (partitionedBy.empty()) {
                if (path.EndsWith("/")) {
                    req.Pattern = path + "*";
                } else {
                    // treat paths as regular wildcard patterns
                    req.Pattern = path;
                }
                reqs.push_back(req);
            } else {
                if (IS3Lister::HasWildcards(path)) {
                    ctx.AddError(TIssue(ctx.GetPosition(read.Pos()), TStringBuilder() << "Path prefix: '" << path << "' contains wildcards"));
                    return false;
                }
                if (!config.Generator) {
                    // Hive-style partitioning
                    TString generated;
                    for (auto& col : config.Columns) {
                        generated += "/" + col + "=*";
                    }
                    generated += "/*";
                    req.Pattern = path + generated;
                    reqs.push_back(req);
                } else {
                    for (auto& rule : config.Generator->GetRules()) {
                        YQL_ENSURE(rule.ColumnValues.size() == config.Columns.size());
                        req.ColumnValues.assign(rule.ColumnValues.begin(), rule.ColumnValues.end());
                        req.Pattern = path + rule.Path + "/*";
                        reqs.push_back(req);
                    }
                }
            }

            for (auto& req : reqs) {
                RequestsByNode_[read.Raw()].push_back(req);
                if (PendingRequests_.find(req) == PendingRequests_.end()) {
                    auto future = Lister_->List(req.Token, req.Url, req.Pattern);
                    PendingRequests_[req] = future;
                    futures.push_back(std::move(future));
                }
            }
        }

        return true;
    }

    const TS3State::TPtr State_;
    const IS3Lister::TPtr Lister_;

    TPendingRequests PendingRequests_;
    TNodeMap<TVector<TListRequest>> RequestsByNode_;
    TNodeMap<TGeneratedColumnsConfig> GenColumnsByNode_;
    NThreading::TFuture<void> AllFuture_;
};

}

THolder<IGraphTransformer> CreateS3IODiscoveryTransformer(TS3State::TPtr state, IHTTPGateway::TPtr gateway) {
    return THolder(new TS3IODiscoveryTransformer(std::move(state), std::move(gateway)));
}

}
