#pragma once
#include <stdbool.h>
void automesh_led_start(void);
void automesh_led_set_registered(bool registered);
void automesh_led_mark_alive(void);
bool automesh_led_is_online(void);

