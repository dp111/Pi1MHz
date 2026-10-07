/* Host tests for rpi/h264dec.c: h264dec_shutdown, the decoder's half of a
 * kernel.now (videoplayer_shutdown), and h264dec_reset, its warm restart
 * on a Beeb reset.
 *
 * The VideoCore runs on across the jump, so what matters is the ORDER in
 * which the decoder lets go, and what it refuses to give back when a step
 * fails: memory the VideoCore may still hold must never return to the GPU
 * pool.  An SMEM FREE is only queued - it has no answer - so the proof that
 * the VideoCore has dropped its imports is its answer to the SMEM service's
 * CLOSE, which follows the FREEs on the same service.  The input staging
 * buffers may go back only after that answer, and only if everything
 * before it succeeded; otherwise they are leaked and the call says so (the
 * player then leaks the frame buffers too).  Ports are disabled by their
 * own state, including after a bring-up that failed half way.
 *
 * Real file, stub MMAL/SMEM/VCHIQ clients that record each call.
 */
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "h264dec_stubs.h"
#include "vchiq.h"
#include "vcsm.h"
#include "mmal_vc.h"
#include "h264dec.h"

/* ---- the stub platform -------------------------------------------------- */

static char log_[512];                 /* the calls, in order, ';'-separated */
static bool fail_destroy, fail_smem_close, fail_output_info, fail_port_disable,
            fail_port_enable;
static uint32_t next_phys = 0x1000000u, next_handle = 0x100u;

static void ev(const char *what)
{
   size_t n = strlen(log_);
   snprintf(log_ + n, sizeof log_ - n, "%s;", what);
}

uint32_t RPI_GetSystemTime(void) { static uint32_t t; return t += 10u; }

bool vchiq_init(void) { return true; }
uint32_t vchiq_alloc_shared(uint32_t size, uint32_t *handle)
{
   (void)size;
   *handle = next_handle++;
   uint32_t p = next_phys;
   next_phys += 0x100000u;
   return p;
}
void vchiq_free_shared(uint32_t handle) { (void)handle; ev("staging_free"); }
void vchiq_poll(void) { }

bool vcsm_init(void) { return true; }
uint32_t vcsm_import(uint32_t busaddr, uint32_t size, const char *name)
{ (void)busaddr; (void)size; (void)name; return next_handle++; }
bool vcsm_free(uint32_t handle) { (void)handle; ev("smem_free"); return true; }
bool vcsm_deinit(void) { ev("smem_close"); return !fail_smem_close; }

bool mmal_vc_init(const mmal_vc_client_callbacks_t *callbacks) { (void)callbacks; return true; }
bool mmal_vc_deinit(void) { ev("mmal_close"); return true; }
bool mmal_vc_component_create(const char *name, uint32_t *handle, uint32_t *inputs, uint32_t *outputs)
{ (void)name; *handle = 7u; *inputs = *outputs = 1u; return true; }
bool mmal_vc_component_enable(uint32_t handle) { (void)handle; return true; }
bool mmal_vc_component_disable(uint32_t handle) { (void)handle; ev("comp_disable"); return true; }
bool mmal_vc_component_destroy(uint32_t handle) { (void)handle; ev("comp_destroy"); return !fail_destroy; }
bool mmal_vc_port_info_get(uint32_t component, uint32_t port_type, uint32_t index, mmal_vc_port_t *port)
{
   if (port_type == MMAL_PORT_TYPE_OUTPUT && fail_output_info)
      return false;
   memset(port, 0, sizeof *port);
   port->component = component;
   port->type = port_type;
   port->index = index;
   return true;
}
bool mmal_vc_port_set_format(mmal_vc_port_t *port) { (void)port; return true; }
bool mmal_vc_port_set_zero_copy(mmal_vc_port_t *port) { (void)port; return true; }
bool mmal_vc_port_enable(mmal_vc_port_t *port) { (void)port; return !fail_port_enable; }
bool mmal_vc_port_disable(mmal_vc_port_t *port)
{
   ev(port->type == MMAL_PORT_TYPE_INPUT ? "in_disable" :
      port->type == MMAL_PORT_TYPE_OUTPUT ? "out_disable" : "ctl_disable");
   return !fail_port_disable;
}
bool mmal_vc_port_flush(mmal_vc_port_t *port) { (void)port; return true; }
bool mmal_vc_port_parameter_set(const mmal_vc_port_t *port, const void *param, uint32_t size)
{ (void)port; (void)param; (void)size; return true; }
bool mmal_vc_submit_buffer(mmal_vc_port_t *port, mmal_vc_buffer_t *buf)
{ (void)port; (void)buf; return true; }
void mmal_vc_poll(void) { }

