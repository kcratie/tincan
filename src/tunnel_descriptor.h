/*
 * EdgeVPNio
 * Copyright 2023, University of Florida
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#ifndef TINCAN_TUNNEL_DESCRIPTOR_H_
#define TINCAN_TUNNEL_DESCRIPTOR_H_
#include "tincan_base.h"
#include "turn_descriptor.h"
namespace tincan
{
    struct TunnelDesc
    {
        TunnelDesc(const Json::Value &desc) : uid{desc[TincanControl::TunnelId].asString()},
                                              node_id{desc[TincanControl::NodeId].asString()}
        {

            Json::Value stuns = desc["StunServers"];
            for (Json::Value::ArrayIndex i = 0; i < stuns.size(); ++i)
            {
                stun_servers.push_back(stuns[i].asString());
            }

            Json::Value turns = desc["TurnServers"];
            for (Json::Value::ArrayIndex i = 0; i < turns.size(); ++i)
            {
                TurnDescriptor turn_desc(
                    turns[i]["Address"].asString(),
                    turns[i]["User"].asString(),
                    turns[i]["Password"].asString());
                turn_descs.push_back(turn_desc);
            }
        }
        const string uid;
        const string node_id;
        vector<string> stun_servers;
        vector<TurnDescriptor> turn_descs;
    };
} // namespace tincan
#endif // TINCAN_TUNNEL_DESCRIPTOR_H_
