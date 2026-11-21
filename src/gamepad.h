#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <ihslib.h>
#include <orbis/Pad.h>

enum {
    GAMEPAD_EXIT = ORBIS_PAD_BUTTON_OPTIONS | ORBIS_PAD_BUTTON_TOUCH_PAD,
    GAMEPAD_SCREEN = ORBIS_PAD_BUTTON_L3 | ORBIS_PAD_BUTTON_R3
};

int gamepad_init(void);
void gamepad_close(void);
unsigned gamepad_prepare_stream(void);
uint32_t gamepad_buttons(void);
void gamepad_set_local(bool local);
void gamepad_attach(IHS_Session *session);
