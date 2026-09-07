// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Apple AOP accelerometer and gyroscope driver
//!
//! Copyright (C) The Asahi Linux Contributors

use core::{
    marker::{PhantomData, PhantomPinned},
    ptr,
    sync::atomic::{AtomicI32, AtomicU64, Ordering},
};

use kernel::{
    bindings, c_str,
    device::{self, Core},
    module_platform_driver, of, platform,
    prelude::*,
    soc::apple::aop::{EPICService, FakehidListener, AOP},
    str::CString,
    sync::{aref::ARef, Arc},
    types::ForeignOwnable,
    ThisModule,
};

const EPIC_SUBTYPE_SET_PROPERTY: u16 = 4;
const REPORT_INTERVAL_US: u32 = 20_000;
const MOTION_REPORT_LEN: usize = 22;

#[derive(Clone, Copy, PartialEq)]
enum MotionKind {
    Accel,
    Gyro,
}

struct MotionConfig {
    kind: MotionKind,
    name: &'static CStr,
    iio_type: u32,
}

const ACCEL_CONFIG: MotionConfig = MotionConfig {
    kind: MotionKind::Accel,
    name: c"aop-accelerometer",
    iio_type: bindings::iio_chan_type_IIO_ACCEL,
};

const GYRO_CONFIG: MotionConfig = MotionConfig {
    kind: MotionKind::Gyro,
    name: c"aop-gyroscope",
    iio_type: bindings::iio_chan_type_IIO_ANGL_VEL,
};

struct MotionData {
    dev: ARef<device::Device>,
    kind: MotionKind,
    values: [AtomicI32; 3],
    aop_timestamp: AtomicU64,
    reports: AtomicU64,
}

impl MotionData {
    fn new(dev: ARef<device::Device>, kind: MotionKind) -> Result<Arc<Self>> {
        Ok(Arc::new(
            Self {
                dev,
                kind,
                values: [const { AtomicI32::new(0) }; 3],
                aop_timestamp: AtomicU64::new(0),
                reports: AtomicU64::new(0),
            },
            GFP_KERNEL,
        )?)
    }
}

impl FakehidListener for MotionData {
    fn process_fakehid_report(&self, timestamp: u64, data: &[u8]) -> Result<()> {
        if data.len() != MOTION_REPORT_LEN {
            dev_err!(
                self.dev,
                "invalid AOP motion report length {}, expected {}",
                data.len(),
                MOTION_REPORT_LEN
            );
            return Err(EIO);
        }

        let x = i32::from_le_bytes(data[6..10].try_into().unwrap());
        let y = i32::from_le_bytes(data[10..14].try_into().unwrap());
        let z = i32::from_le_bytes(data[14..18].try_into().unwrap());
        self.values[0].store(x, Ordering::Relaxed);
        self.values[1].store(y, Ordering::Relaxed);
        self.values[2].store(z, Ordering::Relaxed);
        self.aop_timestamp.store(timestamp, Ordering::Relaxed);
        let report = self.reports.fetch_add(1, Ordering::Relaxed) + 1;
        if report == 1 {
            let sensor = if self.kind == MotionKind::Accel {
                "accelerometer"
            } else {
                "gyroscope"
            };
            dev_info!(
                self.dev,
                "first AOP {} report: timestamp={:#x} raw=({}, {}, {})",
                sensor,
                timestamp,
                x,
                y,
                z
            );
        }
        Ok(())
    }
}

fn set_property(aop: &dyn AOP, svc: &EPICService, tag: u32, data: &[u8]) -> Result<u32> {
    let mut message = KVec::new();
    message.resize(data.len() + 8, 0, GFP_KERNEL)?;
    message[4..8].copy_from_slice(&tag.to_le_bytes());
    message[8..].copy_from_slice(data);
    aop.epic_call(svc, EPIC_SUBTYPE_SET_PROPERTY, &message)
}

fn calibration_for_location(data: &[u8], location: u32) -> Result<&[u8]> {
    /*
     * AppleSPUCalibration treats byte 3 == 1 as a v1 multi-calibration
     * container.  Byte 2 is the entry count; each entry is a little-endian
     * location and length followed by the packet that is actually sent to
     * AOP.  J713's resolved motion data contains one entry at location 0.
     */
    if data.len() < 4 || data[3] != 1 {
        return Ok(data);
    }

    let mut offset = 4usize;
    for _ in 0..data[2] {
        let header_end = offset.checked_add(8).ok_or(EINVAL)?;
        let header = data.get(offset..header_end).ok_or(EINVAL)?;
        let entry_location = u32::from_le_bytes(header[0..4].try_into().unwrap());
        let length = u32::from_le_bytes(header[4..8].try_into().unwrap()) as usize;
        let payload_end = header_end.checked_add(length).ok_or(EINVAL)?;
        let payload = data.get(header_end..payload_end).ok_or(EINVAL)?;
        if entry_location == location {
            return Ok(payload);
        }
        offset = payload_end;
    }

    Err(ENOENT)
}

