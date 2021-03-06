// Copyright (C) 2010  Internet Systems Consortium, Inc. ("ISC")
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
// REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
// AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
// INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
// LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
// OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
// PERFORMANCE OF THIS SOFTWARE.

#include <config.h>

#include <util/io/sockaddr_util.h>

#include <dns/message.h>
#include <dns/messagerenderer.h>
#include <dns/name.h>
#include <dns/opcode.h>
#include <dns/rrclass.h>
#include <dns/rrtype.h>
#include <dns/rrttl.h>
#include <dns/rdataclass.h>
#include <dns/tsig.h>

#include <cc/proto_defs.h>

#include <server_common/portconfig.h>
#include <server_common/keyring.h>

#include <datasrc/client_list.h>
#include <auth/auth_srv.h>
#include <auth/command.h>
#include <auth/common.h>
#include <auth/statistics.h>
#include <auth/statistics_items.h>
#include <auth/datasrc_config.h>

#include <util/unittests/mock_socketsession.h>
#include <dns/tests/unittest_util.h>
#include <testutils/dnsmessage_test.h>
#include <testutils/srv_test.h>
#include <testutils/mockups.h>
#include <testutils/portconfig.h>
#include <testutils/socket_request.h>

#include "statistics_util.h"

#include <gtest/gtest.h>

#include <boost/lexical_cast.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/scoped_ptr.hpp>
#include <boost/foreach.hpp>

#include <vector>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

using namespace std;
using namespace isc::cc;
using namespace isc::dns;
using namespace isc::datasrc;
using namespace isc::util;
using namespace isc::util::io::internal;
using namespace isc::util::unittests;
using namespace isc::dns::rdata;
using namespace isc::data;
using namespace isc::xfr;
using namespace isc::auth;
using namespace isc::asiodns;
using namespace isc::asiolink;
using namespace isc::testutils;
using namespace isc::server_common::portconfig;
using namespace isc::auth::unittest;
using isc::UnitTestUtil;
using boost::scoped_ptr;
using isc::auth::statistics::Counters;

namespace {
const char* const CONFIG_TESTDB =
    "{\"database_file\": \"" TEST_DATA_DIR "/example.sqlite3\"}";
// The following file must be non existent and must be non"creatable" (see
// the sqlite3 test).
const char* const BADCONFIG_TESTDB =
    "{ \"database_file\": \"" TEST_DATA_DIR "/nodir/notexist\"}";

const char* const STATIC_DSRC_FILE = DSRC_DIR "/static.zone";

// This is a configuration that uses the in-memory data source containing
// a signed example zone.
const char* const CONFIG_INMEMORY_EXAMPLE =
    TEST_DATA_DIR "/rfc5155-example.zone.signed";

// shortcut commonly used in tests
typedef boost::shared_ptr<ConfigurableClientList> ListPtr;

class AuthSrvTest : public SrvTestBase {
protected:
    AuthSrvTest() :
        dnss_(),
        server(xfrout, ddns_forwarder),
        // The empty string is expected value of the parameter of
        // requestSocket, not the app_name (there's no fallback, it checks
        // the empty string is passed).
        sock_requestor_(dnss_, address_store_, 53210, "")
    {
        server.setDNSService(dnss_);
        server.setXfrinSession(&notify_session);
        server.createDDNSForwarder();
        checkCountersAreInitialized();
    }

    ~AuthSrvTest() {
        server.destroyDDNSForwarder();
    }

    virtual void processMessage() {
        // If processMessage has been called before, parse_message needs
        // to be reset. If it hasn't, there's no harm in doing so
        parse_message->clear(Message::PARSE);
        server.processMessage(*io_message, *parse_message, *response_obuffer,
                              &dnsserv);
    }

    // Helper for checking Rcode statistic counters;
    // Checks for one specific Rcode statistics counter value
    void checkRcodeCounter(const std::string& rcode_name,
                           const int rcode_value,
                           const int expected_value) const
    {
            EXPECT_EQ(expected_value, rcode_value) <<
                      "Expected Rcode count for " << rcode_name <<
                      " " << expected_value << ", was: " <<
                      rcode_value;
    }

    // Checks whether all Rcode counters are set to zero except the given
    // rcode (it is checked to be set to 'value')
    void checkAllRcodeCountersZeroExcept(const Rcode& rcode, int value) const {
        std::string target_rcode_name = rcode.toText();
        std::transform(target_rcode_name.begin(), target_rcode_name.end(),
                       target_rcode_name.begin(), ::tolower);

        const std::map<std::string, ConstElementPtr>
            stats_map(server.getStatistics()->get("zones")->get("_SERVER_")->
                      get("rcode")->mapValue());

        for (std::map<std::string, ConstElementPtr>::const_iterator
                 i = stats_map.begin(), e = stats_map.end();
             i != e;
             ++i)
        {
            if (i->first.compare(target_rcode_name) == 0) {
                checkRcodeCounter(i->first, i->second->intValue(), value);
            } else {
                checkRcodeCounter(i->first, i->second->intValue(), 0);
            }
        }
    }

    // Convenience method for tests that expect to return SERVFAIL
    // It calls processMessage, checks if there is an answer, and
    // check the header for default SERVFAIL data
    void processAndCheckSERVFAIL() {
        processMessage();
        EXPECT_TRUE(dnsserv.hasAnswer());
        headerCheck(*parse_message, default_qid, Rcode::SERVFAIL(),
                    opcode.getCode(), QR_FLAG, 1, 0, 0, 0);
    }

    // Convenient shortcut of creating a simple request and having the
    // server process it.
    void createAndSendRequest(RRType req_type, Opcode opcode = Opcode::QUERY(),
                              const Name& req_name = Name("example.com"),
                              RRClass req_class = RRClass::IN(),
                              int protocol = IPPROTO_UDP,
                              const char* const remote_address =
                              DEFAULT_REMOTE_ADDRESS,
                              uint16_t remote_port = DEFAULT_REMOTE_PORT)
    {
        UnitTestUtil::createRequestMessage(request_message, opcode,
                                           default_qid, req_name,
                                           req_class, req_type);
        createRequestPacket(request_message, protocol, NULL,
                            remote_address, remote_port);
        parse_message->clear(Message::PARSE);
        server.processMessage(*io_message, *parse_message, *response_obuffer,
                              &dnsserv);
    }

    // Check if the counters exist and are initialized to 0.
    void
    checkCountersAreInitialized() {
        const std::map<std::string, int> expect;
        ConstElementPtr stats = server.getStatistics()->
            get("zones")->get("_SERVER_");
        checkStatisticsCounters(stats, expect);
    }

    MockDNSService dnss_;
    MockXfroutClient xfrout;
    MockSocketSessionForwarder ddns_forwarder;
    AuthSrv server;
    vector<uint8_t> response_data;
    AddressList address_store_;
    TestSocketRequestor sock_requestor_;
};

// A helper function that builds a response to version.bind/TXT/CH that
// should be identical to the response from our builtin (static) data source
// by default.  The resulting wire-format data will be stored in 'data'.
void
createBuiltinVersionResponse(const qid_t qid, vector<uint8_t>& data) {
    const Name version_name("VERSION.BIND.");
    const Name apex_name("BIND.");
    Message message(Message::RENDER);

    UnitTestUtil::createRequestMessage(message, Opcode::QUERY(),
                                       qid, version_name,
                                       RRClass::CH(), RRType::TXT());
    message.setHeaderFlag(Message::HEADERFLAG_QR);
    message.setHeaderFlag(Message::HEADERFLAG_AA);
    RRsetPtr rrset_version = RRsetPtr(new RRset(version_name, RRClass::CH(),
                                                RRType::TXT(), RRTTL(0)));
    rrset_version->addRdata(generic::TXT("\"" PACKAGE_STRING "\""));
    message.addRRset(Message::SECTION_ANSWER, rrset_version);

    RRsetPtr rrset_version_ns = RRsetPtr(new RRset(apex_name, RRClass::CH(),
                                                   RRType::NS(), RRTTL(0)));
    rrset_version_ns->addRdata(generic::NS(apex_name));
    message.addRRset(Message::SECTION_AUTHORITY, rrset_version_ns);

    MessageRenderer renderer;
    message.toWire(renderer);

    data.clear();
    data.assign(static_cast<const uint8_t*>(renderer.getData()),
                static_cast<const uint8_t*>(renderer.getData()) +
                renderer.getLength());
}

void
installDataSrcClientLists(AuthSrv& server, ClientListMapPtr lists) {
    // For now, we use explicit swap than reconfigure() because the latter
    // involves a separate thread and cannot guarantee the new config is
    // available for the subsequent test.
    server.getDataSrcClientsMgr().setDataSrcClientLists(lists);
}

void
updateDatabase(AuthSrv& server, const char* params) {
    const ConstElementPtr config(Element::fromJSON("{"
        "\"IN\": [{"
        "    \"type\": \"sqlite3\","
        "    \"params\": " + string(params) +
        "}]}"));
    installDataSrcClientLists(server, configureDataSource(config));
}

void
updateInMemory(AuthSrv& server, const char* origin, const char* filename,
               bool with_static = true)
{
    string spec_txt = "{"
        "\"IN\": [{"
        "   \"type\": \"MasterFiles\","
        "   \"params\": {"
        "       \"" + string(origin) + "\": \"" + string(filename) + "\""
        "   },"
        "   \"cache-enable\": true"
        "}]";
    if (with_static) {
        spec_txt += ", \"CH\": [{"
        "   \"type\": \"MasterFiles\","
        "   \"cache-enable\": true,"
        "   \"params\": {\"BIND\": \"" + string(STATIC_DSRC_FILE) + "\"}"
            "}]";
    }
    spec_txt += "}";

    const ConstElementPtr config(Element::fromJSON(spec_txt));
    installDataSrcClientLists(server, configureDataSource(config));
}

void
updateBuiltin(AuthSrv& server) {
    const ConstElementPtr config(Element::fromJSON("{"
        "\"CH\": [{"
        "   \"type\": \"MasterFiles\","
        "   \"cache-enable\": true,"
        "   \"params\": {\"BIND\": \"" + string(STATIC_DSRC_FILE) + "\"}"
        "}]}"));
    installDataSrcClientLists(server, configureDataSource(config));
}

// We did not configure any client lists. Therefore it should be REFUSED
TEST_F(AuthSrvTest, noClientList) {
    UnitTestUtil::createRequestMessage(request_message, Opcode::QUERY(),
                                       default_qid, Name("version.bind"),
                                       RRClass::CH(), RRType::TXT());
    createRequestPacket(request_message, IPPROTO_UDP);
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);

    EXPECT_TRUE(dnsserv.hasAnswer());
    headerCheck(*parse_message, default_qid, Rcode::REFUSED(),
                opcode.getCode(), QR_FLAG, 1, 0, 0, 0);

