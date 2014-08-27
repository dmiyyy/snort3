/*
** Copyright (C) 2002-2013 Sourcefire, Inc.
** Copyright (C) 1998-2002 Martin Roesch <roesch@sourcefire.com>
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License Version 2 as
** published by the Free Software Foundation.  You may not use, modify or
** distribute this program under any other version of the GNU General
** Public License.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/
// cd_tcp.cc author Josh Rosenbaum <jrosenba@cisco.com>



#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#ifdef HAVE_DUMBNET_H
#include <dumbnet.h>
#else
#include <dnet.h>
#endif


#include "framework/codec.h"
#include "codecs/decode_module.h"
#include "codecs/ip/checksum.h"
#include "codecs/sf_protocols.h"
#include "protocols/tcp.h"
#include "protocols/ipv6.h"
#include "protocols/packet.h"
#include "packet_io/active.h"
#include "codecs/codec_events.h"
#include "packet_io/sfdaq.h"
#include "parser/parse_ip.h"
#include "sfip/sf_ipvar.h"


namespace
{


#define CD_TCP_NAME "tcp"

static const RuleMap tcp_rules[] =
{
    { DECODE_TCP_DGRAM_LT_TCPHDR, "(" CD_TCP_NAME ") TCP packet len is smaller than 20 bytes" },
    { DECODE_TCP_INVALID_OFFSET, "(" CD_TCP_NAME ") TCP Data Offset is less than 5" },
    { DECODE_TCP_LARGE_OFFSET, "(" CD_TCP_NAME ") TCP Header length exceeds packet length" },

    { DECODE_TCPOPT_BADLEN, "(" CD_TCP_NAME ") Tcp Options found with bad lengths" },
    { DECODE_TCPOPT_TRUNCATED, "(" CD_TCP_NAME ") Truncated Tcp Options" },
    { DECODE_TCPOPT_TTCP, "(" CD_TCP_NAME ") T/TCP Detected" },
    { DECODE_TCPOPT_OBSOLETE, "(" CD_TCP_NAME ") Obsolete TCP Options found" },
    { DECODE_TCPOPT_EXPERIMENTAL, "(" CD_TCP_NAME ") Experimental Tcp Options found" },
    { DECODE_TCPOPT_WSCALE_INVALID, "(" CD_TCP_NAME ") Tcp Window Scale Option found with length > 14" },
    { DECODE_TCP_XMAS, "(" CD_TCP_NAME ") XMAS Attack Detected" },
    { DECODE_TCP_NMAP_XMAS, "(" CD_TCP_NAME ") Nmap XMAS Attack Detected" },
    { DECODE_TCP_BAD_URP, "(" CD_TCP_NAME ") TCP urgent pointer exceeds payload length or no payload" },
    { DECODE_TCP_SYN_FIN, "(" CD_TCP_NAME ") TCP SYN with FIN" },
    { DECODE_TCP_SYN_RST, "(" CD_TCP_NAME ") TCP SYN with RST" },
    { DECODE_TCP_MUST_ACK, "(" CD_TCP_NAME ") TCP PDU missing ack for established session" },
    { DECODE_TCP_NO_SYN_ACK_RST, "(" CD_TCP_NAME ") TCP has no SYN, ACK, or RST" },
    { DECODE_TCP_SHAFT_SYNFLOOD, "(" CD_TCP_NAME ") DDOS shaft synflood" },
    { DECODE_TCP_PORT_ZERO, "(" CD_TCP_NAME ") BAD-TRAFFIC TCP port 0 traffic" },
    { DECODE_DOS_NAPTHA, "(decode) DOS NAPTHA Vulnerability Detected" },
    { DECODE_SYN_TO_MULTICAST, "(decode) Bad Traffic SYN to multicast address" },
    { 0, nullptr }
};


class TcpModule : public DecodeModule
{
public:
    TcpModule() : DecodeModule(CD_TCP_NAME) {}

    const RuleMap* get_rules() const
    { return tcp_rules; }
};



class TcpCodec : public Codec
{
public:
    TcpCodec() : Codec(CD_TCP_NAME)
    {

    };
    virtual ~TcpCodec(){};


    virtual PROTO_ID get_proto_id() { return PROTO_TCP; };
    virtual void get_protocol_ids(std::vector<uint16_t>& v);
    virtual bool decode(const uint8_t *raw_pkt, const uint32_t& raw_len,
        Packet *, uint16_t &lyr_len, uint16_t &);
    virtual bool encode(EncState*, Buffer* out, const uint8_t *raw_in);
    virtual bool update(Packet*, Layer*, uint32_t* len);
    virtual void format(EncodeFlags, const Packet* p, Packet* c, Layer*);
};

static sfip_var_t *SynToMulticastDstIp = NULL;

} // namespace



static int OptLenValidate(const uint8_t *option_ptr,
                                 const uint8_t *end,
                                 const uint8_t *len_ptr,
                                 int expected_len,
                                 Options *tcpopt,
                                 uint8_t *byte_skip);


static void DecodeTCPOptions(const uint8_t *, uint32_t, Packet *);
static inline void TCPMiscTests(Packet *p);

void TcpCodec::get_protocol_ids(std::vector<uint16_t>& v)
{
    v.push_back(IPPROTO_TCP);
}


/*
 * Function: DecodeTCP(uint8_t *, const uint32_t, Packet *)
 *
 * Purpose: Decode the TCP transport layer
 *
 * Arguments: pkt => ptr to the packet data
 *            len => length from here to the end of the packet
 *            p   => Pointer to packet decode struct
 *
 * Returns: void function
 */
