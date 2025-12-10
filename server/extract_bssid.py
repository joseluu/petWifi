import sqlite3
from dotenv import load_dotenv
import os

# Load environment variables
load_dotenv()

# Create a SQL connection to our SQLite database
DB_NAME = os.getenv('DB_FILENAME')
if not DB_NAME:
    print("DB_FILENAME not set in .env")
LOG_FILE = 'wigle_extract_bssid.log'

dbfile = DB_NAME
con = sqlite3.connect(dbfile)

cur = con.cursor()

out_file = 'bssid_list.h'
with open(out_file, 'w') as f:
    f.write('#ifndef BSSID_LIST_H\n')
    f.write('#define BSSID_LIST_H\n\n')
    f.write('const char* bssid_list[][6] = {\n')

    # Execute a query
    for row in cur.execute("SELECT bssid FROM ap_locations"):
        bssid = row[0]
        f.write(f'    {{0x{bssid[0:2]},0x{bssid[3:5]},0x{bssid[6:8]},0x{bssid[9:11]},0x{bssid[12:14]},0x{bssid[15:17]}}},\n')

    f.write('};\n\n')
    f.write('#endif // BSSID_LIST_H\n')



# Be sure to close the connection
con.close()