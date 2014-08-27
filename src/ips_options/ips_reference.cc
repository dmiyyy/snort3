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
// ips_reference.cc author Russ Combs <rucombs@cisco.com>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string>

#include "snort_types.h"
#include "detection/treenodes.h"
#include "snort_debug.h"
#include "snort.h"
#include "detection/detection_defines.h"
#include "framework/ips_option.h"
#include "framework/parameter.h"
#include "framework/module.h"

static const char* s_name = "reference";

//-------------------------------------------------------------------------
// module
//-------------------------------------------------------------------------

static const Parameter reference_params[] =
{
    { "~scheme", Parameter::PT_STRING, nullptr, nullptr,
      "reference scheme" },

    { "~id", Parameter::PT_STRING, nullptr, nullptr,
      "reference id" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

class ReferenceModule : public Module
{
public:
    ReferenceModule() : Module(s_name, reference_params) { };
    bool set(const char*, Value&, SnortConfig*);
    bool begin(const char*, int, SnortConfig*);

    std::string scheme;
    std::string id;
    SnortConfig* snort_config;
};

bool ReferenceModule::begin(const char*, int, SnortConfig* sc)
{
    scheme.clear();
    id.clear();
    snort_config = sc;
    return true;
}

bool ReferenceModule::set(const char*, Value& v, SnortConfig*)
{
    if ( v.is("~scheme") )
        scheme = v.get_string();

    else if ( v.is("~id") )
        id = v.get_string();

    else
        return false;

    return true;
}

//-------------------------------------------------------------------------
// api methods
//-------------------------------------------------------------------------

static Module* mod_ctor()
{
    return new ReferenceModule;
}

static void mod_dtor(Module* m)
{
    delete m;
}

static IpsOption* reference_ctor(Module* p, OptTreeNode* otn)
{
    ReferenceModule* m = (ReferenceModule*)p;
    AddReference(
        m->snort_config, &otn->sigInfo.refs,
        m->scheme.c_str(), m->id.c_str());
    return nullptr;
}

static const IpsApi reference_api =
{
    {
        PT_IPS_OPTION,
        s_name,
        IPSAPI_PLUGIN_V0,
        0,
        mod_ctor,
        mod_dtor
    },
    OPT_TYPE_META,
    0, PROTO_BIT__NONE,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    reference_ctor,
    nullptr,
    nullptr
};

const BaseApi* ips_reference = &reference_api.base;
