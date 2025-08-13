import sqlite3
import datetime
import os

# Database file
DB_NAME = 'iot_locations.db'

def print_timestamps():
    """Print current timestamp and timestamp for 24 hours ago."""
    current_time = datetime.datetime.now()
    time_24h_ago = current_time - datetime.timedelta(hours=24)
    
    print("Debug Timestamps:")
    print(f"Current Timestamp: {current_time.strftime('%Y-%m-%d %H:%M:%S')}")
    print(f"Timestamp 24 Hours Ago: {time_24h_ago.strftime('%Y-%m-%d %H:%M:%S')}")
    print()

def dump_database():
    """Dump contents of the database tables."""
    # Check if database file exists
    if not os.path.exists(DB_NAME):
        print(f"Error: Database file '{DB_NAME}' not found.")
        return

    try:
        conn = sqlite3.connect(DB_NAME)
        cursor = conn.cursor()

        # Check if tables exist
        cursor.execute("SELECT name FROM sqlite_master WHERE type='table'")
        tables = [row[0] for row in cursor.fetchall()]
        expected_tables = ['scans', 'aps', 'positions']
        missing_tables = [t for t in expected_tables if t not in tables]
        if missing_tables:
            print(f"Error: Missing tables: {', '.join(missing_tables)}")
            conn.close()
            return

        # Dump scans table
        print("=== Scans Table ===")
        cursor.execute("SELECT scan_id, timestamp FROM scans")
        scans = cursor.fetchall()
        if not scans:
            print("No data in scans table.")
        else:
            print("scan_id | timestamp")
            print("-" * 50)
            for row in scans:
                print(f"{row[0]} | {row[1]}")
        print()

        # Dump aps table
        print("=== APs Table ===")
        cursor.execute("SELECT scan_id, bssid, rssi FROM aps")
        aps = cursor.fetchall()
        if not aps:
            print("No data in aps table.")
        else:
            print("scan_id | bssid | rssi")
            print("-" * 50)
            for row in aps:
                print(f"{row[0]} | {row[1]} | {row[2]}")
        print()

        # Dump positions table
        print("=== Positions Table ===")
        cursor.execute("SELECT scan_id, lat, lon FROM positions")
        positions = cursor.fetchall()
        if not positions:
            print("No data in positions table.")
        else:
            print("scan_id | lat | lon")
            print("-" * 50)
            for row in positions:
                print(f"{row[0]} | {row[1]:.6f} | {row[2]:.6f}")
        print()

        # Optional: Join scans and positions for last 24 hours
        print("=== Scans and Positions (Last 24 Hours) ===")
        time_24h_ago = (datetime.datetime.now() - datetime.timedelta(hours=24)).strftime('%Y-%m-%d %H:%M:%S')
        cursor.execute("""
            SELECT s.scan_id, s.timestamp, p.lat, p.lon
            FROM scans s
            LEFT JOIN positions p ON s.scan_id = p.scan_id
            WHERE s.timestamp >= ?
            ORDER BY s.timestamp
        """, (time_24h_ago,))
        recent_data = cursor.fetchall()
        if not recent_data:
            print("No data in the last 24 hours.")
        else:
            print("scan_id | timestamp | lat | lon")
            print("-" * 80)
            for row in recent_data:
                lat = f"{row[2]:.6f}" if row[2] is not None else "NULL"
                lon = f"{row[3]:.6f}" if row[3] is not None else "NULL"
                print(f"{row[0]} | {row[1]} | {lat} | {lon}")

        conn.close()

    except sqlite3.Error as e:
        print(f"Database error: {e}")
    except Exception as e:
        print(f"Unexpected error: {e}")

if __name__ == '__main__':
    print("Database Debug Script")
    print("=" * 50)
    print_timestamps()
    dump_database()