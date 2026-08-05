/*
    mmal_vc.h - minimal MMAL-over-VCHIQ client for bare metal

    MMAL (Multi-Media Abstraction Layer) is the RPC protocol the Linux
    userland uses to drive the VideoCore media components - among them
    "ril.video_decode", the hardware H264 decoder. The protocol is a thin
    message layer on top of a VCHIQ service called 'mmal'; this file
    reproduces exactly the wire structures from raspberrypi/userland
    interface/mmal/vc/mmal_vc_msgs.h (BSD-3-Clause, Broadcom Europe Ltd)
    with host pointers replaced by uint32_t, which is what actually
    crosses the wire on a 32-bit ARM anyway.

    The API here is the smallest possible subset: create/enable a
    component, get/set port formats, enable ports, exchange buffers.
    Every port runs in zero-copy mode: payload never crosses the wire at
    all. A buffer travels as a VideoCore memory handle (see vcsm.h) and
    the component reads or writes the caller's VC-heap memory itself -
    the ARM never touches the pixels.
*/

#ifndef RPI_MMAL_VC_H
#define RPI_MMAL_VC_H

#include <stdint.h>
#include <stdbool.h>

/* MMAL fourccs are little-endian, unlike VCHIQ's */
#define MMAL_FOURCC(a,b,c,d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

#define MMAL_MAGIC              MMAL_FOURCC('m','m','a','l')

#define MMAL_ENCODING_H264      MMAL_FOURCC('H','2','6','4')
#define MMAL_ENCODING_I420      MMAL_FOURCC('I','4','2','0')

#define MMAL_STATUS_SUCCESS     0u

/* Common port parameters (mmal_parameters_common.h; group COMMON = 0) */
#define MMAL_PARAMETER_ZERO_COPY 4u  /* MMAL_PARAMETER_BOOLEAN_T */

/* Buffer header flags (mmal_buffer.h) */
#define MMAL_BUFFER_HEADER_FLAG_EOS          (1u << 0)
#define MMAL_BUFFER_HEADER_FLAG_FRAME_START  (1u << 1)
#define MMAL_BUFFER_HEADER_FLAG_FRAME_END    (1u << 2)
#define MMAL_BUFFER_HEADER_FLAG_KEYFRAME     (1u << 3)
#define MMAL_BUFFER_HEADER_FLAG_CONFIG       (1u << 5)
#define MMAL_BUFFER_HEADER_FLAG_TRANSMISSION_FAILED (1u << 10)

/* Events (mmal_events.h) */
#define MMAL_EVENT_ERROR            MMAL_FOURCC('E','R','R','O')
#define MMAL_EVENT_EOS              MMAL_FOURCC('E','E','O','S')
#define MMAL_EVENT_FORMAT_CHANGED   MMAL_FOURCC('E','F','C','H')

/* Port and elementary-stream types */
#define MMAL_PORT_TYPE_CONTROL  1u
#define MMAL_PORT_TYPE_INPUT    2u
#define MMAL_PORT_TYPE_OUTPUT   3u

#define MMAL_ES_TYPE_VIDEO      3u

#define MMAL_TIME_UNKNOWN       INT64_MIN

/* ------------------------------------------------------------------ */
/* Wire structures (pointers become uint32_t handles/cookies)         */
/* ------------------------------------------------------------------ */

typedef struct {
    int32_t num;
    int32_t den;
} mmal_rational_t;

typedef struct {                     /* MMAL_VIDEO_FORMAT_T, 44 bytes */
    uint32_t width;
    uint32_t height;
    int32_t  crop_x, crop_y, crop_width, crop_height;
    mmal_rational_t frame_rate;
    mmal_rational_t par;
    uint32_t color_space;
} mmal_video_format_t;

typedef union {                      /* MMAL_ES_SPECIFIC_FORMAT_T, 44 bytes */
    mmal_video_format_t video;
    uint32_t raw[11];
} mmal_es_specific_format_t;

typedef struct {                     /* MMAL_ES_FORMAT_T, 32 bytes */
    uint32_t type;                   /* MMAL_ES_TYPE_x */
    uint32_t encoding;               /* fourcc */
    uint32_t encoding_variant;
    uint32_t es;                     /* pointer on the wire - ignored by VC */
    uint32_t bitrate;
    uint32_t flags;
    uint32_t extradata_size;
    uint32_t extradata;              /* pointer on the wire - ignored by VC */
} mmal_es_format_t;

