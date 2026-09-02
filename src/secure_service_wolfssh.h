#ifndef PI1MHZ_SECURE_SERVICE_WOLFSSH_H
#define PI1MHZ_SECURE_SERVICE_WOLFSSH_H

#include "secure_service_core.h"

const nts_secure_port *nts_pi_wolfssh_port(void);
void *nts_pi_wolfssh_context(void);
int nts_pi_wolfssh_ready(void);
int nts_pi_wolfssh_random_ready(void);
void nts_pi_wolfssh_poll(void);
void nts_pi_wolfssh_reset(void);

#endif
