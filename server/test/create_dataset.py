import requests
import math
import json
from datetime import datetime

# WiGLE API credentials
WIGLE_API_URL = 'https://api.wigle.net/api/v2/network/search'
WIGLE_AUTH = ('', '')

# Center coordinates and radius
CENTER_LAT = 48.72868
CENTER_LON = 2.22195
RADIUS_M = 250  # 250m radius (500m diameter)

# Conversion factors
METERS_PER_DEGREE_LAT = 111194.0
COS_LAT = math.cos(math.radians(CENTER_LAT))
METERS_PER_DEGREE_LON = METERS_PER_DEGREE_LAT * COS_LAT

# Calculate bounding box
LAT_DELTA = RADIUS_M / METERS_PER_DEGREE_LAT
LON_DELTA = RADIUS_M / METERS_PER_DEGREE_LON
LAT_MIN = CENTER_LAT - LAT_DELTA
LAT_MAX = CENTER_LAT + LAT_DELTA
LON_MIN = CENTER_LON - LON_DELTA
LON_MAX = CENTER_LON + LON_DELTA

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

def query_wigle():
    """Query WiGLE API for APs within the bounding box."""
    params = {
        'latrange1': LAT_MIN,
        'latrange2': LAT_MAX,
        'longrange1': LON_MIN,
        'longrange2': LON_MAX,
        'onlymine': 'false',
        'freenet': 'false',
        'paynet': 'false'
    }
    try:
        response = requests.get(WIGLE_API_URL, params=params, auth=WIGLE_AUTH)
        response.raise_for_status()
        data = response.json()
        if not data.get('success'):
            print(f"WiGLE API error: {data.get('message', 'Unknown error')}")
            return []
        return data.get('results', [])
    except requests.RequestException as e:
        print(f"WiGLE API request failed: {e}")
        return []

def generate_test_dataset():
    """Generate 5 test data points with APs within 250m radius."""
    results = query_wigle()
    if not results:
        print("No APs found in the specified area.")
        return []

    # Filter APs within 250m radius
    valid_aps = []
    for ap in results:
        lat = ap.get('trilat')
        lon = ap.get('trilong')
        bssid = ap.get('netid')
        if lat is None or lon is None or bssid is None:
            continue
        distance = haversine_distance(CENTER_LAT, CENTER_LON, lat, lon)
        if distance <= RADIUS_M:
            valid_aps.append({'bssid': bssid, 'lat': lat, 'lon': lon})

    if len(valid_aps) < 3:
        print(f"Only {len(valid_aps)} valid APs found, need at least 3.")
        return []

    # Generate 5 scan points
    dataset = []
    for scan_id in range(1, 6):
        # Select 3 random APs (to match your data format)
        selected_aps = valid_aps[:3] if len(valid_aps) >= 3 else valid_aps
        # Synthesize RSSI values (realistic range: -30 to -90 dBm)
        aps = [
            {'bssid': ap['bssid'], 'rssi': -60 - (i * 5 + scan_id * 2) % 30}
            for i, ap in enumerate(selected_aps)
        ]
        dataset.append({
            'scan_id': scan_id,
            'aps': aps
        })

    return dataset

def save_dataset(dataset, filename='test_dataset.json'):
    """Save dataset to a JSON file."""
    with open(filename, 'w') as f:
        json.dump(dataset, f, indent=2)
    print(f"Dataset saved to {filename}")

if __name__ == '__main__':
    dataset = generate_test_dataset()
    if dataset:
        save_dataset(dataset)