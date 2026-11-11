// Copy this file to secrets.h and fill it in. secrets.h is ignored by Git, so
// your Wi-Fi and MQTT passwords never end up on GitHub.

#pragma once

const char* WIFI_SSID     = "your-wifi-name";      // must be a 2.4 GHz network
const char* WIFI_PASSWORD = "your-wifi-password";

// Use the Pi's IP address, and reserve it in your router so it doesn't change.
// A .local hostname won't resolve here without extra mDNS code.
const char* MQTT_HOST     = "192.168.1.x";
const int   MQTT_PORT     = 1883;

// Leave both as nullptr if Mosquitto allows anonymous clients.
const char* MQTT_USER     = nullptr;
const char* MQTT_PASSWORD = nullptr;

// Optional, saves battery: give the board a fixed address so it doesn't have
// to ask the router for one on every wake. Pick an address outside the range
// your router hands out (or reserve it in the router), then remove the // from
// the five lines below and fill them in.
// #define USE_STATIC_IP
// const IPAddress STATIC_IP(192, 168, 1, 60);
// const IPAddress STATIC_GATEWAY(192, 168, 1, 1);    // your router
// const IPAddress STATIC_SUBNET(255, 255, 255, 0);
// const IPAddress STATIC_DNS(192, 168, 1, 1);        // usually the router too
