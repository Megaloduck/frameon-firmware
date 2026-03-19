#pragma once
void clock_init(const char *ntpServer);
void clock_draw();   // call every loop() when mode == MODE_CLOCK
