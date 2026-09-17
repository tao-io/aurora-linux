/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Runtime sink HPD on the already admitted USB-C route. Poll only the bound
 * controller's locked cache, not its registers. Firmware/DRM notifications run
 * in process context, outside callbacks; scanout and endpoint DMA stay owned.
 */
enum desktop_hpd_phase {
	DESKTOP_HPD_ONLINE, DESKTOP_HPD_WAIT_LOW, DESKTOP_HPD_OFFLINE,
	DESKTOP_HPD_WAIT_HIGH, DESKTOP_HPD_MODES, DESKTOP_HPD_EDID,
	DESKTOP_HPD_ROUTE_OFF, DESKTOP_HPD_ROUTE_ON,
};
static enum desktop_hpd_phase desktop_hpd_phase;
static u64 desktop_hpd_next, desktop_hpd_deadline;
static int desktop_hpd_failed_port = -1;
static unsigned int desktop_hpd_disconnects, desktop_hpd_connects;
module_param(desktop_hpd_disconnects, uint, 0400);
module_param(desktop_hpd_connects, uint, 0400);

static int native_kms_hotplug_tick(void)
{
	struct m4_dcpext_connector *connector = m4_dcpext_kms_connector(native_kms);
	struct m4_dcpext_native_mode *modes = NULL;
	u64 now = session_now(NULL), generation = 0, connector_generation;
	u32 count;
	bool hpd, disconnect = false, publish = false;
	int ret = 0, partner;

	if (now < desktop_hpd_next)
		return 0;
	desktop_hpd_next = now + 100;
	mutex_lock(&native_route_lock);
	partner = session_atc_read_partner(false);
	if (partner && partner != -ENOLINK) {
		ret = partner;
		goto unlock;
	}
	hpd = !partner && session_atc_hpd;
	if (!hpd && desktop_hpd_failed_port == (int)session_atc_port)
		desktop_hpd_failed_port = -1;
	if (desktop_hpd_deadline && now >= desktop_hpd_deadline &&
	    (desktop_hpd_phase == DESKTOP_HPD_MODES || desktop_hpd_phase == DESKTOP_HPD_EDID) &&
	    !dptx_runtime_hpd_pending && !dptx_runtime_route_pending) {
		/* Discovery failure is not lost command ownership. Send an
		 * acknowledged HPD-low and wait for a physical change instead of
		 * killing the worker or endlessly retraining an attached sink. */
		pr_warn("m4_dcpext_native: ATC%u sink discovery timed out; quiescing HPD and waiting for cable change\n",
			session_atc_port);
		desktop_hpd_failed_port = session_atc_port;
		desktop_hpd_deadline = 0;
	}
	if (desktop_hpd_failed_port == (int)session_atc_port)
		hpd = false;
	if (!hpd && native_kms_connected) {
		/* Reject commits before invalidating EDID/modes or sending the
		 * hotplug event. Connector operations cannot run under route_lock:
		 * a modeset may already hold DRM locks while waiting for it. */
		native_kms_connected = false;
		disconnect = true;
		desktop_hpd_disconnects++;
	}
	if (desktop_hpd_deadline && now >= desktop_hpd_deadline) {
		ret = -ETIMEDOUT;
		goto unlock;
	}
	/* Finish the current HPD command before reversing direction. Rapid
	 * unplug/replug must not reuse its tag or publish the old EDID. */
	if (dptx_runtime_hpd_pending || dptx_runtime_route_pending)
		goto unlock;
	if (desktop_hpd_phase == DESKTOP_HPD_ROUTE_OFF) {
		/* An acknowledged display release leaves the route object and DMA
		 * alive but disables its PHY through the AUX callback. A cable can
		 * wait here with no deadline until either port has a valid sink. */
		if (afk_display_dirty || afk_hpd_dirty || session_atc_active) {
			ret = -EUCLEAN;
			goto unlock;
		}
		desktop_hpd_deadline = 0;
		partner = session_atc_find_partner();
		if (partner < 0) {
			ret = partner == -ENOLINK ? 0 : partner;
			goto unlock;
		}
		ret = session_atc_switch_port(partner);
		if (!ret)
			ret = dpxbar_up();
		if (!ret)
			ret = dptx_runtime_set_route(true);
		if (!ret) {
			desktop_hpd_phase = DESKTOP_HPD_ROUTE_ON;
			desktop_hpd_deadline = now + 5000;
		}
		goto unlock;
	}
	if (desktop_hpd_phase == DESKTOP_HPD_ROUTE_ON) {
		desktop_hpd_phase = DESKTOP_HPD_OFFLINE;
		desktop_hpd_deadline = 0;
	}
	if (!hpd && desktop_hpd_phase != DESKTOP_HPD_OFFLINE &&
	    desktop_hpd_phase != DESKTOP_HPD_WAIT_LOW) {
		edid_native_ready = false;
		if (native_kms_powered)
			ret = m4_dcpext_native_set_power(native_client, false, 0);
		if (ret)
			goto unlock;
		native_kms_powered = false;
		native_kms_generation = 0;
		m4_dcpext_native_invalidate_sink(native_client);
		ret = dptx_runtime_set_hpd(false);
		if (ret)
			goto unlock;
		desktop_hpd_phase = DESKTOP_HPD_WAIT_LOW;
		desktop_hpd_deadline = now + 5000;
		goto unlock;
	}
	if (desktop_hpd_phase == DESKTOP_HPD_WAIT_LOW) {
		desktop_hpd_phase = DESKTOP_HPD_OFFLINE;
		desktop_hpd_deadline = 0;
		pr_info("m4_dcpext_native: sink HPD low acknowledged; connector disconnected, DMA retained\n");
	}
	if (desktop_hpd_phase == DESKTOP_HPD_OFFLINE && !hpd) {
		partner = session_atc_find_partner();
		if (partner < 0) {
			ret = partner == -ENOLINK ? 0 : partner;
			goto unlock;
		}
		if ((unsigned int)partner != session_atc_port) {
			ret = dptx_runtime_set_route(false);
			if (!ret) {
				desktop_hpd_phase = DESKTOP_HPD_ROUTE_OFF;
				desktop_hpd_deadline = now + 5000;
			}
			goto unlock;
		}
	}
	if (desktop_hpd_phase == DESKTOP_HPD_OFFLINE && hpd) {
		/* A reply to a query issued before removal still owns its tag. */
		if (edid_state.pending) {
			if (!desktop_hpd_deadline)
				desktop_hpd_deadline = now + 5000;
			goto unlock;
		}
		edid_state.received = false;
		edid_state.bytes = 0;
		m4_dcpext_native_invalidate_sink(native_client);
		/* Link shutdown parked the crossbar at rate zero. Restore the
		 * same route used before the initial HPD announcement: discovery
		 * precedes the firmware's rate/config callbacks on reconnect too. */
		if (!session_dpxbar_selected) {
			ret = dpxbar_up();
			if (ret)
				goto unlock;
		}
		ret = dptx_runtime_set_hpd(true);
		if (ret)
			goto unlock;
		desktop_hpd_phase = DESKTOP_HPD_WAIT_HIGH;
		desktop_hpd_deadline = now + 5000;
		goto unlock;
	}
	if (desktop_hpd_phase == DESKTOP_HPD_WAIT_HIGH)
		desktop_hpd_phase = DESKTOP_HPD_MODES;
	if (desktop_hpd_phase == DESKTOP_HPD_MODES || desktop_hpd_phase == DESKTOP_HPD_EDID) {
		modes = m4_dcpext_native_modes(native_client, &count, &generation);
		if (IS_ERR(modes)) {
			ret = PTR_ERR(modes) == -ENODATA ? 0 : PTR_ERR(modes);
			modes = NULL;
			goto unlock;
		}
		if (desktop_hpd_phase == DESKTOP_HPD_MODES) {
			edid_native_ready = true;
			desktop_hpd_phase = DESKTOP_HPD_EDID;
		}
		if (edid_state.received) {
			publish = true;
			native_kms_generation = 0;
			native_kms_powered = false;
		}
	}
unlock:
	mutex_unlock(&native_route_lock);
	if (disconnect) {
		m4_dcpext_connector_invalidate(connector, false);
		m4_dcpext_connector_hotplug(connector);
	}
	if (!ret && publish) {
		connector_generation = m4_dcpext_connector_invalidate(connector, true);
		ret = m4_dcpext_connector_publish_native(connector, connector_generation,
			edid_state.raw, edid_state.bytes, modes, count, generation);
		if (!ret) {
			mutex_lock(&native_route_lock);
			native_kms_connected = true;
			desktop_hpd_phase = DESKTOP_HPD_ONLINE;
			desktop_hpd_deadline = 0;
			desktop_hpd_connects++;
			mutex_unlock(&native_route_lock);
			m4_dcpext_connector_hotplug(connector);
			pr_info("m4_dcpext_native: sink HPD high, fresh EDID/modes published to DRM\n");
		}
	}
	kfree(modes);
	return ret;
}