bool TcpCodec::decode(const uint8_t *raw_pkt, const uint32_t& raw_len,
        Packet *p, uint16_t &lyr_len, uint16_t& /*next_prot_id*/)
{
    if(raw_len < tcp::TCP_HEADER_LEN)
    {
        DEBUG_WRAP(DebugMessage(DEBUG_DECODE,
            "TCP packet (len = %d) cannot contain " "20 byte header\n", raw_len););

        codec_events::decoder_event(p, DECODE_TCP_DGRAM_LT_TCPHDR);

        p->tcph = NULL;
        return false;
    }

    /* lay TCP on top of the data cause there is enough of it! */
    tcp::TCPHdr* tcph = reinterpret_cast<tcp::TCPHdr*>(const_cast<uint8_t*>(raw_pkt));
    p->tcph = tcph;

    /* multiply the payload offset value by 4 */
    lyr_len = tcph->hdr_len();

    DEBUG_WRAP(DebugMessage(DEBUG_DECODE, "TCP th_off is %d, passed len is %lu\n",
                TCP_OFFSET(p->tcph), (unsigned long)raw_len););

    if(lyr_len < tcp::TCP_HEADER_LEN)
    {
        DEBUG_WRAP(DebugMessage(DEBUG_DECODE,
            "TCP Data Offset (%d) < lyr_len (%d) \n",
            TCP_OFFSET(p->tcph), lyr_len););

        codec_events::decoder_event(p, DECODE_TCP_INVALID_OFFSET);

        p->tcph = NULL;
        return false;
    }

    if(lyr_len > raw_len)
    {
        DEBUG_WRAP(DebugMessage(DEBUG_DECODE,
            "TCP Data Offset(%d) < longer than payload(%d)!\n",
            TCP_OFFSET(p->tcph) << 2, raw_len););

        codec_events::decoder_event(p, DECODE_TCP_LARGE_OFFSET);

        p->tcph = NULL;
        return false;
    }

    /* Checksum code moved in front of the other decoder alerts.
       If it's a bad checksum (maybe due to encrypted ESP traffic), the other
       alerts could be false positives. */
    if (ScTcpChecksums())
    {
        uint16_t csum;
        if(p->ip_api.is_ip4())
        {
            checksum::Pseudoheader ph;
            ph.sip = p->ip_api.get_ip4_src();
            ph.dip = p->ip_api.get_ip4_dst();
            /* setup the pseudo header for checksum calculation */
            ph.zero = 0;
            ph.protocol = p->ip_api.proto();
            ph.len = htons((uint16_t)raw_len);

            /* if we're being "stateless" we probably don't care about the TCP
             * checksum, but it's not bad to keep around for shits and giggles */
            /* calculate the checksum */
            csum = checksum::tcp_cksum((uint16_t *)(p->tcph), raw_len, &ph);

        }
        /* IPv6 traffic */
        else
        {
            checksum::Pseudoheader6 ph6;
            COPY4(ph6.sip, p->ip_api.get_ip6_src()->u6_addr32);
            COPY4(ph6.dip, p->ip_api.get_ip6_dst()->u6_addr32);
            ph6.zero = 0;
            ph6.protocol = p->ip_api.proto();
            ph6.len = htons((uint16_t)raw_len);


            csum = checksum::tcp_cksum((uint16_t *)(p->tcph), raw_len, &ph6);
        }

        if(csum)
        {
            /* Don't drop the packet if this is encapuslated in Teredo or ESP.
               Just get rid of the TCP header and stop decoding. */
            if (p->decode_flags & DECODE__UNSURE_ENCAP)
            {
                p->tcph = NULL;
                return false;
            }

            p->error_flags |= PKT_ERR_CKSUM_TCP;
            DEBUG_WRAP(DebugMessage(DEBUG_DECODE, "Bad TCP checksum\n",
                                    "0x%x versus 0x%x\n", csum,
                                    ntohs(p->tcph->th_sum)););


            if( ScInlineMode() && ScTcpChecksumDrops() )
            {
                DEBUG_WRAP(DebugMessage(DEBUG_DECODE,
                    "Dropping bad packet (TCP checksum)\n"););
                Active_DropPacket();
            }
        }
        else
        {
            DEBUG_WRAP(DebugMessage(DEBUG_DECODE,"TCP Checksum: OK\n"););
        }
    }

    if(TCP_ISFLAGSET(p->tcph, (TH_FIN|TH_PUSH|TH_URG)))
    {
        if(TCP_ISFLAGSET(p->tcph, (TH_SYN|TH_ACK|TH_RST)))
        {
            codec_events::decoder_event(p, DECODE_TCP_XMAS);
        }
        else
        {
            codec_events::decoder_event(p, DECODE_TCP_NMAP_XMAS);
        }
        // Allowing this packet for further processing
        // (in case there is a valid data inside it).
        /*p->tcph = NULL;
        return;*/
    }

    if(TCP_ISFLAGSET(p->tcph, (TH_SYN)))
    {
        /* check if only SYN is set */
        if( p->tcph->th_flags == TH_SYN )
        {
            if( p->tcph->th_seq == 6060842 )
            {
                if( p->ip_api.id(p) == 413 )
                {
                    codec_events::decoder_event(p, DECODE_DOS_NAPTHA);
                }
            }
        }

        if( sfvar_ip_in(SynToMulticastDstIp, p->ip_api.get_dst()) )
        {
            codec_events::decoder_event(p, DECODE_SYN_TO_MULTICAST);
        }
        if ( (p->tcph->th_flags & TH_RST) )
            codec_events::decoder_event(p, DECODE_TCP_SYN_RST);

        if ( (p->tcph->th_flags & TH_FIN) )
            codec_events::decoder_event(p, DECODE_TCP_SYN_FIN);
    }
    else
    {   // we already know there is no SYN
        if ( !(p->tcph->th_flags & (TH_ACK|TH_RST)) )
            codec_events::decoder_event(p, DECODE_TCP_NO_SYN_ACK_RST);
    }

    if ( (p->tcph->th_flags & (TH_FIN|TH_PUSH|TH_URG)) &&
        !(p->tcph->th_flags & TH_ACK) )
        codec_events::decoder_event(p, DECODE_TCP_MUST_ACK);

    /* stuff more data into the printout data struct */
    p->sp = ntohs(p->tcph->th_sport);
    p->dp = ntohs(p->tcph->th_dport);


    /* if options are present, decode them */
    uint16_t tcp_opt_len = (uint16_t)(tcph->hdr_len() - tcp::TCP_HEADER_LEN);

    if(tcp_opt_len > 0)
    {
        DEBUG_WRAP(DebugMessage(DEBUG_DECODE, "%lu bytes of tcp options....\n",
                    (unsigned long)(tcp_opt_len)););

        DecodeTCPOptions((uint8_t *) (raw_pkt + tcp::TCP_HEADER_LEN), tcp_opt_len, p);
    }
    else
    {
        p->tcp_option_count = 0;
    }

    /* set the data pointer and size */
    p->data = (uint8_t *) (raw_pkt + lyr_len);

    if(lyr_len < raw_len)
    {
        p->dsize = (uint16_t)(raw_len - lyr_len);
    }
    else
    {
        p->dsize = 0;
    }

    if ( (p->tcph->th_flags & TH_URG) &&
        (!p->dsize || ntohs(p->tcph->th_urp) > p->dsize) )
        codec_events::decoder_event(p, DECODE_TCP_BAD_URP);

    p->proto_bits |= PROTO_BIT__TCP;
    TCPMiscTests(p);
    
    return true;
}