typedef struct {                     /* MMAL_PORT_T, 64 bytes */
    uint32_t priv;
    uint32_t name;
    uint32_t type;
    uint16_t index;
    uint16_t index_all;
    uint32_t is_enabled;
    uint32_t format;                 /* pointer on the wire */
    uint32_t buffer_num_min;
    uint32_t buffer_size_min;
    uint32_t buffer_alignment_min;
    uint32_t buffer_num_recommended;
    uint32_t buffer_size_recommended;
    uint32_t buffer_num;
    uint32_t buffer_size;
    uint32_t component;
    uint32_t userdata;
    uint32_t capabilities;
} mmal_port_wire_t;

typedef struct {                     /* MMAL_BUFFER_HEADER_T, 56 bytes */
    uint32_t next;
    uint32_t priv;
    uint32_t cmd;
    uint32_t data;
    uint32_t alloc_size;
    uint32_t length;
    uint32_t offset;
    uint32_t flags;
    int64_t  pts;
    int64_t  dts;
    uint32_t type;
    uint32_t user_data;
} mmal_buffer_header_wire_t;

typedef struct {                     /* MMAL_BUFFER_HEADER_TYPE_SPECIFIC_T */
    uint32_t planes;
    uint32_t offset[4];
    uint32_t pitch[4];
    uint32_t flags;
} mmal_buffer_ts_wire_t;

typedef struct {                     /* MMAL_PARAMETER_HEADER_T */
    uint32_t id;
    uint32_t size;                   /* including this header */
} mmal_parameter_header_t;

/* ------------------------------------------------------------------ */
/* Worker messages (mmal_vc_msgs.h)                                   */
/* ------------------------------------------------------------------ */

#define MMAL_WORKER_VER_MAJOR    16
#define MMAL_WORKER_VER_MINIMUM  10

#define MMAL_WORKER_MAX_MSG_LEN  512
#define MMAL_VC_SHORT_DATA       128
#define MMAL_WORKER_EVENT_SPACE  256
#define MMAL_FORMAT_EXTRADATA_MAX_SIZE 128
#define MMAL_WORKER_PORT_PARAMETER_SPACE 96

enum {
    MMAL_WORKER_QUIT = 1,
    MMAL_WORKER_SERVICE_CLOSED,
    MMAL_WORKER_GET_VERSION,
    MMAL_WORKER_COMPONENT_CREATE,
    MMAL_WORKER_COMPONENT_DESTROY,
    MMAL_WORKER_COMPONENT_ENABLE,
    MMAL_WORKER_COMPONENT_DISABLE,
    MMAL_WORKER_PORT_INFO_GET,
    MMAL_WORKER_PORT_INFO_SET,
    MMAL_WORKER_PORT_ACTION,
    MMAL_WORKER_BUFFER_FROM_HOST,
    MMAL_WORKER_BUFFER_TO_HOST,
    MMAL_WORKER_GET_STATS,
    MMAL_WORKER_PORT_PARAMETER_SET,
    MMAL_WORKER_PORT_PARAMETER_GET,
    MMAL_WORKER_EVENT_TO_HOST,
    MMAL_WORKER_GET_CORE_STATS_FOR_PORT,
    MMAL_WORKER_OPAQUE_ALLOCATOR,
    MMAL_WORKER_CONSUME_MEM,
    MMAL_WORKER_LMK,
    MMAL_WORKER_OPAQUE_ALLOCATOR_DESC,
    MMAL_WORKER_DRM_GET_LHS32,
    MMAL_WORKER_DRM_GET_TIME,
    MMAL_WORKER_BUFFER_FROM_HOST_ZEROLEN,
    MMAL_WORKER_PORT_FLUSH,
    MMAL_WORKER_HOST_LOG,
};

enum {
    MMAL_WORKER_PORT_ACTION_ENABLE = 1,
    MMAL_WORKER_PORT_ACTION_DISABLE,
    MMAL_WORKER_PORT_ACTION_FLUSH,
    MMAL_WORKER_PORT_ACTION_CONNECT,
    MMAL_WORKER_PORT_ACTION_DISCONNECT,
    MMAL_WORKER_PORT_ACTION_SET_REQUIREMENTS,
};