static void frame(uint32_t phys, int64_t pts, bool eos) { (void)phys; (void)pts; (void)eos; }

/* ---- helpers ------------------------------------------------------------ */

static int failures, checks;

#define CHECK(cond, ...) do { \
      checks++; \
      if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                     printf(__VA_ARGS__); printf("\n"); } \
   } while (0)

/* Position of the first call named what in the log, -1 if never made. */
static int at(const char *what)
{
   char key[40];
   snprintf(key, sizeof key, "%s;", what);
   const char *p = strstr(log_, key);
   return p ? (int)(p - log_) : -1;
}
static int count(const char *what)
{
   char key[40];
   int n = 0;
   snprintf(key, sizeof key, "%s;", what);
   for (const char *p = log_; (p = strstr(p, key)) != NULL; p++) n++;
   return n;
}

static void reset(void)
{
   log_[0] = '\0';
   fail_destroy = fail_smem_close = fail_output_info = fail_port_disable = false;
   fail_port_enable = false;
}

/* A running decoder with two frame buffers registered. */
static void bring_up(void)
{
   CHECK(h264dec_init(768u, 576u, frame), "init failed");
   CHECK(h264dec_add_output_buffer(0x2000000u, 768u * 576u * 3u / 2u), "out 0");
   CHECK(h264dec_add_output_buffer(0x3000000u, 768u * 576u * 3u / 2u), "out 1");
   log_[0] = '\0';
}

/* ---- tests -------------------------------------------------------------- */

static void test_clean(void)
{
   reset();
   bring_up();
   CHECK(h264dec_shutdown(), "clean shutdown reported failure: %s", log_);
   CHECK(at("in_disable") >= 0 && at("out_disable") >= 0, "ports not disabled: %s", log_);
   CHECK(at("out_disable") < at("comp_destroy"), "destroyed before the ports were down: %s", log_);
   CHECK(count("smem_free") == 4, "%d imports freed, want 4: %s", count("smem_free"), log_);
   CHECK(at("comp_destroy") < at("smem_free"), "imports freed under a live component: %s", log_);
   CHECK(count("staging_free") == 2, "%d staging buffers returned: %s", count("staging_free"), log_);
   CHECK(at("smem_close") >= 0 && at("smem_close") < at("staging_free"),
         "staging returned before the SMEM close was answered: %s", log_);
   CHECK(at("mmal_close") >= 0, "MMAL service not closed: %s", log_);
   CHECK(!h264dec_running(), "still running");
   /* and it comes up again in the same kernel */
   bring_up();
   CHECK(h264dec_running(), "no second bring-up after a clean shutdown");
   (void)h264dec_shutdown();
}

/* The SMEM close goes unanswered: no proof the FREEs were done, so the
   staging buffers stay where they are and the caller hears about it. */
static void test_smem_close_unanswered(void)
{
   reset();
   bring_up();
   fail_smem_close = true;
   CHECK(!h264dec_shutdown(), "reported clean without the SMEM close answered");
   CHECK(count("staging_free") == 0, "staging returned without proof: %s", log_);
   CHECK(at("mmal_close") >= 0, "MMAL not closed after the SMEM failure: %s", log_);
   reset();
   CHECK(!h264dec_init(768u, 576u, frame), "a second set stranded after a failed shutdown");
}

static void test_destroy_fails(void)
{
   reset();
   bring_up();
   fail_destroy = true;
   CHECK(!h264dec_shutdown(), "reported clean with the component alive");
   CHECK(count("staging_free") == 0, "staging returned under a live component: %s", log_);
   CHECK(at("smem_close") >= 0 && at("mmal_close") >= 0, "services not closed: %s", log_);
}

/* Bring-up failed after the input port was enabled (here: the output port
   cannot be described).  Not running - but the input port is up, and must
   come down before the component goes. */