    ConstElementPtr stats_after = server.getStatistics()->get("zones")->
        get("_SERVER_");
    std::map<std::string, int> expect;
    expect["request.v4"] = 1;
    expect["request.udp"] = 1;
    expect["opcode.query"] = 1;
    expect["responses"] = 1;
    expect["qrynoauthans"] = 1;
    expect["authqryrej"] = 1;
    expect["rcode.refused"] = 1;
    checkStatisticsCounters(stats_after, expect);
}

// Unsupported requests.  Should result in NOTIMP.
TEST_F(AuthSrvTest, unsupportedRequest) {
    unsupportedRequest();
    // unsupportedRequest tries 13 different opcodes
    checkAllRcodeCountersZeroExcept(Rcode::NOTIMP(), 13);
}

// Multiple questions.  Should result in FORMERR.
TEST_F(AuthSrvTest, multiQuestion) {
    multiQuestion();
    checkAllRcodeCountersZeroExcept(Rcode::FORMERR(), 1);
}

// Incoming data doesn't even contain the complete header.  Must be silently
// dropped.
TEST_F(AuthSrvTest, shortMessage) {
    shortMessage();

    ConstElementPtr stats_after = server.getStatistics()->get("zones")->
        get("_SERVER_");
    std::map<std::string, int> expect;
    expect["request.v4"] = 1;
    expect["request.udp"] = 1;
    checkStatisticsCounters(stats_after, expect);
}

// Response messages.  Must be silently dropped, whether it's a valid response
// or malformed or could otherwise cause a protocol error.
TEST_F(AuthSrvTest, response) {
    // isc::testutils::SrvTestBase::response() processes 3 messages.
    response();

    ConstElementPtr stats_after = server.getStatistics()->get("zones")->
        get("_SERVER_");
    std::map<std::string, int> expect;
    expect["request.v4"] = 3;
    expect["request.udp"] = 3;
    checkStatisticsCounters(stats_after, expect);
}

// Query with a broken question
TEST_F(AuthSrvTest, shortQuestion) {
    shortQuestion();
    ConstElementPtr stats_after = server.getStatistics()->get("zones")->
        get("_SERVER_");
    std::map<std::string, int> expect;
    expect["request.v4"] = 1;
    expect["request.udp"] = 1;
    expect["opcode.query"] = 1;
    expect["responses"] = 1;
    expect["rcode.formerr"] = 1;
    expect["qrynoauthans"] = 1;
    checkStatisticsCounters(stats_after, expect);
}

// Query with a broken answer section
TEST_F(AuthSrvTest, shortAnswer) {
    shortAnswer();
    ConstElementPtr stats_after = server.getStatistics()->get("zones")->
        get("_SERVER_");
    std::map<std::string, int> expect;
    expect["request.v4"] = 1;
    expect["request.udp"] = 1;
    expect["opcode.query"] = 1;
    expect["responses"] = 1;
    expect["rcode.formerr"] = 1;
    expect["qrynoauthans"] = 1;
    checkStatisticsCounters(stats_after, expect);
}

// Query with unsupported version of EDNS.
TEST_F(AuthSrvTest, ednsBadVers) {
    ednsBadVers();

    ConstElementPtr stats_after = server.getStatistics()->get("zones")->
        get("_SERVER_");
    std::map<std::string, int> expect;
    expect["request.v4"] = 1;
    expect["request.badednsver"] = 1;
    expect["request.udp"] = 1;
    expect["opcode.query"] = 1;
    expect["responses"] = 1;
    expect["rcode.badvers"] = 1;
    expect["qrynoauthans"] = 1;
    checkStatisticsCounters(stats_after, expect);
}

TEST_F(AuthSrvTest, AXFROverUDP) {
    axfrOverUDP();
}

TEST_F(AuthSrvTest, AXFRSuccess) {
    EXPECT_FALSE(xfrout.isConnected());
    UnitTestUtil::createRequestMessage(request_message, opcode, default_qid,
                                       Name("example.com"), RRClass::IN(),
                                       RRType::AXFR());
    createRequestPacket(request_message, IPPROTO_TCP);
    // On success, the AXFR query has been passed to a separate process,
    // so we shouldn't have to respond.
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);
    EXPECT_FALSE(dnsserv.hasAnswer());
    EXPECT_TRUE(xfrout.isConnected());

    ConstElementPtr stats_after = server.getStatistics()->get("zones")->
        get("_SERVER_");
    std::map<std::string, int> expect;
    expect["request.v4"] = 1;
    expect["request.tcp"] = 1;
    expect["opcode.query"] = 1;
    checkStatisticsCounters(stats_after, expect);
}

// Give the server a signed request, but don't give it the key. It will
// not be able to verify it, returning BADKEY
TEST_F(AuthSrvTest, TSIGSignedBadKey) {
    TSIGKey key("key:c2VjcmV0Cg==:hmac-sha1");
    TSIGContext context(key);
    UnitTestUtil::createRequestMessage(request_message, opcode, default_qid,
                                       Name("version.bind"), RRClass::CH(),
                                       RRType::TXT());
    createRequestPacket(request_message, IPPROTO_UDP, &context);

    // Process the message, but use a different key there
    boost::shared_ptr<TSIGKeyRing> keyring(new TSIGKeyRing);
    server.setTSIGKeyRing(&keyring);
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);

    EXPECT_TRUE(dnsserv.hasAnswer());
    headerCheck(*parse_message, default_qid, TSIGError::BAD_KEY().toRcode(),
                opcode.getCode(), QR_FLAG, 1, 0, 0, 0);
    // We need to parse the message ourself, or getTSIGRecord won't work
    InputBuffer ib(response_obuffer->getData(), response_obuffer->getLength());
    Message m(Message::PARSE);
    m.fromWire(ib);

    const TSIGRecord* tsig = m.getTSIGRecord();
    ASSERT_TRUE(tsig != NULL) <<
        "Missing TSIG signature (we should have one even at error)";
    EXPECT_EQ(TSIGError::BAD_KEY_CODE, tsig->getRdata().getError());
    EXPECT_EQ(0, tsig->getRdata().getMACSize()) <<
        "It should be unsigned with this error";

    // check Rcode counters and TSIG counters
    checkAllRcodeCountersZeroExcept(Rcode::NOTAUTH(), 1);
    ConstElementPtr stats_after = server.getStatistics()->get("zones")->
        get("_SERVER_");
    std::map<std::string, int> expect;
    expect["request.v4"] = 1;
    expect["request.tsig"] = 1;
    expect["request.badsig"] = 1;
    expect["request.udp"] = 1;
    expect["opcode.query"] = 1;
    expect["responses"] = 1;
    expect["response.tsig"] = 1;
    expect["rcode.notauth"] = 1;
    checkStatisticsCounters(stats_after, expect);
}

// Give the server a signed request, but signed by a different key
// (with the same name). It should return BADSIG
TEST_F(AuthSrvTest, TSIGBadSig) {
    TSIGKey key("key:c2VjcmV0Cg==:hmac-sha1");
    TSIGContext context(key);
    UnitTestUtil::createRequestMessage(request_message, opcode, default_qid,
                                       Name("version.bind"), RRClass::CH(),
                                       RRType::TXT());
    createRequestPacket(request_message, IPPROTO_UDP, &context);

    // Process the message, but use a different key there
    boost::shared_ptr<TSIGKeyRing> keyring(new TSIGKeyRing);
    keyring->add(TSIGKey("key:QkFECg==:hmac-sha1"));
    server.setTSIGKeyRing(&keyring);
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);

    EXPECT_TRUE(dnsserv.hasAnswer());
    headerCheck(*parse_message, default_qid, TSIGError::BAD_SIG().toRcode(),
                opcode.getCode(), QR_FLAG, 1, 0, 0, 0);
    // We need to parse the message ourself, or getTSIGRecord won't work
    InputBuffer ib(response_obuffer->getData(), response_obuffer->getLength());
    Message m(Message::PARSE);
    m.fromWire(ib);

    const TSIGRecord* tsig = m.getTSIGRecord();
    ASSERT_TRUE(tsig != NULL) <<
        "Missing TSIG signature (we should have one even at error)";
    EXPECT_EQ(TSIGError::BAD_SIG_CODE, tsig->getRdata().getError());
    EXPECT_EQ(0, tsig->getRdata().getMACSize()) <<
        "It should be unsigned with this error";

    ConstElementPtr stats_after = server.getStatistics()->get("zones")->
        get("_SERVER_");
    std::map<std::string, int> expect;
    expect["request.v4"] = 1;
    expect["request.tsig"] = 1;
    expect["request.badsig"] = 1;
    expect["request.udp"] = 1;
    expect["opcode.query"] = 1;
    expect["responses"] = 1;
    expect["response.tsig"] = 1;
    expect["rcode.notauth"] = 1;
    checkStatisticsCounters(stats_after, expect);
}

// Give the server a signed unsupported request with a bad signature.
// This checks the server first verifies the signature before anything
// else.
TEST_F(AuthSrvTest, TSIGCheckFirst) {
    TSIGKey key("key:c2VjcmV0Cg==:hmac-sha1");
    TSIGContext context(key);
    // Pass a wrong opcode there. The server shouldn't know what to do
    // about it.
    UnitTestUtil::createRequestMessage(request_message, Opcode::RESERVED14(),
                                       default_qid, Name("version.bind"),
                                       RRClass::CH(), RRType::TXT());
    createRequestPacket(request_message, IPPROTO_UDP, &context);

    // Process the message, but use a different key there
    boost::shared_ptr<TSIGKeyRing> keyring(new TSIGKeyRing);
    keyring->add(TSIGKey("key:QkFECg==:hmac-sha1"));
    server.setTSIGKeyRing(&keyring);
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);

    EXPECT_TRUE(dnsserv.hasAnswer());
    headerCheck(*parse_message, default_qid, TSIGError::BAD_SIG().toRcode(),
                Opcode::RESERVED14().getCode(), QR_FLAG, 0, 0, 0, 0);
    // We need to parse the message ourself, or getTSIGRecord won't work
    InputBuffer ib(response_obuffer->getData(), response_obuffer->getLength());
    Message m(Message::PARSE);
    m.fromWire(ib);

    const TSIGRecord* tsig = m.getTSIGRecord();
    ASSERT_TRUE(tsig != NULL) <<
        "Missing TSIG signature (we should have one even at error)";
    EXPECT_EQ(TSIGError::BAD_SIG_CODE, tsig->getRdata().getError());
    EXPECT_EQ(0, tsig->getRdata().getMACSize()) <<
        "It should be unsigned with this error";

    ConstElementPtr stats_after = server.getStatistics()->get("zones")->
        get("_SERVER_");
    std::map<std::string, int> expect;
    expect["request.v4"] = 1;
    expect["request.tsig"] = 1;
    expect["request.badsig"] = 1;
    expect["request.udp"] = 1;
    expect["opcode.other"] = 1;
    expect["responses"] = 1;
    expect["response.tsig"] = 1;
    expect["rcode.notauth"] = 1;
    checkStatisticsCounters(stats_after, expect);
}

