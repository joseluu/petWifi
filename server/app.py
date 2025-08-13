import sqlite3
import datetime
import math
import requests
from requests.auth import HTTPBasicAuth
import base64  # Optional, but using HTTPBasicAuth instead
from flask import Flask, request, render_template, jsonify
import json

app = Flask(__name__)

# Wigle API credentials (replace with your own)
WIGLE_API_NAME = ''  # From wigle.net/account
WIGLE_API_KEY = ''    # From wigle.net/account

# Database setup
DB_NAME = 'iot_locations.db'

def init_db():
    conn = sqlite3.connect(DB_NAME)
    cursor = conn.cursor()
    cursor.execute('''
        CREATE TABLE IF NOT EXISTS ap_locations (
            bssid TEXT PRIMARY KEY,
            lat REAL,
            lon REAL
        )
    ''')
    cursor.execute('''
        CREATE TABLE IF NOT EXISTS scans (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            scan_id INTEGER,
            timestamp DATETIME,
            est_lat REAL,
            est_lon REAL
        )
    ''')
    conn.commit()
    conn.close()

def get_ap_location(bssid):
    conn = sqlite3.connect(DB_NAME)
    cursor = conn.cursor()
    cursor.execute('SELECT lat, lon FROM ap_locations WHERE bssid = ?', (bssid.upper(),))
    result = cursor.fetchone()
    if result:
        conn.close()
        return result

    # Query Wigle API
    url = 'https://api.wigle.net/api/v2/network/search'
    params = {
        'netid': bssid.upper(),
        'onlymine': 'false',
        'freenet': 'false',
        'paynet': 'false'
    }
    headers = {
        'Accept': 'application/json'
    }
    response = requests.get(url, params=params, auth=HTTPBasicAuth(WIGLE_API_NAME, WIGLE_API_KEY), headers=headers)
    
    if response.status_code == 200:
        data = response.json()
        if data.get('success') and data.get('resultCount', 0) > 0:
            result = data['results'][0]
            lat = result.get('trilat')
            lon = result.get('trilong')
            if lat is not None and lon is not None:
                cursor.execute('INSERT INTO ap_locations (bssid, lat, lon) VALUES (?, ?, ?)',
                               (bssid.upper(), lat, lon))
                conn.commit()
                conn.close()
                return lat, lon

    conn.close()
    return None

@app.route('/api/scan', methods=['POST'])
def receive_scan():
    data = request.json
    if not data or 'aps' not in data or 'scan_id' not in data:
        return jsonify({'error': 'Invalid data'}), 400

    aps_with_loc = []
    for ap in data['aps']:
        bssid = ap.get('bssid')
        rssi = ap.get('rssi')
        if bssid and rssi:
            loc = get_ap_location(bssid)
            if loc:
                # Weight based on signal strength (convert dBm to linear scale)
                weight = 10 ** (rssi / 10.0)
                aps_with_loc.append((loc[0], loc[1], weight))

    if aps_with_loc:
        sum_w = sum(w for _, _, w in aps_with_loc)
        if sum_w > 0:
            est_lat = sum(w * lat for lat, _, w in aps_with_loc) / sum_w
            est_lon = sum(w * lon for _, lon, w in aps_with_loc) / sum_w

            conn = sqlite3.connect(DB_NAME)
            cursor = conn.cursor()
            cursor.execute('''
                INSERT INTO scans (scan_id, timestamp, est_lat, est_lon)
                VALUES (?, ?, ?, ?)
            ''', (data['scan_id'], datetime.datetime.now(), est_lat, est_lon))
            conn.commit()
            conn.close()

            return jsonify({'status': 'success'}), 200

    return jsonify({'status': 'no valid APs'}), 200

@app.route('/')
def map_view():
    conn = sqlite3.connect(DB_NAME)
    cursor = conn.cursor()
    one_day_ago = datetime.datetime.now() - datetime.timedelta(hours=24)
    cursor.execute('SELECT est_lat, est_lon FROM scans WHERE timestamp > ? ORDER BY timestamp', (one_day_ago,))
    points = cursor.fetchall()
    conn.close()

    bounds = [[0, 0], [0, 0]]
    if points:
        avg_lat = sum(p[0] for p in points) / len(points)
        avg_lon = sum(p[1] for p in points) / len(points)

        # Approximate deltas for 250m in each direction (500m square)
        delta_lat = 250 / 111000.0  # degrees per meter for latitude
        delta_lon = 250 / (111000.0 * math.cos(math.radians(avg_lat)))  # adjust for longitude

        bounds = [
            [avg_lat - delta_lat, avg_lon - delta_lon],
            [avg_lat + delta_lat, avg_lon + delta_lon]
        ]

    return render_template('map.html', points=points, bounds=bounds)

if __name__ == '__main__':
    init_db()
    app.run(debug=True)