/*
 * Function: DecodeTCPOptions(uint8_t *, uint32_t, Packet *)
 *
 * Purpose: Fairly self explainatory name, don't you think?
 *
 *          TCP Option Header length validation is left to the caller
 *
 *          For a good listing of TCP Options,
 *          http://www.iana.org/assignments/tcp-parameters
 *
 *   ------------------------------------------------------------
 *   From: "Kastenholz, Frank" <FKastenholz@unispherenetworks.com>
 *   Subject: Re: skeeter & bubba TCP options?
 *
 *   ah, the sins of ones youth that never seem to be lost...
 *
 *   it was something that ben levy and stev and i did at ftp many
 *   many moons ago. bridgham and stev were the instigators of it.
 *   the idea was simple, put a dh key exchange directly in tcp
 *   so that all tcp sessions could be encrypted without requiring
 *   any significant key management system. authentication was not
 *   a part of the idea, it was to be provided by passwords or
 *   whatever, which could now be transmitted over the internet
 *   with impunity since they were encrypted... we implemented
 *   a simple form of this (doing the math was non trivial on the
 *   machines of the day). it worked. the only failure that i
 *   remember was that it was vulnerable to man-in-the-middle
 *   attacks.
 *
 *   why "skeeter" and "bubba"? well, that's known only to stev...
 *   ------------------------------------------------------------
 *
 * 4.2.2.5 TCP Options: RFC-793 Section 3.1
 *
 *    A TCP MUST be able to receive a TCP option in any segment. A TCP
 *    MUST ignore without error any TCP option it does not implement,
 *    assuming that the option has a length field (all TCP options
 *    defined in the future will have length fields). TCP MUST be
 *    prepared to handle an illegal option length (e.g., zero) without
 *    crashing; a suggested procedure is to reset the connection and log
 *    the reason.
 *
 * Arguments: o_list => ptr to the option list
 *            o_len => length of the option list
 *            p     => pointer to decoded packet struct
 *
 * Returns: void function
 */
