// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <linux/export.h>
#include <linux/compiler.h>
#include <linux/smp.h>
#include "m4_dcpext_audio.h"

/* Built-in controller instances have kernel lifetime; callbacks check liveness
 * under the instance route lock before touching firmware or session buffers.
 */
static const struct m4_dcpext_audio_ops *audio_ops[2];
void m4_dcpext_audio_register(unsigned int port, const struct m4_dcpext_audio_ops *ops)
{
	if (port < 2)
		smp_store_release(&audio_ops[port], ops);
}
const struct m4_dcpext_audio_ops *m4_dcpext_audio_get(unsigned int port)
{
	return port < 2 ? smp_load_acquire(&audio_ops[port]) : NULL;
}
EXPORT_SYMBOL_GPL(m4_dcpext_audio_get);
