# Rate Limits: SignalBox Platform Research

Research completed 2026-08-18 on category/game update rate limits and consequences for OBS plugin.

---

## TWITCH (Primary Target)

### Rate Limit Architecture

- **Bucket**: 800 points per minute, per client ID
- **Mechanism**: Token bucket. Once exhausted, endpoint returns HTTP 429
- **Reset**: Ratelimit-Reset header (Unix epoch timestamp) indicates when bucket refills
- **Headers returned**: `Ratelimit-Limit` (bucket size), `Ratelimit-Remaining` (points left), `Ratelimit-Reset` (Unix epoch)
- **Scope**: Per client ID (applies to all users of your OAuth app), not per-user-token

### PATCH /helix/channels (Modify Channel Information)

**Endpoint**: `PATCH https://api.helix.twitch.tv/helix/channels`

**Point Cost**: **Unconfirmed as 1 point** (exact cost not found in public documentation; Twitch docs state endpoint costs vary, but individual costs are not listed in publicly accessible API reference). [1]

**Required Scope**: `channel:manage:broadcast`

**Parameters**: Sets `game_id`, `title`, `language`, `broadcaster_language`, etc.

### Non-Rate-Limit Consequences of Frequent Category Changes

**Followers DO NOT receive notifications on category change** — only on "Go Live". There has been a feature request for Twitch to notify followers when a streamer changes category while live, but this is not implemented. [2]

**No documented impact on**: 
- Directory placement or discoverability (placement is driven by viewer count, not category change frequency)
- VOD fragmentation (category changes do not split VODs)
- Stream's raid/watch-party eligibility
- Visibility to viewers (category change is not shown in-stream, only visible via channel page)

**Conclusion**: Category changes are safe to perform frequently from a user-visibility perspective. The only constraint is the API rate limit itself.

### Terms of Service / Developer Agreement

No language found prohibiting frequent or automated category updates, provided rate limits are observed. [3]

---

## RESTREAM (Multi-Streaming Consideration)

### API Capability

Restream **UI-based** category management: Users can set category in Restream for multi-platform sync. [4]

**No documented REST API** for category/game updates found.

### Conflict Behavior

**CRITICAL**: If a streamer uses Restream and connects OBS through Restream's RTMP, updating the category directly via Twitch API (or Twitch web) will **conflict with Restream's stream metadata management**. 

According to Twitch developer forum discussion, Restream acts as a relay layer and manages its own stream setup configurations. When a streamer has configured a "Default RTMP" setup in Restream:
- Changes made directly on Twitch (or via direct API) may not sync back to Restream's UI
- Restream's next sync or configuration push could overwrite direct API changes
- The discrepancy is that titles/categories are "tied to each setup," so updates must happen on the same platform [5]

**Recommendation**: SignalBox should detect if the streamer is using Restream. If so:
1. Log a warning or inform the user of the conflict
2. Optionally disable auto-category if Restream is detected, OR
3. Make category changes via Restream's UI (if REST API becomes available), not direct Twitch API

---

## KICK (Secondary Platform)

### API Capability

Kick has a public API. Update stream title/category via `PATCH /public/v1/channels` with `stream_title` and `category_id` fields. [6]

### Rate Limits

**Unconfirmed**: No official rate limit documentation found for Kick's category endpoint. Kick's public API was recently released (2026 era), but rate limit specifics are not publicly documented. [6]

**Recommendation**: Treat as untested. Adopt conservative approach (e.g., 5–10 second minimum interval) until Kick publishes rate limit documentation.

---

## YOUTUBE LIVE (Secondary Platform)

### API Capability

YouTube Live streams are managed via YouTube Data API v3.

### Quota (Not Rate Limit)

- **10,000 units per day** (daily quota, not minute-based)
- No paid tier to exceed quota
- Quota resets daily at midnight Pacific Time
- Each API operation costs 1–1,600 units depending on endpoint [7]

### Rate Limits

Rate limits exist **on top of quota limits**. Per-second rate limits apply but specifics not documented publicly.

