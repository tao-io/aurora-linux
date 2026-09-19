# Fragment-only notifier flush control

Kernel754cccf0ab3b with the attached temporary patch was tested in
20260919T160550445353Z-factorio-fragment-flush. All five captured frames and
ten API query counts match exactly. Whole submission GPU spans
6.122..6.313 ms: no meaningful improvement from normal ~6.2 ms. Unlike the
previous broad compute-overlap experiment, this runs the default dependency
path and sets only FS+bdc on every scene. Last TA flush remains unchanged.

The patch is removed from active source. Restore module868fa4066a82dd9654544df8bbbc57513c5b05bc592b1ae8723b3af34f915d06
before further controlled measurements. No full CTS or live-menu claim.