TEST_F(AuthSrvTest, AXFRConnectFail) {
    EXPECT_FALSE(xfrout.isConnected()); // check prerequisite
    xfrout.disableConnect();
    UnitTestUtil::createRequestMessage(request_message, opcode, default_qid,
                                       Name("example.com"), RRClass::IN(),
                                       RRType::AXFR());
    createRequestPacket(request_message, IPPROTO_TCP);
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);
    EXPECT_TRUE(dnsserv.hasAnswer());
    headerCheck(*parse_message, default_qid, Rcode::SERVFAIL(),
                opcode.getCode(), QR_FLAG, 1, 0, 0, 0);
    EXPECT_FALSE(xfrout.isConnected());
}

TEST_F(AuthSrvTest, AXFRSendFail) {
    // first send a valid query, making the connection with the xfr process
    // open.
    UnitTestUtil::createRequestMessage(request_message, opcode, default_qid,
                                       Name("example.com"), RRClass::IN(),
                                       RRType::AXFR());
    createRequestPacket(request_message, IPPROTO_TCP);
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);
    EXPECT_TRUE(xfrout.isConnected());

    xfrout.disableSend();
    parse_message->clear(Message::PARSE);
    response_obuffer->clear();
    UnitTestUtil::createRequestMessage(request_message, opcode, default_qid,
                                       Name("example.com"), RRClass::IN(),
                                       RRType::AXFR());
    createRequestPacket(request_message, IPPROTO_TCP);
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);
    EXPECT_TRUE(dnsserv.hasAnswer());
    headerCheck(*parse_message, default_qid, Rcode::SERVFAIL(),
                opcode.getCode(), QR_FLAG, 1, 0, 0, 0);

    // The connection should have been closed due to the send failure.
    EXPECT_FALSE(xfrout.isConnected());
}

TEST_F(AuthSrvTest, AXFRDisconnectFail) {
    // In our usage disconnect() shouldn't fail. But even if it does,
    // it should not disrupt service (so processMessage should have caught it)
    xfrout.disableSend();
    xfrout.disableDisconnect();
    UnitTestUtil::createRequestMessage(request_message, opcode, default_qid,
                                       Name("example.com"), RRClass::IN(),
                                       RRType::AXFR());
    createRequestPacket(request_message, IPPROTO_TCP);
    EXPECT_NO_THROW(server.processMessage(*io_message, *parse_message,
                                          *response_obuffer, &dnsserv));
    // Since the disconnect failed, we should still be 'connected'
    EXPECT_TRUE(xfrout.isConnected());
    // XXX: we need to re-enable disconnect.  otherwise an exception would be
    // thrown via the destructor of the server.
    xfrout.enableDisconnect();
}

TEST_F(AuthSrvTest, IXFRConnectFail) {
    EXPECT_FALSE(xfrout.isConnected()); // check prerequisite
    xfrout.disableConnect();
    UnitTestUtil::createRequestMessage(request_message, opcode, default_qid,
                                       Name("example.com"), RRClass::IN(),
                                       RRType::IXFR());
    createRequestPacket(request_message, IPPROTO_TCP);
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);
    EXPECT_TRUE(dnsserv.hasAnswer());
    headerCheck(*parse_message, default_qid, Rcode::SERVFAIL(),
                opcode.getCode(), QR_FLAG, 1, 0, 0, 0);
    EXPECT_FALSE(xfrout.isConnected());
}

TEST_F(AuthSrvTest, IXFRSendFail) {
    // first send a valid query, making the connection with the xfr process
    // open.
    UnitTestUtil::createRequestMessage(request_message, opcode, default_qid,
                                       Name("example.com"), RRClass::IN(),
                                       RRType::IXFR());
    createRequestPacket(request_message, IPPROTO_TCP);
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);
    EXPECT_TRUE(xfrout.isConnected());

    xfrout.disableSend();
    parse_message->clear(Message::PARSE);
    response_obuffer->clear();
    UnitTestUtil::createRequestMessage(request_message, opcode, default_qid,
                                       Name("example.com"), RRClass::IN(),
                                       RRType::IXFR());
    createRequestPacket(request_message, IPPROTO_TCP);
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);
    EXPECT_TRUE(dnsserv.hasAnswer());
    headerCheck(*parse_message, default_qid, Rcode::SERVFAIL(),
                opcode.getCode(), QR_FLAG, 1, 0, 0, 0);

    // The connection should have been closed due to the send failure.
    EXPECT_FALSE(xfrout.isConnected());
}

TEST_F(AuthSrvTest, IXFRDisconnectFail) {
    // In our usage disconnect() shouldn't fail, but even if it does,
    // procesMessage() should catch it.
    xfrout.disableSend();
    xfrout.disableDisconnect();
    UnitTestUtil::createRequestMessage(request_message, opcode, default_qid,
                                       Name("example.com"), RRClass::IN(),
                                       RRType::IXFR());
    createRequestPacket(request_message, IPPROTO_TCP);
    EXPECT_NO_THROW(server.processMessage(*io_message, *parse_message,
                                          *response_obuffer, &dnsserv));
    EXPECT_TRUE(xfrout.isConnected());
    // XXX: we need to re-enable disconnect.  otherwise an exception would be
    // thrown via the destructor of the server.
    xfrout.enableDisconnect();
}

TEST_F(AuthSrvTest, notify) {
    updateInMemory(server, "example.", CONFIG_INMEMORY_EXAMPLE, false);

    UnitTestUtil::createRequestMessage(request_message, Opcode::NOTIFY(),
                                       default_qid, Name("example"),
                                       RRClass::IN(), RRType::SOA());
    request_message.setHeaderFlag(Message::HEADERFLAG_AA);
    createRequestPacket(request_message, IPPROTO_UDP);
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);
    EXPECT_TRUE(dnsserv.hasAnswer());

    // An internal command message should have been created and sent to an
    // external module.  Check them.
    EXPECT_EQ("Zonemgr", notify_session.getMessageDest());
    EXPECT_EQ("notify",
              notify_session.getSentMessage()->get("command")->get(0)->
                  stringValue());
    ConstElementPtr notify_args =
        notify_session.getSentMessage()->get("command")->get(1);
    EXPECT_EQ("example.", notify_args->get("zone_name")->stringValue());
    EXPECT_EQ(DEFAULT_REMOTE_ADDRESS,
              notify_args->get("master")->stringValue());
    EXPECT_EQ("IN", notify_args->get("zone_class")->stringValue());

    // On success, the server should return a response to the notify.
    headerCheck(*parse_message, default_qid, Rcode::NOERROR(),
                Opcode::NOTIFY().getCode(), QR_FLAG | AA_FLAG, 1, 0, 0, 0);

    // The question must be identical to that of the received notify
    ConstQuestionPtr question = *parse_message->beginQuestion();
    EXPECT_EQ(Name("example"), question->getName());
    EXPECT_EQ(RRClass::IN(), question->getClass());
    EXPECT_EQ(RRType::SOA(), question->getType());

    ConstElementPtr stats_after = server.getStatistics()->get("zones")->
        get("_SERVER_");
    std::map<std::string, int> expect;
    expect["request.v4"] = 1;
    expect["request.udp"] = 1;
    expect["opcode.notify"] = 1;
    expect["responses"] = 1;
    expect["rcode.noerror"] = 1;
    checkStatisticsCounters(stats_after, expect);
}

TEST_F(AuthSrvTest, notifyForCHClass) {
    // Same as the previous test, but for the CH RRClass (so we install the
    // builtin (static) data source.
    updateBuiltin(server);

    UnitTestUtil::createRequestMessage(request_message, Opcode::NOTIFY(),
                                       default_qid, Name("bind"),
                                       RRClass::CH(), RRType::SOA());
    request_message.setHeaderFlag(Message::HEADERFLAG_AA);
    createRequestPacket(request_message, IPPROTO_UDP);
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);
    EXPECT_TRUE(dnsserv.hasAnswer());

    // Other conditions should be the same, so simply confirm the RR class is
    // set correctly.
    ConstElementPtr notify_args =
        notify_session.getSentMessage()->get("command")->get(1);
    EXPECT_EQ("CH", notify_args->get("zone_class")->stringValue());

    ConstElementPtr stats_after = server.getStatistics()->get("zones")->
        get("_SERVER_");
    std::map<std::string, int> expect;
    expect["request.v4"] = 1;
    expect["request.udp"] = 1;
    expect["opcode.notify"] = 1;
    expect["responses"] = 1;
    expect["rcode.noerror"] = 1;
    checkStatisticsCounters(stats_after, expect);
}

TEST_F(AuthSrvTest, notifyEmptyQuestion) {
    request_message.clear(Message::RENDER);
    request_message.setOpcode(Opcode::NOTIFY());
    request_message.setRcode(Rcode::NOERROR());
    request_message.setHeaderFlag(Message::HEADERFLAG_AA);
    request_message.setQid(default_qid);
    request_message.toWire(request_renderer);
    createRequestPacket(request_message, IPPROTO_UDP);
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);
    EXPECT_TRUE(dnsserv.hasAnswer());
    headerCheck(*parse_message, default_qid, Rcode::FORMERR(),
                Opcode::NOTIFY().getCode(), QR_FLAG, 0, 0, 0, 0);

    ConstElementPtr stats_after = server.getStatistics()->get("zones")->
        get("_SERVER_");
    std::map<std::string, int> expect;
    expect["request.v4"] = 1;
    expect["request.udp"] = 1;
    expect["opcode.notify"] = 1;
    expect["responses"] = 1;
    expect["rcode.formerr"] = 1;
    checkStatisticsCounters(stats_after, expect);
}

TEST_F(AuthSrvTest, notifyMultiQuestions) {
    UnitTestUtil::createRequestMessage(request_message, Opcode::NOTIFY(),
                                       default_qid, Name("example.com"),
                                       RRClass::IN(), RRType::SOA());
    // add one more SOA question
    request_message.addQuestion(Question(Name("example.com"), RRClass::IN(),
                                         RRType::SOA()));
    request_message.setHeaderFlag(Message::HEADERFLAG_AA);
    createRequestPacket(request_message, IPPROTO_UDP);
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);
    EXPECT_TRUE(dnsserv.hasAnswer());
    headerCheck(*parse_message, default_qid, Rcode::FORMERR(),
                Opcode::NOTIFY().getCode(), QR_FLAG, 2, 0, 0, 0);
}

