/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Opt-in diagnostic only. The caller sends the existing Asahi m1n1
 * ASCCrashLogEndpoint.crash_soft() command (EP1, 0x40) once, after AP sleep
 * has remained unacknowledged for five seconds. Never extend the deadline.
 * A requested dump permanently disqualifies this session from clean teardown.
 */
struct dcpext_quiesce_dump {
	u64 since;
	bool waiting;
	bool sent;
};

static int dcpext_quiesce_dump_tick(struct dcpext_quiesce_dump *d,
				  bool pending, u64 now, u64 deadline)
{
	if (d->sent)
		return 0;
	if (!pending) {
		d->waiting = false;
		return 0;
	}
	if (!d->waiting) {
		d->since = now;
		d->waiting = true;
	}
	if (now < d->since)
		return -ERANGE;
	if (now >= deadline || deadline - now < 1000 || now - d->since < 5000)
		return 0;
	d->sent = true; /* Set before send: uncertain transport must not retry. */
	return 1;
}
