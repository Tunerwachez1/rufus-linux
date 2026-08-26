# Rufus Linux port - stage 0 build description
# Builds: libbled (vendored, POSIX-patched), the core HAL and rufus-cli.
# The original Windows/MinGW build in the repository root is not touched.

project('rufus-linux', 'c',
  version : '0.1.0-stage0',
  meson_version : '>= 0.60.0',
  default_options : [
    'c_std=gnu11',  # vendored wimlib uses GNU asm/attributes
    'warning_level=2',
    'b_pie=true',
    # optimization is required for _FORTIFY_SOURCE to be active
    'buildtype=debugoptimized',
  ])

udev_dep = dependency('libudev')

# This program runs as root and parses untrusted input (disk images,
# archive headers): enable the standard hardening set.
global_cargs = ['-D_DEFAULT_SOURCE', '-D_FILE_OFFSET_BITS=64',
  '-fstack-protector-strong', '-fstack-clash-protection',
  '-D_FORTIFY_SOURCE=2',
]

add_project_link_arguments('-Wl,-z,relro', '-Wl,-z,now', language : 'c')

inc_core = include_directories('core', 'hal')
inc_bled = include_directories('../src/bled')

# --------------------------------------------------------------------
# bled (busybox-lite decompressor) - vendored sources, POSIX branch added
# by our patches to src/bled/libbb.h and src/bled/bled.h
bled_srcs = files(
  '../src/bled/bled.c',
  '../src/bled/bled_size.c',
  '../src/bled/crc32.c',
  '../src/bled/data_align.c',
  '../src/bled/data_extract_all.c',
  '../src/bled/data_skip.c',
  '../src/bled/decompress_bunzip2.c',
  '../src/bled/decompress_gunzip.c',
  '../src/bled/decompress_uncompress.c',
  '../src/bled/decompress_unlzma.c',
  '../src/bled/decompress_unxz.c',
  '../src/bled/decompress_unzip.c',
  '../src/bled/decompress_unzstd.c',
  '../src/bled/decompress_vtsi.c',
  '../src/bled/filter_accept_all.c',
  '../src/bled/filter_accept_list.c',
  '../src/bled/filter_accept_reject_list.c',
  '../src/bled/find_list_entry.c',
  '../src/bled/fse_decompress.c',
  '../src/bled/header_list.c',
  '../src/bled/header_skip.c',
  '../src/bled/header_verbose_list.c',
  '../src/bled/huf_decompress.c',
  '../src/bled/init_handle.c',
  '../src/bled/open_transformer.c',
  '../src/bled/seek_by_jump.c',
  '../src/bled/seek_by_read.c',
  '../src/bled/xz_dec_bcj.c',
  '../src/bled/xz_dec_lzma2.c',
  '../src/bled/xz_dec_stream.c',
  '../src/bled/xxhash.c',
  '../src/bled/zstd_common.c',
  '../src/bled/zstd_decompress.c',
  '../src/bled/zstd_decompress_block.c',
  '../src/bled/zstd_ddict.c',
  '../src/bled/zstd_entropy_common.c',
  '../src/bled/zstd_error_private.c',
)

libbled = static_library('bled', bled_srcs,
  include_directories : inc_bled,
  c_args : global_cargs + ['-w', '-DBLED_EXPECT_DISK_IMAGE'],
)

# --------------------------------------------------------------------
# ms-sys (MBR/PBR boot code writers) - vendored sources compiled as is.
# The Win32 backend src/ms-sys/file.c is NOT built: core/msys_posix.c
# provides the same file.h contract against POSIX fds instead.
inc_mssys = include_directories('../src/ms-sys/inc')

mssys_srcs = files(
  '../src/ms-sys/br.c',
  '../src/ms-sys/fat12.c',
  '../src/ms-sys/fat16.c',
  '../src/ms-sys/fat32.c',
  '../src/ms-sys/ntfs.c',
  '../src/ms-sys/partition_info.c',
)

libmssys = static_library('mssys', mssys_srcs, 'core/msys_posix.c',
  include_directories : [inc_core, inc_mssys],
  c_args : global_cargs,
)