void DecodeTCPOptions(const uint8_t *start, uint32_t o_len, Packet *p)
{
    const uint8_t *option_ptr = start;
    const uint8_t *end_ptr = start + o_len; /* points to byte after last option */
    const uint8_t *len_ptr;
    uint8_t opt_count = 0;
    u_char done = 0; /* have we reached TCPOPT_EOL yet?*/
    u_char experimental_option_found = 0;      /* are all options RFC compliant? */
    u_char obsolete_option_found = 0;
    u_char ttcp_found = 0;

    int code = 2;
    uint8_t byte_skip;

    /* Here's what we're doing so that when we find out what these
     * other buggers of TCP option codes are, we can do something
     * useful
     *
     * 1) get option code
     * 2) check for enough space for current option code
     * 3) set option data ptr
     * 4) increment option code ptr
     *
     * TCP_OPTLENMAX = 40 because of
     *        (((2^4) - 1) * 4  - tcp::TCP_HEADER_LEN
     *
     */

    if(o_len > TCP_OPTLENMAX)
    {
        /* This shouldn't ever alert if we are doing our job properly
         * in the caller */
        p->tcph = NULL; /* let's just alert */
        DEBUG_WRAP(DebugMessage(DEBUG_DECODE,
                                "o_len(%u) > TCP_OPTLENMAX(%u)\n",
                                o_len, TCP_OPTLENMAX));
        return;
    }

    while((option_ptr < end_ptr) && (opt_count < TCP_OPTLENMAX) && !done)
    {
        p->tcp_options[opt_count].code = *option_ptr;

        if((option_ptr + 1) < end_ptr)
        {
            len_ptr = option_ptr + 1;
        }
        else
        {
            len_ptr = NULL;
        }

        switch(*option_ptr)
        {
        case tcp::TcpOpt::EOL:
            done = 1; /* fall through to the NOP case */
        case tcp::TcpOpt::NOP:
            p->tcp_options[opt_count].len = 0;
            p->tcp_options[opt_count].data = NULL;
            byte_skip = 1;
            code = 0;
            break;
        case tcp::TcpOpt::MAXSEG:
            code = OptLenValidate(option_ptr, end_ptr, len_ptr, TCPOLEN_MAXSEG,
                                  &p->tcp_options[opt_count], &byte_skip);
            break;
        case tcp::TcpOpt::SACKOK:
            code = OptLenValidate(option_ptr, end_ptr, len_ptr, TCPOLEN_SACKOK,
                                  &p->tcp_options[opt_count], &byte_skip);
            break;
        case tcp::TcpOpt::WSCALE:
            code = OptLenValidate(option_ptr, end_ptr, len_ptr, TCPOLEN_WSCALE,
                                  &p->tcp_options[opt_count], &byte_skip);
            if (code == 0)
            {
                if (
                    ((uint16_t) p->tcp_options[opt_count].data[0] > 14))
                {
                    /* LOG INVALID WINDOWSCALE alert */
                    codec_events::decoder_event(p, DECODE_TCPOPT_WSCALE_INVALID);
                }
            }
            break;
        case tcp::TcpOpt::ECHO: /* both use the same lengths */
        case tcp::TcpOpt::ECHOREPLY:
            obsolete_option_found = 1;
            code = OptLenValidate(option_ptr, end_ptr, len_ptr, TCPOLEN_ECHO,
                                  &p->tcp_options[opt_count], &byte_skip);
            break;
        case tcp::TcpOpt::MD5SIG:
            /* RFC 5925 obsoletes this option (see below) */
            obsolete_option_found = 1;
            code = OptLenValidate(option_ptr, end_ptr, len_ptr, TCPOLEN_MD5SIG,
                                  &p->tcp_options[opt_count], &byte_skip);
            break;
        case tcp::TcpOpt::AUTH:
            /* Has to have at least 4 bytes - see RFC 5925, Section 2.2 */
            if ((len_ptr != NULL) && (*len_ptr < 4))
                code = tcp::OPT_BADLEN;
            else
                code = OptLenValidate(option_ptr, end_ptr, len_ptr, -1,
                        &p->tcp_options[opt_count], &byte_skip);
            break;
        case tcp::TcpOpt::SACK:
            code = OptLenValidate(option_ptr, end_ptr, len_ptr, -1,
                                  &p->tcp_options[opt_count], &byte_skip);
            if((code == 0) && (p->tcp_options[opt_count].data == NULL))
                code = tcp::OPT_BADLEN;

            break;
        case tcp::TcpOpt::CC_ECHO:
            ttcp_found = 1;
            /* fall through */
        case tcp::TcpOpt::CC:  /* all 3 use the same lengths / T/TCP */
        case tcp::TcpOpt::CC_NEW:
            code = OptLenValidate(option_ptr, end_ptr, len_ptr, TCPOLEN_CC,
                                  &p->tcp_options[opt_count], &byte_skip);
            break;
        case tcp::TcpOpt::TRAILER_CSUM:
            experimental_option_found = 1;
            code = OptLenValidate(option_ptr, end_ptr, len_ptr, TCPOLEN_TRAILER_CSUM,
                                  &p->tcp_options[opt_count], &byte_skip);
            break;

        case tcp::TcpOpt::TIMESTAMP:
            code = OptLenValidate(option_ptr, end_ptr, len_ptr, TCPOLEN_TIMESTAMP,
                                  &p->tcp_options[opt_count], &byte_skip);
            break;

        case tcp::TcpOpt::SKEETER:
        case tcp::TcpOpt::BUBBA:
        case tcp::TcpOpt::UNASSIGNED:
            obsolete_option_found = 1;
            code = OptLenValidate(option_ptr, end_ptr, len_ptr, -1,
                                  &p->tcp_options[opt_count], &byte_skip);
            break;
        default:
        case tcp::TcpOpt::SCPS:
        case tcp::TcpOpt::SELNEGACK:
        case tcp::TcpOpt::RECORDBOUND:
        case tcp::TcpOpt::CORRUPTION:
        case tcp::TcpOpt::PARTIAL_PERM:
        case tcp::TcpOpt::PARTIAL_SVC:
        case tcp::TcpOpt::ALTCSUM:
        case tcp::TcpOpt::SNAP:
            experimental_option_found = 1;
            code = OptLenValidate(option_ptr, end_ptr, len_ptr, -1,
                                  &p->tcp_options[opt_count], &byte_skip);
            break;
        }

        if(code < 0)
        {
            if(code == tcp::OPT_BADLEN)
            {
                codec_events::decoder_event(p, DECODE_TCPOPT_BADLEN);
            }
            else if(code == tcp::OPT_TRUNC)
            {
                codec_events::decoder_event(p, DECODE_TCPOPT_TRUNCATED);
            }

            /* set the option count to the number of valid
             * options found before this bad one
             * some implementations (BSD and Linux) ignore
             * the bad ones, but accept the good ones */
            p->tcp_option_count = opt_count;

            return;
        }

        opt_count++;

        option_ptr += byte_skip;
    }

    p->tcp_option_count = opt_count;

    if (experimental_option_found)
    {
        codec_events::decoder_event(p, DECODE_TCPOPT_EXPERIMENTAL);
    }
    else if (obsolete_option_found)
    {
        codec_events::decoder_event(p, DECODE_TCPOPT_OBSOLETE);
    }
    else if (ttcp_found)
    {
        codec_events::decoder_event(p, DECODE_TCPOPT_TTCP);
    }

    return;
}



