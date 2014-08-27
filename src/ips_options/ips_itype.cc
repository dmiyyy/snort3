/*
** Copyright (C) 2014 Cisco and/or its affiliates. All rights reserved.
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
// ips_itype.cc author Russ Combs <rucombs@cisco.com>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "snort_types.h"
#include "protocols/packet.h"
#include "snort_debug.h"
#include "sfhashfcn.h"
#include "snort.h"
#include "profiler.h"
#include "sfhashfcn.h"
#include "detection/detection_defines.h"
#include "framework/ips_option.h"
#include "framework/parameter.h"
#include "framework/module.h"
#include "framework/range.h"

static const char* s_name = "itype";

static THREAD_LOCAL ProfileStats icmpTypePerfStats;

class IcmpTypeOption : public IpsOption
{
public:
    IcmpTypeOption(const RangeCheck& c) :
        IpsOption(s_name)
    { config = c; };

    uint32_t hash() const;
    bool operator==(const IpsOption&) const;

    int eval(Cursor&, Packet*);

private:
    RangeCheck config;
};

//-------------------------------------------------------------------------
// class methods
//-------------------------------------------------------------------------

uint32_t IcmpTypeOption::hash() const
{
    uint32_t a,b,c;

    a = config.op;
    b = config.min;
    c = config.max;

    mix(a,b,c);
    mix_str(a,b,c,get_name());
    final(a,b,c);

    return c;
}

bool IcmpTypeOption::operator==(const IpsOption& ips) const
{
    if ( strcmp(get_name(), ips.get_name()) )
        return false;

    IcmpTypeOption& rhs = (IcmpTypeOption&)ips;
    return ( config == rhs.config );
}

int IcmpTypeOption::eval(Cursor&, Packet *p)
{
    int rval = DETECTION_OPTION_NO_MATCH;
    PROFILE_VARS;

    /* return 0  if we don't have an icmp header */
    if(!p->icmph)
        return rval;

    MODULE_PROFILE_START(icmpTypePerfStats);

    if ( config.eval(p->icmph->type) )
        rval = DETECTION_OPTION_MATCH;

    MODULE_PROFILE_END(icmpTypePerfStats);
    return rval;
}

//-------------------------------------------------------------------------
// module
//-------------------------------------------------------------------------

static const Parameter itype_params[] =
{
    { "~range", Parameter::PT_STRING, nullptr, nullptr,
      "check if packet payload size is min<>max | <max | >min" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

class ItypeModule : public Module
{
public:
    ItypeModule() : Module(s_name, itype_params) { };

    bool begin(const char*, int, SnortConfig*);
    bool set(const char*, Value&, SnortConfig*);

    ProfileStats* get_profile() const
    { return &icmpTypePerfStats; };

    RangeCheck data;
};

bool ItypeModule::begin(const char*, int, SnortConfig*)
{
    data.init();
    return true;
}

bool ItypeModule::set(const char*, Value& v, SnortConfig*)
{
    if ( !v.is("~range") )
        return false;

    return data.parse(v.get_string());
}

//-------------------------------------------------------------------------
// api methods
//-------------------------------------------------------------------------

static Module* mod_ctor()
{
    return new ItypeModule;
}

static void mod_dtor(Module* m)
{
    delete m;
}

static IpsOption* itype_ctor(Module* p, OptTreeNode*)
{
    ItypeModule* m = (ItypeModule*)p;
    return new IcmpTypeOption(m->data);
}

static void itype_dtor(IpsOption* p)
{
    delete p;
}

static const IpsApi itype_api =
{
    {
        PT_IPS_OPTION,
        s_name,
        IPSAPI_PLUGIN_V0,
        0,
        mod_ctor,
        mod_dtor
    },
    OPT_TYPE_DETECTION,
    1, PROTO_BIT__ICMP,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    itype_ctor,
    itype_dtor,
    nullptr
};

#ifdef BUILDING_SO
SO_PUBLIC const BaseApi* snort_plugins[] =
{
    &itype_api.base,
    nullptr
};
#else
const BaseApi* ips_itype = &itype_api.base;
#endif

