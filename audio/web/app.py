#!/usr/bin/env python3
"""
Audio Watermark Detection Web GUI

Upload a recording (video/audio), the system analyzes the audio track
and identifies which user/MAC the recording belongs to.

Usage:
    pip install flask numpy
    python3 app.py
    # Open http://localhost:5000

Environment variables:
    USERS_FILE  — path to user list file (one username/MAC per line)
    PORT        — server port (default: 5000)
    UPLOAD_DIR  — upload directory (default: /tmp/watermark_uploads)
"""

import os
import sys
import json
import hashlib
import subprocess
import tempfile
import time
from datetime import datetime

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from flask import Flask, request, jsonify, render_template_string, send_from_directory

app = Flask(__name__)

UPLOAD_DIR = os.environ.get('UPLOAD_DIR', '/tmp/watermark_uploads')
USERS_FILE = os.environ.get('USERS_FILE', '')
TONE_A_HZ = float(os.environ.get('TONE_A_HZ', '18500'))
TONE_B_HZ = float(os.environ.get('TONE_B_HZ', '19500'))
SEGMENT_DURATION = float(os.environ.get('SEGMENT_DURATION', '4.0'))
NUM_BITS = int(os.environ.get('NUM_BITS', '20'))
SAMPLE_RATE = 48000
DETECTION_BANDWIDTH = 200
MAX_UPLOAD_MB = 500

os.makedirs(UPLOAD_DIR, exist_ok=True)

analysis_results = {}


def user_to_pattern(username, num_bits=NUM_BITS):
    h = hashlib.sha256(username.encode('utf-8')).digest()
    bits = []
    for i in range(num_bits):
        byte_idx = i // 8
        bit_idx = i % 8
        if byte_idx < len(h):
            bits.append((h[byte_idx] >> bit_idx) & 1)
        else:
            bits.append(0)
    return bits


def load_users():
    users = []
    if USERS_FILE and os.path.exists(USERS_FILE):
        with open(USERS_FILE, 'r') as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith('#'):
                    users.append(line)
    custom_file = os.path.join(UPLOAD_DIR, 'users.txt')
    if os.path.exists(custom_file):
        with open(custom_file, 'r') as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith('#'):
                    if line not in users:
                        users.append(line)
    return users


def extract_audio(input_file):
    tmp = tempfile.NamedTemporaryFile(suffix='.raw', delete=False)
    tmp_path = tmp.name
    tmp.close()

    cmd = [
        'ffmpeg', '-hide_banner', '-loglevel', 'error',
        '-i', input_file,
        '-ac', '1', '-ar', str(SAMPLE_RATE),
        '-f', 'f32le', '-y', tmp_path
    ]

    try:
        import numpy as np
    except ImportError:
        raise RuntimeError("numpy not installed")

    subprocess.run(cmd, check=True, capture_output=True)
    data = np.fromfile(tmp_path, dtype=np.float32)
    os.unlink(tmp_path)
    return data


def detect_tone_fft(samples, target_freq, sample_rate):
    import numpy as np
    if len(samples) < 1024:
        return 0.0

    window = np.hanning(len(samples))
    windowed = samples * window
    fft_result = np.fft.rfft(windowed)
    fft_magnitude = np.abs(fft_result)
    freqs = np.fft.rfftfreq(len(samples), 1.0 / sample_rate)

    target_mask = (freqs >= target_freq - DETECTION_BANDWIDTH / 2) & \
                  (freqs <= target_freq + DETECTION_BANDWIDTH / 2)
    if not np.any(target_mask):
        return 0.0

    target_power = np.max(fft_magnitude[target_mask])
    noise_mask = (freqs >= target_freq - DETECTION_BANDWIDTH * 3) & \
                 (freqs <= target_freq + DETECTION_BANDWIDTH * 3) & ~target_mask
    noise_floor = np.median(fft_magnitude[noise_mask]) if np.any(noise_mask) else np.median(fft_magnitude)

    if noise_floor < 1e-10:
        return 0.0
    return target_power / noise_floor