fn calibrate_and_enable(aop: &dyn AOP, dev: &platform::Device, svc: &EPICService) -> Result<()> {
    let fwnode = dev.as_ref().fwnode().ok_or(EIO)?;
    for tag in [75u32, 76, 77] {
        let property = CString::try_from_fmt(fmt!("apple,calibration-{tag}"))?;
        let length = fwnode.property_count_elem::<u8>(&property)?;
        let value = fwnode
            .property_read_array_vec::<u8>(&property, length)?
            .required_by(dev.as_ref())?;
        let packet = calibration_for_location(&value, 0)?;
        let status = set_property(aop, svc, tag, packet)?;
        if status != 0 {
            dev_err!(
                dev.as_ref(),
                "AOP rejected calibration tag {} with status {:#x}",
                tag,
                status
            );
            return Err(EIO);
        }
        dev_info!(
            dev.as_ref(),
            "uploaded AOP calibration tag {} ({} byte packet)",
            tag,
            packet.len()
        );
    }
    let status = set_property(aop, svc, 0, &REPORT_INTERVAL_US.to_le_bytes())?;
    if status != 0 {
        dev_err!(
            dev.as_ref(),
            "AOP rejected report enable with status {:#x}",
            status
        );
        return Err(EIO);
    }
    Ok(())
}

unsafe extern "C" fn motion_read_raw(
    iio_dev: *mut bindings::iio_dev,
    chan: *const bindings::iio_chan_spec,
    val: *mut i32,
    val2: *mut i32,
    mask: isize,
) -> i32 {
    let data = unsafe { Arc::<MotionData>::borrow((*iio_dev).priv_.cast()) };
    let raw_mask = bindings::iio_chan_info_enum_IIO_CHAN_INFO_RAW as isize;
    let scale_mask = bindings::iio_chan_info_enum_IIO_CHAN_INFO_SCALE as isize;
    let frequency_mask = bindings::iio_chan_info_enum_IIO_CHAN_INFO_SAMP_FREQ as isize;

    if mask == raw_mask {
        let axis = unsafe { (*chan).address as usize };
        if axis >= data.values.len() {
            return EINVAL.to_errno();
        }
        unsafe { *val = data.values[axis].load(Ordering::Relaxed) };
        return bindings::IIO_VAL_INT as i32;
    }
    if mask == scale_mask {
        unsafe {
            *val = 0;
            *val2 = if data.kind == MotionKind::Accel {
                149_638
            } else {
                266
            };
        }
        return bindings::IIO_VAL_INT_PLUS_NANO as i32;
    }
    if mask == frequency_mask {
        unsafe { *val = 50 };
        return bindings::IIO_VAL_INT as i32;
    }
    EINVAL.to_errno()
}

struct IIOSpec {
    channels: [bindings::iio_chan_spec; 3],
    info: bindings::iio_info,
    _p: PhantomPinned,
}

struct MotionRegistration {
    dev: *mut bindings::iio_dev,
    spec: Pin<KBox<IIOSpec>>,
    registered: bool,
    _p: PhantomData<MotionData>,
}

impl MotionRegistration {
    fn channel(iio_type: u32, axis: i32, address: usize) -> bindings::iio_chan_spec {
        let mut channel = bindings::iio_chan_spec {
            type_: iio_type,
            channel2: axis,
            address: address as _,
            scan_index: -1,
            __bindgen_anon_1: bindings::iio_chan_spec__bindgen_ty_1 {
                scan_type: bindings::iio_scan_type {
                    sign: b's' as _,
                    realbits: 32,
                    storagebits: 32,
                    ..Default::default()
                },
            },
            info_mask_separate: 1 << bindings::iio_chan_info_enum_IIO_CHAN_INFO_RAW,
            info_mask_shared_by_type: (1 << bindings::iio_chan_info_enum_IIO_CHAN_INFO_SCALE)
                | (1 << bindings::iio_chan_info_enum_IIO_CHAN_INFO_SAMP_FREQ),
            ..Default::default()
        };
        channel.set_modified(1);
        channel
    }

