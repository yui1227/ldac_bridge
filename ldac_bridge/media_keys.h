// Inject Windows media keys (routed by Windows to the active media session)

#ifndef MEDIA_KEYS_H
#define MEDIA_KEYS_H

typedef enum {
    MEDIA_KEY_PLAY_PAUSE,
    MEDIA_KEY_STOP,
    MEDIA_KEY_NEXT,
    MEDIA_KEY_PREVIOUS,
} media_key_t;

void media_key_send(media_key_t key);

#endif
