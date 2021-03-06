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

#include <auth/statistics.h>
#include <auth/statistics_items.h>
#include <auth/auth_log.h>

#include <cc/data.h>

#include <dns/message.h>
#include <dns/opcode.h>
#include <dns/rcode.h>

#include <statistics/counter.h>

#include <boost/optional.hpp>

using namespace isc::dns;
using namespace isc::auth;
using namespace isc::statistics;
using namespace isc::auth::statistics;

namespace {

/// \brief Fill isc::data::ElementPtr with given counter.
/// \param counter Counter which stores values to fill
/// \param type_tree CounterSpec corresponding to counter for building item
///                  name
/// \param trees isc::data::ElementPtr to be filled in; caller has ownership of
///              isc::data::ElementPtr
void
fillNodes(const Counter& counter,
          const struct isc::auth::statistics::CounterSpec type_tree[],
          isc::data::ElementPtr& trees)
{
    using namespace isc::data;

    for (int i = 0; type_tree[i].name != NULL; ++i) {
        if (type_tree[i].sub_counters != NULL) {
            isc::data::ElementPtr sub_counters = Element::createMap();
            trees->set(type_tree[i].name, sub_counters);
            fillNodes(counter, type_tree[i].sub_counters, sub_counters);
        } else {
            trees->set(type_tree[i].name,
                       Element::create(static_cast<long int>(
                           counter.get(type_tree[i].counter_id)))
                       );
        }
    }
}

// using -1 as counter_id to state it is not a counter item
const int NOT_ITEM = -1;

const struct CounterSpec msg_counter_request[] = {
    { "v4", NULL, MSG_REQUEST_IPV4 },
    { "v6", NULL, MSG_REQUEST_IPV6 },
    { "edns0", NULL, MSG_REQUEST_EDNS0 },
    { "badednsver", NULL, MSG_REQUEST_BADEDNSVER },
    { "tsig", NULL, MSG_REQUEST_TSIG },
    { "sig0", NULL, MSG_REQUEST_SIG0 },
    { "badsig", NULL, MSG_REQUEST_BADSIG },
    { "udp", NULL, MSG_REQUEST_UDP },
    { "tcp", NULL, MSG_REQUEST_TCP },
    { "dnssec_ok", NULL, MSG_REQUEST_DNSSEC_OK },
    { NULL, NULL, NOT_ITEM }
};
const struct CounterSpec msg_counter_opcode[] = {
    { "query", NULL, MSG_OPCODE_QUERY },
    { "iquery", NULL, MSG_OPCODE_IQUERY },
    { "status", NULL, MSG_OPCODE_STATUS },
    { "notify", NULL, MSG_OPCODE_NOTIFY },
    { "update", NULL, MSG_OPCODE_UPDATE },
    { "other", NULL, MSG_OPCODE_OTHER },
    { NULL, NULL, NOT_ITEM }
};
const struct CounterSpec msg_counter_response[] = {
    { "truncated", NULL, MSG_RESPONSE_TRUNCATED },
    { "edns0", NULL, MSG_RESPONSE_EDNS0 },
    { "tsig", NULL, MSG_RESPONSE_TSIG },
    { "sig0", NULL, MSG_RESPONSE_SIG0 },
    { NULL, NULL, NOT_ITEM }
};
const struct CounterSpec msg_counter_rcode[] = {
    { "noerror", NULL, MSG_RCODE_NOERROR },
    { "formerr", NULL, MSG_RCODE_FORMERR },
    { "servfail", NULL, MSG_RCODE_SERVFAIL },
    { "nxdomain", NULL, MSG_RCODE_NXDOMAIN },
    { "notimp", NULL, MSG_RCODE_NOTIMP },
    { "refused", NULL, MSG_RCODE_REFUSED },
    { "yxdomain", NULL, MSG_RCODE_YXDOMAIN },
    { "yxrrset", NULL, MSG_RCODE_YXRRSET },
    { "nxrrset", NULL, MSG_RCODE_NXRRSET },
    { "notauth", NULL, MSG_RCODE_NOTAUTH },
    { "notzone", NULL, MSG_RCODE_NOTZONE },
    { "badvers", NULL, MSG_RCODE_BADVERS },
    { "other", NULL, MSG_RCODE_OTHER },
    { NULL, NULL, NOT_ITEM }
};
const struct CounterSpec msg_counter_tree[] = {
    { "request", msg_counter_request, NOT_ITEM },
    { "opcode", msg_counter_opcode, NOT_ITEM },
    { "responses", NULL, MSG_RESPONSE },
    { "response", msg_counter_response, NOT_ITEM },
    { "qrysuccess", NULL, MSG_QRYSUCCESS },
    { "qryauthans", NULL, MSG_QRYAUTHANS },
    { "qrynoauthans", NULL, MSG_QRYNOAUTHANS },
    { "qryreferral", NULL, MSG_QRYREFERRAL },
    { "qrynxrrset", NULL, MSG_QRYNXRRSET },
    { "authqryrej", NULL, MSG_QRYREJECT },
    { "rcode", msg_counter_rcode, NOT_ITEM },
    { NULL, NULL, NOT_ITEM }
};

} // anonymous namespace