    fn new(
        data: Arc<MotionData>,
        config: &'static MotionConfig,
        module: &ThisModule,
    ) -> Result<Self> {
        let spec = KBox::pin(
            IIOSpec {
                channels: [
                    Self::channel(config.iio_type, bindings::iio_modifier_IIO_MOD_X as i32, 0),
                    Self::channel(config.iio_type, bindings::iio_modifier_IIO_MOD_Y as i32, 1),
                    Self::channel(config.iio_type, bindings::iio_modifier_IIO_MOD_Z as i32, 2),
                ],
                info: bindings::iio_info {
                    read_raw: Some(motion_read_raw),
                    ..Default::default()
                },
                _p: PhantomPinned,
            },
            GFP_KERNEL,
        )?;
        let mut this = Self {
            dev: ptr::null_mut(),
            spec,
            registered: false,
            _p: PhantomData,
        };
        this.dev = unsafe { bindings::iio_device_alloc(data.dev.as_raw(), 0) };
        if this.dev.is_null() {
            return Err(ENOMEM);
        }
        unsafe {
            (*this.dev).priv_ = data.clone().into_foreign().cast();
            (*this.dev).name = config.name.as_ptr() as _;
            (*this.dev).channels = this.spec.channels.as_ptr();
            (*this.dev).num_channels = this.spec.channels.len() as i32;
            (*this.dev).info = &this.spec.info;
            (*this.dev).modes = bindings::INDIO_DIRECT_MODE as i32;
        }
        let ret = unsafe { bindings::__iio_device_register(this.dev, module.as_ptr()) };
        if ret < 0 {
            return Err(Error::from_errno(ret));
        }
        this.registered = true;
        Ok(this)
    }
}

impl Drop for MotionRegistration {
    fn drop(&mut self) {
        if !self.dev.is_null() {
            unsafe {
                if self.registered {
                    bindings::iio_device_unregister(self.dev);
                }
                Arc::<MotionData>::from_foreign((*self.dev).priv_.cast());
                bindings::iio_device_free(self.dev);
            }
        }
    }
}

unsafe impl Send for MotionRegistration {}
unsafe impl Sync for MotionRegistration {}

struct IIOAopMotionDriver {
    _registration: MotionRegistration,
    aop: Arc<dyn AOP>,
    service: EPICService,
}

kernel::of_device_table!(
    OF_TABLE,
    MODULE_OF_TABLE,
    <IIOAopMotionDriver as platform::Driver>::IdInfo,
    [
        (of::DeviceId::new(c_str!("apple,aop-accel")), &ACCEL_CONFIG),
        (of::DeviceId::new(c_str!("apple,aop-gyro")), &GYRO_CONFIG),
    ]
);

impl platform::Driver for IIOAopMotionDriver {
    type IdInfo = &'static MotionConfig;

    const OF_ID_TABLE: Option<of::IdTable<Self::IdInfo>> = Some(&OF_TABLE);

    fn probe(
        pdev: &platform::Device<Core>,
        info: Option<&Self::IdInfo>,
    ) -> impl PinInit<Self, Error> {
        let config = *info.ok_or(ENODEV)?;
        let dev = pdev.as_ref();
        let parent = dev.parent().ok_or(ENODEV)?;
        // SAFETY: the parent AOP driver stores a repr(transparent) Arc<dyn AOP>.
        let aop_ptr = unsafe { Pin::<KBox<Arc<dyn AOP>>>::borrow(parent.get_drvdata()) };
        let aop = (&*aop_ptr).clone();
        // SAFETY: AOP creates each platform child with EPICService platform data.
        let service = unsafe { *((*dev.as_raw()).platform_data as *const EPICService) };
        let data = MotionData::new(dev.into(), config.kind)?;
        aop.add_fakehid_listener(service, data.clone())?;
        if let Err(error) = calibrate_and_enable(aop.as_ref(), pdev, &service) {
            aop.remove_fakehid_listener(&service);
            return Err(error);
        }
        let registration = match MotionRegistration::new(data, config, &THIS_MODULE) {
            Ok(registration) => registration,
            Err(error) => {
                let _ = set_property(aop.as_ref(), &service, 0, &0u32.to_le_bytes());
                aop.remove_fakehid_listener(&service);
                return Err(error);
            }
        };
        dev_info!(
            dev,
            "enabled {} Hz calibrated AOP motion reports",
            1_000_000 / REPORT_INTERVAL_US
        );
        Ok(Self {
            _registration: registration,
            aop,
            service,
        })
    }
}

impl Drop for IIOAopMotionDriver {
    fn drop(&mut self) {
        let _ = set_property(self.aop.as_ref(), &self.service, 0, &0u32.to_le_bytes());
        self.aop.remove_fakehid_listener(&self.service);
    }
}

unsafe impl Send for IIOAopMotionDriver {}
unsafe impl Sync for IIOAopMotionDriver {}

module_platform_driver! {
    type: IIOAopMotionDriver,
    name: "iio_aop_motion",
    description: "Apple AOP accelerometer and gyroscope driver",
    license: "Dual MIT/GPL",
    alias: ["platform:iio_aop_motion"],
}
