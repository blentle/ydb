#include "token_accessor_client_factory.h"
#include "token_accessor_client.h"

#include <util/string/cast.h>

namespace NYql {

namespace {

class TTokenAccessorCredentialsProviderFactory : public NYdb::ICredentialsProviderFactory {
public:
    TTokenAccessorCredentialsProviderFactory(
        const TString& tokenAccessorEndpoint,
        bool useSsl,
        const TString& sslCaCert,
        const TString& serviceAccountId,
        const TString& serviceAccountIdSignature,
        const TDuration& refreshPeriod,
        const TDuration& requestTimeout
    )
        : TokenAccessorEndpoint(tokenAccessorEndpoint)
        , UseSsl(useSsl)
        , SslCaCert(sslCaCert)
        , ServiceAccountId(serviceAccountId)
        , ServiceAccountIdSignature(serviceAccountIdSignature)
        , RefreshPeriod(refreshPeriod)
        , RequestTimeout(requestTimeout)
    {
    }

    std::shared_ptr<NYdb::ICredentialsProvider> CreateProvider() const override {
        return CreateTokenAccessorCredentialsProvider(TokenAccessorEndpoint, UseSsl, SslCaCert, ServiceAccountId, ServiceAccountIdSignature, RefreshPeriod, RequestTimeout);
    }

private:
    const TString TokenAccessorEndpoint;
    const bool UseSsl;
    const TString SslCaCert;
    const TString ServiceAccountId;
    const TString ServiceAccountIdSignature;
    const TDuration RefreshPeriod;
    const TDuration RequestTimeout;
};

}

std::shared_ptr<NYdb::ICredentialsProviderFactory> CreateTokenAccessorCredentialsProviderFactory(
    const TString& tokenAccessorEndpoint,
    bool useSsl,
    const TString& sslCaCert,
    const TString& serviceAccountId,
    const TString& serviceAccountIdSignature,
    const TDuration& refreshPeriod,
    const TDuration& requestTimeout)
{
    return std::make_shared<TTokenAccessorCredentialsProviderFactory>(tokenAccessorEndpoint, useSsl, sslCaCert, serviceAccountId, serviceAccountIdSignature, refreshPeriod, requestTimeout);
}
}