/* TCP-layer decoder alerts */
static inline void TCPMiscTests(Packet *p)
{
    if ( ((p->tcph->th_flags & TH_NORESERVED) == TH_SYN ) &&
         (p->tcph->th_seq == htonl(674711609)) )
        codec_events::decoder_event(p, DECODE_TCP_SHAFT_SYNFLOOD);

    if (p->sp == 0 || p->dp == 0)
        codec_events::decoder_event(p, DECODE_TCP_PORT_ZERO);
}


/******************************************************************
 ******************** E N C O D E R  ******************************
 ******************************************************************/

//-------------------------------------------------------------------------
// TCP
// encoder creates TCP RST
// should always try to use acceptable ack since we send RSTs in a
// stateless fashion ... from rfc 793:
//
// In all states except SYN-SENT, all reset (RST) segments are validated
// by checking their SEQ-fields.  A reset is valid if its sequence number
// is in the window.  In the SYN-SENT state (a RST received in response
// to an initial SYN), the RST is acceptable if the ACK field
// acknowledges the SYN.
//-------------------------------------------------------------------------

bool TcpCodec::encode (EncState* enc, Buffer* out, const uint8_t* raw_in)
{
    int ctl;
    const tcp::TCPHdr* hi = reinterpret_cast<const tcp::TCPHdr*>(raw_in);
    bool attach_payload = (enc->type == EncodeType::ENC_TCP_FIN || 
        enc->type == EncodeType::ENC_TCP_PUSH);

    // working our way backwards throught he packet. First, attach a payload
    if ( attach_payload && enc->payLoad && enc->payLen > 0 )
    {
        if (!update_buffer(out, enc->payLen))
            return false;
        memcpy(out->base, enc->payLoad, enc->payLen);
    }

    if (!update_buffer(out, tcp::get_tcp_hdr_len(hi)))
        return false;

    tcp::TCPHdr* ho = reinterpret_cast<tcp::TCPHdr*>(out->base);
    ctl = (hi->th_flags & TH_SYN) ? 1 : 0;

    if ( forward(enc->flags) )
    {
        ho->th_sport = hi->th_sport;
        ho->th_dport = hi->th_dport;

        // th_seq depends on whether the data passes or drops
        if ( DAQ_GetInterfaceMode(enc->p->pkth) != DAQ_MODE_INLINE )
            ho->th_seq = htonl(ntohl(hi->th_seq) + enc->p->dsize + ctl);
        else
            ho->th_seq = hi->th_seq;

        ho->th_ack = hi->th_ack;
    }
    else
    {
        ho->th_sport = hi->th_dport;
        ho->th_dport = hi->th_sport;

        ho->th_seq = hi->th_ack;
        ho->th_ack = htonl(ntohl(hi->th_seq) + enc->p->dsize + ctl);
    }

    if ( enc->flags & ENC_FLAG_SEQ )
    {
        uint32_t seq = ntohl(ho->th_seq);
        seq += (enc->flags & ENC_FLAG_VAL);
        ho->th_seq = htonl(seq);
    }

    ho->th_offx2 = 0;
    tcp::set_tcp_offset(ho, (TCP_HDR_LEN >> 2));
    ho->th_win = ho->th_urp = 0;

    if ( attach_payload )
    {
        ho->th_flags = TH_ACK;
        if ( enc->type == EncodeType::ENC_TCP_PUSH )
        {
            ho->th_flags |= TH_PUSH;
            ho->th_win = htons(65535);
        }
        else
        {
            ho->th_flags |= TH_FIN;
        }
    }
    else
    {
        ho->th_flags = TH_RST | TH_ACK;
    }

    // in case of ip6 extension headers, this gets next correct
    enc->proto = IPPROTO_TCP;
    ho->th_sum = 0;
    const ip::IpApi * const ip_api = &enc->p->ip_api;

    if (ip_api->is_ip4()) {
        checksum::Pseudoheader ps;
        int len = buff_diff(out, (uint8_t*)ho);

        const IP4Hdr* const ip4h = ip_api->get_ip4h();
        ps.sip = ip4h->get_src();
        ps.dip = ip4h->get_dst();
        ps.zero = 0;
        ps.protocol = IPPROTO_TCP;
        ps.len = htons((uint16_t)len);
        ho->th_sum = checksum::tcp_cksum((uint16_t *)ho, len, &ps);
    }
    else
    {
        checksum::Pseudoheader6 ps6;
        int len = buff_diff(out, (uint8_t*) ho);

        const ip::IP6Hdr* const ip6h = ip_api->get_ip6h();
        memcpy(ps6.sip, ip6h->get_src()->u6_addr8, sizeof(ps6.sip));
        memcpy(ps6.dip, ip6h->get_dst()->u6_addr8, sizeof(ps6.dip));
        ps6.zero = 0;
        ps6.protocol = IPPROTO_TCP;
        ps6.len = htons((uint16_t)len);
        ho->th_sum = checksum::tcp_cksum((uint16_t *)ho, len, &ps6);
    }

    return true;
}

