//! Buffered record handoff, detached writer thread, flush, and fork reset.

use super::*;
use crate::ffi;
use core::ffi::{c_int, c_void};
use core::mem::{size_of, zeroed};
use core::ptr;

use super::io::store_buffer;
use super::store::open_store;

unsafe extern "C" fn writer_main(_arg: *mut c_void) -> *mut c_void {
    libc::pthread_mutex_lock(ptr::addr_of_mut!(G_QLOCK));
    loop {
        while G_QN == 0 {
            libc::pthread_cond_wait(ptr::addr_of_mut!(G_QWORK), ptr::addr_of_mut!(G_QLOCK));
        }
        let b = (*ptr::addr_of_mut!(G_Q).cast::<QueueItem>()).p;
        let n = (*ptr::addr_of_mut!(G_Q).cast::<QueueItem>()).n;
        G_BUSY = 1;
        libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_QLOCK));
        store_buffer(b, n);
        libc::pthread_mutex_lock(ptr::addr_of_mut!(G_QLOCK));
        G_BUSY = 0;
        G_QN -= 1;
        ptr::copy(
            ptr::addr_of_mut!(G_Q).cast::<QueueItem>().add(1),
            ptr::addr_of_mut!(G_Q).cast::<QueueItem>(),
            G_QN as usize,
        );
        if G_NSPARE < TC_QMAX as c_int {
            let ix = G_NSPARE as usize;
            ptr::addr_of_mut!(G_SPARE)
                .cast::<*mut u8>()
                .add(ix)
                .write(b);
            G_NSPARE += 1;
        } else {
            libc::free(b.cast::<c_void>());
        }
        libc::pthread_cond_broadcast(ptr::addr_of_mut!(G_QDONE));
    }
}

extern "C" fn writer_main_entry(arg: *mut c_void) -> *mut c_void {
    unsafe { writer_main(arg) }
}

unsafe fn take_buffer() -> *mut u8 {
    let mut b = ptr::null_mut();
    libc::pthread_mutex_lock(ptr::addr_of_mut!(G_QLOCK));
    if G_NSPARE != 0 {
        G_NSPARE -= 1;
        b = ptr::addr_of_mut!(G_SPARE)
            .cast::<*mut u8>()
            .add(G_NSPARE as usize)
            .read();
    }
    libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_QLOCK));
    if !b.is_null() {
        b
    } else {
        libc::malloc(TC_BUF_BYTES).cast::<u8>()
    }
}

unsafe fn hand_off_locked() {
    if G_BUF.is_null() || G_BUF_N == 0 {
        return;
    }
    libc::pthread_mutex_lock(ptr::addr_of_mut!(G_QLOCK));
    if G_WRITER == 0 {
        let mut at = zeroed::<libc::pthread_attr_t>();
        libc::pthread_attr_init(&mut at);
        libc::pthread_attr_setdetachstate(&mut at, libc::PTHREAD_CREATE_DETACHED);
        G_WRITER = (libc::pthread_create(
            ptr::addr_of_mut!(G_WRITER_TID),
            &at,
            writer_main_entry,
            ptr::null_mut(),
        ) == 0) as c_int;
        libc::pthread_attr_destroy(&mut at);
    }
    if G_WRITER == 0 {
        libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_QLOCK));
        store_buffer(G_BUF, G_BUF_N);
        G_BUF_N = 0;
        return;
    }
    while G_QN == TC_QMAX as c_int {
        libc::pthread_cond_wait(ptr::addr_of_mut!(G_QDONE), ptr::addr_of_mut!(G_QLOCK));
    }
    let ix = G_QN as usize;
    ptr::addr_of_mut!(G_Q)
        .cast::<QueueItem>()
        .add(ix)
        .write(QueueItem {
            p: G_BUF,
            n: G_BUF_N,
        });
    G_QN += 1;
    libc::pthread_cond_signal(ptr::addr_of_mut!(G_QWORK));
    libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_QLOCK));
    G_BUF = take_buffer();
    G_BUF_N = 0;
}

extern "C" fn flush_atexit() {
    unsafe {
        ocerz_tcache_flush();
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_tcache_put(rec: *const ffi::OcerzTcRecHead) {
    if rec.is_null()
        || (*rec).size as usize > TC_REC_MAX
        || (*rec).size % 8 != 0
        || ((*rec).size as usize) < size_of::<ffi::OcerzTcRecHead>()
    {
        return;
    }
    libc::pthread_mutex_lock(ptr::addr_of_mut!(G_LOCK));
    if !open_store() || G_FULL.load(core::sync::atomic::Ordering::Relaxed) != 0 {
        libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_LOCK));
        return;
    }
    if G_ZBUF.is_null() {
        let zs = compression_encode_scratch_buffer_size(COMPRESSION_LZ4_RAW);
        G_ZBUF = libc::malloc(TC_REC_MAX).cast::<u8>();
        G_OBUF = libc::malloc(TC_OUT_BYTES).cast::<u8>();
        G_ZSCRATCH = libc::malloc(if zs != 0 { zs } else { 1 }).cast::<u8>();
        if G_ZBUF.is_null() || G_OBUF.is_null() || G_ZSCRATCH.is_null() {
            libc::free(G_ZBUF.cast::<c_void>());
            G_ZBUF = ptr::null_mut();
            libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_LOCK));
            return;
        }
        libc::atexit(flush_atexit);
    }
    if !G_BUF.is_null() && G_BUF_N.wrapping_add((*rec).size as usize) > TC_BUF_BYTES {
        hand_off_locked();
    }
    if G_BUF.is_null() {
        G_BUF = take_buffer();
    }
    if !G_BUF.is_null() {
        libc::memcpy(
            G_BUF.add(G_BUF_N).cast::<c_void>(),
            rec.cast::<c_void>(),
            (*rec).size as usize,
        );
        G_BUF_N = G_BUF_N.wrapping_add((*rec).size as usize);
    }
    libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_LOCK));
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_tcache_flush() {
    if G_ZBUF.is_null() {
        return;
    }
    libc::pthread_mutex_lock(ptr::addr_of_mut!(G_LOCK));
    hand_off_locked();
    libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_LOCK));

    let mut until = zeroed::<libc::timespec>();
    libc::clock_gettime(libc::CLOCK_REALTIME, &mut until);
    until.tv_sec = until.tv_sec.wrapping_add(3);
    libc::pthread_mutex_lock(ptr::addr_of_mut!(G_QLOCK));
    while (G_QN != 0 || G_BUSY != 0)
        && libc::pthread_cond_timedwait(
            ptr::addr_of_mut!(G_QDONE),
            ptr::addr_of_mut!(G_QLOCK),
            &until,
        ) == 0
    {}
    libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_QLOCK));
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_tcache_child() {
    libc::pthread_mutex_init(ptr::addr_of_mut!(G_LOCK), ptr::null());
    libc::pthread_mutex_init(ptr::addr_of_mut!(G_QLOCK), ptr::null());
    libc::pthread_cond_init(ptr::addr_of_mut!(G_QWORK), ptr::null());
    libc::pthread_cond_init(ptr::addr_of_mut!(G_QDONE), ptr::null());
    G_WRITER = 0;
    G_QN = 0;
    G_BUSY = 0;
    if G_DFD >= 0 {
        libc::close(G_DFD);
    }
    G_DFD = -1;
    G_BUF_N = 0;
}
