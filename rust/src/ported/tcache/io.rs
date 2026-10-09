//! Shared-index lookup, record checksums, compression, and publication.

use super::*;
use crate::ffi;
use core::ffi::{c_char, c_int, c_void};
use core::mem::size_of;
use core::ptr;
use core::sync::atomic::{AtomicU64, Ordering};

use super::store::{free_bytes, open_store};

unsafe fn slot_hash(mut key: u64) -> u64 {
    key ^= key >> 33;
    key = key.wrapping_mul(0xff51afd7ed558ccd);
    key ^ (key >> 29)
}

#[inline(always)]
unsafe fn stored_sum(z: *const TcStored) -> u64 {
    let mut p = z.add(1).cast::<u8>();
    let mut n = (*z).size as usize - size_of::<TcStored>();
    let mut h = store::mix(
        store::mix(0x452821e638d01377, (*z).key),
        ((*z).raw as u64) << 32 | (*z).zlen as u64,
    );
    while n >= 8 {
        h = store::mix(h, ptr::read_unaligned(p.cast::<u64>()));
        p = p.add(8);
        n -= 8;
    }
    h
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_tcache_find(key: u64) -> *const ffi::OcerzTcRecHead {
    let mut found: *const ffi::OcerzTcRecHead = ptr::null();
    libc::pthread_mutex_lock(ptr::addr_of_mut!(G_LOCK));
    if open_store() {
        let mut i = slot_hash(key) & G_MASK;
        let mut k = 0;
        while k < TC_PROBE {
            let slot = G_SLOT.add(i as usize);
            let sk = AtomicU64::from_ptr(ptr::addr_of_mut!((*slot).key)).load(Ordering::Acquire);
            if sk == 0 {
                break;
            }
            if sk != key {
                i = (i + 1) & G_MASK;
                k += 1;
                continue;
            }
            let loc = AtomicU64::from_ptr(ptr::addr_of_mut!((*slot).loc)).load(Ordering::Acquire);
            if loc == 0 {
                break;
            }
            let no = loc >> TC_LOC_OFF_BITS;
            let off = loc & ((1u64 << TC_LOC_OFF_BITS) - 1);
            let mut z = store::file_at(no, off, size_of::<TcStored>() as u64).cast::<TcStored>();
            if z.is_null()
                || (*z).magic != TC_ZREC_MAGIC
                || (*z).key != key
                || ((*z).size as usize) < size_of::<TcStored>()
                || (*z).size as usize > TC_REC_MAX
                || (*z).size % 8 != 0
                || (*z).zlen > (*z).size - size_of::<TcStored>() as u32
                || (*z).raw as usize > TC_REC_MAX - size_of::<ffi::OcerzTcRecHead>()
                || (*z).raw % 8 != 0
            {
                break;
            }
            z = store::file_at(no, off, (*z).size as u64).cast::<TcStored>();
            if z.is_null() || (*z).sum != stored_sum(z) {
                break;
            }
            let r = G_RBUF.cast::<ffi::OcerzTcRecHead>();
            let dst = r.add(1).cast::<u8>();
            let raw = (*z).raw as usize;
            let zlen = (*z).zlen as usize;
            let got = if zlen == raw {
                libc::memcpy(dst.cast::<c_void>(), z.add(1).cast::<c_void>(), raw);
                raw
            } else {
                compression_decode_buffer(
                    dst,
                    TC_REC_MAX - size_of::<ffi::OcerzTcRecHead>(),
                    z.add(1).cast::<u8>(),
                    zlen,
                    G_DSCRATCH.cast::<c_void>(),
                    COMPRESSION_LZ4_RAW,
                )
            };
            if got != raw {
                break;
            }
            (*r).magic = ffi::OCERZ_TC_REC_MAGIC;
            (*r).size = (size_of::<ffi::OcerzTcRecHead>() + raw) as u32;
            (*r).key = key;
            (*r).sum = (*z).sum;
            found = r;
            break;
        }
    }
    libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_LOCK));
    found
}

pub(super) unsafe fn publish(key: u64, loc: u64) {
    let mut i = slot_hash(key) & G_MASK;
    let mut k = 0;
    while k < TC_PROBE {
        let slot = G_SLOT.add(i as usize);
        let key_atomic = AtomicU64::from_ptr(ptr::addr_of_mut!((*slot).key));
        let mut sk = key_atomic.load(Ordering::Acquire);
        if sk == 0 {
            sk = match key_atomic.compare_exchange(0, key, Ordering::AcqRel, Ordering::Acquire) {
                Ok(_) => key,
                Err(actual) => actual,
            };
        }
        if sk == key {
            AtomicU64::from_ptr(ptr::addr_of_mut!((*slot).loc)).store(loc, Ordering::Release);
            return;
        }
        i = (i + 1) & G_MASK;
        k += 1;
    }
    if G_FULL.load(Ordering::Relaxed) == 0 && G_LOG > 0 {
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: TCACHE[%d] index has no room near key %#llx\n".as_ptr(),
            libc::getpid(),
            key as libc::c_ulonglong,
        );
    }
    G_FULL.store(1, Ordering::Relaxed);
    AtomicU64::from_ptr(ptr::addr_of_mut!((*G_HDR).pad[0])).store(1, Ordering::Relaxed);
}