bool TcpCodec::update(Packet* p, Layer* lyr, uint32_t* len)
{
    tcp::TCPHdr* h = reinterpret_cast<tcp::TCPHdr*>(lyr->start);

    *len += h->hdr_len() + p->dsize;

    if ( !PacketWasCooked(p) || (p->packet_flags & PKT_REBUILT_FRAG) )
    {
        h->th_sum = 0;

        if (p->ip_api.is_ip4())
        {
            checksum::Pseudoheader ps;
            const ip::IP4Hdr* ip4h = p->ip_api.get_ip4h();
            ps.sip = ip4h->get_src();
            ps.dip = ip4h->get_dst();;
            ps.zero = 0;
            ps.protocol = IPPROTO_TCP;
            ps.len = htons((uint16_t)*len);
            h->th_sum = checksum::tcp_cksum((uint16_t *)h, *len, &ps);
        }
        else
        {
            checksum::Pseudoheader6 ps6;
            const ip::IP6Hdr* ip6h = p->ip_api.get_ip6h();
            memcpy(ps6.sip, ip6h->get_src()->u6_addr32, sizeof(ps6.sip));
            memcpy(ps6.dip, ip6h->get_dst()->u6_addr32, sizeof(ps6.dip));
            ps6.zero = 0;
            ps6.protocol = IPPROTO_TCP;
            ps6.len = htons((uint16_t)*len);
            h->th_sum = checksum::tcp_cksum((uint16_t *)h, *len, &ps6);
        }
    }

    return true;
}

