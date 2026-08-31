/* Pi1MHz services-port wrapper for the NetTools secure ABI.
 *
 * FIQ only latches a command and publishes BUSY. All RNG, FatFs, lwIP and
 * wolfSSH work runs from the ordinary Pi1MHz poll loop.
 */
#include "Pi1MHz.h"
#include "services.h"
#include "secure_service.h"
#include "secure_service_core.h"
#include "secure_service_wolfssh.h"

#include <stdbool.h>
#include <stdint.h>

#define SEC_BUSY 0x80u

static volatile bool pending;
static volatile uint32_t pending_cp;
static volatile uint32_t pending_addr;
static bool reset_pending;
static nts_secure_service service;
/* Report only capabilities which the hardware provider has reached. */
static volatile uint8_t capability_features;
static volatile uint8_t capability_managed_ssh;

static void secure_refresh_capabilities(void)
{
    bool random_ready = nts_pi_wolfssh_random_ready();
    bool ssh_ready = nts_pi_wolfssh_ready();
    service.managed_ssh = ssh_ready;
    capability_managed_ssh = ssh_ready ? 1u : 0u;
    capability_features = (uint8_t)((random_ready ? 1u : 0u) |
        (ssh_ready ? 2u : 0u) |
        (ssh_ready && service.port != NULL &&
         service.port->ssh_password != NULL ? 4u : 0u) |
        (ssh_ready && service.port != NULL &&
         service.port->sftp_open != NULL ? 8u : 0u));
}

_Static_assert(NTS_SEC_CAPS == SERVICE_CMD_SECURE_FIRST,
               "secure service first command mismatch");
_Static_assert(NTS_SEC_SFTP_CLOSE == SERVICE_CMD_SECURE_LAST,
               "secure service command exceeds reserved range");

static void secure_write_capabilities(uint8_t *command)
{
    command[1] = 1u;
    command[2] = 1u;
    command[3] = capability_features;
    command[4] = 0xB8u;
    command[5] = 0x88u;
    command[6] = capability_managed_ssh;
    command[7] = 1u; /* wolfSSH/wolfCrypt provider */
    command[8] = (uint8_t)'N';
    command[9] = (uint8_t)'T';
    command[10] = (uint8_t)'S';
}

/* Real hardware bring-up trace: shares the net_service stage-marker byte
   (fixed command page + 0xFF, beyond NET_IO_MAX so no command payload can
   reach it) so an SSH capability-probe timeout, which waits via the same
   net_dispatch/net_debug_stage path, reports which of these two services
   last touched it. 0x8x distinguishes this service from net_service's 1-6
   range. Harmless in a non-debug build - one extra store. */
static void secure_debug_mark(uint8_t stage)
{
   uint32_t p = (DISC_RAM_BASE | 0xFF0000u | (0xF0u << 8)) + 0xFFu;
   Pi1MHz->JIM_ram[p] = stage;
}

void secure_service_command(uint32_t command_pointer, uint32_t addr,
                            uint8_t data)
{
    (void)data;
    secure_debug_mark(0x81u);
    /* Capability discovery is a fixed memory reply and is deliberately
       independent of the main poll table. This also distinguishes a missing
       command route from a stalled secure provider on physical hardware. */
    if (Pi1MHz->JIM_ram[command_pointer] == NTS_SEC_CAPS) {
        secure_debug_mark(0x82u);
        secure_write_capabilities(&Pi1MHz->JIM_ram[command_pointer]);
        secure_debug_mark(0x83u);
        Pi1MHz_MemoryWrite(addr, NTS_OK);
        secure_debug_mark(0x84u);
        return;
    }
    secure_debug_mark(0x85u);
    /* The 8-bit host issues mailbox operations synchronously, so there is at
       most one live caller. Always latch the newest command, like the native
       net service does. In particular this prevents a command arriving
       around host-reset reinitialisation from being left behind with a BUSY
       result after an older pending request is retired. */
    pending_cp = command_pointer;
    pending_addr = addr;
    pending = true;
    Pi1MHz_MemoryWrite(addr, SEC_BUSY);
}

static void secure_poll(void)
{
    if (reset_pending) {
        nts_pi_wolfssh_reset();
        service.port = nts_pi_wolfssh_port();
        service.opaque = nts_pi_wolfssh_context();
        secure_refresh_capabilities();
        reset_pending = false;
    }

    nts_pi_wolfssh_poll();
    secure_refresh_capabilities();
    if (pending) {
        uint32_t cp = pending_cp;
        uint32_t addr = pending_addr;
        uint8_t result;
        if (cp < DISC_RAM_BASE) {
            result = NTS_ERR_PARAM;
        } else {
            result = nts_secure_dispatch(
                &service, &Pi1MHz->JIM_ram[cp],
                &Pi1MHz->JIM_ram[DISC_RAM_BASE], DISC_RAM_SIZE);
        }
        pending = false;
        Pi1MHz_MemoryWrite(addr, result);
    }
}

void secure_service_init(uint8_t instance, uint8_t address)
{
    (void)instance;
    (void)address;
    /* Defer provider reset to the ordinary poll context. Do not clear a
       pending command here: the services callback is already live and could
       have published BUSY immediately before or during this initializer.
       Dropping that command would strand the host forever. */
    reset_pending = true;
    /* The services port routes this fixed ABI range directly. Keep the
       registry claim for compatibility and diagnostics, but never suppress
       the poller if reset-time registry state rejects a renewal. */
    (void)services_register(SERVICE_CMD_SECURE_FIRST,
                            SERVICE_CMD_SECURE_LAST,
                            secure_service_command);
    Pi1MHz_Register_Poll(secure_poll, "secure");
}