def analyze_file(filepath):
    import numpy as np

    audio_data = extract_audio(filepath)
    duration = len(audio_data) / SAMPLE_RATE
    samples_per_segment = int(SEGMENT_DURATION * SAMPLE_RATE)
    total_segments = len(audio_data) // samples_per_segment

    segments = []
    pattern_bits = []

    for i in range(min(total_segments, NUM_BITS * 3)):
        start = i * samples_per_segment
        end = start + samples_per_segment
        segment = audio_data[start:end]

        strength_a = detect_tone_fft(segment, TONE_A_HZ, SAMPLE_RATE)
        strength_b = detect_tone_fft(segment, TONE_B_HZ, SAMPLE_RATE)

        if strength_a > 2.0 or strength_b > 2.0:
            bit = 0 if strength_a > strength_b else 1
            confidence = max(strength_a, strength_b) / (min(strength_a, strength_b) + 0.001)
        else:
            bit = -1
            confidence = 0.0

        segments.append({
            'index': i,
            'tone_a': round(float(strength_a), 2),
            'tone_b': round(float(strength_b), 2),
            'bit': bit,
            'confidence': round(float(confidence), 2)
        })

        if i < NUM_BITS:
            pattern_bits.append(bit)

    pattern_str = ''.join(str(b) if b >= 0 else '?' for b in pattern_bits)
    detected_count = sum(1 for b in pattern_bits if b >= 0)

    users = load_users()
    matches = []

    for username in users:
        expected = user_to_pattern(username, len(pattern_bits))
        match_count = 0
        total = 0
        for d, e in zip(pattern_bits, expected):
            if d >= 0:
                total += 1
                if d == e:
                    match_count += 1
        if total > 0:
            score = match_count / total
            matches.append({
                'username': username,
                'score': round(score * 100, 1),
                'matched_bits': match_count,
                'total_bits': total
            })

    matches.sort(key=lambda x: x['score'], reverse=True)

    return {
        'duration': round(duration, 1),
        'total_segments': total_segments,
        'analyzed_segments': len(segments),
        'pattern': pattern_str,
        'detected_bits': detected_count,
        'total_bits': len(pattern_bits),
        'segments': segments,
        'matches': matches[:20],
        'best_match': matches[0] if matches else None,
        'users_checked': len(users),
        'watermark_detected': detected_count > 0
    }


