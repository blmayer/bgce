#!/bin/bash

# Create a test directory for configuration
mkdir -p /tmp/bgce

# Create a test configuration file
cat > /tmp/bgce/config << 'CONFIG_EOF'
# BGCE Background Configuration
background_type=image
image_path=/usr/share/backgrounds/default.jpg
scale_mode=tiled
CONFIG_EOF

# Create a simple test image if it doesn't exist
if [ ! -f /usr/share/backgrounds/default.jpg ]; then
    mkdir -p /usr/share/backgrounds
    echo "Creating test image...
" | convert -size 800x600 -pointsize 20 -fill white -draw "text 100,300 'BGCE Test Image'" gradient:blue-red /usr/share/backgrounds/default.jpg 2>/dev/null || \
    echo "Please install ImageMagick to create test image or provide your own image file"
fi

# Run the server with our test configuration
echo "Running BGCE server with test configuration..."
echo "Config path: /tmp/bgce/config"
echo "Image path: /usr/share/backgrounds/default.jpg"

# Build the project
make clean && make

# Run the server with our test configuration
LD_LIBRARY_PATH=. ./bgce --config /tmp/bgce/config
