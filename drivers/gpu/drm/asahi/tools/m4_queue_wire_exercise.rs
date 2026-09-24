// Appended inside the host copy of g16_queue to inspect privately owned state.
// Firmware observations use the qualified baseline's raw offsets, independently
// of the candidate's named layout; changed producer/consumer offsets disagree.
pub(crate) fn exercise(variant:u64) {
    let dev=crate::driver::AsahiDevice;let vm=crate::mmu::Vm{slot:variant as u32};
    let uat=crate::mmu::Uat{vm:crate::mmu::Vm{slot:0}};
    let mut context=Context::new(&dev,&vm).unwrap();assert!(!context.registered().unwrap());
    context.data.write(0,&[1,2,4]).unwrap();assert!(context.registered().unwrap());
    for capacity in [0,1,3,65537] {assert!(matches!(Queue::new(&dev,&vm,&context,capacity),Err(crate::EINVAL)));}
    let capacity=if variant%2==0 {8}else{256};
    let mut q=Queue::new(&dev,&vm,&context,capacity).unwrap();
    assert_eq!(q.status().unwrap(),[0;6]);q.set_render_context(variant as u32).unwrap();
    assert_eq!(q.set_render_context(64),Err(crate::EINVAL));
    q.prepare_barrier().unwrap();assert_eq!(q.prepare_barrier(),Err(crate::EBUSY));
    fn retire(q:&mut Queue,next:u32){for off in [0x20,0x24,0x28]{q.info.u32(off,next).unwrap();}q.ring_state.u32(0x30,next).unwrap();}
    retire(&mut q,1);
    for _ in 0..capacity*2 {
        let (_,next)=q.append_batch_idle(&[0xfffffc2000001000]).unwrap();
        let pending=q.status().unwrap();assert!(!q.consumed_through(&pending,next));
        retire(&mut q,next);let status=q.status().unwrap();assert!(q.consumed_through(&status,next));
    }
    assert_eq!(q.append_idle(1),Err(crate::EINVAL));assert_eq!(q.append_batch(&[]),Err(crate::EINVAL));
    let (_,next)=q.append_batch(&[0xfffffc2000010000,0xfffffc2000020000]).unwrap();
    assert_eq!(q.append_batch_idle(&[0xfffffc2000030000]),Err(crate::EBUSY));retire(&mut q,next);
    q.advance_threshold().unwrap();q.submitted=u64::MAX;assert_eq!(q.advance_threshold(),Err(crate::EOVERFLOW));
    let mut n=ClientNotifications::new(&dev,&uat,&vm).unwrap();
    let previous=0x7fffff00u32.wrapping_add((variant as u32)<<8);
    n.prime_unused_stamps(true,previous).unwrap();n.prime_unused_stamps(false,previous).unwrap();
    for i in 0..3 {assert_eq!(n.stamps(i).unwrap(),[previous;2]);}
    for render in [true,false,true] {
        let checkpoint=n.checkpoint();n.reserve(render).unwrap();n.restore(checkpoint).unwrap();
        let (notes,counter)=n.reserve(render).unwrap();
        for note in notes {for value in [note.address,note.threshold,note.stamp,note.fw_stamp,note.event_seq,counter] {crate::record(4,value,0,&[]);}}
        n.publish(render).unwrap();assert_eq!(n.restore(checkpoint),Err(crate::EBUSY));
    }
    n.prime_unused_stamps(true,123).unwrap();n.prime_unused_stamps(false,123).unwrap();
    for i in 0..3 {assert_eq!(n.stamps(i).unwrap(),[previous;2]);}
}
