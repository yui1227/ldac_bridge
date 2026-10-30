// Inject Windows media keys

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "media_keys.h"

void media_key_send(media_key_t key){
    WORD vk;
    switch (key){
        case MEDIA_KEY_PLAY_PAUSE: vk = VK_MEDIA_PLAY_PAUSE; break;
        case MEDIA_KEY_STOP:       vk = VK_MEDIA_STOP;       break;
        case MEDIA_KEY_NEXT:       vk = VK_MEDIA_NEXT_TRACK; break;
        case MEDIA_KEY_PREVIOUS:   vk = VK_MEDIA_PREV_TRACK; break;
        default: return;
    }
    INPUT inputs[2];
    ZeroMemory(inputs, sizeof(inputs));
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = vk;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = vk;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, inputs, sizeof(INPUT));
}