HTML_TEMPLATE = '''<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Audio Watermark Detection</title>
<style>
* { margin: 0; padding: 0; box-sizing: border-box; }
body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; background: #0f1117; color: #e1e4e8; min-height: 100vh; }
.container { max-width: 960px; margin: 0 auto; padding: 24px; }
h1 { font-size: 24px; font-weight: 600; margin-bottom: 8px; color: #fff; }
.subtitle { color: #8b949e; margin-bottom: 32px; font-size: 14px; }
.card { background: #161b22; border: 1px solid #30363d; border-radius: 8px; padding: 24px; margin-bottom: 20px; }
.card h2 { font-size: 16px; font-weight: 600; margin-bottom: 16px; color: #c9d1d9; }
.upload-zone { border: 2px dashed #30363d; border-radius: 8px; padding: 48px 24px; text-align: center; cursor: pointer; transition: all 0.2s; }
.upload-zone:hover, .upload-zone.dragover { border-color: #58a6ff; background: rgba(88,166,255,0.05); }
.upload-zone input { display: none; }
.upload-zone p { color: #8b949e; margin-top: 8px; font-size: 14px; }
.upload-zone .icon { font-size: 48px; margin-bottom: 12px; }
.btn { display: inline-block; padding: 8px 20px; background: #238636; color: #fff; border: none; border-radius: 6px; font-size: 14px; font-weight: 500; cursor: pointer; transition: background 0.15s; }
.btn:hover { background: #2ea043; }
.btn:disabled { background: #21262d; color: #484f58; cursor: not-allowed; }
.btn-blue { background: #1f6feb; }
.btn-blue:hover { background: #388bfd; }
.progress { display: none; margin-top: 16px; }
.progress-bar { height: 4px; background: #21262d; border-radius: 2px; overflow: hidden; }
.progress-fill { height: 100%; background: #58a6ff; width: 0%; transition: width 0.3s; }
.progress-text { color: #8b949e; font-size: 13px; margin-top: 8px; }
.result { display: none; }
.match-box { padding: 20px; border-radius: 8px; margin-bottom: 16px; }
.match-found { background: rgba(35,134,54,0.15); border: 1px solid #238636; }
.match-none { background: rgba(218,55,60,0.1); border: 1px solid #da373c; }
.match-partial { background: rgba(210,153,34,0.1); border: 1px solid #d29922; }
.match-name { font-size: 24px; font-weight: 700; color: #fff; }
.match-score { font-size: 14px; color: #8b949e; margin-top: 4px; }
.info-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(140px, 1fr)); gap: 12px; margin-top: 16px; }
.info-item { background: #0d1117; padding: 12px; border-radius: 6px; }
.info-label { font-size: 11px; color: #8b949e; text-transform: uppercase; letter-spacing: 0.5px; }
.info-value { font-size: 18px; font-weight: 600; margin-top: 4px; color: #fff; }
.pattern-display { font-family: 'SF Mono', 'Fira Code', monospace; font-size: 16px; background: #0d1117; padding: 12px 16px; border-radius: 6px; margin-top: 12px; word-break: break-all; letter-spacing: 2px; }
.bit-0 { color: #58a6ff; }
.bit-1 { color: #f0883e; }
.bit-unknown { color: #484f58; }
table { width: 100%; border-collapse: collapse; font-size: 13px; }
th { text-align: left; padding: 8px 12px; border-bottom: 1px solid #30363d; color: #8b949e; font-weight: 500; }
td { padding: 8px 12px; border-bottom: 1px solid #21262d; }
tr:hover { background: rgba(255,255,255,0.02); }
.score-bar { height: 6px; background: #21262d; border-radius: 3px; width: 100px; display: inline-block; vertical-align: middle; margin-right: 8px; }
.score-fill { height: 100%; border-radius: 3px; }
.score-high { background: #238636; }
.score-mid { background: #d29922; }
.score-low { background: #da373c; }
.seg-table { max-height: 400px; overflow-y: auto; }
.users-section { margin-top: 16px; }
.users-section textarea { width: 100%; height: 120px; background: #0d1117; border: 1px solid #30363d; border-radius: 6px; color: #e1e4e8; padding: 12px; font-family: monospace; font-size: 13px; resize: vertical; }
.users-section label { font-size: 13px; color: #8b949e; display: block; margin-bottom: 6px; }
.file-info { color: #8b949e; font-size: 13px; margin-top: 8px; }
.tabs { display: flex; gap: 4px; margin-bottom: 16px; }
.tab { padding: 6px 16px; border-radius: 6px; cursor: pointer; font-size: 13px; color: #8b949e; background: transparent; border: 1px solid transparent; }
.tab.active { background: #21262d; color: #e1e4e8; border-color: #30363d; }
.tab-content { display: none; }
.tab-content.active { display: block; }
.status-dot { display: inline-block; width: 8px; height: 8px; border-radius: 50%; margin-right: 6px; }
.dot-green { background: #238636; }
.dot-red { background: #da373c; }
.dot-yellow { background: #d29922; }
</style>
</head>
<body>
<div class="container">
    <h1>Audio Watermark Detection</h1>
    <p class="subtitle">Upload a recording to identify the source user/MAC via A/B audio fingerprint analysis</p>

    <div class="card">
        <h2>Upload Recording</h2>
        <div class="upload-zone" id="dropZone" onclick="document.getElementById('fileInput').click()">
            <div class="icon">&#x1F50A;</div>
            <div><strong>Drop a file here or click to browse</strong></div>
            <p>Supports: MP4, MKV, AVI, MP3, WAV, AAC, FLAC, TS (max {{ max_upload }}MB)</p>
            <input type="file" id="fileInput" accept="video/*,audio/*,.ts,.mkv,.avi,.flac">
        </div>
        <div class="file-info" id="fileInfo"></div>

        <div class="users-section">
            <label>User Database (one username/MAC per line)</label>
            <textarea id="userList" placeholder="john123&#10;AA:BB:CC:DD:EE:FF&#10;user_456&#10;...">{{ existing_users }}</textarea>
            <button class="btn" onclick="saveUsers()" style="margin-top: 8px; font-size: 12px; padding: 4px 12px;">Save User List</button>
        </div>

        <div class="progress" id="progress">
            <div class="progress-bar"><div class="progress-fill" id="progressFill"></div></div>
            <div class="progress-text" id="progressText">Uploading...</div>
        </div>
    </div>

    <div class="result" id="result">
        <div class="card">
            <h2>Detection Result</h2>
            <div id="matchBox"></div>
            <div class="info-grid" id="infoGrid"></div>
            <div id="patternSection"></div>
        </div>

        <div class="card">
            <div class="tabs">
                <div class="tab active" onclick="switchTab('matches')">User Matches</div>
                <div class="tab" onclick="switchTab('segments')">Segment Analysis</div>
            </div>
            <div class="tab-content active" id="tab-matches"></div>
            <div class="tab-content" id="tab-segments"></div>
        </div>
    </div>

    <div class="card" id="historyCard" style="display:none">
        <h2>Recent Analyses</h2>
        <div id="historyList"></div>
    </div>
</div>

<script>
const dropZone = document.getElementById('dropZone');
const fileInput = document.getElementById('fileInput');

['dragenter','dragover'].forEach(e => {
    dropZone.addEventListener(e, ev => { ev.preventDefault(); dropZone.classList.add('dragover'); });
});
['dragleave','drop'].forEach(e => {
    dropZone.addEventListener(e, ev => { ev.preventDefault(); dropZone.classList.remove('dragover'); });
});
dropZone.addEventListener('drop', ev => {
    if (ev.dataTransfer.files.length) { fileInput.files = ev.dataTransfer.files; handleFile(ev.dataTransfer.files[0]); }
});
fileInput.addEventListener('change', () => { if (fileInput.files.length) handleFile(fileInput.files[0]); });

function handleFile(file) {
    const maxBytes = {{ max_upload }} * 1024 * 1024;
    if (file.size > maxBytes) { alert('File too large. Max ' + {{ max_upload }} + 'MB'); return; }

    document.getElementById('fileInfo').textContent = file.name + ' (' + (file.size / 1024 / 1024).toFixed(1) + ' MB)';

    const formData = new FormData();
    formData.append('file', file);
    formData.append('users', document.getElementById('userList').value);

    const progress = document.getElementById('progress');
    const progressFill = document.getElementById('progressFill');
    const progressText = document.getElementById('progressText');

    progress.style.display = 'block';
    document.getElementById('result').style.display = 'none';
    progressFill.style.width = '0%';
    progressText.textContent = 'Uploading...';

    const xhr = new XMLHttpRequest();
    xhr.upload.onprogress = (e) => {
        if (e.lengthComputable) {
            const pct = (e.loaded / e.total * 50);
            progressFill.style.width = pct + '%';
            if (pct >= 50) progressText.textContent = 'Analyzing audio...';
        }
    };

    xhr.onload = () => {
        progressFill.style.width = '100%';
        if (xhr.status === 200) {
            const data = JSON.parse(xhr.responseText);
            progressText.textContent = 'Analysis complete';
            setTimeout(() => { progress.style.display = 'none'; }, 500);
            showResult(data);
        } else {
            progressText.textContent = 'Error: ' + xhr.responseText;
            progressFill.style.background = '#da373c';
        }
    };

    xhr.onerror = () => { progressText.textContent = 'Upload failed'; progressFill.style.background = '#da373c'; };
    xhr.open('POST', '/api/analyze');
    xhr.send(formData);

    setTimeout(() => { progressFill.style.width = '60%'; progressText.textContent = 'Extracting audio...'; }, 2000);
    setTimeout(() => { progressFill.style.width = '75%'; progressText.textContent = 'Running FFT analysis...'; }, 5000);
    setTimeout(() => { progressFill.style.width = '90%'; progressText.textContent = 'Matching patterns...'; }, 8000);
}

function showResult(data) {
    document.getElementById('result').style.display = 'block';

    const matchBox = document.getElementById('matchBox');
    if (data.best_match && data.best_match.score >= 80) {
        matchBox.innerHTML = '<div class="match-box match-found">' +
            '<div class="match-name">' + escHtml(data.best_match.username) + '</div>' +
            '<div class="match-score">Confidence: ' + data.best_match.score + '% (' +
            data.best_match.matched_bits + '/' + data.best_match.total_bits + ' bits matched)</div></div>';
    } else if (data.best_match && data.best_match.score >= 50) {
        matchBox.innerHTML = '<div class="match-box match-partial">' +
            '<div class="match-name">Possible: ' + escHtml(data.best_match.username) + '</div>' +
            '<div class="match-score">Low confidence: ' + data.best_match.score + '% - recording may be degraded</div></div>';
    } else if (!data.watermark_detected) {
        matchBox.innerHTML = '<div class="match-box match-none">' +
            '<div class="match-name">No watermark detected</div>' +
            '<div class="match-score">No A/B tone pattern found in the audio. The recording may not be from a watermarked stream.</div></div>';
    } else {
        matchBox.innerHTML = '<div class="match-box match-none">' +
            '<div class="match-name">No match found</div>' +
            '<div class="match-score">Watermark pattern detected but no matching user in database. Add more users and retry.</div></div>';
    }

    const grid = document.getElementById('infoGrid');
    grid.innerHTML =
        infoItem('Duration', data.duration + 's') +
        infoItem('Segments', data.analyzed_segments) +
        infoItem('Bits Detected', data.detected_bits + '/' + data.total_bits) +
        infoItem('Users Checked', data.users_checked) +
        infoItem('Watermark', data.watermark_detected ?
            '<span class="status-dot dot-green"></span>Found' :
            '<span class="status-dot dot-red"></span>None');

    const patSection = document.getElementById('patternSection');
    let patHtml = '<div style="margin-top:16px"><strong style="font-size:13px;color:#8b949e">DETECTED PATTERN</strong></div>';
    patHtml += '<div class="pattern-display">';
    for (const ch of data.pattern) {
        if (ch === '0') patHtml += '<span class="bit-0">0</span>';
        else if (ch === '1') patHtml += '<span class="bit-1">1</span>';
        else patHtml += '<span class="bit-unknown">?</span>';
    }
    patHtml += '</div>';
    patSection.innerHTML = patHtml;

    // Matches table
    const matchTab = document.getElementById('tab-matches');
    if (data.matches.length > 0) {
        let html = '<table><thead><tr><th>Rank</th><th>Username / MAC</th><th>Score</th><th>Bits</th></tr></thead><tbody>';
        data.matches.forEach((m, i) => {
            const cls = m.score >= 80 ? 'score-high' : m.score >= 50 ? 'score-mid' : 'score-low';
            html += '<tr><td>#' + (i+1) + '</td><td><strong>' + escHtml(m.username) + '</strong></td>' +
                '<td><div class="score-bar"><div class="score-fill ' + cls + '" style="width:' + m.score + '%"></div></div>' +
                m.score + '%</td><td>' + m.matched_bits + '/' + m.total_bits + '</td></tr>';
        });
        html += '</tbody></table>';
        matchTab.innerHTML = html;
    } else {
        matchTab.innerHTML = '<p style="color:#8b949e;padding:16px">No users in database to match against. Add usernames above.</p>';
    }

    // Segments table
    const segTab = document.getElementById('tab-segments');
    let segHtml = '<div class="seg-table"><table><thead><tr><th>Seg</th><th>Time</th><th>Tone A</th><th>Tone B</th><th>Bit</th><th>Confidence</th></tr></thead><tbody>';
    data.segments.forEach(s => {
        const timeStart = (s.index * {{ seg_dur }}).toFixed(0);
        const timeEnd = ((s.index + 1) * {{ seg_dur }}).toFixed(0);
        const bitStr = s.bit >= 0 ? (s.bit === 0 ? '<span class="bit-0">A (0)</span>' : '<span class="bit-1">B (1)</span>') : '<span class="bit-unknown">?</span>';
        segHtml += '<tr><td>' + s.index + '</td><td>' + timeStart + '-' + timeEnd + 's</td>' +
            '<td>' + s.tone_a + '</td><td>' + s.tone_b + '</td><td>' + bitStr + '</td><td>' + s.confidence + '</td></tr>';
    });
    segHtml += '</tbody></table></div>';
    segTab.innerHTML = segHtml;

    // Save to history
    saveHistory(data);
}

function infoItem(label, value) {
    return '<div class="info-item"><div class="info-label">' + label + '</div><div class="info-value">' + value + '</div></div>';
}

function switchTab(name) {
    document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
    document.querySelectorAll('.tab-content').forEach(t => t.classList.remove('active'));
    event.target.classList.add('active');
    document.getElementById('tab-' + name).classList.add('active');
}

function escHtml(s) {
    const d = document.createElement('div'); d.textContent = s; return d.innerHTML;
}

function saveUsers() {
    fetch('/api/save-users', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({users: document.getElementById('userList').value})
    }).then(r => r.json()).then(d => {
        alert('User list saved (' + d.count + ' users)');
    });
}

function saveHistory(data) {
    let history = JSON.parse(localStorage.getItem('wm_history') || '[]');
    history.unshift({
        time: new Date().toISOString(),
        match: data.best_match ? data.best_match.username : 'none',
        score: data.best_match ? data.best_match.score : 0,
        bits: data.detected_bits,
        duration: data.duration
    });
    if (history.length > 20) history = history.slice(0, 20);
    localStorage.setItem('wm_history', JSON.stringify(history));
    showHistory();
}

function showHistory() {
    const history = JSON.parse(localStorage.getItem('wm_history') || '[]');
    if (history.length === 0) return;

    const card = document.getElementById('historyCard');
    card.style.display = 'block';

    let html = '<table><thead><tr><th>Time</th><th>Match</th><th>Score</th><th>Bits</th><th>Duration</th></tr></thead><tbody>';
    history.forEach(h => {
        const t = new Date(h.time);
        const cls = h.score >= 80 ? 'dot-green' : h.score >= 50 ? 'dot-yellow' : 'dot-red';
        html += '<tr><td>' + t.toLocaleString() + '</td><td><span class="status-dot ' + cls + '"></span>' +
            escHtml(h.match) + '</td><td>' + h.score + '%</td><td>' + h.bits + '</td><td>' + h.duration + 's</td></tr>';
    });
    html += '</tbody></table>';
    document.getElementById('historyList').innerHTML = html;
}

showHistory();
</script>
</body>
</html>'''


