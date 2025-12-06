# Another pet tracker

Ths traker is meant for use in dense areas where there are numerous wifi hotspots.
Using lora, the base station interrogates the tracker periodically to get a report of the pet tracker surrouding wifi base stations.
The tracker reports the bssids it receives along with the signal strength (rssi) of the stations.
Geolocation is done on the station side with the bssid as data using the wigle.net database.