# --------------------------------------------------------------------
# libcdio (ISO9660/Joliet/RockRidge + UDF parsing) - vendored sources,
# compiled unmodified. Our Linux config.h comes FIRST on the include path
# so quoted "config.h" includes resolve to it instead of the MinGW one.
inc_cdio = include_directories(
  'vendor/cdio-linux',
  '../src/libcdio',
  '../src/libcdio/driver',
  '../src/libcdio/iso9660',
  '../src/libcdio/udf',
)

cdio_srcs = files(
  '../src/libcdio/driver/ds.c',
  '../src/libcdio/driver/_cdio_stdio.c',
  '../src/libcdio/driver/util.c',
  '../src/libcdio/driver/read.c',
  '../src/libcdio/driver/utf8.c',
  '../src/libcdio/driver/track.c',
  '../src/libcdio/driver/sector.c',
  '../src/libcdio/driver/logging.c',
  '../src/libcdio/driver/memory.c',
  '../src/libcdio/driver/_cdio_stream.c',
  '../src/libcdio/driver/disc.c',
  '../src/libcdio/udf/udf.c',
  '../src/libcdio/udf/udf_fs.c',
  '../src/libcdio/udf/udf_time.c',
  '../src/libcdio/udf/udf_file.c',
  '../src/libcdio/udf/filemode.c',
  '../src/libcdio/iso9660/rock.c',
  '../src/libcdio/iso9660/xa.c',
  '../src/libcdio/iso9660/iso9660.c',
  '../src/libcdio/iso9660/iso9660_fs.c',
)

libcdio = static_library('cdio', cdio_srcs,
  'vendor/cdio-linux/cdio_stubs.c',
  include_directories : inc_cdio,
  c_args : global_cargs + ['-w', '-DHAVE_CONFIG_H'],
)

# --------------------------------------------------------------------
# wimlib 1.14.4 (upstream tarball, LGPLv3/GPLv3) - vendored unmodified.
# Our hand-written vendor/wimlib-linux/config.h stands in for the one
# autotools would generate; the Win32 and ntfs-3g integration modules are
# not built. Used by the file-copy pipeline to split >4 GiB *.wim files
# into FAT32-compatible .swm parts (Windows setup reassembles them).
inc_wim = include_directories(
  'vendor/wimlib',
  'vendor/wimlib/include',
  'vendor/wimlib-linux',
)

wim_srcs = files(
  'vendor/wimlib/src/add_image.c',
  'vendor/wimlib/src/avl_tree.c',
  'vendor/wimlib/src/blob_table.c',
  'vendor/wimlib/src/compress.c',
  'vendor/wimlib/src/compress_common.c',
  'vendor/wimlib/src/compress_parallel.c',
  'vendor/wimlib/src/compress_serial.c',
  'vendor/wimlib/src/cpu_features.c',
  'vendor/wimlib/src/decompress.c',
  'vendor/wimlib/src/decompress_common.c',
  'vendor/wimlib/src/delete_image.c',
  'vendor/wimlib/src/dentry.c',
  'vendor/wimlib/src/divsufsort.c',
  'vendor/wimlib/src/encoding.c',
  'vendor/wimlib/src/error.c',
  'vendor/wimlib/src/export_image.c',
  'vendor/wimlib/src/extract.c',
  'vendor/wimlib/src/file_io.c',
  'vendor/wimlib/src/header.c',
  'vendor/wimlib/src/inode.c',
  'vendor/wimlib/src/inode_fixup.c',
  'vendor/wimlib/src/inode_table.c',
  'vendor/wimlib/src/integrity.c',
  'vendor/wimlib/src/iterate_dir.c',
  'vendor/wimlib/src/join.c',
  'vendor/wimlib/src/lcpit_matchfinder.c',
  'vendor/wimlib/src/lzms_common.c',
  'vendor/wimlib/src/lzms_compress.c',
  'vendor/wimlib/src/lzms_decompress.c',
  'vendor/wimlib/src/lzx_common.c',
  'vendor/wimlib/src/lzx_compress.c',
  'vendor/wimlib/src/lzx_decompress.c',
  'vendor/wimlib/src/metadata_resource.c',
  'vendor/wimlib/src/mount_image.c',
  'vendor/wimlib/src/pathlist.c',
  'vendor/wimlib/src/paths.c',
  'vendor/wimlib/src/pattern.c',
  'vendor/wimlib/src/progress.c',
  'vendor/wimlib/src/reference.c',
  'vendor/wimlib/src/registry.c',
  'vendor/wimlib/src/reparse.c',
  'vendor/wimlib/src/resource.c',
  'vendor/wimlib/src/scan.c',
  'vendor/wimlib/src/security.c',
  'vendor/wimlib/src/sha1.c',
  'vendor/wimlib/src/solid.c',
  'vendor/wimlib/src/split.c',
  'vendor/wimlib/src/tagged_items.c',
  'vendor/wimlib/src/template.c',
  'vendor/wimlib/src/test_support.c',
  'vendor/wimlib/src/textfile.c',
  'vendor/wimlib/src/threads.c',
  'vendor/wimlib/src/timestamp.c',
  'vendor/wimlib/src/unix_apply.c',
  'vendor/wimlib/src/unix_capture.c',
  'vendor/wimlib/src/update_image.c',
  'vendor/wimlib/src/util.c',
  'vendor/wimlib/src/verify.c',
  'vendor/wimlib/src/wimboot.c',
  'vendor/wimlib/src/wim.c',
  'vendor/wimlib/src/write.c',
  'vendor/wimlib/src/xml.c',
  'vendor/wimlib/src/xmlproc.c',
  'vendor/wimlib/src/xml_windows.c',
  'vendor/wimlib/src/xpress_compress.c',
  'vendor/wimlib/src/xpress_decompress.c',
)

