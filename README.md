# GDEH0169E01 on ESP32-S3

![alt text](<docs/Screenshot 2026-02-11 204546.png>)

## Web uploader

The ESP32 hosts a small web app from SPIFFS that lets you crop, dither, and upload
photos directly to the display.

1. Build and flash the app.
2. Flash the SPIFFS image (serves /index.html, /app.js, /styles.css).
3. Open the device in a browser: http://<device-ip>/

Upload endpoint: `POST /image` with 80000 raw sp6 bytes.

### Generate and upload from Python

```bash
python tools/convert_and_upload.py tools/testme.png --dither fs --url http://<device-ip>/image
```