void TcpCodec::format(EncodeFlags f, const Packet* p, Packet* c, Layer* lyr)
{
    tcp::TCPHdr* ch = (tcp::TCPHdr*)lyr->start;
    c->tcph = ch;

    if ( reverse(f) )
    {
        int i = lyr - c->layers;
        tcp::TCPHdr* ph = (tcp::TCPHdr*)p->layers[i].start;

        ch->th_sport = ph->th_dport;
        ch->th_dport = ph->th_sport;
    }
    c->sp = ntohs(ch->th_sport);
    c->dp = ntohs(ch->th_dport);
}


static int OptLenValidate(const uint8_t *option_ptr,
                                 const uint8_t *end,
                                 const uint8_t *len_ptr,
                                 int expected_len,
                                 Options *tcpopt,
                                 uint8_t *byte_skip)
{
    *byte_skip = 0;

    if(len_ptr == NULL)
        return tcp::OPT_TRUNC;


    if(*len_ptr == 0 || expected_len == 0 || expected_len == 1)
    {
        return tcp::OPT_BADLEN;
    }
    else if(expected_len > 1)
    {
        /* not enough data to read in a perfect world */
        if((option_ptr + expected_len) > end)
            return tcp::OPT_TRUNC;

        if(*len_ptr != expected_len)
            return tcp::OPT_BADLEN;
    }
    else /* expected_len < 0 (i.e. variable length) */
    {
        /* RFC sez that we MUST have atleast this much data */
        if(*len_ptr < 2)
            return tcp::OPT_BADLEN;

        /* not enough data to read in a perfect world */
        if((option_ptr + *len_ptr) > end)
            return tcp::OPT_TRUNC;
    }

    tcpopt->len = *len_ptr - 2;

    if(*len_ptr == 2)
        tcpopt->data = NULL;
    else
        tcpopt->data = option_ptr + 2;

    *byte_skip = *len_ptr;

    return 0;
}


// KEEPING FOR TESTING AND PROFILING
#if 0


/*
 * Checksum Functions
 */