TEST_F(AuthSrvTest, notifyNonSOAQuestion) {
    UnitTestUtil::createRequestMessage(request_message, Opcode::NOTIFY(),
                                       default_qid, Name("example.com"),
                                       RRClass::IN(), RRType::NS());
    request_message.setHeaderFlag(Message::HEADERFLAG_AA);
    createRequestPacket(request_message, IPPROTO_UDP);
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);
    EXPECT_TRUE(dnsserv.hasAnswer());
    headerCheck(*parse_message, default_qid, Rcode::FORMERR(),
                Opcode::NOTIFY().getCode(), QR_FLAG, 1, 0, 0, 0);
}

TEST_F(AuthSrvTest, notifyWithoutAA) {
    updateInMemory(server, "example.", CONFIG_INMEMORY_EXAMPLE, false);

    // implicitly leave the AA bit off.  our implementation will accept it.
    UnitTestUtil::createRequestMessage(request_message, Opcode::NOTIFY(),
                                       default_qid, Name("example"),
                                       RRClass::IN(), RRType::SOA());
    createRequestPacket(request_message, IPPROTO_UDP);
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);
    EXPECT_TRUE(dnsserv.hasAnswer());
    headerCheck(*parse_message, default_qid, Rcode::NOERROR(),
                Opcode::NOTIFY().getCode(), QR_FLAG | AA_FLAG, 1, 0, 0, 0);
}

TEST_F(AuthSrvTest, notifyWithErrorRcode) {
    updateInMemory(server, "example.", CONFIG_INMEMORY_EXAMPLE, false);

    UnitTestUtil::createRequestMessage(request_message, Opcode::NOTIFY(),
                                       default_qid, Name("example"),
                                       RRClass::IN(), RRType::SOA());
    request_message.setHeaderFlag(Message::HEADERFLAG_AA);
    request_message.setRcode(Rcode::SERVFAIL());
    createRequestPacket(request_message, IPPROTO_UDP);
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);
    EXPECT_TRUE(dnsserv.hasAnswer());
    headerCheck(*parse_message, default_qid, Rcode::NOERROR(),
                Opcode::NOTIFY().getCode(), QR_FLAG | AA_FLAG, 1, 0, 0, 0);
}

TEST_F(AuthSrvTest, notifyWithoutRecipient) {
    updateInMemory(server, "example.", CONFIG_INMEMORY_EXAMPLE, false);

    // Emulate the case where msgq tells auth there's no Zonemgr module.
    notify_session.setMessage(isc::config::createAnswer(CC_REPLY_NO_RECPT,
                                                        "no recipient"));

    UnitTestUtil::createRequestMessage(request_message, Opcode::NOTIFY(),
                                       default_qid, Name("example"),
                                       RRClass::IN(), RRType::SOA());
    request_message.setHeaderFlag(Message::HEADERFLAG_AA);
    createRequestPacket(request_message, IPPROTO_UDP);

    // we simply ignore the notify and let it be resent if an internal error
    // happens.
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);
    // want_answer should have been set to true so auth can catch it if zonemgr
    // is not running.
    EXPECT_TRUE(notify_session.wasAnswerWanted());
    EXPECT_FALSE(dnsserv.hasAnswer());
}

TEST_F(AuthSrvTest, notifySendFail) {
    updateInMemory(server, "example.", CONFIG_INMEMORY_EXAMPLE, false);

    notify_session.disableSend();

    UnitTestUtil::createRequestMessage(request_message, Opcode::NOTIFY(),
                                       default_qid, Name("example"),
                                       RRClass::IN(), RRType::SOA());
    request_message.setHeaderFlag(Message::HEADERFLAG_AA);
    createRequestPacket(request_message, IPPROTO_UDP);

    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);
    EXPECT_FALSE(dnsserv.hasAnswer());
}

TEST_F(AuthSrvTest, notifyReceiveFail) {
    updateInMemory(server, "example.", CONFIG_INMEMORY_EXAMPLE, false);

    notify_session.disableReceive();

    UnitTestUtil::createRequestMessage(request_message, Opcode::NOTIFY(),
                                       default_qid, Name("example"),
                                       RRClass::IN(), RRType::SOA());
    request_message.setHeaderFlag(Message::HEADERFLAG_AA);
    createRequestPacket(request_message, IPPROTO_UDP);
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);
    EXPECT_FALSE(dnsserv.hasAnswer());
}

TEST_F(AuthSrvTest, notifyWithBogusSessionMessage) {
    updateInMemory(server, "example.", CONFIG_INMEMORY_EXAMPLE, false);

    notify_session.setMessage(Element::fromJSON("{\"foo\": 1}"));

    UnitTestUtil::createRequestMessage(request_message, Opcode::NOTIFY(),
                                       default_qid, Name("example"),
                                       RRClass::IN(), RRType::SOA());
    request_message.setHeaderFlag(Message::HEADERFLAG_AA);
    createRequestPacket(request_message, IPPROTO_UDP);
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);
    EXPECT_FALSE(dnsserv.hasAnswer());
}

TEST_F(AuthSrvTest, notifyWithSessionMessageError) {
    updateInMemory(server, "example.", CONFIG_INMEMORY_EXAMPLE, false);

    notify_session.setMessage(
        Element::fromJSON("{\"result\": [1, \"FAIL\"]}"));

    UnitTestUtil::createRequestMessage(request_message, Opcode::NOTIFY(),
                                       default_qid, Name("example"),
                                       RRClass::IN(), RRType::SOA());
    request_message.setHeaderFlag(Message::HEADERFLAG_AA);
    createRequestPacket(request_message, IPPROTO_UDP);
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);
    EXPECT_FALSE(dnsserv.hasAnswer());
}

TEST_F(AuthSrvTest, notifyNotAuth) {
    // If the server doesn't have authority of the specified zone in NOTIFY,
    // it will return NOTAUTH
    updateInMemory(server, "example.", CONFIG_INMEMORY_EXAMPLE, false);

    UnitTestUtil::createRequestMessage(request_message, Opcode::NOTIFY(),
                                       default_qid, Name("example.com"),
                                       RRClass::IN(), RRType::SOA());
    request_message.setHeaderFlag(Message::HEADERFLAG_AA);
    createRequestPacket(request_message, IPPROTO_UDP);
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);
    EXPECT_TRUE(dnsserv.hasAnswer());
    headerCheck(*parse_message, default_qid, Rcode::NOTAUTH(),
                Opcode::NOTIFY().getCode(), QR_FLAG /* no AA */, 1, 0, 0, 0);
}

TEST_F(AuthSrvTest, notifyNotAuthSubDomain) {
    // Similar to the previous case, but checking partial match doesn't confuse
    // the processing.
    updateInMemory(server, "example.", CONFIG_INMEMORY_EXAMPLE, false);

    UnitTestUtil::createRequestMessage(request_message, Opcode::NOTIFY(),
                                       default_qid, Name("child.example"),
                                       RRClass::IN(), RRType::SOA());
    createRequestPacket(request_message, IPPROTO_UDP);
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);
    headerCheck(*parse_message, default_qid, Rcode::NOTAUTH(),
                Opcode::NOTIFY().getCode(), QR_FLAG, 1, 0, 0, 0);
}

TEST_F(AuthSrvTest, notifyNotAuthNoClass) {
    // Likewise, and there's not even a data source in the specified class.
    updateInMemory(server, "example.", CONFIG_INMEMORY_EXAMPLE, false);

    UnitTestUtil::createRequestMessage(request_message, Opcode::NOTIFY(),
                                       default_qid, Name("example"),
                                       RRClass::CH(), RRType::SOA());
    createRequestPacket(request_message, IPPROTO_UDP);
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);
    headerCheck(*parse_message, default_qid, Rcode::NOTAUTH(),
                Opcode::NOTIFY().getCode(), QR_FLAG, 1, 0, 0, 0);
}

// Try giving the server a TSIG signed request and see it can anwer signed as
// well
TEST_F(AuthSrvTest, TSIGSigned) {
    // Prepare key, the client message, etc
    updateBuiltin(server);
    const TSIGKey key("key:c2VjcmV0Cg==:hmac-sha1");
    TSIGContext context(key);
    UnitTestUtil::createRequestMessage(request_message, opcode, default_qid,
                                       Name("VERSION.BIND."), RRClass::CH(),
                                       RRType::TXT());
    createRequestPacket(request_message, IPPROTO_UDP, &context);

    // Run the message through the server
    boost::shared_ptr<TSIGKeyRing> keyring(new TSIGKeyRing);
    keyring->add(key);
    server.setTSIGKeyRing(&keyring);
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);

    // What did we get?
    EXPECT_TRUE(dnsserv.hasAnswer());
    headerCheck(*parse_message, default_qid, Rcode::NOERROR(),
                opcode.getCode(), QR_FLAG | AA_FLAG, 1, 1, 1, 0);
    // We need to parse the message ourself, or getTSIGRecord won't work
    InputBuffer ib(response_obuffer->getData(), response_obuffer->getLength());
    Message m(Message::PARSE);
    m.fromWire(ib);

    const TSIGRecord* tsig = m.getTSIGRecord();
    ASSERT_TRUE(tsig != NULL) << "Missing TSIG signature";
    TSIGError error(context.verify(tsig, response_obuffer->getData(),
                                   response_obuffer->getLength()));
    EXPECT_EQ(TSIGError::NOERROR(), error) <<
        "The server signed the response, but it doesn't seem to be valid";

    checkAllRcodeCountersZeroExcept(Rcode::NOERROR(), 1);
    ConstElementPtr stats_after = server.getStatistics()->get("zones")->
        get("_SERVER_");
    std::map<std::string, int> expect;
    expect["request.v4"] = 1;
    expect["request.tsig"] = 1;
    expect["request.udp"] = 1;
    expect["opcode.query"] = 1;
    expect["responses"] = 1;
    expect["response.tsig"] = 1;
    expect["qrysuccess"] = 1;
    expect["qryauthans"] = 1;
    expect["rcode.noerror"] = 1;
    checkStatisticsCounters(stats_after, expect);
}

// Same test emulating the UDPServer class behavior (defined in libasiolink).
// This is not a good test in that it assumes internal implementation details
// of UDPServer, but we've encountered a regression due to the introduction
// of that class, so we add a test for that case to prevent such a regression
// in future.
// Besides, the generalization of UDPServer is probably too much for the
// authoritative only server in terms of performance, and it's quite likely
// we need to drop it for the authoritative server implementation.
// At that point we can drop this test, too.
TEST_F(AuthSrvTest, builtInQueryViaDNSServer) {
    updateBuiltin(server);
    UnitTestUtil::createRequestMessage(request_message, Opcode::QUERY(),
                                       default_qid, Name("VERSION.BIND."),
                                       RRClass::CH(), RRType::TXT());
    createRequestPacket(request_message, IPPROTO_UDP);

    (*server.getDNSLookupProvider())(*io_message, parse_message,
                                     response_message,
                                     response_obuffer, &dnsserv);
    (*server.getDNSAnswerProvider())(*io_message, parse_message,
                                     response_message, response_obuffer);

    createBuiltinVersionResponse(default_qid, response_data);
    EXPECT_PRED_FORMAT4(UnitTestUtil::matchWireData,
                        response_obuffer->getData(),
                        response_obuffer->getLength(),
                        &response_data[0], response_data.size());

    // After it has been run, the message should be cleared
    EXPECT_EQ(0, parse_message->getRRCount(Message::SECTION_QUESTION));
}

