#!/bin/bash

# Target directory
DEST_DIR="$HOME/Documents/Arduino/libraries/JBHaMqttDiscovery"

echo "Copying files to $DEST_DIR..."

# Remove the old copy so deleted or renamed files don't linger
rm -rf "${DEST_DIR:?}"

# Create the destination directory
mkdir -p "$DEST_DIR"

# Copy library.properties and keywords.txt if they exist
[ -f "library.properties" ] && cp "library.properties" "$DEST_DIR/"
[ -f "keywords.txt" ] && cp "keywords.txt" "$DEST_DIR/"

# Copy src directory
if [ -d "src" ]; then
    cp -R src/* "$DEST_DIR/"
    # Remove main.cpp if it exists in the destination (often used for testing in PIO)
    [ -f "$DEST_DIR/main.cpp" ] && rm "$DEST_DIR/main.cpp"
fi

# Copy examples directory
if [ -d "examples" ]; then
    cp -R examples "$DEST_DIR/"
fi

echo "Done!"
