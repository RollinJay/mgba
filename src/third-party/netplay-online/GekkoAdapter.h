/* SPDX-License-Identifier: MIT
 * Part of netplay-online (https://github.com/RollinJay/Nakama-Matchmaker). */
#pragma once

struct GekkoNetAdapter;

namespace NetplayOnline {

class PeerLinks;

/* A GekkoNet network adapter that carries GekkoNet's packets as PeerLinks game packets.
 * A remote player's address is its Nakama user id. GekkoNet's adapter callbacks carry no
 * context, so one PeerLinks at a time can be attached; GekkoNet calls them from the
 * thread that updates the session, which must be the thread that owns the PeerLinks. */
GekkoNetAdapter* gekkoAdapter(PeerLinks*);
void gekkoAdapterDetach();

}