// In the following tests we confirm the response data is rendered in
// wire format in the expected way.

// The most primitive check: checking the result of the processMessage()
// method
TEST_F(AuthSrvTest, builtInQuery) {
    updateBuiltin(server);
    UnitTestUtil::createRequestMessage(request_message, Opcode::QUERY(),
                                       default_qid, Name("VERSION.BIND."),
                                       RRClass::CH(), RRType::TXT());
    createRequestPacket(request_message, IPPROTO_UDP);
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);
    createBuiltinVersionResponse(default_qid, response_data);
    EXPECT_PRED_FORMAT4(UnitTestUtil::matchWireData,
                        response_obuffer->getData(),
                        response_obuffer->getLength(),
                        &response_data[0], response_data.size());
    checkAllRcodeCountersZeroExcept(Rcode::NOERROR(), 1);
}

// Same type of test as builtInQueryViaDNSServer but for an error response.
TEST_F(AuthSrvTest, iqueryViaDNSServer) {
    updateBuiltin(server);
    createDataFromFile("iquery_fromWire.wire");
    (*server.getDNSLookupProvider())(*io_message, parse_message,
                                     response_message,
                                     response_obuffer, &dnsserv);
    (*server.getDNSAnswerProvider())(*io_message, parse_message,
                                     response_message, response_obuffer);

    UnitTestUtil::readWireData("iquery_response_fromWire.wire",
                               response_data);
    EXPECT_PRED_FORMAT4(UnitTestUtil::matchWireData,
                        response_obuffer->getData(),
                        response_obuffer->getLength(),
                        &response_data[0], response_data.size());
}

// Install a Sqlite3 data source with testing data.
#ifdef USE_STATIC_LINK
TEST_F(AuthSrvTest, DISABLED_updateConfig) {
#else
TEST_F(AuthSrvTest, updateConfig) {
#endif
    updateDatabase(server, CONFIG_TESTDB);

    // query for existent data in the installed data source.  The resulting
    // response should have the AA flag on, and have an RR in each answer
    // and authority section.
    createDataFromFile("examplequery_fromWire.wire");
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);
    EXPECT_TRUE(dnsserv.hasAnswer());
    headerCheck(*parse_message, default_qid, Rcode::NOERROR(),
                opcode.getCode(), QR_FLAG | AA_FLAG, 1, 1, 1, 0);
}

#ifdef USE_STATIC_LINK
TEST_F(AuthSrvTest, DISABLED_datasourceFail) {
#else
TEST_F(AuthSrvTest, datasourceFail) {
#endif
    updateDatabase(server, CONFIG_TESTDB);

    // This query will hit a corrupted entry of the data source (the zoneload
    // tool and the data source itself naively accept it).  This will result
    // in a SERVFAIL response, and the answer and authority sections should
    // be empty.
    createDataFromFile("badExampleQuery_fromWire.wire");
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);
    EXPECT_TRUE(dnsserv.hasAnswer());
    headerCheck(*parse_message, default_qid, Rcode::SERVFAIL(),
                opcode.getCode(), QR_FLAG, 1, 0, 0, 0);

    checkAllRcodeCountersZeroExcept(Rcode::SERVFAIL(), 1);
    ConstElementPtr stats = server.getStatistics()->get("zones")->
        get("_SERVER_");
    std::map<std::string, int> expect;
    expect["request.v4"] = 1;
    expect["request.udp"] = 1;
    expect["opcode.query"] = 1;
    expect["responses"] = 1;
    expect["qrynoauthans"] = 1;
    expect["rcode.servfail"] = 1;
    checkStatisticsCounters(stats, expect);
}

#ifdef USE_STATIC_LINK
TEST_F(AuthSrvTest, DISABLED_updateConfigFail) {
#else
TEST_F(AuthSrvTest, updateConfigFail) {
#endif
    // First, load a valid data source.
    updateDatabase(server, CONFIG_TESTDB);

    // Next, try to update it with a non-existent one.  This should fail.
    EXPECT_THROW(updateDatabase(server, BADCONFIG_TESTDB),
                 isc::datasrc::DataSourceError);

    // The original data source should still exist.
    createDataFromFile("examplequery_fromWire.wire");
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);
    EXPECT_TRUE(dnsserv.hasAnswer());
    headerCheck(*parse_message, default_qid, Rcode::NOERROR(),
                opcode.getCode(), QR_FLAG | AA_FLAG, 1, 1, 1, 0);
}

TEST_F(AuthSrvTest, updateWithInMemoryClient) {
    // Test configuring memory data source.  Detailed test cases are covered
    // in the configuration tests.  We only check the AuthSrv interface here.

    // Create an empty in-memory
    const ConstElementPtr config(Element::fromJSON("{"
        "\"IN\": [{"
        "   \"type\": \"MasterFiles\","
        "   \"params\": {},"
        "   \"cache-enable\": true"
        "}]}"));
    installDataSrcClientLists(server, configureDataSource(config));
    // after successful configuration, we should have one (with empty zoneset).

    // The memory data source is empty, should return REFUSED rcode.
    createDataFromFile("examplequery_fromWire.wire");
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);
    EXPECT_TRUE(dnsserv.hasAnswer());
    headerCheck(*parse_message, default_qid, Rcode::REFUSED(),
                opcode.getCode(), QR_FLAG, 1, 0, 0, 0);
}

TEST_F(AuthSrvTest, queryWithInMemoryClientNoDNSSEC) {
    // In this example, we do simple check that query is handled from the
    // query handler class, and confirm it returns no error and a non empty
    // answer section.  Detailed examination on the response content
    // for various types of queries are tested in the query tests.
    updateInMemory(server, "example.", CONFIG_INMEMORY_EXAMPLE);

    createDataFromFile("nsec3query_nodnssec_fromWire.wire");
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);

    EXPECT_TRUE(dnsserv.hasAnswer());
    headerCheck(*parse_message, default_qid, Rcode::NOERROR(),
                opcode.getCode(), QR_FLAG | AA_FLAG, 1, 1, 2, 1);
}

TEST_F(AuthSrvTest, queryWithInMemoryClientDNSSEC) {
    // Similar to the previous test, but the query has the DO bit on.
    // The response should contain RRSIGs, and should have more RRs than
    // the previous case.
    updateInMemory(server, "example.", CONFIG_INMEMORY_EXAMPLE);

    createDataFromFile("nsec3query_fromWire.wire");
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);

    EXPECT_TRUE(dnsserv.hasAnswer());
    headerCheck(*parse_message, default_qid, Rcode::NOERROR(),
                opcode.getCode(), QR_FLAG | AA_FLAG, 1, 2, 3, 3);
}

TEST_F(AuthSrvTest, chQueryWithInMemoryClient) {
    // Set up the in-memory
    updateInMemory(server, "example.", CONFIG_INMEMORY_EXAMPLE);

    // This shouldn't affect the result of class CH query
    UnitTestUtil::createRequestMessage(request_message, Opcode::QUERY(),
                                       default_qid, Name("VERSION.BIND."),
                                       RRClass::CH(), RRType::TXT());
    createRequestPacket(request_message, IPPROTO_UDP);
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);
    EXPECT_TRUE(dnsserv.hasAnswer());
    headerCheck(*parse_message, default_qid, Rcode::NOERROR(),
                opcode.getCode(), QR_FLAG | AA_FLAG, 1, 1, 1, 0);
}

#ifdef USE_STATIC_LINK
TEST_F(AuthSrvTest, DISABLED_queryCounterTruncTest) {
#else
TEST_F(AuthSrvTest, queryCounterTruncTest) {
#endif
    // use CONFIG_TESTDB for large-rdata.example.com.
    updateDatabase(server, CONFIG_TESTDB);

    // Create UDP message and process.
    // large-rdata.example.com. TXT; expect it exceeds 512 octet
    UnitTestUtil::createRequestMessage(request_message, Opcode::QUERY(),
                                       default_qid,
                                       Name("large-rdata.example.com."),
                                       RRClass::IN(), RRType::TXT());
    createRequestPacket(request_message, IPPROTO_UDP);
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);

    ConstElementPtr stats_after = server.getStatistics()->
        get("zones")->get("_SERVER_");
    std::map<std::string, int> expect;
    expect["request.v4"] = 1;
    expect["request.udp"] = 1;
    expect["opcode.query"] = 1;
    expect["responses"] = 1;
    expect["response.truncated"] = 1;
    expect["qrysuccess"] = 1;
    expect["qryauthans"] = 1;
    expect["rcode.noerror"] = 1;
    checkStatisticsCounters(stats_after, expect);
}
// Submit UDP normal query and check query counter
TEST_F(AuthSrvTest, queryCounterUDPNormal) {
    // Create UDP message and process.
    UnitTestUtil::createRequestMessage(request_message, Opcode::QUERY(),
                                       default_qid, Name("example.com"),
                                       RRClass::IN(), RRType::NS());
    createRequestPacket(request_message, IPPROTO_UDP);
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);

    ConstElementPtr stats_after = server.getStatistics()->
        get("zones")->get("_SERVER_");
    std::map<std::string, int> expect;
    expect["request.v4"] = 1;
    expect["request.udp"] = 1;
    expect["opcode.query"] = 1;
    expect["responses"] = 1;
    expect["qrynoauthans"] = 1;
    expect["authqryrej"] = 1;
    expect["rcode.refused"] = 1;
    checkStatisticsCounters(stats_after, expect);
}

// Submit UDP normal query with DNSSEC and check query counter
TEST_F(AuthSrvTest, queryCounterUDPNormalWithDNSSEC) {
    // Create UDP message and process.
    UnitTestUtil::createDNSSECRequestMessage(request_message, Opcode::QUERY(),
                                             default_qid, Name("example.com"),
                                             RRClass::IN(), RRType::NS());
    createRequestPacket(request_message, IPPROTO_UDP);
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);

    ConstElementPtr stats_after = server.getStatistics()->
        get("zones")->get("_SERVER_");
    std::map<std::string, int> expect;
    expect["request.v4"] = 1;
    expect["request.edns0"] = 1;
    expect["request.udp"] = 1;
    expect["request.dnssec_ok"] = 1;
    expect["opcode.query"] = 1;
    expect["responses"] = 1;
    expect["qrynoauthans"] = 1;
    expect["authqryrej"] = 1;
    expect["rcode.refused"] = 1;
    // XXX: with the current implementation, EDNS0 is omitted in
    // makeErrorMessage.
    expect["response.edns0"] = 0;
    checkStatisticsCounters(stats_after, expect);
}

