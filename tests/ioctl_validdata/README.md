# EXFAT_IOC_*_VALID_DATA ioctl test suite

This is a test suite for the valid data length(VDL) manipulation ioctl. As it
involves the use of `mount` as means of cache invalidation, the test suite is
meant to run as root, preferably on a VM. The best place in which to run it
would be CD/CI pipelines like Github or Gitlab.

Refrain from running it on your real machine.

## 0000-ioctl-behaviour.sh

(ex)FAT is Microsoft's invention, so the idea is that alternative
implementations(Linux) should strictly regard their implementation as the
de-facto standard. `SetValidData()` WinAPI only accept the new VDL in the range
`[old VDL, isize]`. This unit tests if our ioctl behaves the same way as
`SetValidData()` does(with the exception of mtime update behaviour).

## Tests through to 0004

With regards to how garbage data is returned through disk IO syscalls and how
caching/paging influences the behaviour.

"Dirtied" pages should always return the written data whereas whether to
invalidate pages containing zeros already cached before expanding the VDL should
be up to the kernel. Said pages are not invalidated so the result should be
"INCOHERENT". This is the intended behaviour, at least at the time of writing
the test. There's no correct behaviour. It doesn't matter if the kernel decides
to keep the "wrong" pages or drop them and do IO to page garbage data in.
