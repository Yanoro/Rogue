#!/usr/bin/env bash

# Default to current directory if no argument is provided
SEARCH_DIR="${1:-.}"

if [ ! -d "$SEARCH_DIR" ]; then
    echo "Error: Directory '$SEARCH_DIR' not found."
    exit 1
fi

echo "Searching for Aseprite files in: $SEARCH_DIR"

# Find all .aseprite or .ase files recursively in the specified directory
find "$SEARCH_DIR" -type f \( -name "*.aseprite" -o -name "*.ase" \) | while read -r file; do
    # Remove the extension to get the base name
    base_name="${file%.*}"
    
    echo "Exporting $file..."
    
    # Run aseprite in batch mode
    # --list-tags: Exports the animation tags (Idle, Run, etc) into the JSON
    # --data: The metadata file
    # --sheet: The sprite sheet image
    # --format: Format for the JSON data (hash is generally easier to lookup by frame name if needed, but array is standard)
    
    aseprite -b "$file" \
        --data "$base_name.json" \
        --format json-array \
        --sheet "$base_name.png" \
        --list-tags
        
    if [ $? -eq 0 ]; then
        echo "  -> Generated $base_name.png & $base_name.json"
    else
        echo "  -> FAILED to export $file"
    fi
done

echo "Export complete."