typedef struct {                     /* mmal_worker_msg_header, 24 bytes */
    uint32_t magic;                  /* MMAL_MAGIC */
    uint32_t msgid;
    uint32_t control_service;        /* VC-side cookie */
    uint32_t context;                /* our cookie, echoed in replies */
    uint32_t status;
    uint32_t dummy;                  /* pad to 8-byte multiple */
} mmal_worker_msg_header_t;

typedef struct {
    mmal_worker_msg_header_t header;
    uint32_t flags;
    uint32_t major, minor, minimum;
} mmal_worker_version_t;

typedef struct {
    mmal_worker_msg_header_t header;
    uint32_t client_component;
    char name[128];
    uint32_t pid;
} mmal_worker_component_create_t;

typedef struct {
    mmal_worker_msg_header_t header;
    uint32_t status;
    uint32_t component_handle;
    uint32_t input_num, output_num, clock_num;
} mmal_worker_component_create_reply_t;

typedef struct {
    mmal_worker_msg_header_t header;
    uint32_t component_handle;
} mmal_worker_component_handle_msg_t;   /* enable/disable/destroy */

typedef struct {
    mmal_worker_msg_header_t header;
    uint32_t status;
} mmal_worker_reply_t;

typedef struct {
    mmal_worker_msg_header_t header;
    uint32_t component_handle;
    uint32_t port_type;
    uint32_t index;
} mmal_worker_port_info_get_t;

typedef struct {
    mmal_worker_msg_header_t header;
    uint32_t component_handle;
    uint32_t port_type;
    uint32_t index;
    mmal_port_wire_t port;
    mmal_es_format_t format;
    mmal_es_specific_format_t es;
    uint8_t extradata[MMAL_FORMAT_EXTRADATA_MAX_SIZE];
} mmal_worker_port_info_set_t;

typedef struct {
    mmal_worker_msg_header_t header;
    uint32_t status;
    uint32_t component_handle;
    uint32_t port_type;
    uint32_t index;
    int32_t  found;
    uint32_t port_handle;
    mmal_port_wire_t port;
    mmal_es_format_t format;
    mmal_es_specific_format_t es;
    uint8_t extradata[MMAL_FORMAT_EXTRADATA_MAX_SIZE];
} mmal_worker_port_info_t;

typedef struct {
    mmal_worker_msg_header_t header;
    uint32_t component_handle;
    uint32_t port_handle;
    uint32_t action;
    union {
        struct {
            mmal_port_wire_t port;
        } enable;
        struct {
            uint32_t component_handle;
            uint32_t port_handle;
        } connect;
    } param;
} mmal_worker_port_action_t;

typedef struct {
    mmal_worker_msg_header_t header;
    uint32_t component_handle;
    uint32_t port_handle;
    mmal_parameter_header_t param;
    uint32_t space[MMAL_WORKER_PORT_PARAMETER_SPACE];
} mmal_worker_port_param_set_t;

typedef struct {                     /* MMAL_DRIVER_BUFFER_T */
    uint32_t magic;
    uint32_t component_handle;
    uint32_t port_handle;
    uint32_t client_context;
} mmal_driver_buffer_wire_t;

typedef struct {                     /* mmal_worker_buffer_from_host, 292 bytes */
    mmal_worker_msg_header_t header;
    mmal_driver_buffer_wire_t drvbuf;
    mmal_driver_buffer_wire_t drvbuf_ref;
    mmal_buffer_header_wire_t buffer_header;
    mmal_buffer_ts_wire_t buffer_header_type_specific;
    uint32_t is_zero_copy;
    uint32_t has_reference;
    uint32_t payload_in_message;
    uint8_t short_data[MMAL_VC_SHORT_DATA];
} mmal_worker_buffer_from_host_t;

typedef struct {                     /* mmal_worker_event_to_host */
    mmal_worker_msg_header_t header;
    uint32_t client_component;
    uint32_t port_type;
    uint32_t port_num;
    uint32_t cmd;
    uint32_t length;
    uint8_t data[MMAL_WORKER_EVENT_SPACE];
    uint32_t delayed_buffer;
} mmal_worker_event_to_host_t;

