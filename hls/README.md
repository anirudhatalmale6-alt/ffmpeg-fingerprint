# HLS WebVTT Fingerprint Module

Per-viewer fingerprint text overlay via HLS WebVTT subtitles.
Works alongside the TS-level fingerprint — use this for players that support
HLS subtitle tracks but ignore CEA-608/708 closed captions.

## How it works

1. Master playlist gets an injected `#EXT-X-MEDIA:TYPE=SUBTITLES` line with
   `FORCED=YES, DEFAULT=YES, AUTOSELECT=YES` — forces the player to render it.
2. Each viewer gets a unique subtitle playlist (`subs.m3u8`) keyed by session token.
3. Individual `.vtt` segments contain the fingerprint text with randomised position
   (changes per segment, deterministic per session — anti-tamper).

## Files

- `hls-fingerprint.js` — Core module (no dependencies, pure Node.js)
- `example-server.js` — Express integration example

## Quick start

```bash
npm install express
node example-server.js
```

## API

### Module

```js
const HLSFingerprint = require('./hls-fingerprint');
const fp = new HLSFingerprint({ segmentDuration: 4, playlistDepth: 10 });
```

### Control

```js
fp.show(sessionToken, 'USERNAME_123');       // activate
fp.show(sessionToken, 'USERNAME_123', 300);  // auto-hide after 5 min
fp.hide(sessionToken);                        // deactivate
fp.remove(sessionToken);                      // delete session
fp.status(sessionToken);                      // { text, active }
```

### HLS routes

```js
fp.getMasterPlaylist(originalM3U8, sessionToken, channelId, baseUrl);
fp.getSubPlaylist(sessionToken, channelId, mediaSequence);
fp.getVTT(sessionToken, segmentNum);
fp.parseMediaSequence(videoPlaylist);  // utility
```

### REST endpoints (example-server.js)

```
POST /api/fingerprint/show   { session, text, duration }
POST /api/fingerprint/hide   { session }
GET  /api/fingerprint/status ?session=TOKEN

GET  /live/:user/:pass/:channelId.m3u8     — modified master playlist
GET  /live/hls/fp/:channelId/subs.m3u8     — subtitle media playlist
GET  /live/hls/fp/:channelId/sub_:num.vtt  — individual VTT segment
```

## Integration with your panel

Your Express server intercepts the master playlist request, calls
`fp.getMasterPlaylist()` to inject the subtitle track, then serves it.
When a viewer connects, call `fp.show(sessionToken, username)` to activate
their fingerprint. The subtitle playlist and VTT segments are generated
on-the-fly per request.

The session token should be unique per viewer — use their username, MAC,
or a combination. The text parameter is what appears on screen.
