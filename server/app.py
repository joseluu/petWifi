import sqlite3
import datetime
import math
import requests
from requests.auth import HTTPBasicAuth
from flask import Flask, request, render_template, jsonify
from dotenv import load_dotenv
from zoneinfo import ZoneInfo
import os
import logging

# Load environment variables
load_dotenv()

app = Flask(__name__)

# WiGLE API credentials
WIGLE_USER = os.getenv('WIGLE_USER')
WIGLE_TOKEN = os.getenv('WIGLE_TOKEN')
WIGLE_API_URL = 'https://api.wigle.net/api/v2/network/search'

# WiGLE resets each account's daily query allowance at 00:00 US/Pacific, so the
# call counter is bucketed by Pacific calendar day to line up with that reset.
PACIFIC = ZoneInfo('America/Los_Angeles')

# Load center points from .env
map_center_str = os.getenv('MAP_CENTER')
if not map_center_str:
    map_center_str = "48.853480768362225,2.3488040743567162"  # Default to Paris center
try:
    # Parse comma-separated lat,lon pairs
    coords = map_center_str.split(',')
    if len(coords) != 2:
        raise ValueError("MAP_CENTER must have exactly two values (lat,lon)")
    MAP_CENTER_LAT = float(coords[0])
    MAP_CENTER_LON = float(coords[1])
except ValueError as e:
    raise ValueError(f"Invalid MAP_CENTER format in .env: {e}")
MAP_RADIUS = float(os.getenv('MAP_RADIUS', '500'))  # default to 500 meters if not set

# Database and logging setup
DB_NAME = os.getenv('DB_FILENAME')
if not DB_NAME:
    print("DB_FILENAME not set in .env")
LOG_FILE = 'wigle_requests.log'