_Static_assert(sizeof(mmal_worker_msg_header_t) == 24, "msg header size");
_Static_assert(sizeof(mmal_port_wire_t) == 64, "port wire size");
_Static_assert(sizeof(mmal_es_format_t) == 32, "format wire size");
_Static_assert(sizeof(mmal_es_specific_format_t) == 44, "es wire size");
_Static_assert(sizeof(mmal_buffer_header_wire_t) == 56, "buffer header size");
/* 292 bytes of fields, padded to 296 by the int64 alignment - matches the
   userland struct, whose sizeof is what travels as the message length */
_Static_assert(sizeof(mmal_worker_buffer_from_host_t) == 296, "buffer msg size");
_Static_assert(sizeof(mmal_worker_port_info_set_t) <= MMAL_WORKER_MAX_MSG_LEN, "info set fits");
_Static_assert(sizeof(mmal_worker_port_info_t) <= MMAL_WORKER_MAX_MSG_LEN, "info fits");

/* ------------------------------------------------------------------ */
/* Client API                                                         */
/* ------------------------------------------------------------------ */

/* Our handle for a component port. The wire copies are kept here so a
   set-format or enable can echo back everything the VC last told us. */
typedef struct {
    uint32_t component;              /* component handle */
    uint32_t type;                   /* MMAL_PORT_TYPE_x */
    uint32_t index;
    uint32_t handle;                 /* port handle for buffer traffic */
    mmal_port_wire_t port;
    mmal_es_format_t format;
    mmal_es_specific_format_t es;
    uint8_t extradata[MMAL_FORMAT_EXTRADATA_MAX_SIZE];
} mmal_vc_port_t;

/* A payload buffer. 'busaddr' must point into the VC heap
   (vchiq_alloc_shared / screen_allocate_buffer) so the VideoCore can read
   and write it directly and the ARM sees it uncached. */
typedef struct mmal_vc_buffer {
    uint32_t busaddr;
    uint32_t vc_handle;              /* SMEM handle for this memory
                                        (vcsm_import) - how the VideoCore
                                        names the buffer on the wire */
    uint32_t alloc_size;
    uint32_t length;
    uint32_t offset;
    uint32_t flags;
    int64_t  pts;
    uint32_t cmd;
    mmal_vc_port_t *port;            /* which port it was last sent to */
    bool in_flight;
} mmal_vc_buffer_t;

typedef struct {
    /* A buffer previously submitted has come back (input: released;
       output: filled - length/flags/pts updated). */
    void (*buffer_done)(mmal_vc_buffer_t *buf);
    /* Component event, e.g. MMAL_EVENT_FORMAT_CHANGED. 'data' is only
       valid during the call. */
    void (*event)(uint32_t port_type, uint32_t port_num, uint32_t cmd,
                  const uint8_t *data, uint32_t length);
} mmal_vc_client_callbacks_t;

bool mmal_vc_init(const mmal_vc_client_callbacks_t *callbacks);
bool mmal_vc_component_create(const char *name, uint32_t *handle,
                              uint32_t *inputs, uint32_t *outputs);
bool mmal_vc_component_enable(uint32_t handle);
bool mmal_vc_port_info_get(uint32_t component, uint32_t port_type,
                           uint32_t index, mmal_vc_port_t *port);
bool mmal_vc_port_set_format(mmal_vc_port_t *port);
bool mmal_vc_port_enable(mmal_vc_port_t *port);
bool mmal_vc_port_disable(mmal_vc_port_t *port);
/* Discard anything in flight on the port (PORT_ACTION flush). */
bool mmal_vc_port_flush(mmal_vc_port_t *port);
bool mmal_vc_port_parameter_set(mmal_vc_port_t *port,
                                const void *param, uint32_t size);
/* Switch a port to zero-copy: payloads stay in the VC heap and buffers
   cross as memory handles (buf->vc_handle) instead of being copied.
   Required on every port used here; call between port_info_get and
   port_enable. */
bool mmal_vc_port_set_zero_copy(mmal_vc_port_t *port);
/* Submit a buffer. Input buffers carry data (length > 0, or EOS);
   output buffers are registered empty and come back filled. The buffer
   must carry a valid vc_handle. */
bool mmal_vc_submit_buffer(mmal_vc_port_t *port, mmal_vc_buffer_t *buf);
/* Pump VCHIQ and dispatch MMAL callbacks. */
void mmal_vc_poll(void);

#endif /* RPI_MMAL_VC_H */
