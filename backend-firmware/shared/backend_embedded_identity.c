#include "backend_identity.h"

#if (defined(FOF_BACKEND_UPLINK) + defined(FOF_BACKEND_SCANNER)) != 1
#error "exactly one backend image must be selected"
#endif

#if defined(FOF_BACKEND_UPLINK)
#define BACKEND_EMBEDDED_KIND BACKEND_IMAGE_UPLINK
#define BACKEND_EMBEDDED_TARGET FOF_BACKEND_UPLINK_TARGET
#define BACKEND_EMBEDDED_PROJECT FOF_BACKEND_UPLINK_PROJECT
#define BACKEND_EMBEDDED_CRC UINT32_C(0xF08BCDE4)
#else
#define BACKEND_EMBEDDED_KIND BACKEND_IMAGE_SCANNER
#define BACKEND_EMBEDDED_TARGET FOF_BACKEND_SCANNER_TARGET
#define BACKEND_EMBEDDED_PROJECT FOF_BACKEND_SCANNER_PROJECT
#define BACKEND_EMBEDDED_CRC UINT32_C(0x9DD382FF)
#endif

const backend_embedded_identity_record_t fof_backend_embedded_identity
    __attribute__((used, aligned(4), section(".fof_backend_identity"))) = {
        .magic = FOF_BACKEND_IDENTITY_MAGIC,
        .schema = FOF_BACKEND_IDENTITY_SCHEMA,
        .image_kind = BACKEND_EMBEDDED_KIND,
        .target = BACKEND_EMBEDDED_TARGET,
        .project = BACKEND_EMBEDDED_PROJECT,
        .hardware = FOF_BACKEND_HARDWARE,
        .version = FOF_VERSION_BACKEND,
        .crc32 = BACKEND_EMBEDDED_CRC,
    };