# Configure logging
logging.basicConfig(
    filename=LOG_FILE,
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s'
)

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
            est_lon REAL,
            cat_vbatt REAL,
            cat_rssi INTEGER,
            cat_snr INTEGER,
            sta_vbatt REAL,
            sta_rssi INTEGER,
            sta_snr INTEGER
        )
    ''')
    cursor.execute('''
        CREATE TABLE IF NOT EXISTS wigle_usage (
            day TEXT PRIMARY KEY,
            count INTEGER NOT NULL DEFAULT 0
        )
    ''')
    conn.commit()
    conn.close()

# A BSSID that WiGLE cannot locate is cached as a row with NULL lat/lon so we
# stop re-querying the (rate-limited) WiGLE API for it on every scan -- that
# repeated querying was exhausting the daily quota (HTTP 429). Such "not found"
# entries are re-checked at most once per NEGATIVE_TTL in case WiGLE later adds
# the AP.
NEGATIVE_TTL = datetime.timedelta(days=30)


def _negative_cache_valid(lastupdt):
    """True if a negative (not-found) cache entry is still fresh."""
    if not lastupdt:
        return False
    try:
        ts = datetime.datetime.fromisoformat(lastupdt)
    except (ValueError, TypeError):
        return False
    return datetime.datetime.now() - ts < NEGATIVE_TTL


def _record_wigle_call():
    """Increment the WiGLE call counter for the current US/Pacific day.

    Counts every actual outbound WiGLE request (cache hits do not call this), so
    it tracks consumption against WiGLE's daily quota. Uses its own short-lived
    connection so the count is committed regardless of how get_ap_location's
    main transaction ends.
    """
    day = datetime.datetime.now(PACIFIC).date().isoformat()
    conn = sqlite3.connect(DB_NAME)
    try:
        conn.execute(
            'INSERT INTO wigle_usage (day, count) VALUES (?, 1) '
            'ON CONFLICT(day) DO UPDATE SET count = count + 1',
            (day,))
        conn.commit()
    finally:
        conn.close()


def get_ap_location(bssid):
    """Query local database first, then WiGLE API if BSSID is missing.

    Both successful and "not found" results are cached, so an un-locatable
    BSSID is not re-queried against WiGLE on every scan. Transient failures
    (HTTP errors, rate limiting, network problems) are NOT cached and will be
    retried on the next scan.
    """
    bssid = bssid.upper()
    conn = sqlite3.connect(DB_NAME)
    cursor = conn.cursor()
    cursor.execute('SELECT lat, lon, lastupdt FROM ap_locations WHERE bssid = ?', (bssid,))
    result = cursor.fetchone()
    if result:
        lat, lon, lastupdt = result
        if lat is not None and lon is not None:
            conn.close()
            return {'lat': lat, 'lon': lon, 'error': None}
        # Negative cache entry: WiGLE previously had no location for this BSSID.
        if _negative_cache_valid(lastupdt):
            conn.close()
            return {'lat': None, 'lon': None, 'error': None}
        # Stale negative entry: fall through and re-query WiGLE.

    # BSSID unknown (or stale negative), query WiGLE API
    params = {
        'netid': bssid,
        'onlymine': 'false',
        'freenet': 'false',
        'paynet': 'false'
    }
    headers = {'Accept': 'application/json'}
    logging.info(f"WiGLE API request for BSSID {bssid}: {params}")
    _record_wigle_call()
    try:
        response = requests.get(WIGLE_API_URL, params=params, auth=HTTPBasicAuth(WIGLE_USER, WIGLE_TOKEN), headers=headers)
        logging.info(f"WiGLE API response status for BSSID {bssid}: {response.status_code}")
        if response.status_code != 200:
            # Transient (e.g. 429 "too many queries today", 5xx): do not cache.
            conn.close()
            error_msg = f"WiGLE HTTP {response.status_code}: {response.text}"
            logging.error(error_msg)
            return {'lat': None, 'lon': None, 'error': error_msg}

        data = response.json()
        if not data.get('success'):
            # API-level failure (auth / rate-limit message / ...): do not cache.
            conn.close()
            error_msg = f"WiGLE API error: {data.get('message', 'Unknown error')}"
            logging.error(error_msg)
            return {'lat': None, 'lon': None, 'error': error_msg}

        now = datetime.datetime.now().isoformat()
        results_list = data.get('results') or []
        result = results_list[0] if data.get('resultCount', 0) > 0 and results_list else None
        lat = result.get('trilat') if result else None
        lon = result.get('trilong') if result else None
        if lat is not None and lon is not None:
            cursor.execute(
                'INSERT OR REPLACE INTO ap_locations (bssid, lat, lon, lastupdt) VALUES (?, ?, ?, ?)',
                (bssid, lat, lon, now))
            conn.commit()
            conn.close()
            return {'lat': lat, 'lon': lon, 'error': None}

        # WiGLE succeeded but has no location for this BSSID: cache the negative
        # result so we stop re-querying it (the cause of quota exhaustion).
        cursor.execute(
            'INSERT OR REPLACE INTO ap_locations (bssid, lat, lon, lastupdt) VALUES (?, NULL, NULL, ?)',
            (bssid, now))
        conn.commit()
        conn.close()
        logging.info(f"WiGLE has no location for BSSID {bssid}; cached as not-found")
        return {'lat': None, 'lon': None, 'error': None}
    except requests.RequestException as e:
        conn.close()
        error_msg = f"WiGLE request failed: {str(e)}"
        logging.error(error_msg)
        return {'lat': None, 'lon': None, 'error': error_msg}


@app.route('/api/track', methods=['GET'])
def track_device():
    """Track a device's location based on its BSSID."""
    scan_id = request.args.get('scan_id', type=int)
    bssids = request.args.getlist('bssid[]')
    rssis = request.args.getlist('rssi[]')
    sta_rssi = request.args.get('sta_rssi', type=int)
    cat_rssi = request.args.get('cat_rssi', type=int)
    sta_snr = request.args.get('sta_snr', type=int)
    cat_snr = request.args.get('cat_snr', type=int)
    print(f"Track: scan_id={scan_id}, sta_rssi={sta_rssi}, cat_rssi={cat_rssi}, \
          sta_snr={sta_snr}, cat_snr={cat_snr}, "
          f"bssid[0]={bssids[0] if bssids else None}, rssi[0]={rssis[0] if rssis else None}")

    aps = []
    for bssid, rssi in zip(bssids, rssis):
        if not bssid or not rssi:
            return jsonify({'error': 'Missing bssid or rssi parameters'}), 400
        aps.append({'bssid': bssid, 'rssi': int(rssi)})

    common_data = {
        'sta_vbatt': request.args.get('sta_vbatt', type=float, default=0.0),
        'sta_rssi': request.args.get('sta_rssi', type=int, default=0),
        'sta_snr': request.args.get('sta_snr', type=int, default=0),
        'cat_vbatt': request.args.get('cat_vbatt', type=float, default=0.0),
        'cat_rssi': request.args.get('cat_rssi', type=int, default=0),
        'cat_snr': request.args.get('cat_snr', type=int, default=0)
    }
    aps_with_loc = []
    errors = []
    return process_aps(scan_id, aps, aps_with_loc, common_data, errors)


@app.route('/api/wigle_usage', methods=['GET'])
def wigle_usage():
    """Report how many WiGLE queries were made, bucketed by US/Pacific day.

    WiGLE exposes no remaining-quota endpoint, so this is our own count of
    outbound calls. 'today' is the count since the last 00:00 Pacific reset.
    """
    conn = sqlite3.connect(DB_NAME)
    cursor = conn.cursor()
    cursor.execute('SELECT day, count FROM wigle_usage ORDER BY day DESC LIMIT 30')
    rows = cursor.fetchall()
    conn.close()
    today = datetime.datetime.now(PACIFIC).date().isoformat()
    today_count = next((c for d, c in rows if d == today), 0)
    return jsonify({
        'pacific_day': today,
        'today': today_count,
        'history': [{'day': d, 'count': c} for d, c in rows]
    }), 200


@app.route('/api/scan', methods=['POST'])
def receive_scan():
    """Handle incoming scan data and estimate IoT position."""
    data = request.json
    if not data or 'aps' not in data or 'scan_id' not in data:
        return jsonify({'error': 'Invalid data format'}), 400

    aps_with_loc = []
    errors = []
    return process_aps(data['scan_id'], data['aps'], aps_with_loc, data, errors)


def process_aps(scan_id, aps, aps_with_loc, common_data, errors):
    print(str(aps))
    for ap in aps:
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
                INSERT INTO scans (scan_id, timestamp, est_lat, est_lon, sta_vbatt, sta_rssi, sta_snr, cat_vbatt, cat_rssi, cat_snr)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            ''', (scan_id, datetime.datetime.now(), est_lat, est_lon,
                  common_data['sta_vbatt'], common_data['sta_rssi'], common_data['sta_snr'],
                  common_data['cat_vbatt'], common_data['cat_rssi'], common_data['cat_snr']))
            conn.commit()
            conn.close()
        else:
            response['status'] = 'no valid weights'
    else:
        response['status'] = 'no valid APs'

    return jsonify(response), 200


