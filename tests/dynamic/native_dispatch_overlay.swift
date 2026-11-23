// Pins the guest Dispatch overlay: the DispatchGroup and DispatchTime APIs
// used below are Swift overlay symbols that native mode must answer with the
// x86_64 /usr/lib/swift/libswiftDispatch.dylib copied into runtime/guest.
import Dispatch

let group = DispatchGroup()
group.enter()
group.leave()
if group.wait(timeout: .now() + 1) == .success {
    print("dispatch overlay ok")
} else {
    print("dispatch overlay timeout")
}
