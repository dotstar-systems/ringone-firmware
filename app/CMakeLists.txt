# Ring•One Firmware · Copyright (c) 2026 Dotstar Systems · Apache 2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ringone_firmware VERSION 0.1.0)

target_sources(app PRIVATE
  src/main.c
  src/wifi_prov.c
  src/influx_telemetry.c
  src/mqtt_client.c
  src/sntp_sync.c
  src/watchdog.c
  src/ringone_shell.c
  ../drivers/sensors/ringone_sensors.c
  ../drivers/sensors/ringone_ppg.c
)

target_include_directories(app PRIVATE
  ../drivers/sensors
)
