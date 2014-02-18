// Copyright (C) 2012  Internet Systems Consortium, Inc. ("ISC")
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

#ifndef __STATISTICS_ITEMS_H
#define __STATISTICS_ITEMS_H 1

/// This file declares a set of statistics items in Auth module for internal
/// use. This file is intended to be included in statistics.cc and unittests.

namespace isc {
namespace auth {
namespace statistics {

struct CounterSpec {
    /// \brief name Name of this node. This appears in the spec file.
    const char* const name;
    /// \brief sub_counters If this is a branch node, sub_counters points to
    ///                     CounterSpec which contains child nodes. Otherwise,
    ///                     for leaf nodes, sub_counters is NULL.
    const struct CounterSpec* const sub_counters;
    /// \brief counter_id If this is a leaf node, counter_id is an enumerator
    ///                   of this item. Otherwise, for branch nodes, counter_id
    ///                   is NOT_ITEM.
    const int counter_id;
};

enum MSGCounterType {
    MSG_REQUEST_IPV4,    ///< Number of IPv4 requests received by the b10-auth server.
    MSG_REQUEST_IPV6,    ///< Number of IPv6 requests received by the b10-auth server.
    MSG_REQUEST_EDNS0,    ///< Number of requests with EDNS0 received by the b10-auth server.
    MSG_REQUEST_BADEDNSVER,    ///< Number of requests with unsupported EDNS version received by the b10-auth server.
    MSG_REQUEST_TSIG,    ///< Number of requests with TSIG received by the b10-auth server.
    MSG_REQUEST_SIG0,    ///< Number of requests with SIG(0) received by the b10-auth server; currently not implemented in BIND 10.
    MSG_REQUEST_BADSIG,    ///< Number of requests with invalid TSIG or SIG(0) signature received by the b10-auth server.
    MSG_REQUEST_UDP,    ///< Number of UDP requests received by the b10-auth server.
    MSG_REQUEST_TCP,    ///< Number of TCP requests received by the b10-auth server.
    MSG_REQUEST_DNSSEC_OK,    ///< Number of requests with "DNSSEC OK" (DO) bit was set received by the b10-auth server.
    MSG_OPCODE_QUERY,    ///< Number of OpCode=Query requests received by the b10-auth server.
    MSG_OPCODE_IQUERY,    ///< Number of OpCode=IQuery requests received by the b10-auth server.
    MSG_OPCODE_STATUS,    ///< Number of OpCode=Status requests received by the b10-auth server.
    MSG_OPCODE_NOTIFY,    ///< Number of OpCode=Notify requests received by the b10-auth server.
    MSG_OPCODE_UPDATE,    ///< Number of OpCode=Update requests received by the b10-auth server.
    MSG_OPCODE_OTHER,    ///< Number of requests in other OpCode received by the b10-auth server.
    MSG_RESPONSE,    ///< Number of responses sent by the b10-auth server.
    MSG_RESPONSE_TRUNCATED,    ///< Number of truncated responses sent by the b10-auth server.
    MSG_RESPONSE_EDNS0,    ///< Number of responses with EDNS0 sent by the b10-auth server.
    MSG_RESPONSE_TSIG,    ///< Number of responses with TSIG sent by the b10-auth server.
    MSG_RESPONSE_SIG0,    ///< Number of responses with SIG(0) sent by the b10-auth server; currently not implemented in BIND 10.
    MSG_QRYSUCCESS,    ///< Number of queries received by the b10-auth server resulted in rcode = NoError and the number of answer RR >= 1.
    MSG_QRYAUTHANS,    ///< Number of queries received by the b10-auth server resulted in authoritative answer.
    MSG_QRYNOAUTHANS,    ///< Number of queries received by the b10-auth server resulted in non-authoritative answer.
    MSG_QRYREFERRAL,    ///< Number of queries received by the b10-auth server resulted in referral answer.
    MSG_QRYNXRRSET,    ///< Number of queries received by the b10-auth server resulted in NoError and AA bit is set in the response, but the number of answer RR == 0.
    MSG_QRYREJECT,    ///< Number of authoritative queries rejected by the b10-auth server.
    MSG_RCODE_NOERROR,    ///< Number of requests received by the b10-auth server resulted in RCODE = 0 (NoError).
    MSG_RCODE_FORMERR,    ///< Number of requests received by the b10-auth server resulted in RCODE = 1 (FormErr).
    MSG_RCODE_SERVFAIL,    ///< Number of requests received by the b10-auth server resulted in RCODE = 2 (ServFail).
    MSG_RCODE_NXDOMAIN,    ///< Number of requests received by the b10-auth server resulted in RCODE = 3 (NXDomain).
    MSG_RCODE_NOTIMP,    ///< Number of requests received by the b10-auth server resulted in RCODE = 4 (NotImp).
    MSG_RCODE_REFUSED,    ///< Number of requests received by the b10-auth server resulted in RCODE = 5 (Refused).
    MSG_RCODE_YXDOMAIN,    ///< Number of requests received by the b10-auth server resulted in RCODE = 6 (YXDomain).
    MSG_RCODE_YXRRSET,    ///< Number of requests received by the b10-auth server resulted in RCODE = 7 (YXRRSet).
    MSG_RCODE_NXRRSET,    ///< Number of requests received by the b10-auth server resulted in RCODE = 8 (NXRRSet).
    MSG_RCODE_NOTAUTH,    ///< Number of requests received by the b10-auth server resulted in RCODE = 9 (NotAuth).
    MSG_RCODE_NOTZONE,    ///< Number of requests received by the b10-auth server resulted in RCODE = 10 (NotZone).
    MSG_RCODE_BADVERS,    ///< Number of requests received by the b10-auth server resulted in RCODE = 16 (BADVERS).
    MSG_RCODE_OTHER,    ///< Number of requests received by the b10-auth server resulted in other RCODEs.
    // End of counter types
    MSG_COUNTER_TYPES  ///< The number of defined counters
};

extern const int opcode_to_msgcounter[];
extern const size_t num_opcode_to_msgcounter;
extern const int rcode_to_msgcounter[];
extern const size_t num_rcode_to_msgcounter;

} // namespace statistics
} // namespace auth
} // namespace isc

#endif // __STATISTICS_ITEMS_H

// Local Variables:
// mode: c++
// End:
