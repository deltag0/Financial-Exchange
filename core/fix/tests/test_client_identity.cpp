#include <gtest/gtest.h>

#include "client_identity.hpp"
#include "fix_parser.hpp"
#include "fix_task.hpp"

#include <string>

#if __cplusplus >= 201703L
#define throw(...)
#endif

#include <quickfix/SessionSettings.h>

#if __cplusplus >= 201703L
#undef throw
#endif

namespace exchange::core::fix {
namespace {

const FIX::SessionID PRIMARY_SESSION("FIX.4.2", "EXCHANGE", "CLIENT-A");
const FIX::SessionID ALTERNATE_SESSION("FIX.4.2", "EXCHANGE", "CLIENT-A-ALT");
const FIX::SessionID OTHER_CLIENT_SESSION("FIX.4.2", "EXCHANGE", "CLIENT-B");
const FIX::SessionID UNKNOWN_SESSION("FIX.4.2", "EXCHANGE", "UNKNOWN");

const ClientIdentityResolver& identityResolver() {
    static const ClientIdentityResolver resolver({
        {PRIMARY_SESSION, domain::ClientId{7001}},
        {ALTERNATE_SESSION, domain::ClientId{7001}},
        {OTHER_CLIENT_SESSION, domain::ClientId{8002}},
    });
    return resolver;
}

FIX::Message makeNewOrder(const std::string& clientCommandId) {
    FIX::Message message;
    message.getHeader().setField(FIX::MsgType("D"));
    message.setField(FIX::ClOrdID(clientCommandId));
    message.setField(FIX::Symbol("SPY"));
    message.setField(FIX::Side(FIX::Side_BUY));
    message.setField(FIX::OrderQty(10));
    message.setField(FIX::Price(100.0));
    message.setField(FIX::OrdType(FIX::OrdType_LIMIT));
    message.setField(FIX::TimeInForce(FIX::TimeInForce_GOOD_TILL_CANCEL));
    return message;
}

FIX::Message makeCancel(const std::string& clientCommandId, const std::string& targetOrderId) {
    FIX::Message message;
    message.getHeader().setField(FIX::MsgType("F"));
    message.setField(FIX::ClOrdID(clientCommandId));
    message.setField(FIX::StringField(FIX::FIELD::OrderID, targetOrderId));
    message.setField(FIX::Symbol("SPY"));
    return message;
}

} // namespace

TEST(FixClientIdentityTest, IsStableAcrossRepeatedParsingReconnectAndAlternateSessions) {
    const FIX::SessionID simulatedReconnect("FIX.4.2", "EXCHANGE", "CLIENT-A");

    const auto first = parseFixMessage(makeNewOrder("PRIMARY-1"), PRIMARY_SESSION, identityResolver());
    const auto repeated = parseFixMessage(makeNewOrder("PRIMARY-2"), PRIMARY_SESSION, identityResolver());
    const auto reconnected = parseFixMessage(makeNewOrder("RECONNECTED"), simulatedReconnect, identityResolver());
    const auto alternate = parseFixMessage(makeNewOrder("ALTERNATE"), ALTERNATE_SESSION, identityResolver());
    const auto other = parseFixMessage(makeNewOrder("OTHER"), OTHER_CLIENT_SESSION, identityResolver());

    EXPECT_EQ(first.clientId, domain::ClientId{7001});
    EXPECT_EQ(repeated.clientId, first.clientId);
    EXPECT_EQ(reconnected.clientId, first.clientId);
    EXPECT_EQ(alternate.clientId, first.clientId);
    EXPECT_EQ(other.clientId, domain::ClientId{8002});
    EXPECT_NE(other.clientId, first.clientId);
}

TEST(FixClientIdentityTest, NewOrderAndCancelUseConfiguredIdentity) {
    const auto newOrder = parseFixMessage(makeNewOrder("NEW"), PRIMARY_SESSION, identityResolver());
    const auto cancel = parseFixMessage(makeCancel("CANCEL", "99"), ALTERNATE_SESSION, identityResolver());

    EXPECT_EQ(newOrder.clientId, domain::ClientId{7001});
    EXPECT_EQ(cancel.clientId, domain::ClientId{7001});
    EXPECT_EQ(cancel.targetOrderId, std::optional<domain::TargetOrderId>{domain::TargetOrderId{99}});
}

TEST(FixClientIdentityTest, UnknownExactIdentityIsRejectedWithoutACommand) {
    EXPECT_THROW(parseFixMessage(makeNewOrder("UNKNOWN"), UNKNOWN_SESSION, identityResolver()), FixValidationError);
    const FIX::SessionID unconfiguredQualifier("FIX.4.2", "EXCHANGE", "CLIENT-A", "QUALIFIER");
    EXPECT_THROW(parseFixMessage(makeNewOrder("QUALIFIED"), unconfiguredQualifier, identityResolver()),
                 FixValidationError);

    const auto valid = parseFixMessage(makeNewOrder("AFTER"), PRIMARY_SESSION, identityResolver());
    EXPECT_EQ(valid.clientId, domain::ClientId{7001});
    ASSERT_TRUE(valid.clientCommandId.has_value());
    EXPECT_EQ(valid.clientCommandId->value(), "AFTER");
}

TEST(FixTaskClientIdentityTest, UnknownIdentityNeverEntersSequencingIngress) {
    core::SharedQueue<sequencer::sequenceMessage> sequencingIngressQueue(8);
    core::Bus bus(8);
    admission::CommandAdmissionIndex admissionIndex(8);
    task::FixTask fixTask(sequencingIngressQueue, bus, identityResolver(), admissionIndex);

    fixTask.fromApp(makeNewOrder("UNKNOWN"), UNKNOWN_SESSION);

    sequencer::sequenceMessage output{};
    EXPECT_FALSE(fixTask.processNextStagedCommand());
    EXPECT_FALSE(sequencingIngressQueue.pop(output));
    EXPECT_EQ(fixTask.statistics().normalizationRejections, 1u);
}

TEST(FixClientIdentityConfigurationTest, RepositorySessionHasStableConfiguredClientId) {
    const FIX::SessionSettings settings(EXCHANGE_CONFIG_PATH);
    const ClientIdentityResolver resolver(settings);
    const FIX::SessionSettings restartedSettings(EXCHANGE_CONFIG_PATH);
    const ClientIdentityResolver restartedResolver(restartedSettings);
    const FIX::SessionID repositorySession("FIX.4.2", "EXCHANGE", "CLIENT");

    EXPECT_EQ(resolver.resolve(repositorySession), std::optional<domain::ClientId>{domain::ClientId{1}});
    EXPECT_EQ(restartedResolver.resolve(repositorySession), resolver.resolve(repositorySession));
}

TEST(FixClientIdentityConfigurationTest, RejectsZeroAndDuplicateExactSessionMappings) {
    EXPECT_THROW(ClientIdentityResolver({{PRIMARY_SESSION, domain::ClientId{0}}}), std::invalid_argument);
    EXPECT_THROW(
        ClientIdentityResolver({{PRIMARY_SESSION, domain::ClientId{1}}, {PRIMARY_SESSION, domain::ClientId{2}}}),
        std::invalid_argument);
}

} // namespace exchange::core::fix
