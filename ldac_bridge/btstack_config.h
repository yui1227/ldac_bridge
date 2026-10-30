// BTstack configuration for ldac_bridge (Classic only, A2DP Source)

#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

// Port related features
#define HAVE_ASSERT
#define HAVE_BTSTACK_STDIN
#define HAVE_MALLOC
#define HAVE_POSIX_FILE_IO
#define HAVE_POSIX_TIME

// BTstack features that can be enabled
#define ENABLE_CLASSIC
#define ENABLE_LOG_ERROR
#define ENABLE_LOG_INFO

// Let the application pick the codec (LDAC) after all remote capabilities are known
#define ENABLE_A2DP_EXPLICIT_CONFIG

// Allow large ACL packets so that 2-DH5/3-DH5 can carry LDAC 990 kbps
#define HCI_ACL_PAYLOAD_SIZE (1691 + 4)
#define HCI_INCOMING_PRE_BUFFER_SIZE 14

#define NVM_NUM_LINK_KEYS 16

#endif
