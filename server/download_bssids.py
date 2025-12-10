import sqlite3
import requests
from requests.auth import HTTPBasicAuth
from dotenv import load_dotenv
import os
import math
import logging
from datetime import datetime

# Load environment variables
load_dotenv()

# WiGLE API credentials
WIGLE_USER = os.getenv('WIGLE_USER')
WIGLE_TOKEN = os.getenv('WIGLE_TOKEN')
WIGLE_API_URL = 'https://api.wigle.net/api/v2/network/search'

# Load center points from .env
center_points_str = os.getenv('CENTER_POINTS')
if not center_points_str:
    raise ValueError("CENTER_POINTS not found in .env file")
try:
    # Parse comma-separated lat,lon pairs
    coords = center_points_str.split(',')
    if len(coords) % 2 != 0:
        raise ValueError("CENTER_POINTS must have an even number of values (lat,lon pairs)")
    center_points = [(float(coords[i]), float(coords[i+1])) for i in range(0, len(coords), 2)]
except ValueError as e:
    raise ValueError(f"Invalid CENTER_POINTS format in .env: {e}")

# Database and logging setup
DB_NAME = 'wigle_cache8.db'
LOG_FILE = 'wigle_requests8.log'

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
            lon REAL,
            lastupdt TEXT,
            road TEXT,
            channel INTEGER,
            housenumber TEXT
        )
    ''')
    conn.commit()
    conn.close()

def haversine_distance(lat1, lon1, lat2, lon2):
    """Calculate distance between two points in meters using Haversine formula."""
    R = 6371000  # Earth's radius in meters
    phi1 = math.radians(lat1)
    phi2 = math.radians(lat2)
    delta_phi = math.radians(lat2 - lat1)
    delta_lambda = math.radians(lon2 - lon1)
    a = math.sin(delta_phi / 2) ** 2 + math.cos(phi1) * math.cos(phi2) * math.sin(delta_lambda / 2) ** 2
    c = 2 * math.atan2(math.sqrt(a), math.sqrt(1 - a))
    return R * c

def get_bounding_box(lat, lon, radius_m=1000):
    """Calculate bounding box for a given center point and radius."""
    METERS_PER_DEGREE_LAT = 111194.0
    COS_LAT = math.cos(math.radians(lat))
    METERS_PER_DEGREE_LON = METERS_PER_DEGREE_LAT * COS_LAT
    lat_delta = radius_m / METERS_PER_DEGREE_LAT
    lon_delta = radius_m / METERS_PER_DEGREE_LON
    return {
        'latrange1': lat - lat_delta,
        'latrange2': lat + lat_delta,
        'longrange1': lon - lon_delta,
        'longrange2': lon + lon_delta
    }

def query_wigle(center_lat, center_lon):
    """Query WiGLE API for APs within 1km radius, handling pagination."""
    params = get_bounding_box(center_lat, center_lon)
    params.update({
        'onlymine': 'false',
        'freenet': 'false',
        'paynet': 'false',
        'resultsPerPage': '100',
        'variance': 0.003
    })
    all_results = []
    search_after = None
    headers = {'Accept': 'application/json'}

    while True:
        if search_after:
            params['searchAfter'] = search_after
        logging.info(f"WiGLE API request for center ({center_lat}, {center_lon}): {params}")
        try:
            response = requests.get(WIGLE_API_URL, params=params, auth=HTTPBasicAuth(WIGLE_USER, WIGLE_TOKEN), headers=headers)
            logging.info(f"WiGLE API response status: {response.status_code}")
            response.raise_for_status()
            data = response.json()
            if not data.get('success'):
                logging.error(f"WiGLE API error: {data.get('message', 'Unknown error')}")
                break
            results = data.get('results', [])
            if not results:
                break
            # Filter results within 1km radius
            for ap in results:
                lat = ap.get('trilat')
                lon = ap.get('trilong')
                bssid = ap.get('netid')
                lastupdt = ap.get('lastupdt')
                road = ap.get('road')
                channel = ap.get('channel')
                housenumber = ap.get('housenumber')
                if lat is None or lon is None or bssid is None:
                    continue
                if haversine_distance(center_lat, center_lon, lat, lon) <= 1000:
                    all_results.append({'bssid': bssid.upper(), 'lat': lat, 'lon': lon, 'lastupdt': lastupdt,
                                        'road': road, 'channel': channel, 'housenumber': housenumber})
            search_after = data.get('searchAfter')
            if not search_after:
                break
        except requests.RequestException as e:
            logging.error(f"WiGLE API request failed: {str(e)}")
            break

    return all_results

def store_aps_in_db(aps):
    """Store APs in the SQLite database."""
    conn = sqlite3.connect(DB_NAME)
    cursor = conn.cursor()
    for ap in aps:
        cursor.execute('''
            INSERT OR IGNORE INTO ap_locations (bssid, lat, lon, lastupdt, road, channel, housenumber)
            VALUES (?, ?, ?, ?, ?, ?, ?)
        ''', (ap['bssid'], ap['lat'], ap['lon'], ap['lastupdt'], ap['road'], ap['channel'], ap['housenumber']))
    conn.commit()
    conn.close()

def download_bssids():
    """Download BSSIDs for each center point and store in database."""
    init_db()
    for lat, lon in center_points:
        print(f"Fetching BSSIDs for center point ({lat}, {lon})...")
        aps = query_wigle(lat, lon)
        if aps:
            print(f"Found {len(aps)} APs for center point ({lat}, {lon}).")
            store_aps_in_db(aps)
        else:
            print(f"No APs found for center point ({lat}, {lon}).")
        logging.info(f"Completed fetching {len(aps)} APs for center ({lat}, {lon})")

if __name__ == '__main__':
    download_bssids()
    print(f"Data stored in {DB_NAME}. API requests logged in {LOG_FILE}.")