**Game/Category Specifics**: No category-specific rate limit or quota cost found. This is likely a low-cost operation (1–10 units), but unconfirmed. [7]

### Recommendation

YouTube is quota-bound (daily, not per-minute). A single category change likely costs < 10 units, so daily quota is not a practical constraint for reasonable auto-detection intervals.

---

## Practical Guidance: Recommended Minimum Interval

### Decision Factors

1. **Twitch is the active constraint**: 800 points/min bucket. Even if PATCH /helix/channels costs 1 point (likely), you could theoretically change 800 times per minute. But **operational good practice** discourages this.

2. **Existing plugin precedent**: Game Detector (FabioZumbi12) OBS plugin includes a configurable delay on category changes to "prevent Twitch rate limited errors and deny premature category changes," but specific recommended interval is not documented. [8]

3. **Real-world triggers**: Alt-tabbing, game crashes, launcher closing can trigger bursts of detection events within seconds. Plugin should debounce or throttle to avoid:
   - Unnecessary API calls
   - Noisy directory updates
   - Potential algorithmic flagging by Twitch (though undocumented)

4. **Non-rate-limit risk is low**: No discovery penalty, no follower spam, no VOD impact. Main risk is wasting API points on transient detections.

### Recommendation

**Minimum interval: 30–60 seconds** between category changes on the same platform.

**Rationale**:
- **30 seconds** is aggressive but safe: 2 changes/min × 60 min = 120 changes/hour = well under 800-point bucket
- **60 seconds** is conservative and operationally sensible: gives user a moment to see a category before auto-detect fires again
- Both well below any documented or suspected Twitch friction
- Covers Restream sync conflicts (30–60s gives Restream time to detect a change if streamer corrects it)

**Debounce strategy**: If detection fires multiple times within the interval window (e.g., rapid alt-tabs), hold the most recent game and batch into a single API call after the interval expires, rather than queuing multiple changes.

### Per-Platform Intervals

| Platform | Suggested Min Interval | Rationale |
|----------|------------------------|-----------|
| Twitch   | 30–60 sec              | 800 points/min limit, operational good practice |
| Restream | 60 sec (via Twitch API) | Avoid conflicts with Restream's own sync logic |
| Kick     | 60 sec                 | Unknown rate limits; conservative approach |
| YouTube  | 30 sec                 | Daily quota (not per-min); low operational impact |

---

## Sources

[1] Twitch Helix API Reference: https://dev.twitch.tv/docs/api/reference
[2] Twitch Feature Request – Notifications on Category Change: https://twitch.uservoice.com/forums/310228-account-management/suggestions/40350886-send-out-notifications-when-the-category-is-change
[3] Twitch API Concepts (Rate Limits): https://dev.twitch.tv/docs/api/guide
[4] Restream API Docs: https://developers.restream.io/
[5] Twitch Developer Forum – Issues with Restream.io: https://discuss.dev.twitch.com/t/issues-when-streamers-are-using-restream-io/15689
[6] KICK REST API Documentation: https://github.com/mattseabrook/KICK.com-Streaming-REST-API
[7] YouTube Data API v3 Quota & Limits: https://www.getphyllo.com/post/youtube-api-limits-how-to-calculate-api-usage-cost-and-fix-exceeded-api-quota
[8] Game Detector OBS Plugin (FabioZumbi12): https://github.com/FabioZumbi12/game-detector

---

## Key Unknowns & Recommendations for Future Work

1. **PATCH /helix/channels exact cost**: Contact Twitch developer support or reverse-engineer from rate limit headers during testing
2. **Kick rate limits**: Monitor Kick developer forum for public documentation; test conservatively in beta
3. **Restream REST API**: As of 2026-08, no documented REST endpoint for category updates exists. Watch for future Restream API updates
4. **Game Detector delay config**: Check FabioZumbi12's GitHub wiki or issues for exact recommended delay value
5. **YouTube game category cost**: Test in production to confirm unit cost, though quota is not a practical concern