libwim = static_library('wim', wim_srcs,
  include_directories : inc_wim,
  c_args : global_cargs + ['-w', '-DHAVE_CONFIG_H', '-D_GNU_SOURCE'],
)

# --------------------------------------------------------------------
# Core + HAL + CLI

core_lib = static_library('rufuscore',
  'core/log.c',
  'core/image.c',
  'core/job.c',
  c_args : global_cargs,
)

part_lib = static_library('rufuspart', 'core/partition.c',
  include_directories : [inc_core, inc_mssys],
  c_args : global_cargs,
)

pipe_lib = static_library('rufuspipe',
  'core/filesys.c',
  'core/isocopy.c',
  include_directories : [inc_core, inc_cdio, inc_wim],
  c_args : global_cargs,
)

libwim_dep = declare_dependency(link_with : libwim)

hal_lib = static_library('rufushallinux',
  'hal-linux/devices.c',
  'hal-linux/blockio.c',
  include_directories : inc_core,
  c_args : global_cargs,
  dependencies : udev_dep,
)

rufus_cli = executable('rufus-cli',
  'cli/main.c',
  include_directories : [inc_core, inc_bled, inc_mssys],
  c_args : global_cargs,
  link_with : [core_lib, hal_lib, libbled, libmssys, part_lib, pipe_lib,
    libcdio],
  dependencies : [libwim_dep],
)

# --------------------------------------------------------------------
# Tests

test_msys = executable('test-msys',
  'tests/test-msys.c',
  include_directories : [inc_core, inc_mssys],
  c_args : global_cargs,
  link_with : [core_lib, libmssys],
)
test('msys', test_msys)

test_partition = executable('test-partition',
  'tests/test-partition.c',
  include_directories : [inc_core, inc_mssys],
  c_args : global_cargs,
  link_with : [core_lib, libmssys, part_lib],
)
test('partition', test_partition)

test_job = executable('test-job',
  'tests/test-job.c',
  include_directories : [inc_core],
  c_args : global_cargs,
  link_with : [core_lib],
)
test('job', test_job)

test_cdio = executable('test-cdio',
  'tests/test-cdio.c',
  include_directories : [inc_core, inc_cdio],
  c_args : global_cargs,
  link_with : [libcdio],
)
# not registered with test(): needs a real ISO path as argument

test_wimsplit = executable('test-wimsplit',
  'tests/test-wimsplit.c',
  include_directories : [inc_wim],
  c_args : global_cargs,
  link_with : [core_lib, libwim],
)
test('wimsplit', test_wimsplit)