@app.route('/')
def index():
    existing_users = ''
    users_file = os.path.join(UPLOAD_DIR, 'users.txt')
    if os.path.exists(users_file):
        with open(users_file, 'r') as f:
            existing_users = f.read()
    elif USERS_FILE and os.path.exists(USERS_FILE):
        with open(USERS_FILE, 'r') as f:
            existing_users = f.read()

    return render_template_string(HTML_TEMPLATE,
                                  max_upload=MAX_UPLOAD_MB,
                                  seg_dur=SEGMENT_DURATION,
                                  existing_users=existing_users)


@app.route('/api/analyze', methods=['POST'])
def analyze():
    if 'file' not in request.files:
        return 'No file uploaded', 400

    file = request.files['file']
    if not file.filename:
        return 'Empty filename', 400

    ext = os.path.splitext(file.filename)[1].lower()
    allowed = {'.mp4', '.mkv', '.avi', '.ts', '.mp3', '.wav', '.aac',
               '.flac', '.m4a', '.ogg', '.webm', '.mov', '.wmv'}
    if ext not in allowed:
        return f'Unsupported file type: {ext}', 400

    timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
    safe_name = timestamp + '_' + file.filename.replace('/', '_').replace('\\', '_')
    filepath = os.path.join(UPLOAD_DIR, safe_name)
    file.save(filepath)

    users_text = request.form.get('users', '')
    if users_text.strip():
        users_path = os.path.join(UPLOAD_DIR, 'users.txt')
        with open(users_path, 'w') as f:
            f.write(users_text)

    try:
        result = analyze_file(filepath)
        result['filename'] = file.filename
        result['timestamp'] = timestamp

        result_path = os.path.join(UPLOAD_DIR, timestamp + '_result.json')
        with open(result_path, 'w') as f:
            json.dump(result, f, indent=2)

        return jsonify(result)
    except Exception as e:
        return str(e), 500
    finally:
        if os.path.exists(filepath):
            os.unlink(filepath)