// Submit TCP normal query and check query counter
TEST_F(AuthSrvTest, queryCounterTCPNormal) {
    // Create TCP message and process.
    UnitTestUtil::createRequestMessage(request_message, Opcode::QUERY(),
                                       default_qid, Name("example.com"),
                                       RRClass::IN(), RRType::NS());
    createRequestPacket(request_message, IPPROTO_TCP);
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);

    ConstElementPtr stats_after = server.getStatistics()->
        get("zones")->get("_SERVER_");
    std::map<std::string, int> expect;
    expect["request.v4"] = 1;
    expect["request.tcp"] = 1;
    expect["opcode.query"] = 1;
    expect["responses"] = 1;
    expect["qrynoauthans"] = 1;
    expect["authqryrej"] = 1;
    expect["rcode.refused"] = 1;
    checkStatisticsCounters(stats_after, expect);
}

// Submit TCP AXFR query and check query counter
TEST_F(AuthSrvTest, queryCounterTCPAXFR) {
    UnitTestUtil::createRequestMessage(request_message, opcode, default_qid,
                         Name("example.com"), RRClass::IN(), RRType::AXFR());
    createRequestPacket(request_message, IPPROTO_TCP);
    // On success, the AXFR query has been passed to a separate process,
    // so auth itself shouldn't respond.
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);
    EXPECT_FALSE(dnsserv.hasAnswer());

    ConstElementPtr stats_after = server.getStatistics()->
        get("zones")->get("_SERVER_");
    std::map<std::string, int> expect;
    expect["request.v4"] = 1;
    expect["request.tcp"] = 1;
    expect["opcode.query"] = 1;
    checkStatisticsCounters(stats_after, expect);
}

// Submit TCP IXFR query and check query counter
TEST_F(AuthSrvTest, queryCounterTCPIXFR) {
    UnitTestUtil::createRequestMessage(request_message, opcode, default_qid,
                         Name("example.com"), RRClass::IN(), RRType::IXFR());
    createRequestPacket(request_message, IPPROTO_TCP);
    // On success, the IXFR query has been passed to a separate process,
    // so auth itself shouldn't respond.
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);
    EXPECT_FALSE(dnsserv.hasAnswer());

    ConstElementPtr stats_after = server.getStatistics()->
        get("zones")->get("_SERVER_");
    std::map<std::string, int> expect;
    expect["request.v4"] = 1;
    expect["request.tcp"] = 1;
    expect["opcode.query"] = 1;
    checkStatisticsCounters(stats_after, expect);
}

TEST_F(AuthSrvTest, queryCounterOpcodes) {
    int other_expected = 0;
    for (int i = 0; i < isc::auth::statistics::num_opcode_to_msgcounter; ++i) {
        std::string item_name;
        int expected;
        if (isc::auth::statistics::opcode_to_msgcounter[i] ==
                isc::auth::statistics::MSG_OPCODE_OTHER)
        {
            item_name = "OTHER";
            other_expected += i + 1;
            expected = other_expected;
        } else {
            item_name = Opcode(i).toText();
            expected = i + 1;
        }
        std::transform(item_name.begin(), item_name.end(), item_name.begin(),
                       ::tolower);

        // The counter should be initialized to expected value.
        EXPECT_EQ(expected - (i + 1),
                  server.getStatistics()->get("zones")->get("_SERVER_")->
                  get("opcode")->get(item_name)->intValue());

        // For each possible opcode, create a request message and send it
        UnitTestUtil::createRequestMessage(request_message, Opcode(i),
                                           default_qid, Name("example.com"),
                                           RRClass::IN(), RRType::NS());
        createRequestPacket(request_message, IPPROTO_UDP);

        // "send" the request N-th times where N is i + 1 for i-th code.
        // we intentionally use different values for each code
        for (int j = 0; j <= i; ++j) {
            parse_message->clear(Message::PARSE);
            server.processMessage(*io_message, *parse_message,
                                  *response_obuffer,
                                  &dnsserv);
        }

        // Confirm the counter.
        // This test only checks for opcodes; some part of the other items
        // depends on the opcode.
        EXPECT_EQ(expected,
                  server.getStatistics()->get("zones")->get("_SERVER_")->
                  get("opcode")->get(item_name)->intValue());
    }
}

// class for queryCounterUnexpected test
// getProtocol() returns IPPROTO_IP
class DummyUnknownSocket : public IOSocket {
public:
    DummyUnknownSocket() {}
    virtual int getNative() const { return (0); }
    virtual int getProtocol() const { return (IPPROTO_IP); }
};

// function for queryCounterUnexpected test
// returns a reference to a static object of DummyUnknownSocket
IOSocket&
getDummyUnknownSocket() {
    static DummyUnknownSocket socket;
    return (socket);
}

// Submit unexpected type of query and check it is ignored
TEST_F(AuthSrvTest, queryCounterUnexpected) {
    // This code isn't exception safe, but we'd rather keep the code
    // simpler and more readable as this is only for tests

    // Create UDP query packet.
    UnitTestUtil::createRequestMessage(request_message, Opcode::QUERY(),
                                       default_qid, Name("example.com"),
                                       RRClass::IN(), RRType::NS());
    createRequestPacket(request_message, IPPROTO_UDP);

    // Modify the message.
    endpoint.reset(IOEndpoint::create(IPPROTO_UDP,
                                      IOAddress(DEFAULT_REMOTE_ADDRESS),
                                      53210));
    io_message.reset(new IOMessage(request_renderer.getData(),
                                   request_renderer.getLength(),
                                   getDummyUnknownSocket(), *endpoint));

    EXPECT_FALSE(dnsserv.hasAnswer());
}

TEST_F(AuthSrvTest, stop) {
    // normal case is covered in command_unittest.cc.  we should primarily
    // test it here, but the current design of the stop test takes time,
    // so we consolidate the cases in the command tests.
    // If/when the interval timer has finer granularity we'll probably add
    // our own tests here, so we keep this empty test case.
}

TEST_F(AuthSrvTest, listenAddresses) {
    isc::testutils::portconfig::listenAddresses(server);
    // Check it requests the correct addresses
    const char* tokens[] = {
        "TCP:127.0.0.1:53210:1",
        "UDP:127.0.0.1:53210:2",
        "TCP:::1:53210:3",
        "UDP:::1:53210:4",
        NULL
    };
    sock_requestor_.checkTokens(tokens, sock_requestor_.given_tokens_,
                                "Given tokens");
    // It returns back to empty set of addresses afterwards, so
    // they should be released
    sock_requestor_.checkTokens(tokens, sock_requestor_.released_tokens_,
                                "Released tokens");
}

TEST_F(AuthSrvTest, processNormalQuery_reuseRenderer1) {
    UnitTestUtil::createRequestMessage(request_message, Opcode::QUERY(),
                                       default_qid, Name("example.com"),
                                       RRClass::IN(), RRType::NS());

    request_message.setHeaderFlag(Message::HEADERFLAG_AA);
    createRequestPacket(request_message, IPPROTO_UDP);
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);
    EXPECT_NE(request_message.getRcode(), parse_message->getRcode());
}

TEST_F(AuthSrvTest, processNormalQuery_reuseRenderer2) {
    UnitTestUtil::createRequestMessage(request_message, Opcode::QUERY(),
                                       default_qid, Name("example.com"),
                                       RRClass::IN(), RRType::SOA());

    request_message.setHeaderFlag(Message::HEADERFLAG_AA);
    createRequestPacket(request_message, IPPROTO_UDP);
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);
    ConstQuestionPtr question = *parse_message->beginQuestion();
    EXPECT_STRNE(question->getType().toText().c_str(),
                 RRType::NS().toText().c_str());
}
//
// Tests for catching exceptions in various stages of the query processing
//
// These tests work by defining two proxy classes, that act as an in-memory
// client by default, but can throw exceptions at various points.
//
namespace {

/// The possible methods to throw in, either in FakeClient or
/// FakeZoneFinder
enum ThrowWhen {
    THROW_NEVER,
    THROW_AT_FIND_ZONE,
    THROW_AT_GET_ORIGIN,
    THROW_AT_GET_CLASS,
    THROW_AT_FIND,
    THROW_AT_FIND_ALL,
    THROW_AT_FIND_NSEC3
};

/// convenience function to check whether and what to throw
void
checkThrow(ThrowWhen method, ThrowWhen throw_at, bool isc_exception) {
    if (method == throw_at) {
        if (isc_exception) {
            isc_throw(isc::Exception, "foo");
        } else {
            throw std::exception();
        }
    }
}

/// \brief proxy class for the ZoneFinder returned by the Client
///        proxied by FakeClient
///
/// See the documentation for FakeClient for more information,
/// all methods simply check whether they should throw, and if not, call
/// their proxied equivalent.
class FakeZoneFinder : public isc::datasrc::ZoneFinder {
public:
    FakeZoneFinder(isc::datasrc::ZoneFinderPtr zone_finder,
                   ThrowWhen throw_when, bool isc_exception,
                   ConstRRsetPtr fake_rrset) :
        real_zone_finder_(zone_finder),
        throw_when_(throw_when),
        isc_exception_(isc_exception),
        fake_rrset_(fake_rrset)
    {}

    virtual isc::dns::Name
    getOrigin() const {
        checkThrow(THROW_AT_GET_ORIGIN, throw_when_, isc_exception_);
        return (real_zone_finder_->getOrigin());
    }

    virtual isc::dns::RRClass
    getClass() const {
        checkThrow(THROW_AT_GET_CLASS, throw_when_, isc_exception_);
        return (real_zone_finder_->getClass());
    }

    virtual isc::datasrc::ZoneFinderContextPtr
    find(const isc::dns::Name& name,
         const isc::dns::RRType& type,
         isc::datasrc::ZoneFinder::FindOptions options)
    {
        using namespace isc::datasrc;
        checkThrow(THROW_AT_FIND, throw_when_, isc_exception_);
        // If faked RRset was specified on construction and it matches the
        // query, return it instead of searching the real data source.
        if (fake_rrset_ && fake_rrset_->getName() == name &&
            fake_rrset_->getType() == type)
        {
            return (ZoneFinderContextPtr(new ZoneFinder::GenericContext(
                                             *this, options,
                                             ResultContext(SUCCESS,
                                                           fake_rrset_))));
        }
        return (real_zone_finder_->find(name, type, options));
    }

    virtual isc::datasrc::ZoneFinderContextPtr
    findAll(const isc::dns::Name& name,
            std::vector<isc::dns::ConstRRsetPtr> &target,
            const FindOptions options = FIND_DEFAULT)
    {
        checkThrow(THROW_AT_FIND_ALL, throw_when_, isc_exception_);
        return (real_zone_finder_->findAll(name, target, options));
    }

