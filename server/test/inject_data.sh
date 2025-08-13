#!/bin/bash

# Path to the JSON dataset file
DATASET_FILE="test_dataset.json"

# Flask server endpoint
ENDPOINT="http://127.0.0.1:5000/api/scan"

# Check if the dataset file exists
if [ ! -f "$DATASET_FILE" ]; then
    echo "Error: Dataset file $DATASET_FILE not found."
    exit 1
fi

# Check if jq is installed
if ! command -v jq &> /dev/null; then
    echo "Error: jq is required to parse JSON. Please install jq."
    exit 1
fi

# Read the dataset and send each scan
while IFS= read -r line; do
    # Skip empty lines
    [ -z "$line" ] && continue
    # Send JSON data to the Flask server
    echo "Sending scan data: $line"
    response=$(curl -s -w "%{http_code}" -X POST -H "Content-Type: application/json" -d "$line" "$ENDPOINT")
    http_code=${response: -3}
    body=${response%???}
    if [ "$http_code" -eq 200 ]; then
        echo "Successfully sent scan. Response: $body"
    else
        echo "Failed to send scan. HTTP Code: $http_code, Response: $body"
    fi
    # Wait 60 seconds to simulate periodic data
    sleep 6
done < <(jq -c '.[]' "$DATASET_FILE")

echo "All scans sent."
