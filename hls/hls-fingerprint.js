'use strict';

/**
 * HLS Fingerprint Module
 *
 * Generates per-viewer WebVTT subtitle tracks for HLS streams.
 * Each viewer gets unique fingerprint text with random positioning (anti-tamper).
 * Designed to plug into an Express server serving HLS content.
 *
 * Usage:
 *   const HLSFingerprint = require('./hls-fingerprint');
 *   const fp = new HLSFingerprint({ segmentDuration: 4, playlistDepth: 10 });
 *
 *   // Show fingerprint for a session
 *   fp.show(sessionToken, 'USERNAME_123');
 *
 *   // Generate responses for Express routes
 *   fp.getMasterPlaylist(originalM3U8, sessionToken, channelId);
 *   fp.getSubPlaylist(sessionToken, channelId, mediaSequence);
 *   fp.getVTT(sessionToken, segmentNum);
 *
 *   // Hide fingerprint
 *   fp.hide(sessionToken);
 */

class HLSFingerprint {
    constructor(options = {}) {
        this.segmentDuration = options.segmentDuration || 4;
        this.playlistDepth = options.playlistDepth || 10;
        this.language = options.language || 'eng';
        this.sessions = new Map();
        this.cleanupInterval = setInterval(() => this._cleanup(), 60000);
    }

    /**
     * Activate fingerprint for a session.
     * @param {string} sessionToken - Unique viewer session token
     * @param {string} text - Fingerprint text (username, MAC, etc.)
     * @param {number} [durationSec=0] - Auto-hide after N seconds (0 = permanent until hide())
     */
    show(sessionToken, text, durationSec = 0) {
        this.sessions.set(sessionToken, {
            text: text,
            active: true,
            startTime: Date.now(),
            expireTime: durationSec > 0 ? Date.now() + durationSec * 1000 : 0,
            seed: Math.floor(Math.random() * 1000000)
        });
    }

    /**
     * Deactivate fingerprint for a session.
     * @param {string} sessionToken
     */
    hide(sessionToken) {
        const session = this.sessions.get(sessionToken);
        if (session) {
            session.active = false;
        }
    }

    /**
     * Remove session entirely.
     * @param {string} sessionToken
     */
    remove(sessionToken) {
        this.sessions.delete(sessionToken);
    }

    /**
     * Get session state.
     * @param {string} sessionToken
     * @returns {object|null}
     */
    status(sessionToken) {
        const session = this.sessions.get(sessionToken);
        if (!session) return null;
        if (session.expireTime > 0 && Date.now() > session.expireTime) {
            session.active = false;
        }
        return { text: session.text, active: session.active };
    }

    /**
     * Inject subtitle track into an HLS master playlist.
     * @param {string} originalM3U8 - Original master playlist content
     * @param {string} sessionToken - Viewer session token
     * @param {string} channelId - Channel identifier
     * @param {string} [baseUrl=''] - Base URL prefix for subtitle playlist
     * @returns {string} Modified master playlist
     */
    getMasterPlaylist(originalM3U8, sessionToken, channelId, baseUrl = '') {
        const subsUrl = `${baseUrl}/live/hls/fp/${channelId}/subs.m3u8?session=${encodeURIComponent(sessionToken)}`;

        const subtitleLine =
            `#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID="fp",NAME="fingerprint",` +
            `DEFAULT=YES,AUTOSELECT=YES,FORCED=YES,` +
            `LANGUAGE="${this.language}",URI="${subsUrl}"`;

        const lines = originalM3U8.split('\n');
        const result = [];
        let subtitleAdded = false;

        for (const line of lines) {
            if (line.startsWith('#EXT-X-STREAM-INF:')) {
                if (!subtitleAdded) {
                    result.push(subtitleLine);
                    subtitleAdded = true;
                }
                if (!line.includes('SUBTITLES=')) {
                    result.push(line.replace(/\r?$/, ',SUBTITLES="fp"'));
                } else {
                    result.push(line);
                }
            } else {
                result.push(line);
            }
        }

        if (!subtitleAdded) {
            result.splice(1, 0, subtitleLine);
        }

        return result.join('\n');
    }

    /**
     * Generate the subtitle media playlist (subs.m3u8).
     * Must sync with the video playlist's media sequence.
     * @param {string} sessionToken
     * @param {string} channelId
     * @param {number} mediaSequence - Current EXT-X-MEDIA-SEQUENCE from the video playlist
     * @returns {string} Subtitle media playlist
     */
    getSubPlaylist(sessionToken, channelId, mediaSequence) {
        const lines = [
            '#EXTM3U',
            '#EXT-X-VERSION:3',
            `#EXT-X-TARGETDURATION:${this.segmentDuration}`,
            `#EXT-X-MEDIA-SEQUENCE:${mediaSequence}`
        ];

        for (let i = 0; i < this.playlistDepth; i++) {
            const segNum = mediaSequence + i;
            lines.push(`#EXTINF:${this.segmentDuration}.000,`);
            lines.push(`sub_${segNum}.vtt?session=${encodeURIComponent(sessionToken)}`);
        }

        return lines.join('\n') + '\n';
    }

    /**
     * Generate a single WebVTT segment with fingerprint cue.
     * @param {string} sessionToken
     * @param {number} segmentNum - Segment number (matches media sequence + index)
     * @returns {string} WebVTT content
     */
    getVTT(sessionToken, segmentNum) {
        const session = this.sessions.get(sessionToken);

        if (!session || !session.active) {
            return 'WEBVTT\n\n';
        }

        if (session.expireTime > 0 && Date.now() > session.expireTime) {
            session.active = false;
            return 'WEBVTT\n\n';
        }

        const text = session.text;
        const dur = this.segmentDuration;

        const pos = this._randomPosition(session.seed, segmentNum);

        const startTime = this._formatTime(0);
        const endTime = this._formatTime(dur);

        return [
            'WEBVTT',
            '',
            `${startTime} --> ${endTime} position:${pos.x}% line:${pos.y}%`,
            text,
            ''
        ].join('\n');
    }

    /**
     * Parse media sequence from a video playlist.
     * Utility to sync subtitle playlist with video.
     * @param {string} videoPlaylist - Video media playlist content
     * @returns {number} Media sequence number
     */
    parseMediaSequence(videoPlaylist) {
        const match = videoPlaylist.match(/#EXT-X-MEDIA-SEQUENCE:(\d+)/);
        return match ? parseInt(match[1], 10) : 0;
    }

    /**
     * Generate random position for anti-tamper.
     * Position changes per segment but is deterministic per session+segment
     * so repeated requests for the same segment return the same position.
     */
    _randomPosition(seed, segmentNum) {
        let h = (seed * 2654435761 + segmentNum * 1103515245) >>> 0;
        const x = 5 + (h % 61);
        h = (h * 1103515245 + 12345) >>> 0;
        const y = 5 + (h % 71);
        return { x, y };
    }

    _formatTime(seconds) {
        const h = Math.floor(seconds / 3600);
        const m = Math.floor((seconds % 3600) / 60);
        const s = Math.floor(seconds % 60);
        const ms = Math.round((seconds % 1) * 1000);
        return `${String(h).padStart(2, '0')}:${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}.${String(ms).padStart(3, '0')}`;
    }

    _cleanup() {
        const now = Date.now();
        const staleThreshold = 3600000;
        for (const [token, session] of this.sessions) {
            if (!session.active && now - session.startTime > staleThreshold) {
                this.sessions.delete(token);
            }
        }
    }

    destroy() {
        clearInterval(this.cleanupInterval);
        this.sessions.clear();
    }
}

module.exports = HLSFingerprint;