static void test_half_up(void)
{
   reset();
   fail_output_info = true;
   CHECK(!h264dec_init(768u, 576u, frame), "init succeeded without an output port");
   CHECK(!h264dec_running(), "running");
   fail_output_info = false;
   log_[0] = '\0';
   CHECK(h264dec_shutdown(), "half-up shutdown reported failure: %s", log_);
   CHECK(at("in_disable") >= 0 && at("in_disable") < at("comp_destroy"),
         "input port left enabled: %s", log_);
   CHECK(at("out_disable") < 0, "an output port never enabled was disabled: %s", log_);
   CHECK(count("staging_free") == 2, "staging not returned: %s", log_);
}

static void test_nothing_up(void)
{
   reset();
   CHECK(h264dec_shutdown(), "shutdown with nothing up failed");
   CHECK(at("comp_destroy") < 0 && count("staging_free") == 0, "work done with nothing up: %s", log_);
}

/* A warm restart lets go of the output imports (the caller frees the frame
   buffers next) and keeps the decoder up. */
static void test_reset_clean(void)
{
   reset();
   bring_up();
   CHECK(h264dec_reset(), "clean reset reported failure: %s", log_);
   CHECK(at("in_disable") >= 0, "input port not disabled: %s", log_);
   CHECK(count("smem_free") == 2, "%d output imports freed, want 2: %s", count("smem_free"), log_);
   CHECK(h264dec_running(), "not running after a clean reset");
}

/* A port the VideoCore will not disable may still hold buffers: nothing is
   handed back, the caller is told to leak, and the decoder is condemned. */
static void test_reset_disable_fails(void)
{
   reset();
   bring_up();
   for (int i = 0; i < H264DEC_INPUT_BUFFERS; i++) {      /* both AUs with the VC */
      uint32_t max = 0;
      CHECK(h264dec_get_input_buffer(&max) != NULL, "no input buffer %d", i);
      CHECK(h264dec_submit_input(100u, i, false), "submit %d", i);
   }
   fail_port_disable = true;
   CHECK(!h264dec_reset(), "reset reported clean with the port still up");
   CHECK(count("smem_free") == 0, "imports freed under a live port: %s", log_);
   CHECK(!h264dec_running(), "still running after a failed reset");
   uint32_t free_slots = 9u;
   bool eos = false;
   h264dec_input_state(&free_slots, &eos);
   (void)eos;
   CHECK(free_slots == 0u, "%u input slots handed back as free", free_slots);
   reset();
   CHECK(!h264dec_init(768u, 576u, frame), "a second set brought up after a failed reset");
   /* a kernel.now still tries to take it all down */
   log_[0] = '\0';
   CHECK(h264dec_shutdown(), "shutdown after a failed reset: %s", log_);
   CHECK(at("in_disable") >= 0 && at("comp_destroy") >= 0, "not taken down: %s", log_);
}

/* The re-enable at the end of a reset failed, so the input port is down: a
   second reset (two BREAKs) must not try to disable it - the VideoCore
   refuses that - and so must not condemn a healthy decoder. */
static void test_reset_input_down(void)
{
   reset();
   bring_up();
   fail_port_enable = true;
   CHECK(h264dec_reset(), "reset failed");
   fail_port_enable = false;
   fail_port_disable = true;          /* the VC's answer for a port already down */
   log_[0] = '\0';
   CHECK(h264dec_reset(), "second reset condemned the decoder: %s", log_);
   CHECK(at("in_disable") < 0, "disabled a port that was down: %s", log_);
   CHECK(h264dec_running(), "not running");
}

/* Each case in a child: h264dec.c keeps its state in statics. */
static void run(void (*fn)(void), const char *name)
{
   fflush(stdout);
   pid_t pid = fork();
   if (pid == 0) {
      failures = 0;
      fn();
      fflush(stdout);
      _exit(failures > 255 ? 255 : failures);
   }
   int status = 0;
   (void)waitpid(pid, &status, 0);
   checks++;
   if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      failures++;
      printf("FAIL case %s\n", name);
   }
}

int main(void)
{
   run(test_clean, "clean");
   run(test_smem_close_unanswered, "smem_close_unanswered");
   run(test_destroy_fails, "destroy_fails");
   run(test_half_up, "half_up");
   run(test_nothing_up, "nothing_up");
   run(test_reset_clean, "reset_clean");
   run(test_reset_disable_fails, "reset_disable_fails");
   run(test_reset_input_down, "reset_input_down");
   printf("%d checks, %d failed\n", checks, failures);
   if (failures == 0)
      printf("H264DEC TESTS PASSED\n");
   return failures ? 1 : 0;
}
