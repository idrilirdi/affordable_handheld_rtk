#!/bin/bash
# Host-native parser tests. nmea.cpp has no Arduino dependencies on purpose.
set -e
cd "$(dirname "$0")"
g++ -std=c++17 -Wall -Wextra -Werror -O1 -o /tmp/nmea_test nmea_test.cpp ../src/nmea.cpp ../src/skyview.cpp ../src/textbuf.cpp
/tmp/nmea_test

g++ -std=c++17 -Wall -Wextra -Werror -O1 -o /tmp/rtcm_test rtcm_test.cpp ../src/rtcm.cpp
/tmp/rtcm_test

g++ -std=c++17 -Wall -Wextra -Werror -O1 -o /tmp/diag_test diag_test.cpp ../src/diag.cpp ../src/rtcm.cpp ../src/skyview.cpp ../src/textbuf.cpp
/tmp/diag_test

g++ -std=c++17 -Wall -Wextra -Werror -O1 -o /tmp/fieldstats_test fieldstats_test.cpp ../src/fieldstats.cpp ../src/textbuf.cpp
/tmp/fieldstats_test

g++ -std=c++17 -Wall -Wextra -Werror -O1 -o /tmp/skyview_test skyview_test.cpp ../src/skyview.cpp ../src/textbuf.cpp ../src/nmea.cpp
/tmp/skyview_test

g++ -std=c++17 -Wall -Wextra -Werror -O1 -o /tmp/status_led_test status_led_test.cpp ../src/status_led.cpp ../src/scatter.cpp ../src/nmea.cpp ../src/skyview.cpp ../src/textbuf.cpp
/tmp/status_led_test

g++ -std=c++17 -Wall -Wextra -Werror -O1 -o /tmp/points_test points_test.cpp ../src/points_format.cpp ../src/nmea.cpp ../src/skyview.cpp ../src/textbuf.cpp
/tmp/points_test

g++ -std=c++17 -Wall -Wextra -Werror -O1 -o /tmp/uplink_split_test uplink_split_test.cpp ../src/uplink_split.cpp
/tmp/uplink_split_test
