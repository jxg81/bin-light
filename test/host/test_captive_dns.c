// The captive-portal DNS responder (SPEC.md 3.4).
//
// Worth testing hard despite being small: build_reply() does pointer
// arithmetic over a packet that arrives from **anything that can associate to
// an open access point**. Every length field in it is attacker-controlled, and
// the failure mode is not a wrong answer but a read off the end of the buffer.
// So most of what follows is malformed input, not happy paths.
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "esp_err.h"

#include "captive_dns.c"

static int g_fail;

static void check(const char *name, bool ok, const char *detail)
{
    printf("%s %-54s %s\n", ok ? "PASS" : "FAIL", name, detail);
    if (!ok) g_fail++;
}

// Builds a minimal well-formed query for "binlight.local" unless `name` says
// otherwise. Returns the length.
static int make_query(uint8_t *buf, const char *const *labels, int nlabels,
                      uint16_t qtype, uint16_t qclass, uint16_t flags)
{
    int off = 0;
    wr16(buf + OFF_ID, 0x1234);
    wr16(buf + OFF_FLAGS, flags);
    wr16(buf + OFF_QDCOUNT, 1);
    wr16(buf + OFF_ANCOUNT, 0);
    wr16(buf + 8, 0);
    wr16(buf + 10, 0);
    off = DNS_HEADER_LEN;
    for (int i = 0; i < nlabels; i++) {
        int len = (int)strlen(labels[i]);
        buf[off++] = (uint8_t)len;
        memcpy(buf + off, labels[i], len);
        off += len;
    }
    buf[off++] = 0;
    wr16(buf + off, qtype);
    off += 2;
    wr16(buf + off, qclass);
    off += 2;
    return off;
}

static const char *const BINLIGHT_LOCAL[] = { "binlight", "local" };
static const char *const GENERATE_204[] = { "connectivitycheck", "gstatic", "com" };

