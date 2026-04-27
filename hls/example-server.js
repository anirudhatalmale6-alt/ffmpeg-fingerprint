'use strict';

/**
 * Example: HLS Fingerprint Integration with Express
 *
 * Shows how to wire the HLSFingerprint module into an Express server
 * that already serves HLS streams via Xtream Codes panel.
 *
 * Adapt the route paths and auth logic to match your panel's URL scheme.
 */

const express = require('express');
const http = require('http');
const HLSFingerprint = require('./hls-fingerprint');

const app = express();

const fp = new HLSFingerprint({
    segmentDuration: 4,
    playlistDepth: 10,
    language: 'eng'
});

// ------------------------------------------------------------------
// 1. Show / hide fingerprint (call from your panel or Python trigger)
// ------------------------------------------------------------------

// POST /api/fingerprint/show
// Body: { "session": "TOKEN", "text": "USERNAME_123", "duration": 0 }
app.post('/api/fingerprint/show', express.json(), (req, res) => {
    const { session, text, duration } = req.body;
    if (!session || !text) {
        return res.status(400).json({ error: 'session and text required' });
    }
    fp.show(session, text, duration || 0);
    res.json({ ok: true, session, text });
});

// POST /api/fingerprint/hide
// Body: { "session": "TOKEN" }
app.post('/api/fingerprint/hide', express.json(), (req, res) => {
    const { session } = req.body;
    if (!session) return res.status(400).json({ error: 'session required' });
    fp.hide(session);
    res.json({ ok: true, session, active: false });
});

// GET /api/fingerprint/status?session=TOKEN
app.get('/api/fingerprint/status', (req, res) => {
    const status = fp.status(req.query.session);
    if (!status) return res.status(404).json({ error: 'session not found' });
    res.json(status);
});

// ------------------------------------------------------------------
// 2. Master playlist — inject subtitle track per viewer
// ------------------------------------------------------------------

// Your panel already serves:  /live/{user}/{pass}/{channelId}.m3u8
// Intercept that request, fetch original, inject fingerprint subtitle track.

app.get('/live/:user/:pass/:channelId.m3u8', (req, res) => {
    const { user, pass, channelId } = req.params;
    const sessionToken = `${user}_${channelId}`;

    // Fetch the original master playlist from upstream (your FFmpeg / panel)
    const upstreamUrl = `http://127.0.0.1:8080/live/${user}/${pass}/${channelId}.m3u8`;

    httpGet(upstreamUrl, (err, body) => {
        if (err) return res.status(502).send('upstream error');

        const modified = fp.getMasterPlaylist(body, sessionToken, channelId, '');
        res.set('Content-Type', 'application/vnd.apple.mpegurl');
        res.send(modified);
    });
});

// ------------------------------------------------------------------
// 3. Subtitle media playlist (subs.m3u8)
// ------------------------------------------------------------------

app.get('/live/hls/fp/:channelId/subs.m3u8', (req, res) => {
    const { channelId } = req.params;
    const sessionToken = req.query.session;

    if (!sessionToken) return res.status(400).send('missing session');

    // Get media sequence from the current video playlist.
    // In production, cache this or read from your segment index.
    const videoPlaylistUrl = `http://127.0.0.1:8080/live/hls/${channelId}/index.m3u8`;

    httpGet(videoPlaylistUrl, (err, body) => {
        let mediaSeq = 0;
        if (!err && body) {
            mediaSeq = fp.parseMediaSequence(body);
        }

        const playlist = fp.getSubPlaylist(sessionToken, channelId, mediaSeq);
        res.set('Content-Type', 'application/vnd.apple.mpegurl');
        res.send(playlist);
    });
});

// ------------------------------------------------------------------
// 4. Individual VTT segments
// ------------------------------------------------------------------

app.get('/live/hls/fp/:channelId/sub_:segNum.vtt', (req, res) => {
    const sessionToken = req.query.session;
    const segNum = parseInt(req.params.segNum, 10);

    if (!sessionToken) return res.status(400).send('missing session');

    const vtt = fp.getVTT(sessionToken, segNum);
    res.set('Content-Type', 'text/vtt');
    res.send(vtt);
});

// ------------------------------------------------------------------
// Helper: simple HTTP GET
// ------------------------------------------------------------------

function httpGet(url, cb) {
    http.get(url, (resp) => {
        let data = '';
        resp.on('data', (chunk) => { data += chunk; });
        resp.on('end', () => cb(null, data));
    }).on('error', (err) => cb(err, null));
}

// ------------------------------------------------------------------
// Start
// ------------------------------------------------------------------

const PORT = process.env.PORT || 3000;
app.listen(PORT, () => {
    console.log(`HLS fingerprint server on port ${PORT}`);
});
