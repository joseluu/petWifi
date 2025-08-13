import sqlite3
import datetime
import math
import requests
from requests.auth import HTTPBasicAuth
from flask import Flask, request, render_template, jsonify
from dotenv import load_dotenv
import os

# Load environment variables from .env file
load_dotenv()

app = Flask(__name__)

# WiGLE API credentials from .env
WIGLE_USER = os.getenv('WIGLE_USER')
WIGLE_TOKEN = os.getenv('WIGLE_TOKEN')
WIGLE_API_URL = 'https://api.wigle.net/api/v2/network/search'

# Database setup
DB_NAME = 'iot_locations.db'

def init_db():
    """Initialize the SQLite database."""
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
    """Query WiGLE API or database for AP location."""
    conn = sqlite3.connect(DB_NAME)
    cursor = conn.cursor()
    cursor.execute('SELECT lat, lon FROM ap_locations WHERE bssid = ?', (bssid.upper(),))
    result = cursor.fetchone()
    if result:
        conn.close()
        return {'lat': result[0], 'lon': result[1], 'error': None}

    # Query WiGLE API
    params = {
        'netid': bssid.upper(),
        'onlymine': 'false',
        'freenet': 'false',
        'paynet': 'false'
    }
    headers = {'Accept': 'application/json'}
    try:
        response = requests.get(WIGLE_API_URL, params=params, auth=HTTPBasicAuth(WIGLE_USER, WIGLE_TOKEN), headers=headers)
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
                    return {'lat': lat, 'lon': lon, 'error': None}
                else:
                    conn.close()
                    return {'lat': None, 'lon': None, 'error': 'WiGLE returned no location data'}
            else:
                conn.close()
                return {'lat': None, 'lon': None, 'error': f"WiGLE API error: {data.get('message', 'Unknown error')}"}
        else:
            conn.close()
            return {'lat': None, 'lon': None, 'error': f"WiGLE HTTP {response.status_code}: {response.text}"}
    except requests.RequestException as e:
        conn.close()
        return {'lat': None, 'lon': None, 'error': f"WiGLE request failed: {str(e)}"}

@app.route('/api/scan', methods=['POST'])
def receive_scan():
    """Handle incoming scan data and estimate IoT position."""
    data = request.json
    if not data or 'aps' not in data or 'scan_id' not in data:
        return jsonify({'error': 'Invalid data format'}), 400

    aps_with_loc = []
    errors = []
    for ap in data['aps']:
        bssid = ap.get('bssid')
        rssi = ap.get('rssi')
        if bssid and rssi:
            loc = get_ap_location(bssid)
            if loc['lat'] is not None and loc['lon'] is not None:
                # Weight based on signal strength (convert dBm to linear scale)
                weight = 10 ** (rssi / 10.0)
                aps_with_loc.append((loc['lat'], loc['lon'], weight))
            if loc['error']:
                errors.append({'bssid': bssid, 'error': loc['error']})

    response = {'status': 'success', 'errors': errors}
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
        else:
            response['status'] = 'no valid weights'
    else:
        response['status'] = 'no valid APs'

    return jsonify(response), 200

@app.route('/')
def map_view():
    """Render map with IoT positions from the last 24 hours."""
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