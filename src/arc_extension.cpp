#include "arcdps_api.h"
#include "boons.h"
#include "shared_state.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace {

struct TrackedStack {
    std::uint64_t expires_at_ms = 0;
};

using StackMap = std::unordered_map<std::uint32_t, TrackedStack>;
std::array<StackMap, boonpanel::kBoonCount> g_tracked;
std::mutex g_tracker_mutex;
boonpanel::SharedState g_shared;
bool g_feed_seen = false;
std::uint32_t g_synthetic_id = 1;

std::uint32_t TrackId(const ArcCbtEvent& ev) noexcept {
    std::uint32_t id = 0;
    std::memcpy(&id, &ev.pad61, sizeof(id));
    return id;
}

bool IsSelf(const ArcAgent* agent) noexcept {
    return agent != nullptr && agent->self != 0;
}

void ClearExpired(std::uint64_t now) {
    for (auto& boon_stacks : g_tracked) {
        for (auto it = boon_stacks.begin(); it != boon_stacks.end();) {
            if (it->second.expires_at_ms <= now) {
                it = boon_stacks.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void PublishSnapshot(std::uint64_t now) {
    std::array<std::uint32_t, boonpanel::kBoonCount> track_counts{};
    std::array<std::uint64_t,
        boonpanel::kBoonCount * boonpanel::kMaxTrackedStacks> expiries{};

    ClearExpired(now);
    for (std::size_t i = 0; i < boonpanel::kBoonCount; ++i) {
        std::size_t slot = 0;
        for (const auto& item : g_tracked[i]) {
            if (slot >= boonpanel::kMaxTrackedStacks) {
                break;
            }
            expiries[i * boonpanel::kMaxTrackedStacks + slot] = item.second.expires_at_ms;
            ++slot;
        }
        track_counts[i] = static_cast<std::uint32_t>(slot);
    }

    g_shared.Publish(track_counts.data(), expiries.data(), g_feed_seen);
}

void OnCombatLocal(ArcCbtEvent* ev, ArcAgent* src, ArcAgent* dst,
                   char* /*skillname*/, std::uint64_t /*id*/, std::uint64_t /*revision*/) {
    if (!ev) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_tracker_mutex);
    const std::uint64_t now = boonpanel::TickNowMs();

    if (ev->is_statechange == CBTS_MAPCHANGE) {
        for (auto& boon_stacks : g_tracked) {
            boon_stacks.clear();
        }
        g_feed_seen = false;
        PublishSnapshot(now);
        return;
    }

    const int index = boonpanel::IndexForSkillId(ev->skillid);
    if (index < 0) {
        return;
    }

    auto& stacks = g_tracked[static_cast<std::size_t>(index)];
    const std::uint32_t track_id = TrackId(*ev);

    switch (ev->is_statechange) {
    case CBTS_BUFFAPPLY: {
        if (!IsSelf(dst)) {
            return;
        }
        const std::int32_t duration = ev->value;
        if (duration <= 0) {
            return;
        }
        // A zero track ID is not safely correlatable with later change/remove events.
        // Use a generated key so the visible boon duration still appears; the next
        // remove-all or expiry will clear it.
        const std::uint32_t key = track_id != 0 ? track_id : (0x80000000u | g_synthetic_id++);
        stacks[key] = { now + static_cast<std::uint32_t>(duration) };
        g_feed_seen = true;
        break;
    }
    case CBTS_BUFFINITIAL: {
        if (!IsSelf(dst)) {
            return;
        }
        // For BUFFINITIAL, value follows BUFFAPPLY; buff_dmg carries the
        // original stack duration and is only a fallback if value is absent.
        const std::int32_t duration = ev->value > 0 ? ev->value : ev->buff_dmg;
        if (duration <= 0) {
            return;
        }
        const std::uint32_t key = track_id != 0 ? track_id : (0x80000000u | g_synthetic_id++);
        stacks[key] = { now + static_cast<std::uint32_t>(duration) };
        g_feed_seen = true;
        break;
    }
    case CBTS_BUFFACTIVE: {
        if (!IsSelf(src)) {
            return;
        }
        // BUFFACTIVE stores the trackable id in dst_agent, not in pad61.
        const std::uint32_t active_track_id = static_cast<std::uint32_t>(ev->dst_agent);
        if (active_track_id != 0 && ev->value > 0) {
            stacks[active_track_id] = { now + static_cast<std::uint32_t>(ev->value) };
        }
        g_feed_seen = true;
        break;
    }
    case CBTS_BUFFDEACTIVE: {
        if (!IsSelf(src)) {
            return;
        }
        if (track_id != 0) {
            if (ev->value <= 0) {
                stacks.erase(track_id);
            } else {
                stacks[track_id] = { now + static_cast<std::uint32_t>(ev->value) };
            }
        }
        g_feed_seen = true;
        break;
    }
    case CBTS_BUFFCHANGE: {
        // BUFFCHANGE's dst_agent is the agent whose existing stack changed.
        // Do not count buffs this player applies to someone else.
        if (!IsSelf(dst)) {
            return;
        }
        if (track_id != 0) {
            if (ev->overstack_value == 0) {
                stacks.erase(track_id);
            } else {
                stacks[track_id] = { now + ev->overstack_value };
            }
        }
        g_feed_seen = true;
        break;
    }
    case CBTS_BUFFREMOVE_SINGLE: {
        if (!IsSelf(src)) {
            return;
        }
        if (track_id != 0) {
            stacks.erase(track_id);
        }
        g_feed_seen = true;
        break;
    }
    case CBTS_BUFFREMOVE_ALL: {
        if (!IsSelf(src)) {
            return;
        }
        stacks.clear();
        g_feed_seen = true;
        break;
    }
    default:
        return;
    }

    PublishSnapshot(now);
}

ArcDpsExports g_arc_exports{};

ArcDpsExports* ArcModInit() {
    g_shared.Open();
    {
        std::lock_guard<std::mutex> lock(g_tracker_mutex);
        for (auto& boon_stacks : g_tracked) {
            boon_stacks.clear();
        }
        g_feed_seen = false;
        PublishSnapshot(boonpanel::TickNowMs());
    }

    g_arc_exports = {};
    g_arc_exports.size = sizeof(ArcDpsExports);
    g_arc_exports.sig = 0x42504E31u; // "BPN1" - choose a unique ID if you fork this project.
    g_arc_exports.imguivers = 19270; // ArcDPS currently uses ImGui 1.92.7; no Arc UI callback is used.
    g_arc_exports.out_name = "BoonPanel data bridge";
    g_arc_exports.out_build = "0.1.0";
    g_arc_exports.combat_local = reinterpret_cast<void*>(static_cast<ArcCombatLocal>(&OnCombatLocal));
    return &g_arc_exports;
}

void ArcModRelease() {
    {
        std::lock_guard<std::mutex> lock(g_tracker_mutex);
        for (auto& boon_stacks : g_tracked) {
            boon_stacks.clear();
        }
        g_feed_seen = false;
        PublishSnapshot(boonpanel::TickNowMs());
    }
    g_shared.Close();
}

} // namespace

extern "C" __declspec(dllexport) void* get_init_addr(
    char* /*arcversionstr*/, void* /*imguicontext*/, void* /*id3dptr*/,
    HANDLE /*arcdll*/, void* /*mallocfn*/, void* /*freefn*/, std::uint32_t /*imguiversion*/) {
    return reinterpret_cast<void*>(&ArcModInit);
}

extern "C" __declspec(dllexport) void* get_release_addr(std::uint32_t /*reason*/) {
    return reinterpret_cast<void*>(&ArcModRelease);
}