unsafe fn open_data() -> bool {
    if G_DFD >= 0 {
        return true;
    }
    let mut tries = 0;
    while tries < 8 {
        let no = AtomicU64::from_ptr(ptr::addr_of_mut!((*G_HDR).next_file))
            .fetch_add(1, Ordering::Relaxed);
        if no == 0 || no >= (1 << 24) {
            return false;
        }
        let mut path = [0 as c_char; 1200];
        libc::snprintf(
            path.as_mut_ptr(),
            path.len(),
            c"%s/d-%llu.td".as_ptr(),
            ptr::addr_of_mut!(G_DIR).cast::<c_char>(),
            no as libc::c_ulonglong,
        );
        let fd = libc::open(
            path.as_ptr(),
            libc::O_WRONLY | libc::O_CREAT | libc::O_EXCL | libc::O_CLOEXEC,
            0o644 as c_int,
        );
        if fd < 0 {
            tries += 1;
            continue;
        }
        let h = TcDatHdr {
            magic: TC_DAT_MAGIC,
            version: 1,
            fp: G_FP,
        };
        if libc::pwrite(
            fd,
            (&h as *const TcDatHdr).cast::<c_void>(),
            size_of::<TcDatHdr>(),
            0,
        ) != size_of::<TcDatHdr>() as libc::ssize_t
        {
            libc::close(fd);
            libc::unlink(path.as_ptr());
            return false;
        }
        G_DNO = no;
        G_DFD = fd;
        G_DOFF = size_of::<TcDatHdr>() as u64;
        return true;
    }
    false
}

pub(super) unsafe fn write_stored(p: *const u8, n: usize) {
    if n == 0 || G_HDR.is_null() || G_FULL.load(Ordering::Relaxed) != 0 {
        return;
    }
    let bytes = AtomicU64::from_ptr(ptr::addr_of_mut!((*G_HDR).bytes)).load(Ordering::Relaxed);
    if bytes.wrapping_add(n as u64) > G_ROOM_END
        || free_bytes() < G_FLOOR_BYTES.wrapping_add(n as u64)
    {
        if G_LOG > 0 {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: TCACHE[%d] %s: out of disk room (%llu MB free); no longer writing\n"
                    .as_ptr(),
                libc::getpid(),
                ptr::addr_of_mut!(G_DIR).cast::<c_char>(),
                (free_bytes() >> 20) as libc::c_ulonglong,
            );
        }
        G_FULL.store(1, Ordering::Relaxed);
        return;
    }
    if !open_data() {
        return;
    }
    let mut done = 0usize;
    while done < n {
        let w = libc::pwrite(
            G_DFD,
            p.add(done).cast::<c_void>(),
            n - done,
            G_DOFF.wrapping_add(done as u64) as libc::off_t,
        );
        if w < 0 {
            if *libc::__error() == libc::EINTR {
                continue;
            }
            libc::close(G_DFD);
            G_DFD = -1;
            return;
        }
        done = done.wrapping_add(w as usize);
    }
    let mut at = 0usize;
    while at < n {
        let z = p.add(at).cast::<TcStored>();
        publish(
            (*z).key,
            (G_DNO << TC_LOC_OFF_BITS) | G_DOFF.wrapping_add(at as u64),
        );
        at = at.wrapping_add((*z).size as usize);
    }
    G_DOFF = G_DOFF.wrapping_add(n as u64);
    let total = AtomicU64::from_ptr(ptr::addr_of_mut!((*G_HDR).bytes))
        .fetch_add(n as u64, Ordering::Relaxed)
        .wrapping_add(n as u64);
    if total > G_CAP_BYTES {
        if G_FULL.load(Ordering::Relaxed) == 0 && G_LOG > 0 {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: TCACHE[%d] %s holds %llu MB; no longer writing\n".as_ptr(),
                libc::getpid(),
                ptr::addr_of_mut!(G_DIR).cast::<c_char>(),
                (total >> 20) as libc::c_ulonglong,
            );
        }
        G_FULL.store(1, Ordering::Relaxed);
        AtomicU64::from_ptr(ptr::addr_of_mut!((*G_HDR).pad[0])).store(1, Ordering::Relaxed);
    }
}

pub(super) unsafe fn store_buffer(raw_buf: *const u8, n: usize) {
    let mut o = 0usize;
    let mut at = 0usize;
    while at < n {
        let rec = raw_buf.add(at).cast::<ffi::OcerzTcRecHead>();
        at = at.wrapping_add((*rec).size as usize);
        let raw = (*rec).size - size_of::<ffi::OcerzTcRecHead>() as u32;
        let mut zl = compression_encode_buffer(
            G_ZBUF,
            TC_REC_MAX,
            rec.add(1).cast::<u8>(),
            raw as usize,
            G_ZSCRATCH.cast::<c_void>(),
            COMPRESSION_LZ4_RAW,
        );
        let mut payload: *const u8 = G_ZBUF;
        if zl == 0 || zl >= raw as usize {
            zl = raw as usize;
            payload = rec.add(1).cast::<u8>();
        }
        let size = (size_of::<TcStored>().wrapping_add(zl).wrapping_add(7)) & !7;
        if o.wrapping_add(size) > TC_OUT_BYTES {
            write_stored(G_OBUF, o);
            o = 0;
        }
        let z = G_OBUF.add(o).cast::<TcStored>();
        z.write(TcStored {
            magic: TC_ZREC_MAGIC,
            size: size as u32,
            key: (*rec).key,
            sum: 0,
            raw,
            zlen: zl as u32,
        });
        libc::memcpy(z.add(1).cast::<c_void>(), payload.cast::<c_void>(), zl);
        ptr::write_bytes(
            z.add(1).cast::<u8>().add(zl),
            0,
            size - size_of::<TcStored>() - zl,
        );
        (*z).sum = stored_sum(z);
        o = o.wrapping_add(size);
    }
    write_stored(G_OBUF, o);
}