int main(void)
{
    uint8_t buf[DNS_BUF_SIZE];
    int len, reply;

    printf("\n== captive DNS: answers ==\n");

    s_answer_addr = inet_addr("192.168.4.1");

    len = make_query(buf, BINLIGHT_LOCAL, 2, DNS_TYPE_A, DNS_CLASS_IN, 0x0100);
    reply = build_reply(buf, len);
    check("A query for binlight.local is answered", reply == len + 16, "question echoed + 16-byte A record");
    check("  response bit set", (rd16(buf + OFF_FLAGS) & FLAG_QR) != 0, "QR=1");
    check("  authoritative", (rd16(buf + OFF_FLAGS) & FLAG_AA) != 0, "AA=1");
    check("  rcode is NOERROR", (rd16(buf + OFF_FLAGS) & FLAG_RCODE) == 0, "rcode=0");
    check("  exactly one answer", rd16(buf + OFF_ANCOUNT) == 1, "ANCOUNT=1");
    check("  question preserved", rd16(buf + OFF_QDCOUNT) == 1, "QDCOUNT=1");
    check("  id echoed", rd16(buf + OFF_ID) == 0x1234, "clients match on it");
    {
        uint8_t *a = buf + len;
        uint32_t ip;
        memcpy(&ip, a + 12, 4);
        check("  answer is a compression pointer to the question",
              a[0] == 0xC0 && a[1] == DNS_HEADER_LEN, "0xC00C");
        check("  answer type/class", rd16(a + 2) == DNS_TYPE_A && rd16(a + 4) == DNS_CLASS_IN, "A / IN");
        check("  TTL is zero", a[6] == 0 && a[7] == 0 && a[8] == 0 && a[9] == 0,
              "or the phone caches it onto the home LAN");
        check("  rdlength", rd16(a + 10) == 4, "4");
        check("  points at the AP", ip == inet_addr("192.168.4.1"), "192.168.4.1");
    }

    // The whole point of the hijack: a name we have never heard of, asked by
    // the phone's connectivity probe, must still come back as us.
    len = make_query(buf, GENERATE_204, 3, DNS_TYPE_A, DNS_CLASS_IN, 0x0100);
    reply = build_reply(buf, len);
    {
        uint32_t ip;
        memcpy(&ip, buf + len + 12, 4);
        check("connectivitycheck.gstatic.com is hijacked too",
              reply == len + 16 && ip == inet_addr("192.168.4.1"), "every name resolves to us");
    }

    printf("\n== captive DNS: non-A queries get an empty NOERROR ==\n");

    // Answering AAAA with silence would stall the phone for seconds waiting on
    // a timeout; an empty NOERROR makes it fall straight through to the A.
    len = make_query(buf, BINLIGHT_LOCAL, 2, 28 /* AAAA */, DNS_CLASS_IN, 0x0100);
    reply = build_reply(buf, len);
    check("AAAA answered with no records", reply == len && rd16(buf + OFF_ANCOUNT) == 0,
          "not silence - silence costs a client timeout");
    check("  still marked a response", (rd16(buf + OFF_FLAGS) & FLAG_QR) != 0, "QR=1");
    check("  still NOERROR", (rd16(buf + OFF_FLAGS) & FLAG_RCODE) == 0, "empty, not refused");

    len = make_query(buf, BINLIGHT_LOCAL, 2, DNS_TYPE_A, 3 /* CHAOS */, 0x0100);
    reply = build_reply(buf, len);
    check("non-IN class answered with no records", reply == len && rd16(buf + OFF_ANCOUNT) == 0, "class CH");

    printf("\n== captive DNS: malformed and hostile input is dropped ==\n");

    len = make_query(buf, BINLIGHT_LOCAL, 2, DNS_TYPE_A, DNS_CLASS_IN, 0x0100);

    check("truncated below a header", build_reply(buf, DNS_HEADER_LEN - 1) < 0, "len < 12");
    check("header only, no question", build_reply(buf, DNS_HEADER_LEN) < 0, "QNAME runs off the end");

    {
        uint8_t t[DNS_BUF_SIZE];
        memcpy(t, buf, len);
        // Truncated mid-QNAME: the length byte claims more than is present.
        check("QNAME length byte overruns the packet", build_reply(t, DNS_HEADER_LEN + 4) < 0,
              "claims 8 bytes, 3 remain");
    }

    {
        uint8_t t[DNS_BUF_SIZE];
        memcpy(t, buf, len);
        // A well-formed name whose QTYPE/QCLASS were cut off.
        check("QNAME complete but QTYPE/QCLASS truncated", build_reply(t, len - 3) < 0,
              "needs 4 bytes after the name");
    }

    {
        uint8_t t[DNS_BUF_SIZE];
        memcpy(t, buf, len);
        t[DNS_HEADER_LEN] = 0xC0;  // compression pointer where a label must be
        t[DNS_HEADER_LEN + 1] = 0x0C;
        check("compression pointer in the question is rejected", build_reply(t, len) < 0,
              "a query has nothing to compress against");
    }

    {
        uint8_t t[DNS_BUF_SIZE];
        memcpy(t, buf, len);
        wr16(t + OFF_FLAGS, 0x8180); // already a response
        check("a response is not answered", build_reply(t, len) < 0, "QR=1 in, dropped");
    }

    {
        uint8_t t[DNS_BUF_SIZE];
        memcpy(t, buf, len);
        wr16(t + OFF_FLAGS, 0x0800); // opcode 1 = inverse query
        check("non-QUERY opcode is dropped", build_reply(t, len) < 0, "opcode != 0");
    }

    {
        uint8_t t[DNS_BUF_SIZE];
        memcpy(t, buf, len);
        wr16(t + OFF_QDCOUNT, 0);
        check("zero questions is dropped", build_reply(t, len) < 0, "QDCOUNT=0");
    }

    {
        // A maximum-length name, so the answer would push past the buffer.
        uint8_t t[DNS_BUF_SIZE];
        int off = DNS_HEADER_LEN;
        wr16(t + OFF_FLAGS, 0x0100);
        wr16(t + OFF_QDCOUNT, 1);
        while (off < DNS_BUF_SIZE - 8) {
            int lab = 60;
            if (off + 1 + lab > DNS_BUF_SIZE - 8) break;
            t[off++] = (uint8_t)lab;
            memset(t + off, 'a', lab);
            off += lab;
        }
        t[off++] = 0;
        wr16(t + off, DNS_TYPE_A); off += 2;
        wr16(t + off, DNS_CLASS_IN); off += 2;
        check("an oversized name cannot push the answer past the buffer",
              build_reply(t, off) < 0, "no write beyond DNS_BUF_SIZE");
    }

    {
        // Every truncation of a valid packet must be dropped or answered, but
        // never read out of bounds. Relies on the harness being run under a
        // sanitizer to be fully meaningful; at minimum it must not crash.
        uint8_t t[DNS_BUF_SIZE];
        bool survived = true;
        for (int n = 0; n <= len; n++) {
            memcpy(t, buf, len);
            int r = build_reply(t, n);
            if (r > DNS_BUF_SIZE) survived = false;
        }
        check("every truncation of a valid query is handled", survived,
              "0..len bytes, no oversized reply");
    }

    {
        // Byte-level fuzzing over the header and question, to catch anything
        // the hand-written cases missed.
        uint8_t t[DNS_BUF_SIZE];
        bool survived = true;
        for (int pos = 0; pos < len; pos++) {
            for (int v = 0; v < 256; v++) {
                memcpy(t, buf, len);
                t[pos] = (uint8_t)v;
                int r = build_reply(t, len);
                if (r > DNS_BUF_SIZE) survived = false;
            }
        }
        check("single-byte mutations never produce an oversized reply", survived,
              "len * 256 mutations");
    }

    printf("\n%s (%d failure%s)\n", g_fail ? "FAILURES" : "all passed", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