@app.route('/')
def map_view():
    """Render map with IoT positions from the last 24 hours, including timestamps."""
    conn = sqlite3.connect(DB_NAME)
    cursor = conn.cursor()
    one_day_ago = datetime.datetime.now() - datetime.timedelta(hours=24)
    cursor.execute('SELECT est_lat, est_lon, timestamp FROM scans WHERE timestamp > ? ORDER BY timestamp', (one_day_ago,))
    points = cursor.fetchall()
    conn.close()

    bounds = [[0, 0], [0, 0]]
    #
    if points:
        avg_lat = sum(p[0] for p in points) / len(points)
        avg_lon = sum(p[1] for p in points) / len(points)

    else:
        avg_lat = MAP_CENTER_LAT
        avg_lon = MAP_CENTER_LON

    # Approximate deltas for 100m in each direction (200m square)
    delta_lat = MAP_RADIUS / 111000.0  # degrees per meter for latitude
    delta_lon = MAP_RADIUS / (111000.0 * math.cos(math.radians(avg_lat)))  # adjust for longitude

    bounds = [
        [avg_lat - delta_lat, avg_lon - delta_lon],
        [avg_lat + delta_lat, avg_lon + delta_lon]
    ]
    return render_template('map.html', points=points, bounds=bounds)

if __name__ == '__main__':
    init_db()
    app.run(debug=True, port=4201)