    virtual FindNSEC3Result
    findNSEC3(const isc::dns::Name& name, bool recursive) {
        checkThrow(THROW_AT_FIND_NSEC3, throw_when_, isc_exception_);
        return (real_zone_finder_->findNSEC3(name, recursive));
    }

private:
    isc::datasrc::ZoneFinderPtr real_zone_finder_;
    ThrowWhen throw_when_;
    bool isc_exception_;
    ConstRRsetPtr fake_rrset_;
};

/// \brief Proxy FakeClient that can throw exceptions at specified times
///
/// Currently it is used as an 'InMemoryClient' using setInMemoryClient,
/// but it is in effect a general datasource client.
class FakeClient : public isc::datasrc::DataSourceClient {
public:
    /// \brief Create a proxy memory client
    ///
    /// \param real_client The real (in-memory) client to proxy
    /// \param throw_when if set to any value other than never, that is
    ///        the method that will throw an exception (either in this
    ///        class or the related FakeZoneFinder)
    /// \param isc_exception if true, throw isc::Exception, otherwise,
    ///                      throw std::exception
    /// \param fake_rrset If non NULL, it will be used as an answer to
    /// find() for that name and type.
    FakeClient(const DataSourceClient* real_client,
               ThrowWhen throw_when, bool isc_exception,
               ConstRRsetPtr fake_rrset = ConstRRsetPtr()) :
        real_client_ptr_(real_client),
        throw_when_(throw_when),
        isc_exception_(isc_exception),
        fake_rrset_(fake_rrset)
    {}

    /// \brief proxy call for findZone
    ///
    /// if this instance was constructed with throw_when set to find_zone,
    /// this method will throw. Otherwise, it will return a FakeZoneFinder
    /// instance which will throw at the method specified at the
    /// construction of this instance.
    virtual FindResult
    findZone(const isc::dns::Name& name) const {
        checkThrow(THROW_AT_FIND_ZONE, throw_when_, isc_exception_);
        const FindResult result =
            real_client_ptr_->findZone(name);
        return (FindResult(result.code, isc::datasrc::ZoneFinderPtr(
                                        new FakeZoneFinder(result.zone_finder,
                                                           throw_when_,
                                                           isc_exception_,
                                                           fake_rrset_))));
    }

    isc::datasrc::ZoneUpdaterPtr
    getUpdater(const isc::dns::Name&, bool, bool) const {
        isc_throw(isc::NotImplemented,
                  "Update attempt on in fake data source");
    }
    std::pair<isc::datasrc::ZoneJournalReader::Result,
              isc::datasrc::ZoneJournalReaderPtr>
    getJournalReader(const isc::dns::Name&, uint32_t, uint32_t) const {
        isc_throw(isc::NotImplemented, "Journaling isn't supported for "
                  "fake data source");
    }
private:
    const DataSourceClient* real_client_ptr_;
    ThrowWhen throw_when_;
    bool isc_exception_;
    ConstRRsetPtr fake_rrset_;
};

class FakeList : public isc::datasrc::ConfigurableClientList {
public:
    /// \brief Creates a fake list for the given in-memory client
    ///
    /// It will create a FakeClient for each client in the original list,
    /// with the given arguments, which is used when searching for the
    /// corresponding data source.
    FakeList(const boost::shared_ptr<isc::datasrc::ConfigurableClientList>
             real_list, ThrowWhen throw_when, bool isc_exception,
             ConstRRsetPtr fake_rrset = ConstRRsetPtr()) :
        ConfigurableClientList(RRClass::IN()),
        real_(real_list)
    {
        BOOST_FOREACH(const DataSourceInfo& info, real_->getDataSources()) {
             const isc::datasrc::DataSourceClientPtr
                 client(new FakeClient(info.data_src_client_ != NULL ?
                                       info.data_src_client_ :
                                       info.getCacheClient(),
                                       throw_when, isc_exception, fake_rrset));
             clients_.push_back(client);
             data_sources_.push_back(
                 DataSourceInfo(client.get(),
                                isc::datasrc::DataSourceClientContainerPtr(),
                                boost::shared_ptr<
                                isc::datasrc::internal::CacheConfig>(),
                                RRClass::IN(), ""));
        }
    }
private:
    const boost::shared_ptr<isc::datasrc::ConfigurableClientList> real_;
    vector<isc::datasrc::DataSourceClientPtr> clients_;
};

} // end anonymous namespace for throwing proxy classes

// Test for the tests
//
// Set the proxies to never throw, this should have the same result as
// queryWithInMemoryClientNoDNSSEC, and serves to test the two proxy classes
TEST_F(AuthSrvTest, queryWithInMemoryClientProxy) {
    // Set real inmem client to proxy
    updateInMemory(server, "example.", CONFIG_INMEMORY_EXAMPLE);
    boost::shared_ptr<isc::datasrc::ConfigurableClientList> list;
    DataSrcClientsMgr& mgr = server.getDataSrcClientsMgr();
    {
        DataSrcClientsMgr::Holder holder(mgr);
        list.reset(new FakeList(holder.findClientList(RRClass::IN()),
                                THROW_NEVER, false));
    }
    ClientListMapPtr lists(new std::map<RRClass, ListPtr>);
    lists->insert(pair<RRClass, ListPtr>(RRClass::IN(), list));
    server.getDataSrcClientsMgr().setDataSrcClientLists(lists);

    createDataFromFile("nsec3query_nodnssec_fromWire.wire");
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);

    EXPECT_TRUE(dnsserv.hasAnswer());
    headerCheck(*parse_message, default_qid, Rcode::NOERROR(),
                opcode.getCode(), QR_FLAG | AA_FLAG, 1, 1, 2, 1);
}

// Convenience function for the rest of the tests, set up a proxy
// to throw in the given method
// If isc_exception is true, it will throw isc::Exception, otherwise
// it will throw std::exception
// If non null rrset is given, it will be passed to the proxy so it can
// return some faked response.
void
setupThrow(AuthSrv& server, ThrowWhen throw_when, bool isc_exception,
           ConstRRsetPtr rrset = ConstRRsetPtr())
{
    updateInMemory(server, "example.", CONFIG_INMEMORY_EXAMPLE);

    boost::shared_ptr<isc::datasrc::ConfigurableClientList> list;
    DataSrcClientsMgr& mgr = server.getDataSrcClientsMgr();
    {           // we need to limit the scope so swap is outside of it
        DataSrcClientsMgr::Holder holder(mgr);
        list.reset(new FakeList(holder.findClientList(RRClass::IN()),
                                throw_when, isc_exception, rrset));
    }
    ClientListMapPtr lists(new std::map<RRClass, ListPtr>);
    lists->insert(pair<RRClass, ListPtr>(RRClass::IN(), list));
    mgr.setDataSrcClientLists(lists);
}

TEST_F(AuthSrvTest, queryWithThrowingProxyServfails) {
    // Test the common cases, all of which should simply return SERVFAIL
    // Use THROW_NEVER as end marker
    ThrowWhen throws[] = { THROW_AT_FIND_ZONE,
                           THROW_AT_GET_ORIGIN,
                           THROW_AT_FIND,
                           THROW_AT_FIND_NSEC3,
                           THROW_NEVER };
    UnitTestUtil::createDNSSECRequestMessage(request_message, opcode,
                                             default_qid, Name("foo.example."),
                                             RRClass::IN(), RRType::TXT());
    for (ThrowWhen* when(throws); *when != THROW_NEVER; ++when) {
        createRequestPacket(request_message, IPPROTO_UDP);
        setupThrow(server, *when, true);
        processAndCheckSERVFAIL();
        // To be sure, check same for non-isc-exceptions
        createRequestPacket(request_message, IPPROTO_UDP);
        setupThrow(server, *when, false);
        processAndCheckSERVFAIL();
    }
}

// Throw isc::Exception in getClass(). (Currently?) getClass is not called
// in the processMessage path, so this should result in a normal answer
TEST_F(AuthSrvTest, queryWithInMemoryClientProxyGetClass) {
    createDataFromFile("nsec3query_nodnssec_fromWire.wire");
    setupThrow(server, THROW_AT_GET_CLASS, true);

    // getClass is not called so it should just answer
    server.processMessage(*io_message, *parse_message, *response_obuffer,
                          &dnsserv);

    EXPECT_TRUE(dnsserv.hasAnswer());
    headerCheck(*parse_message, default_qid, Rcode::NOERROR(),
                opcode.getCode(), QR_FLAG | AA_FLAG, 1, 1, 2, 1);
}

TEST_F(AuthSrvTest, queryWithThrowingInToWire) {
    // Set up a faked data source.  It will return an empty RRset for the
    // query.
    ConstRRsetPtr empty_rrset(new RRset(Name("foo.example"),
                                        RRClass::IN(), RRType::TXT(),
                                        RRTTL(0)));
    setupThrow(server, THROW_NEVER, true, empty_rrset);

    // Repeat the query processing two times.  Due to the faked RRset,
    // toWire() should throw, and it should result in SERVFAIL.
    OutputBufferPtr orig_buffer;
    for (int i = 0; i < 2; ++i) {
        UnitTestUtil::createDNSSECRequestMessage(request_message, opcode,
                                                 default_qid,
                                                 Name("foo.example."),
                                                 RRClass::IN(), RRType::TXT());
        createRequestPacket(request_message, IPPROTO_UDP);
        server.processMessage(*io_message, *parse_message, *response_obuffer,
                              &dnsserv);
        headerCheck(*parse_message, default_qid, Rcode::SERVFAIL(),
                    opcode.getCode(), QR_FLAG, 1, 0, 0, 0);

        // Make a backup of the original buffer for latest tests and replace
        // it with a new one
        if (!orig_buffer) {
            orig_buffer = response_obuffer;
            response_obuffer.reset(new OutputBuffer(0));
        }
        request_message.clear(Message::RENDER);
        parse_message->clear(Message::PARSE);
    }

    // Now check if the original buffer is intact
    parse_message->clear(Message::PARSE);
    InputBuffer ibuffer(orig_buffer->getData(), orig_buffer->getLength());
    parse_message->fromWire(ibuffer);
    headerCheck(*parse_message, default_qid, Rcode::SERVFAIL(),
                opcode.getCode(), QR_FLAG, 1, 0, 0, 0);
}

//
// DDNS related tests
//

