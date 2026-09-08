#!/bin/bash
# Host-native parser tests. nmea.cpp has no Arduino dependencies on purpose.
set -e
cd "$(dirname "$0")"
g++ -std=c++17 -Wall -Wextra -Werror -O1 -o /tmp/nmea_test nmea_test.cpp ../src/nmea.cpp ../src/skyview.cpp ../src/textbuf.cpp
/tmp/nmea_test




g++ -std=c++17 -Wall -Wextra -Werror -O1 -o /tmp/skyview_test skyview_test.cpp ../src/skyview.cpp ../src/textbuf.cpp ../src/nmea.cpp
/tmp/skyview_test