@app.route('/api/save-users', methods=['POST'])
def save_users():
    data = request.get_json()
    users_text = data.get('users', '')
    users_path = os.path.join(UPLOAD_DIR, 'users.txt')
    with open(users_path, 'w') as f:
        f.write(users_text)
    count = sum(1 for line in users_text.strip().split('\n') if line.strip() and not line.strip().startswith('#'))
    return jsonify({'ok': True, 'count': count})


@app.route('/api/generate-pattern', methods=['POST'])
def generate_pattern():
    data = request.get_json()
    username = data.get('username', '')
    bits = data.get('bits', NUM_BITS)
    if not username:
        return 'username required', 400
    pattern = user_to_pattern(username, bits)
    return jsonify({
        'username': username,
        'pattern': ''.join(str(b) for b in pattern),
        'bits': len(pattern)
    })


if __name__ == '__main__':
    port = int(os.environ.get('PORT', 5000))
    print(f"Audio Watermark Detection GUI running on http://0.0.0.0:{port}")
    print(f"Upload directory: {UPLOAD_DIR}")
    if USERS_FILE:
        print(f"Users file: {USERS_FILE}")
    print(f"Tone A: {TONE_A_HZ}Hz, Tone B: {TONE_B_HZ}Hz")
    print(f"Segment duration: {SEGMENT_DURATION}s, Pattern bits: {NUM_BITS}")
    app.run(host='0.0.0.0', port=port, debug=False)