// Helper subroutine to check if the given socket address has the expected
// address and port.  It depends on specific output of getnameinfo() (while
// there can be multiple textual representation of the same address) but
// in practice it should be reliable.
void
checkAddrPort(const struct sockaddr& actual_sa,
              const string& expected_addr, uint16_t expected_port)
{
    // ASIO does not set as_len, which is not a problem on most
    // systems, but it will make getnameinfo() fail on NetBSD 4
    // So we make a copy and if the field is available, we set it
    const socklen_t sa_len = getSALength(actual_sa);
    struct sockaddr_storage ss;
    memcpy(&ss, &actual_sa, sa_len);

    struct sockaddr* sa = convertSockAddr(&ss);
#ifdef HAVE_SA_LEN
    sa->sa_len = sa_len;
#endif
    char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    const int error = getnameinfo(sa, sa_len, hbuf, sizeof(hbuf),
                                  sbuf, sizeof(sbuf),
                                  NI_NUMERICHOST | NI_NUMERICSERV);
    if (error != 0) {
        isc_throw(isc::Unexpected, "getnameinfo failed: " <<
                  gai_strerror(error));
    }
    EXPECT_EQ(expected_addr, hbuf);
    EXPECT_EQ(boost::lexical_cast<string>(expected_port), sbuf);
}

TEST_F(AuthSrvTest, DDNSForward) {
    EXPECT_FALSE(ddns_forwarder.isConnected());

    // Repeat sending an update request 4 times, differing some network
    // parameters: UDP/IPv4, TCP/IPv4, UDP/IPv6, TCP/IPv6, in this order.
    // By doing that we can also confirm the forwarder connection will be
    // established exactly once, and kept established.
    for (size_t i = 0; i < 4; ++i) {
        // Use different names for some different cases
        const Name zone_name = Name(i < 2 ? "example.com" : "example.org");
        const socklen_t family = (i < 2) ? AF_INET : AF_INET6;
        const char* const remote_addr =
            (family == AF_INET) ? "192.0.2.1" : "2001:db8::1";
        const uint16_t remote_port =
            (family == AF_INET) ? 53214 : 53216;
        const int protocol = ((i % 2) == 0) ? IPPROTO_UDP : IPPROTO_TCP;

        createAndSendRequest(RRType::SOA(), Opcode::UPDATE(), zone_name,
                             RRClass::IN(), protocol, remote_addr,
                             remote_port);
        EXPECT_FALSE(dnsserv.hasAnswer());
        EXPECT_TRUE(ddns_forwarder.isConnected());

        // Examine the pushed data (note: currently "local end" has a dummy
        // value equal to remote)
        EXPECT_EQ(family, ddns_forwarder.getPushedFamily());
        const int expected_type =
            (protocol == IPPROTO_UDP) ? SOCK_DGRAM : SOCK_STREAM;
        EXPECT_EQ(expected_type, ddns_forwarder.getPushedType());
        EXPECT_EQ(protocol, ddns_forwarder.getPushedProtocol());
        checkAddrPort(ddns_forwarder.getPushedRemoteend(),
                      remote_addr, remote_port);
        checkAddrPort(ddns_forwarder.getPushedLocalend(),
                      remote_addr, remote_port);
        EXPECT_EQ(io_message->getDataSize(),
                  ddns_forwarder.getPushedData().size());
        EXPECT_EQ(0, memcmp(io_message->getData(),
                            &ddns_forwarder.getPushedData()[0],
                            ddns_forwarder.getPushedData().size()));
    }
}

TEST_F(AuthSrvTest, DDNSForwardConnectFail) {
    // make connect attempt fail.  It should result in SERVFAIL.  Note that
    // the question (zone) section should be cleared for opcode of update.
    ddns_forwarder.disableConnect();
    createAndSendRequest(RRType::SOA(), Opcode::UPDATE());
    EXPECT_TRUE(dnsserv.hasAnswer());
    headerCheck(*parse_message, default_qid, Rcode::SERVFAIL(),
                Opcode::UPDATE().getCode(), QR_FLAG, 0, 0, 0, 0);
    EXPECT_FALSE(ddns_forwarder.isConnected());

    // Now make connect okay again.  Despite the previous failure the new
    // connection should now be established.
    ddns_forwarder.enableConnect();
    createAndSendRequest(RRType::SOA(), Opcode::UPDATE());
    EXPECT_FALSE(dnsserv.hasAnswer());
    EXPECT_TRUE(ddns_forwarder.isConnected());
}

TEST_F(AuthSrvTest, DDNSForwardPushFail) {
    // Make first request succeed, which will establish the connection.
    EXPECT_FALSE(ddns_forwarder.isConnected());
    createAndSendRequest(RRType::SOA(), Opcode::UPDATE());
    EXPECT_TRUE(ddns_forwarder.isConnected());

    // make connect attempt fail.  It should result in SERVFAIL.  The
    // connection should be closed.  Use IPv6 address for varying log output.
    ddns_forwarder.disablePush();
    createAndSendRequest(RRType::SOA(), Opcode::UPDATE(), Name("example.com"),
                         RRClass::IN(), IPPROTO_UDP, "2001:db8::2");
    EXPECT_TRUE(dnsserv.hasAnswer());
    headerCheck(*parse_message, default_qid, Rcode::SERVFAIL(),
                Opcode::UPDATE().getCode(), QR_FLAG, 0, 0, 0, 0);
    EXPECT_FALSE(ddns_forwarder.isConnected());

    // Allow push again.  Connection will be reopened, and the request will
    // be forwarded successfully.
    ddns_forwarder.enablePush();
    createAndSendRequest(RRType::SOA(), Opcode::UPDATE());
    EXPECT_FALSE(dnsserv.hasAnswer());
    EXPECT_TRUE(ddns_forwarder.isConnected());
}

TEST_F(AuthSrvTest, DDNSForwardClose) {
    scoped_ptr<AuthSrv> tmp_server(new AuthSrv(xfrout, ddns_forwarder));
    tmp_server->createDDNSForwarder();
    UnitTestUtil::createRequestMessage(request_message, Opcode::UPDATE(),
                                       default_qid, Name("example.com"),
                                       RRClass::IN(), RRType::SOA());
    createRequestPacket(request_message, IPPROTO_UDP);
    tmp_server->processMessage(*io_message, *parse_message, *response_obuffer,
                               &dnsserv);
    EXPECT_FALSE(dnsserv.hasAnswer());
    EXPECT_TRUE(ddns_forwarder.isConnected());

    // Destroy the server.  The forwarder should close the connection.
    tmp_server.reset();
    EXPECT_FALSE(ddns_forwarder.isConnected());
}

namespace {
    // Send a basic command without arguments, and check the response has
    // result code 0
    void sendSimpleCommand(AuthSrv& server, const std::string& command) {
        ConstElementPtr response = execAuthServerCommand(server, command,
                                                         ConstElementPtr());
        int command_result = -1;
        isc::config::parseAnswer(command_result, response);
        EXPECT_EQ(0, command_result);
    }

    void sendCommand(AuthSrv& server, const std::string& command,
                     ConstElementPtr args, int expected_result) {
        ConstElementPtr response = execAuthServerCommand(server, command,
                                                         args);
        int command_result = -1;
        isc::config::parseAnswer(command_result, response);
        EXPECT_EQ(expected_result, command_result);
    }
} // end anonymous namespace

TEST_F(AuthSrvTest, DDNSForwardCreateDestroy) {
    // Test that AuthSrv returns NOTIMP before ddns forwarder is created,
    // that the ddns_forwarder is connected when the 'start_ddns_forwarder'
    // command has been sent, and that it is no longer connected and auth
    // returns NOTIMP after the stop_ddns_forwarding command is sent.
    scoped_ptr<AuthSrv> tmp_server(new AuthSrv(xfrout, ddns_forwarder));

    // Prepare update message to send
    UnitTestUtil::createRequestMessage(request_message, Opcode::UPDATE(),
                                       default_qid, Name("example.com"),
                                       RRClass::IN(), RRType::SOA());
    createRequestPacket(request_message, IPPROTO_UDP);

    // before creating forwarder. isConnected() should be false and
    // rcode to UPDATE should be NOTIMP
    parse_message->clear(Message::PARSE);
    tmp_server->processMessage(*io_message, *parse_message, *response_obuffer,
                               &dnsserv);
    EXPECT_FALSE(ddns_forwarder.isConnected());
    EXPECT_TRUE(dnsserv.hasAnswer());
    headerCheck(*parse_message, default_qid, Rcode::NOTIMP(),
                Opcode::UPDATE().getCode(), QR_FLAG, 0, 0, 0, 0);

    // now create forwarder
    sendSimpleCommand(*tmp_server, "start_ddns_forwarder");

    // our mock does not respond, and since auth is supposed to send it on,
    // there should now be no result when an UPDATE is sent
    parse_message->clear(Message::PARSE);
    tmp_server->processMessage(*io_message, *parse_message, *response_obuffer,
                               &dnsserv);
    EXPECT_FALSE(dnsserv.hasAnswer());
    EXPECT_TRUE(ddns_forwarder.isConnected());

    // If we send a start again, the connection should be recreated,
    // visible because isConnected() reports false until an actual message
    // has been forwarded
    sendSimpleCommand(*tmp_server, "start_ddns_forwarder");

    EXPECT_FALSE(ddns_forwarder.isConnected());
    parse_message->clear(Message::PARSE);
    tmp_server->processMessage(*io_message, *parse_message, *response_obuffer,
                               &dnsserv);
    EXPECT_FALSE(dnsserv.hasAnswer());
    EXPECT_TRUE(ddns_forwarder.isConnected());

    // Now tell it to stop forwarder, should respond with NOTIMP again
    sendSimpleCommand(*tmp_server, "stop_ddns_forwarder");

    parse_message->clear(Message::PARSE);
    tmp_server->processMessage(*io_message, *parse_message, *response_obuffer,
                               &dnsserv);
    EXPECT_FALSE(ddns_forwarder.isConnected());
    EXPECT_TRUE(dnsserv.hasAnswer());
    headerCheck(*parse_message, default_qid, Rcode::NOTIMP(),
                Opcode::UPDATE().getCode(), QR_FLAG, 0, 0, 0, 0);

    // Sending stop again should make no difference
    sendSimpleCommand(*tmp_server, "stop_ddns_forwarder");

    parse_message->clear(Message::PARSE);
    tmp_server->processMessage(*io_message, *parse_message, *response_obuffer,
                               &dnsserv);
    EXPECT_FALSE(ddns_forwarder.isConnected());
    EXPECT_TRUE(dnsserv.hasAnswer());
    headerCheck(*parse_message, default_qid, Rcode::NOTIMP(),
                Opcode::UPDATE().getCode(), QR_FLAG, 0, 0, 0, 0);
}

TEST_F(AuthSrvTest, loadZoneCommand) {
    // Just some very basic tests, to check the command is accepted, and that
    // it raises on bad arguments, but not on correct ones (full testing
    // is handled in the unit tests for the corresponding classes)

    // Empty map should fail
    ElementPtr args(Element::createMap());
    sendCommand(server, "loadzone", args, 1);
    // Setting an origin should be enough (even if it isn't actually loaded,
    // it should be initially accepted)
    args->set("origin", Element::create("example.com"));
    sendCommand(server, "loadzone", args, 0);
}

}