namespace isc {
namespace auth {
namespace statistics {

// Note: opcode in this array must be start with 0 and be sequential
const int opcode_to_msgcounter[] = {
    MSG_OPCODE_QUERY,    // Opcode =  0: Query
    MSG_OPCODE_IQUERY,   // Opcode =  1: IQuery
    MSG_OPCODE_STATUS,   // Opcode =  2: Status
    MSG_OPCODE_OTHER,    // Opcode =  3: (Unassigned)
    MSG_OPCODE_NOTIFY,   // Opcode =  4: Notify
    MSG_OPCODE_UPDATE,   // Opcode =  5: Update
    MSG_OPCODE_OTHER,    // Opcode =  6: (Unassigned)
    MSG_OPCODE_OTHER,    // Opcode =  7: (Unassigned)
    MSG_OPCODE_OTHER,    // Opcode =  8: (Unassigned)
    MSG_OPCODE_OTHER,    // Opcode =  9: (Unassigned)
    MSG_OPCODE_OTHER,    // Opcode = 10: (Unassigned)
    MSG_OPCODE_OTHER,    // Opcode = 11: (Unassigned)
    MSG_OPCODE_OTHER,    // Opcode = 12: (Unassigned)
    MSG_OPCODE_OTHER,    // Opcode = 13: (Unassigned)
    MSG_OPCODE_OTHER,    // Opcode = 14: (Unassigned)
    MSG_OPCODE_OTHER     // Opcode = 15: (Unassigned)
};
const size_t num_opcode_to_msgcounter =
    sizeof(opcode_to_msgcounter) / sizeof(opcode_to_msgcounter[0]);

// Note: rcode in this array must be start with 0 and be sequential
const int rcode_to_msgcounter[] = {
    MSG_RCODE_NOERROR,       // Rcode =  0: NoError
    MSG_RCODE_FORMERR,       // Rcode =  1: FormErr
    MSG_RCODE_SERVFAIL,      // Rcode =  2: ServFail
    MSG_RCODE_NXDOMAIN,      // Rcode =  3: NXDomain
    MSG_RCODE_NOTIMP,        // Rcode =  4: NotImp
    MSG_RCODE_REFUSED,       // Rcode =  5: Refused
    MSG_RCODE_YXDOMAIN,      // Rcode =  6: YXDomain
    MSG_RCODE_YXRRSET,       // Rcode =  7: YXRRSet
    MSG_RCODE_NXRRSET,       // Rcode =  8: NXRRSet
    MSG_RCODE_NOTAUTH,       // Rcode =  9: NotAuth
    MSG_RCODE_NOTZONE,       // Rcode = 10: NotZone
    MSG_RCODE_OTHER,         // Rcode = 11: (Unassigned)
    MSG_RCODE_OTHER,         // Rcode = 12: (Unassigned)
    MSG_RCODE_OTHER,         // Rcode = 13: (Unassigned)
    MSG_RCODE_OTHER,         // Rcode = 14: (Unassigned)
    MSG_RCODE_OTHER,         // Rcode = 15: (Unassigned)
    MSG_RCODE_BADVERS        // Rcode = 16: BADVERS
};
const size_t num_rcode_to_msgcounter =
    sizeof(rcode_to_msgcounter) / sizeof(rcode_to_msgcounter[0]);

Counters::Counters() :
    server_msg_counter_(MSG_COUNTER_TYPES)
{}

void
Counters::incRequest(const MessageAttributes& msgattrs) {
    // protocols carrying request
    if (msgattrs.getRequestIPVersion() == AF_INET) {
        server_msg_counter_.inc(MSG_REQUEST_IPV4);
    } else if (msgattrs.getRequestIPVersion() == AF_INET6) {
        server_msg_counter_.inc(MSG_REQUEST_IPV6);
    }
    if (msgattrs.getRequestTransportProtocol() == IPPROTO_UDP) {
        server_msg_counter_.inc(MSG_REQUEST_UDP);
    } else if (msgattrs.getRequestTransportProtocol() == IPPROTO_TCP) {
        server_msg_counter_.inc(MSG_REQUEST_TCP);
    }

    // Opcode
    const boost::optional<isc::dns::Opcode>& opcode =
        msgattrs.getRequestOpCode();
    // Increment opcode counter only if the opcode exists; opcode can be empty
    // if a short message which does not contain DNS header is received, or
    // a response message (i.e. QR bit is set) is received.
    if (opcode) {
        server_msg_counter_.inc(opcode_to_msgcounter[opcode.get().getCode()]);
    }

    // TSIG
    if (msgattrs.requestHasTSIG()) {
        server_msg_counter_.inc(MSG_REQUEST_TSIG);
    }
    if (msgattrs.requestHasBadSig()) {
        server_msg_counter_.inc(MSG_REQUEST_BADSIG);
        // If signature validation failed, no other request attributes (except
        // for opcode) are reliable. Skip processing of the rest of request
        // counters.
        return;
    }

    // EDNS0
    if (msgattrs.requestHasEDNS0()) {
        server_msg_counter_.inc(MSG_REQUEST_EDNS0);
    }

    // DNSSEC OK bit
    if (msgattrs.requestHasDO()) {
        server_msg_counter_.inc(MSG_REQUEST_DNSSEC_OK);
    }
}

void
Counters::incResponse(const MessageAttributes& msgattrs,
                      const Message& response)
{
    // responded
    server_msg_counter_.inc(MSG_RESPONSE);

    // response truncated
    if (msgattrs.responseIsTruncated()) {
        server_msg_counter_.inc(MSG_RESPONSE_TRUNCATED);
    }

    // response EDNS
    ConstEDNSPtr response_edns = response.getEDNS();
    if (response_edns && response_edns->getVersion() == 0) {
        server_msg_counter_.inc(MSG_RESPONSE_EDNS0);
    }

    // response TSIG
    if (msgattrs.responseHasTSIG()) {
        server_msg_counter_.inc(MSG_RESPONSE_TSIG);
    }

    // response SIG(0) is currently not implemented

    // RCODE
    const unsigned int rcode = response.getRcode().getCode();
    const unsigned int rcode_type =
        rcode < num_rcode_to_msgcounter ?
        rcode_to_msgcounter[rcode] : MSG_RCODE_OTHER;
    server_msg_counter_.inc(rcode_type);
    // Unsupported EDNS version
    if (rcode == Rcode::BADVERS().getCode()) {
        server_msg_counter_.inc(MSG_REQUEST_BADEDNSVER);
    }

    const boost::optional<isc::dns::Opcode>& opcode =
        msgattrs.getRequestOpCode();
    if (!opcode) {
        isc_throw(isc::Unexpected, "Opcode of the request is empty while it is"
                                   " responded");
    }
    if (!msgattrs.requestHasBadSig() && opcode.get() == Opcode::QUERY()) {
        // compound attributes
        const unsigned int answer_rrs =
            response.getRRCount(Message::SECTION_ANSWER);
        const bool is_aa_set =
            response.getHeaderFlag(Message::HEADERFLAG_AA);

        if (is_aa_set) {
            // QryAuthAns
            server_msg_counter_.inc(MSG_QRYAUTHANS);
        } else {
            // QryNoAuthAns
            server_msg_counter_.inc(MSG_QRYNOAUTHANS);
        }

        if (rcode == Rcode::NOERROR_CODE) {
            if (answer_rrs > 0) {
                // QrySuccess
                server_msg_counter_.inc(MSG_QRYSUCCESS);
            } else {
                if (is_aa_set) {
                    // QryNxrrset
                    server_msg_counter_.inc(MSG_QRYNXRRSET);
                } else {
                    // QryReferral
                    server_msg_counter_.inc(MSG_QRYREFERRAL);
                }
            }
        } else if (rcode == Rcode::REFUSED_CODE) {
            if (!response.getHeaderFlag(Message::HEADERFLAG_RD)) {
                // AuthRej
                server_msg_counter_.inc(MSG_QRYREJECT);
            }
        }
    }
}

void
Counters::inc(const MessageAttributes& msgattrs, const Message& response,
              const bool done)
{
    // increment request counters
    incRequest(msgattrs);

    if (done) {
        // increment response counters if answer was sent
        incResponse(msgattrs, response);
    }
}

Counters::ConstItemTreePtr
Counters::get() const {
    using namespace isc::data;

    isc::data::ElementPtr item_tree = Element::createMap();

    isc::data::ElementPtr zones = Element::createMap();
    item_tree->set("zones", zones);

    isc::data::ElementPtr server = Element::createMap();
    fillNodes(server_msg_counter_, msg_counter_tree, server);
    zones->set("_SERVER_", server);

    return (item_tree);
}

} // namespace statistics
} // namespace auth
} // namespace isc