/*
*  checksum tcp
*
*  h    - pseudo header - 12 bytes
*  d    - tcp hdr + payload
*  dlen - length of tcp hdr + payload in bytes
*
*/
static inline unsigned short in_chksum_tcp(pseudoheader *ph,
    unsigned short * d, int dlen )
{
   uint16_t *h = (uint16_t *)ph;
   unsigned int cksum;
   unsigned short answer=0;

   /* PseudoHeader must have 12 bytes */
   cksum  = h[0];
   cksum += h[1];
   cksum += h[2];
   cksum += h[3];
   cksum += h[4];
   cksum += h[5];

   /* TCP hdr must have 20 hdr bytes */
   cksum += d[0];
   cksum += d[1];
   cksum += d[2];
   cksum += d[3];
   cksum += d[4];
   cksum += d[5];
   cksum += d[6];
   cksum += d[7];
   cksum += d[8];
   cksum += d[9];

   dlen  -= 20; /* bytes   */
   d     += 10; /* short's */

   while(dlen >=32)
   {
     cksum += d[0];
     cksum += d[1];
     cksum += d[2];
     cksum += d[3];
     cksum += d[4];
     cksum += d[5];
     cksum += d[6];
     cksum += d[7];
     cksum += d[8];
     cksum += d[9];
     cksum += d[10];
     cksum += d[11];
     cksum += d[12];
     cksum += d[13];
     cksum += d[14];
     cksum += d[15];
     d     += 16;
     dlen  -= 32;
   }

   while(dlen >=8)
   {
     cksum += d[0];
     cksum += d[1];
     cksum += d[2];
     cksum += d[3];
     d     += 4;
     dlen  -= 8;
   }

   while(dlen > 1)
   {
     cksum += *d++;
     dlen  -= 2;
   }

   if( dlen == 1 )
   {
    /* printf("new checksum odd byte-packet\n"); */
    *(unsigned char*)(&answer) = (*(unsigned char*)d);

    /* cksum += (uint16_t) (*(uint8_t*)d); */

     cksum += answer;
   }

   cksum  = (cksum >> 16) + (cksum & 0x0000ffff);
   cksum += (cksum >> 16);

   return (unsigned short)(~cksum);
}
/*
*  checksum tcp for IPv6.
*
*  h    - pseudo header - 12 bytes
*  d    - tcp hdr + payload
*  dlen - length of tcp hdr + payload in bytes
*
*/
static inline unsigned short in_chksum_tcp6(pseudoheader6 *ph,
    unsigned short * d, int dlen )
{
   uint16_t *h = (uint16_t *)ph;
   unsigned int cksum;
   unsigned short answer=0;

   /* PseudoHeader must have 36 bytes */
   cksum  = h[0];
   cksum += h[1];
   cksum += h[2];
   cksum += h[3];
   cksum += h[4];
   cksum += h[5];
   cksum += h[6];
   cksum += h[7];
   cksum += h[8];
   cksum += h[9];
   cksum += h[10];
   cksum += h[11];
   cksum += h[12];
   cksum += h[13];
   cksum += h[14];
   cksum += h[15];
   cksum += h[16];
   cksum += h[17];

   /* TCP hdr must have 20 hdr bytes */
   cksum += d[0];
   cksum += d[1];
   cksum += d[2];
   cksum += d[3];
   cksum += d[4];
   cksum += d[5];
   cksum += d[6];
   cksum += d[7];
   cksum += d[8];
   cksum += d[9];

   dlen  -= 20; /* bytes   */
   d     += 10; /* short's */

   while(dlen >=32)
   {
     cksum += d[0];
     cksum += d[1];
     cksum += d[2];
     cksum += d[3];
     cksum += d[4];
     cksum += d[5];
     cksum += d[6];
     cksum += d[7];
     cksum += d[8];
     cksum += d[9];
     cksum += d[10];
     cksum += d[11];
     cksum += d[12];
     cksum += d[13];
     cksum += d[14];
     cksum += d[15];
     d     += 16;
     dlen  -= 32;
   }

   while(dlen >=8)
   {
     cksum += d[0];
     cksum += d[1];
     cksum += d[2];
     cksum += d[3];
     d     += 4;
     dlen  -= 8;
   }

   while(dlen > 1)
   {
     cksum += *d++;
     dlen  -= 2;
   }

   if( dlen == 1 )
   {
    /* printf("new checksum odd byte-packet\n"); */
    *(unsigned char*)(&answer) = (*(unsigned char*)d);

    /* cksum += (uint16_t) (*(uint8_t*)d); */

     cksum += answer;
   }

   cksum  = (cksum >> 16) + (cksum & 0x0000ffff);
   cksum += (cksum >> 16);

   return (unsigned short)(~cksum);
}

#endif

//-------------------------------------------------------------------------
// api
//-------------------------------------------------------------------------

static Module* mod_ctor()
{
    return new TcpModule;
}

static void mod_dtor(Module* m)
{
    delete m;
}

/*
 * Static api functions.  there are NOT part of the TCPCodec class,
 * but provide global initializers and/or destructors to the class
 */

static void tcp_codec_ginit()
{
    SynToMulticastDstIp = sfip_var_from_string(
        "[232.0.0.0/8,233.0.0.0/8,239.0.0.0/8]");

    if( SynToMulticastDstIp == NULL )
        FatalError("Could not initialize SynToMulticastDstIp\n");

}

static void tcp_codec_gterm()
{
    if( SynToMulticastDstIp )
        sfvar_free(SynToMulticastDstIp);
}

static Codec* ctor(Module*)
{ return new TcpCodec(); }

static void dtor(Codec *cd)
{ delete cd; }

static const CodecApi tcp_api =
{
    {
        PT_CODEC,
        CD_TCP_NAME,
        CDAPI_PLUGIN_V0,
        0,
        mod_ctor,
        mod_dtor,
    },
    tcp_codec_ginit, // pinit
    tcp_codec_gterm, // pterm
    nullptr, // tinit
    nullptr, // tterm
    ctor, // ctor
    dtor, // dtor
};

const BaseApi* cd_tcp = &tcp_api.base;
